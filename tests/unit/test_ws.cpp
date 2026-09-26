// Natter - WebSocket framing, handshake and a loopback connection.
// SPDX-License-Identifier: MIT

#include "testing.h"
#include "natter/util.h"
#include "natter/websocket.h"
#include "natter/ws_frame.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace natter;
using namespace natter::ws;

namespace {

Frame
decodeOne(const std::string& bytes)
{
	FrameDecoder decoder;
	decoder.feed(bytes);
	Frame frame;
	FrameDecoder::Status status = decoder.next(frame);
	if (status != FrameDecoder::Status::Ready)
		testing::fail(__FILE__, __LINE__, "frame did not decode");
	return frame;
}

}  // namespace

TEST(ws_encode_small_unmasked)
{
	std::string bytes = encodeFrame(Opcode::Text, "Hello");
	// RFC 6455 section 5.7: a single-frame unmasked text message.
	CHECK_EQ(bytes, std::string("\x81\x05Hello", 7));
}


TEST(ws_encode_masked_rfc_example)
{
	const uint8_t key[4] = {0x37, 0xfa, 0x21, 0x3d};
	std::string bytes = encodeFrame(Opcode::Text, "Hello", true, key);
	CHECK_EQ(bytes, std::string("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11));
	Frame frame = decodeOne(bytes);
	CHECK(frame.masked);
	CHECK_EQ(frame.payload, std::string("Hello"));
	CHECK(frame.fin);
	CHECK(frame.opcode == Opcode::Text);
}


TEST(ws_length_encodings)
{
	for (size_t size : {size_t(0), size_t(125), size_t(126), size_t(65535), size_t(65536),
			size_t(200000)}) {
		std::string payload(size, 'x');
		for (size_t i = 0; i < size; i++)
			payload[i] = static_cast<char>('a' + i % 26);
		std::string bytes = encodeClientFrame(Opcode::Binary, payload);
		size_t header = size < 126 ? 2 : size <= 65535 ? 4 : 10;
		CHECK_EQ(bytes.size(), header + 4 + size);
		Frame frame = decodeOne(bytes);
		CHECK(frame.opcode == Opcode::Binary);
		CHECK(frame.payload == payload);
	}
	std::string medium = encodeFrame(Opcode::Text, std::string(300, 'a'));
	CHECK_EQ(static_cast<uint8_t>(medium[1]), 126);
	CHECK_EQ(static_cast<uint8_t>(medium[2]), 1);
	CHECK_EQ(static_cast<uint8_t>(medium[3]), 44);
}


TEST(ws_decoder_incremental_and_multiple)
{
	std::string stream = encodeFrame(Opcode::Text, "one") + encodeFrame(Opcode::Ping, "p")
		+ encodeFrame(Opcode::Text, std::string(1000, 'z'));
	FrameDecoder decoder;
	std::vector<Frame> frames;
	for (char c : stream) {
		decoder.feed(&c, 1);
		Frame frame;
		while (decoder.next(frame) == FrameDecoder::Status::Ready)
			frames.push_back(frame);
	}
	REQUIRE(frames.size() == 3);
	CHECK_EQ(frames[0].payload, std::string("one"));
	CHECK(frames[1].opcode == Opcode::Ping);
	CHECK_EQ(frames[2].payload.size(), size_t(1000));
	CHECK_EQ(decoder.buffered(), size_t(0));
}


TEST(ws_decoder_rejects_bad_frames)
{
	auto status = [](const std::string& bytes) {
		FrameDecoder decoder(1024);
		decoder.feed(bytes);
		Frame frame;
		return decoder.next(frame);
	};
	CHECK(status(std::string("\x83\x00", 2)) == FrameDecoder::Status::Error);   // opcode 3
	CHECK(status(std::string("\xC1\x00", 2)) == FrameDecoder::Status::Error);   // RSV1
	CHECK(status(std::string("\x09\x00", 2)) == FrameDecoder::Status::Error);   // fragmented ping
	CHECK(status(std::string("\x89\x7E\x00\x80", 4)) == FrameDecoder::Status::Error);
	CHECK(status(encodeFrame(Opcode::Text, std::string(2000, 'a')))
		== FrameDecoder::Status::Error);   // over the limit
	CHECK(status(std::string("\x81", 1)) == FrameDecoder::Status::NeedMore);
	FrameDecoder big(1024);
	big.feed(encodeFrame(Opcode::Text, std::string(2000, 'a')));
	Frame frame;
	big.next(frame);
	CHECK_EQ(big.errorCloseCode(), uint16_t(1009));
}


TEST(ws_fragmentation_with_interleaved_control)
{
	std::string stream = encodeFrame(Opcode::Text, "Hel", false)
		+ encodeFrame(Opcode::Ping, "are you there")
		+ encodeFrame(Opcode::Continuation, "lo, ", false)
		+ encodeFrame(Opcode::Continuation, "world", true);
	FrameDecoder decoder;
	decoder.feed(stream);
	MessageAssembler assembler;
	Frame frame;
	int pings = 0;
	std::vector<std::string> messages;
	while (decoder.next(frame) == FrameDecoder::Status::Ready) {
		if (isControl(frame.opcode)) {
			pings++;
			continue;
		}
		DataMessage message;
		if (assembler.add(std::move(frame), message) == MessageAssembler::Status::Ready)
			messages.push_back(message.data);
	}
	CHECK_EQ(pings, 1);
	REQUIRE(messages.size() == 1);
	CHECK_EQ(messages[0], std::string("Hello, world"));

	MessageAssembler strict;
	DataMessage message;
	Frame orphan;
	orphan.opcode = Opcode::Continuation;
	CHECK(strict.add(std::move(orphan), message) == MessageAssembler::Status::Error);
	MessageAssembler interleaved;
	Frame first;
	first.opcode = Opcode::Text;
	first.fin = false;
	Frame second;
	second.opcode = Opcode::Binary;
	CHECK(interleaved.add(std::move(first), message) == MessageAssembler::Status::NeedMore);
	CHECK(interleaved.add(std::move(second), message) == MessageAssembler::Status::Error);
}


TEST(ws_close_payload)
{
	std::string payload = makeClosePayload(1001, "going away");
	uint16_t code = 0;
	std::string reason;
	CHECK(parseClosePayload(payload, code, reason));
	CHECK_EQ(code, uint16_t(1001));
	CHECK_EQ(reason, std::string("going away"));
	CHECK(parseClosePayload("", code, reason));
	CHECK_EQ(code, uint16_t(1005));
	CHECK(!parseClosePayload("x", code, reason));
	CHECK(makeClosePayload(1000, std::string(300, 'r')).size() <= 125);
}


TEST(ws_handshake_accept_rfc_example)
{
	CHECK_EQ(acceptForKey("dGhlIHNhbXBsZSBub25jZQ=="),
		std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
	CHECK_EQ(makeKey().size(), size_t(24));
	CHECK(makeKey() != makeKey());
}


TEST(ws_url_parsing)
{
	std::optional<Url> url = parseUrl("wss://wss-primary.slack.com/websocket/abc?x=1");
	REQUIRE(url.has_value());
	CHECK(url->secure);
	CHECK_EQ(url->host, std::string("wss-primary.slack.com"));
	CHECK_EQ(url->port, 443);
	CHECK_EQ(url->resource, std::string("/websocket/abc?x=1"));
	url = parseUrl("ws://127.0.0.1:8080?ticket=1");
	REQUIRE(url.has_value());
	CHECK(!url->secure);
	CHECK_EQ(url->port, 8080);
	CHECK_EQ(url->resource, std::string("/?ticket=1"));
	url = parseUrl("ws://[::1]:9000/x");
	REQUIRE(url.has_value());
	CHECK_EQ(url->host, std::string("::1"));
	CHECK_EQ(url->port, 9000);
	CHECK(!parseUrl("ftp://x/").has_value());
	CHECK(!parseUrl("ws://:80/").has_value());
	CHECK(!parseUrl("ws://host:99999/").has_value());
}


TEST(ws_handshake_request_and_response)
{
	Url url = *parseUrl("ws://127.0.0.1:8080/rtm?t=1");
	std::string request = buildHandshakeRequest(url, "a2V5a2V5a2V5a2V5a2V5aw==",
		{{"Cookie", "d=xoxd-1"}}, "Natter/test");
	CHECK(startsWith(request, "GET /rtm?t=1 HTTP/1.1\r\n"));
	CHECK(request.find("Host: 127.0.0.1:8080\r\n") != std::string::npos);
	CHECK(request.find("Upgrade: websocket\r\n") != std::string::npos);
	CHECK(request.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
	CHECK(request.find("Cookie: d=xoxd-1\r\n") != std::string::npos);
	CHECK(endsWith(request, "\r\n\r\n"));
	Url secure = *parseUrl("wss://example.com/x");
	CHECK(buildHandshakeRequest(secure, "k", {}).find("Host: example.com\r\n")
		!= std::string::npos);

	std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
	std::optional<HandshakeResponse> good = parseHandshakeResponse(
		"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
		"Connection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n");
	REQUIRE(good.has_value());
	CHECK(checkHandshakeResponse(*good, key).ok());

	std::optional<HandshakeResponse> wrongAccept = parseHandshakeResponse(
		"HTTP/1.1 101 OK\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n"
		"Sec-WebSocket-Accept: bm9wZQ==\r\n\r\n");
	REQUIRE(wrongAccept.has_value());
	Status status = checkHandshakeResponse(*wrongAccept, key);
	CHECK(!status.ok() && status.error().code == "handshake_accept");

	std::optional<HandshakeResponse> denied = parseHandshakeResponse(
		"HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
	REQUIRE(denied.has_value());
	status = checkHandshakeResponse(*denied, key);
	CHECK(!status.ok() && status.error().kind == ErrorKind::Auth);
	CHECK(!parseHandshakeResponse("garbage").has_value());
}


TEST(ws_backoff_grows_and_resets)
{
	Backoff backoff(100, 1000, 2.0, 0.0);
	CHECK_EQ(backoff.nextDelayMs(), 100);
	CHECK_EQ(backoff.nextDelayMs(), 200);
	CHECK_EQ(backoff.nextDelayMs(), 400);
	CHECK_EQ(backoff.nextDelayMs(), 800);
	CHECK_EQ(backoff.nextDelayMs(), 1000);
	CHECK_EQ(backoff.nextDelayMs(), 1000);
	backoff.reset();
	CHECK_EQ(backoff.nextDelayMs(), 100);
	Backoff jittery(1000, 60000, 2.0, 0.2);
	int delay = jittery.nextDelayMs();
	CHECK(delay >= 800 && delay <= 1200);
}

// ---- loopback connection -----------------------------------------------------------

namespace {

// A one-shot WebSocket server on 127.0.0.1 written with the same codec.
class LoopbackServer {
public:
	LoopbackServer()
	{
		fListen = ::socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		::setsockopt(fListen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = 0;
		::bind(fListen, reinterpret_cast<sockaddr*>(&address), sizeof(address));
		::listen(fListen, 1);
		socklen_t length = sizeof(address);
		::getsockname(fListen, reinterpret_cast<sockaddr*>(&address), &length);
		port = ntohs(address.sin_port);
	}

	~LoopbackServer()
	{
		join();
		if (fClient >= 0)
			::close(fClient);
		::close(fListen);
	}

	void start(std::function<void(LoopbackServer&)> script)
	{
		fThread = std::thread([this, script] {
			fClient = ::accept(fListen, nullptr, nullptr);
			if (fClient < 0)
				return;
			std::string head;
			char c;
			while (head.find("\r\n\r\n") == std::string::npos && ::recv(fClient, &c, 1, 0) == 1)
				head += c;
			request = head;
			size_t keyAt = head.find("Sec-WebSocket-Key: ");
			std::string key = head.substr(keyAt + 19, head.find("\r\n", keyAt) - keyAt - 19);
			std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
				"Connection: Upgrade\r\nSec-WebSocket-Accept: " + acceptForKey(key) + "\r\n\r\n";
			sendRaw(response);
			script(*this);
		});
	}

	void join()
	{
		if (fThread.joinable())
			fThread.join();
	}

	void sendRaw(const std::string& bytes)
	{
		::send(fClient, bytes.data(), bytes.size(), 0);
	}

	// Next frame from the client (they must be masked).
	bool receive(Frame& frame)
	{
		for (;;) {
			FrameDecoder::Status status = fDecoder.next(frame);
			if (status == FrameDecoder::Status::Ready)
				return true;
			if (status == FrameDecoder::Status::Error)
				return false;
			char buffer[4096];
			ssize_t n = ::recv(fClient, buffer, sizeof(buffer), 0);
			if (n <= 0)
				return false;
			fDecoder.feed(buffer, static_cast<size_t>(n));
		}
	}

	int port = 0;
	std::string request;

private:
	int fListen = -1;
	int fClient = -1;
	std::thread fThread;
	FrameDecoder fDecoder;
};

}  // namespace


TEST(ws_loopback_exchange)
{
	LoopbackServer server;
	std::atomic<bool> allMasked{true};
	std::string echoed;
	std::string pong;
	uint16_t clientCloseCode = 0;
	server.start([&](LoopbackServer& s) {
		// Split message with a ping in between, then a binary one.
		s.sendRaw(encodeFrame(Opcode::Text, "{\"type\":", false)
			+ encodeFrame(Opcode::Ping, "hb")
			+ encodeFrame(Opcode::Continuation, "\"hello\"}", true)
			+ encodeFrame(Opcode::Binary, std::string("\x00\x01\x02", 3)));
		Frame frame;
		while (s.receive(frame)) {
			allMasked = allMasked && frame.masked;
			if (frame.opcode == Opcode::Pong)
				pong = frame.payload;
			else if (frame.opcode == Opcode::Text) {
				echoed = frame.payload;
				s.sendRaw(encodeFrame(Opcode::Close, makeClosePayload(4000, "bye")));
			} else if (frame.opcode == Opcode::Close) {
				std::string reason;
				parseClosePayload(frame.payload, clientCloseCode, reason);
				break;
			}
		}
	});

	WebSocketOptions options;
	options.pingIntervalMs = 0;
	WebSocketConnection connection(options);
	Status status = connection.connect("ws://127.0.0.1:" + std::to_string(server.port) + "/rtm?x=1",
		{{"Cookie", "d=xoxd-test"}});
	REQUIRE(status.ok());
	CHECK(connection.isOpen());

	std::vector<std::string> texts;
	std::vector<std::string> binaries;
	WebSocketConnection::Handlers handlers;
	handlers.onText = [&](std::string&& text) {
		texts.push_back(text);
		connection.sendText("echo:" + text);
	};
	handlers.onBinary = [&](std::string&& data) { binaries.push_back(data); };
	CloseInfo close = connection.run(handlers);

	CHECK(server.request.find("GET /rtm?x=1 HTTP/1.1") == 0);
	CHECK(server.request.find("Cookie: d=xoxd-test\r\n") != std::string::npos);
	REQUIRE(texts.size() == 1);
	CHECK_EQ(texts[0], std::string("{\"type\":\"hello\"}"));
	REQUIRE(binaries.size() == 1);
	CHECK_EQ(binaries[0].size(), size_t(3));
	CHECK_EQ(close.code, uint16_t(4000));
	CHECK_EQ(close.reason, std::string("bye"));
	CHECK(close.byServer);
	CHECK(!connection.isOpen());
	server.join();
	CHECK(allMasked.load());
	CHECK_EQ(pong, std::string("hb"));
	CHECK_EQ(echoed, std::string("echo:{\"type\":\"hello\"}"));
	CHECK_EQ(clientCloseCode, uint16_t(4000));
}


TEST(ws_loopback_client_close_and_timeout)
{
	LoopbackServer server;
	server.start([](LoopbackServer& s) {
		Frame frame;
		while (s.receive(frame)) {
			if (frame.opcode == Opcode::Close) {
				s.sendRaw(encodeFrame(Opcode::Close, frame.payload));
				break;
			}
		}
	});
	WebSocketConnection connection;
	REQUIRE(connection.connect("ws://127.0.0.1:" + std::to_string(server.port) + "/").ok());
	std::thread closer([&connection] {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		connection.close(1000, "done");
	});
	CloseInfo close = connection.run({});
	closer.join();
	CHECK_EQ(close.code, uint16_t(1000));
	CHECK(!close.byServer);
	CHECK(!connection.sendText("late"));

	// Nobody listening: the connect fails cleanly.
	WebSocketConnection nowhere;
	Status status = nowhere.connect("ws://127.0.0.1:1/");
	CHECK(!status.ok() && status.error().kind == ErrorKind::Transport);
}


TEST(ws_client_reconnects_with_new_target)
{
	// Two one-shot servers; the provider hands out the second after the first closes.
	LoopbackServer first;
	LoopbackServer second;
	auto script = [](LoopbackServer& s) {
		s.sendRaw(encodeFrame(Opcode::Text, "hi from " + std::to_string(s.port)));
		s.sendRaw(encodeFrame(Opcode::Close, makeClosePayload(1001, "")));
		Frame frame;
		while (s.receive(frame)) {
		}
	};
	first.start(script);
	second.start(script);

	std::atomic<int> calls{0};
	std::mutex lock;
	std::vector<std::string> received;
	std::vector<int> delays;
	WebSocketClient::Callbacks callbacks;
	callbacks.onText = [&](std::string&& text) {
		std::lock_guard<std::mutex> guard(lock);
		received.push_back(text);
	};
	callbacks.onDisconnected = [&](const CloseInfo&, const Error&, int retryInMs, int) {
		std::lock_guard<std::mutex> guard(lock);
		delays.push_back(retryInMs);
	};
	WebSocketClient client([&]() -> Result<WebSocketTarget> {
			int n = calls++;
			if (n >= 2)
				return Error(ErrorKind::Auth, "invalid_auth");   // stops the client
			LoopbackServer& server = n == 0 ? first : second;
			return WebSocketTarget{"ws://127.0.0.1:" + std::to_string(server.port) + "/", {}};
		},
		callbacks, WebSocketOptions(), Backoff(10, 50, 2.0, 0.0));
	client.start();
	for (int i = 0; i < 200 && client.running(); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	client.stop();
	std::lock_guard<std::mutex> guard(lock);
	REQUIRE(received.size() == 2);
	CHECK(received[0] != received[1]);
	REQUIRE(delays.size() == 3);
	CHECK(delays[0] >= 0);
	CHECK(delays[1] >= delays[0]);
	CHECK_EQ(delays[2], -1);   // auth errors are not retried
	CHECK_EQ(calls.load(), 3);
}
