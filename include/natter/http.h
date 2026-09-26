// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace natter {

using Headers = std::vector<std::pair<std::string, std::string>>;
using FormFields = std::vector<std::pair<std::string, std::string>>;

// Case-insensitive header lookup (the first match).
std::optional<std::string> findHeader(const Headers& headers, std::string_view name);

struct MultipartPart {
	std::string name;
	std::string data;         // the content, unless filePath is set
	std::string filePath;     // stream the part from this file instead
	std::string fileName;     // makes the part a file upload
	std::string contentType;  // optional
};

enum class BodyKind { None, Form, Json, Multipart, Raw };

struct HttpRequest {
	std::string method = "GET";
	std::string url;
	Headers headers;

	BodyKind bodyKind = BodyKind::None;
	FormFields form;                     // BodyKind::Form
	std::string body;                    // BodyKind::Json and BodyKind::Raw
	std::string contentType;             // BodyKind::Raw (default octet-stream)
	std::vector<MultipartPart> parts;    // BodyKind::Multipart

	long timeoutMs = 30000;          // whole transfer; 0 means no limit
	long connectTimeoutMs = 15000;
	bool followRedirects = false;

	// Streams the response body instead of collecting it in HttpResponse::body.
	// Return false to abort the transfer.
	std::function<bool(const char* data, size_t size)> sink;
	// Upload/download progress; return false to abort.
	std::function<bool(uint64_t done, uint64_t total)> progress;

	// Convenience builders.
	static HttpRequest get(std::string url);
	static HttpRequest postForm(std::string url, FormFields fields);
	static HttpRequest postJson(std::string url, std::string json);
};

struct HttpResponse {
	int status = 0;                  // 0 when the transport failed
	Headers headers;
	std::string body;
	std::string effectiveUrl;        // after redirects
	std::string transportError;      // empty unless the transport failed
	bool aborted = false;            // the sink or progress callback said stop

	bool transportOk() const { return transportError.empty() && status != 0; }
	bool success() const { return status >= 200 && status < 300; }
	std::optional<std::string> header(std::string_view name) const
	{
		return findHeader(headers, name);
	}
	// Seconds from a Retry-After header (delta-seconds form), or -1.
	int retryAfterSeconds() const;
};

// The seam between the Slack client and the network. perform() must be
// callable concurrently from several threads.
class HttpTransport {
public:
	virtual ~HttpTransport() = default;
	virtual HttpResponse perform(const HttpRequest& request) = 0;
};

struct RetryPolicy {
	int maxRateLimitRetries = 3;    // extra attempts after a 429
	int maxWaitSeconds = 120;       // longest single Retry-After we will sleep
	int defaultWaitSeconds = 5;     // when a 429 has no Retry-After header
	// Sleeps between attempts; returns false if the caller was cancelled.
	// Tests replace it so they do not really sleep.
	std::function<bool(int seconds)> sleeper;
	// Told about every 429 before the wait (method-free: it sees the URL).
	std::function<void(const std::string& url, int seconds)> onRateLimited;
};

// perform() plus the Retry-After dance: a 429 waits the advertised time and
// tries again, up to policy.maxRateLimitRetries times.
HttpResponse performWithRetry(HttpTransport& transport,
	const HttpRequest& request, const RetryPolicy& policy);

// libcurl implementation. Thread-safe: every call uses its own easy handle
// taken from a small pool, so keep-alive connections are reused.
class CurlTransport : public HttpTransport {
public:
	struct Options {
		std::string userAgent;   // default "Natter/<version>"
		std::string caFile;      // PEM bundle; empty = libcurl's default
		std::string caPath;
		std::string proxy;
		bool verbose = false;
		bool verifyPeer = true;
	};

	CurlTransport();
	explicit CurlTransport(Options options);
	~CurlTransport() override;

	HttpResponse perform(const HttpRequest& request) override;

private:
	void* acquireHandle();
	void releaseHandle(void* handle);

	Options fOptions;
	std::mutex fPoolLock;
	std::vector<void*> fPool;
};

// Shared default transport (a CurlTransport with default options).
std::shared_ptr<HttpTransport> defaultTransport();

const char* natterVersion();

}  // namespace natter
