// Natter - CurlTransport against a tiny in-process HTTP server.
// SPDX-License-Identifier: MIT

#include "testing.h"
#include "natter/http.h"
#include "natter/util.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

using namespace natter;

namespace {

struct SeenRequest {
	std::string method;
	std::string path;
	Headers headers;
	std::string body;
};

// Accepts connections on 127.0.0.1, one thread each; `respond` builds the
// whole HTTP response for a request. Every response closes the connection.
class TinyServer {
public:
	explicit TinyServer(std::function<std::string(const SeenRequest&)> respond)
		:
		fRespond(std::move(respond))
	{
		fListen = ::socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		::setsockopt(fListen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		::bind(fListen, reinterpret_cast<sockaddr*>(&address), sizeof(address));
		::listen(fListen, 16);
		socklen_t length = sizeof(address);
		::getsockname(fListen, reinterpret_cast<sockaddr*>(&address), &length);
		port = ntohs(address.sin_port);
		fAccept = std::thread([this] { acceptLoop(); });
	}

	~TinyServer()
	{
		fStopping = true;
		::shutdown(fListen, SHUT_RDWR);
		::close(fListen);
		fAccept.join();
		for (std::thread& worker : fWorkers)
			worker.join();
	}

	std::string url(const std::string& path) const
	{
		return "http://127.0.0.1:" + std::to_string(port) + path;
	}

	std::vector<SeenRequest> seen()
	{
		std::lock_guard<std::mutex> guard(fLock);
		return fSeen;
	}

	int port = 0;

private:
	void acceptLoop()
	{
		while (!fStopping) {
			int client = ::accept(fListen, nullptr, nullptr);
			if (client < 0)
				return;
			std::lock_guard<std::mutex> guard(fLock);
			fWorkers.emplace_back([this, client] { serve(client); });
		}
	}

	void serve(int client)
	{
		std::string data;
		char buffer[8192];
		size_t headEnd;
		while ((headEnd = data.find("\r\n\r\n")) == std::string::npos) {
			ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
			if (n <= 0) {
				::close(client);
				return;
			}
			data.append(buffer, static_cast<size_t>(n));
		}
		SeenRequest request;
		std::string head = data.substr(0, headEnd);
		size_t lineEnd = head.find("\r\n");
		std::string requestLine = head.substr(0, lineEnd);
		request.method = requestLine.substr(0, requestLine.find(' '));
		size_t pathStart = requestLine.find(' ') + 1;
		request.path = requestLine.substr(pathStart, requestLine.rfind(' ') - pathStart);
		size_t pos = lineEnd + 2;
		while (pos < head.size()) {
			size_t end = head.find("\r\n", pos);
			if (end == std::string::npos)
				end = head.size();
			std::string line = head.substr(pos, end - pos);
			size_t colon = line.find(':');
			if (colon != std::string::npos)
				request.headers.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
			pos = end + 2;
		}
		size_t length = std::stoul(findHeader(request.headers, "Content-Length").value_or("0"));
		request.body = data.substr(headEnd + 4);
		while (request.body.size() < length) {
			ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
			if (n <= 0)
				break;
			request.body.append(buffer, static_cast<size_t>(n));
		}
		{
			std::lock_guard<std::mutex> guard(fLock);
			fSeen.push_back(request);
		}
		std::string response = fRespond(request);
		::send(client, response.data(), response.size(), MSG_NOSIGNAL);
		::shutdown(client, SHUT_WR);
		while (::recv(client, buffer, sizeof(buffer), 0) > 0) {
		}
		::close(client);
	}

	std::function<std::string(const SeenRequest&)> fRespond;
	int fListen = -1;
	std::atomic<bool> fStopping{false};
	std::thread fAccept;
	std::mutex fLock;
	std::vector<std::thread> fWorkers;
	std::vector<SeenRequest> fSeen;
};


std::string
reply(int status, const std::string& body, const std::string& extraHeaders = {})
{
	return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: "
		+ std::to_string(body.size()) + "\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body;
}

}  // namespace


TEST(http_get_with_headers)
{
	TinyServer server([](const SeenRequest&) {
		return reply(200, "{\"ok\":true}", "X-Test: yes\r\nContent-Type: application/json\r\n");
	});
	CurlTransport transport;
	HttpRequest request = HttpRequest::get(server.url("/api/thing?x=1"));
	request.headers.emplace_back("X-Custom", "hello");
	HttpResponse response = transport.perform(request);
	CHECK(response.transportOk());
	CHECK_EQ(response.status, 200);
	CHECK_EQ(response.body, std::string("{\"ok\":true}"));
	CHECK_EQ(response.header("x-test").value_or(""), std::string("yes"));
	std::vector<SeenRequest> seen = server.seen();
	REQUIRE(seen.size() == 1);
	CHECK_EQ(seen[0].method, std::string("GET"));
	CHECK_EQ(seen[0].path, std::string("/api/thing?x=1"));
	CHECK_EQ(findHeader(seen[0].headers, "X-Custom").value_or(""), std::string("hello"));
	CHECK(startsWith(findHeader(seen[0].headers, "User-Agent").value_or(""), "Natter/"));
}


TEST(http_post_bodies)
{
	TinyServer server([](const SeenRequest&) { return reply(200, "ok"); });
	CurlTransport transport;
	transport.perform(HttpRequest::postForm(server.url("/form"),
		{{"token", "xoxc-1"}, {"text", "a b&c"}}));
	transport.perform(HttpRequest::postJson(server.url("/json"), "{\"a\":1}"));
	HttpRequest raw;
	raw.method = "POST";
	raw.url = server.url("/raw");
	raw.bodyKind = BodyKind::Raw;
	raw.body = std::string("\x00\x01\x02", 3);
	transport.perform(raw);
	HttpRequest empty;
	empty.method = "POST";
	empty.url = server.url("/empty");
	transport.perform(empty);

	std::vector<SeenRequest> seen = server.seen();
	REQUIRE(seen.size() == 4);
	CHECK_EQ(seen[0].body, std::string("token=xoxc-1&text=a%20b%26c"));
	CHECK_EQ(findHeader(seen[0].headers, "Content-Type").value_or(""),
		std::string("application/x-www-form-urlencoded"));
	CHECK_EQ(seen[1].body, std::string("{\"a\":1}"));
	CHECK(startsWith(findHeader(seen[1].headers, "Content-Type").value_or(""),
		"application/json"));
	CHECK_EQ(seen[2].body.size(), size_t(3));
	CHECK_EQ(findHeader(seen[2].headers, "Content-Type").value_or(""),
		std::string("application/octet-stream"));
	CHECK_EQ(seen[3].method, std::string("POST"));
	CHECK(!findHeader(seen[3].headers, "Expect").has_value());
}


TEST(http_multipart)
{
	std::string path = std::string(NATTER_TEST_TMP_DIR) + "/part.txt";
	{
		std::ofstream out(path);
		out << "file contents here";
	}
	TinyServer server([](const SeenRequest&) { return reply(200, "ok"); });
	CurlTransport transport;
	HttpRequest request;
	request.method = "POST";
	request.url = server.url("/upload");
	request.bodyKind = BodyKind::Multipart;
	request.parts.push_back(MultipartPart{"channel", "C01", "", "", ""});
	request.parts.push_back(MultipartPart{"file", "", path, "notes.txt", "text/plain"});
	HttpResponse response = transport.perform(request);
	CHECK_EQ(response.status, 200);
	std::vector<SeenRequest> seen = server.seen();
	REQUIRE(seen.size() == 1);
	CHECK(startsWith(findHeader(seen[0].headers, "Content-Type").value_or(""),
		"multipart/form-data; boundary="));
	const std::string& body = seen[0].body;
	CHECK(body.find("name=\"channel\"") != std::string::npos);
	CHECK(body.find("\r\n\r\nC01\r\n") != std::string::npos);
	CHECK(body.find("name=\"file\"; filename=\"notes.txt\"") != std::string::npos);
	CHECK(body.find("Content-Type: text/plain") != std::string::npos);
	CHECK(body.find("file contents here") != std::string::npos);
	std::remove(path.c_str());
}


TEST(http_redirects_keep_the_cookie)
{
	TinyServer server([](const SeenRequest& request) {
		if (request.path == "/")
			return reply(302, "", "Location: /messages\r\n");
		if (request.path == "/messages")
			return reply(302, "", "Location: /final\r\n");
		return reply(403, "page with token");
	});
	CurlTransport transport;
	HttpRequest request = HttpRequest::get(server.url("/"));
	request.followRedirects = true;
	request.headers.emplace_back("Cookie", "d=xoxd-abc%2F; d-s=123");
	HttpResponse response = transport.perform(request);
	CHECK_EQ(response.status, 403);
	CHECK_EQ(response.body, std::string("page with token"));
	CHECK(endsWith(response.effectiveUrl, "/final"));
	std::vector<SeenRequest> seen = server.seen();
	REQUIRE(seen.size() == 3);
	for (const SeenRequest& hop : seen) {
		std::string cookie = findHeader(hop.headers, "Cookie").value_or("");
		CHECK(cookie.find("d=xoxd-abc%2F") != std::string::npos);
		CHECK(cookie.find("d-s=123") != std::string::npos);
	}

	// Without followRedirects the 302 comes back as is.
	HttpResponse direct = transport.perform(HttpRequest::get(server.url("/")));
	CHECK_EQ(direct.status, 302);
	CHECK_EQ(direct.header("Location").value_or(""), std::string("/messages"));
	// A reused handle does not keep the previous request's cookies.
	std::vector<SeenRequest> after = server.seen();
	CHECK(!findHeader(after.back().headers, "Cookie").has_value());
}


TEST(http_timeout_sink_and_abort)
{
	TinyServer server([](const SeenRequest& request) {
		if (request.path == "/slow")
			std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		return reply(200, std::string(100000, 'x'));
	});
	CurlTransport transport;
	HttpRequest slow = HttpRequest::get(server.url("/slow"));
	slow.timeoutMs = 300;
	HttpResponse timedOut = transport.perform(slow);
	CHECK(!timedOut.transportOk());
	CHECK(!timedOut.transportError.empty());
	CHECK_EQ(timedOut.status, 0);

	size_t streamed = 0;
	HttpRequest streaming = HttpRequest::get(server.url("/big"));
	streaming.sink = [&streamed](const char*, size_t size) {
		streamed += size;
		return true;
	};
	uint64_t lastProgress = 0;
	streaming.progress = [&lastProgress](uint64_t done, uint64_t) {
		lastProgress = done;
		return true;
	};
	HttpResponse response = transport.perform(streaming);
	CHECK_EQ(response.status, 200);
	CHECK(response.body.empty());
	CHECK_EQ(streamed, size_t(100000));
	CHECK_EQ(lastProgress, uint64_t(100000));

	HttpRequest aborted = HttpRequest::get(server.url("/big"));
	aborted.sink = [](const char*, size_t) { return false; };
	HttpResponse stopped = transport.perform(aborted);
	CHECK(stopped.aborted);
}


TEST(http_retry_after_and_threads)
{
	std::atomic<int> hits{0};
	TinyServer server([&hits](const SeenRequest& request) {
		if (request.path == "/limited" && hits++ == 0)
			return reply(429, "{\"ok\":false,\"error\":\"ratelimited\"}", "Retry-After: 1\r\n");
		return reply(200, "{\"ok\":true}");
	});
	CurlTransport transport;
	std::vector<int> waits;
	RetryPolicy policy;
	policy.sleeper = [&waits](int seconds) {
		waits.push_back(seconds);
		return true;
	};
	HttpResponse response = performWithRetry(transport,
		HttpRequest::get(server.url("/limited")), policy);
	CHECK_EQ(response.status, 200);
	REQUIRE(waits.size() == 1);
	CHECK_EQ(waits[0], 1);

	// One transport, many threads.
	std::atomic<int> ok{0};
	std::vector<std::thread> threads;
	for (int t = 0; t < 8; t++) {
		threads.emplace_back([&] {
			for (int i = 0; i < 5; i++) {
				if (transport.perform(HttpRequest::get(server.url("/any"))).status == 200)
					ok++;
			}
		});
	}
	for (std::thread& thread : threads)
		thread.join();
	CHECK_EQ(ok.load(), 40);

	// Nothing listening.
	HttpResponse refused = transport.perform(HttpRequest::get("http://127.0.0.1:1/"));
	CHECK(!refused.transportOk());
}
