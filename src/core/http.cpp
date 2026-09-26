// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT

#include "natter/http.h"
#include "natter/util.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#ifndef NATTER_VERSION
#define NATTER_VERSION "0.1.0"
#endif

namespace natter {

const char*
natterVersion()
{
	return NATTER_VERSION;
}


std::optional<std::string>
findHeader(const Headers& headers, std::string_view name)
{
	for (const auto& [key, value] : headers) {
		if (equalsIgnoreCase(key, name))
			return value;
	}
	return std::nullopt;
}


HttpRequest
HttpRequest::get(std::string url)
{
	HttpRequest request;
	request.method = "GET";
	request.url = std::move(url);
	return request;
}


HttpRequest
HttpRequest::postForm(std::string url, FormFields fields)
{
	HttpRequest request;
	request.method = "POST";
	request.url = std::move(url);
	request.bodyKind = BodyKind::Form;
	request.form = std::move(fields);
	return request;
}


HttpRequest
HttpRequest::postJson(std::string url, std::string body)
{
	HttpRequest request;
	request.method = "POST";
	request.url = std::move(url);
	request.bodyKind = BodyKind::Json;
	request.body = std::move(body);
	return request;
}


int
HttpResponse::retryAfterSeconds() const
{
	auto value = header("Retry-After");
	if (!value)
		return -1;
	std::string text = trim(*value);
	if (text.empty())
		return -1;
	char* end = nullptr;
	long seconds = std::strtol(text.c_str(), &end, 10);
	if (end == text.c_str() || seconds < 0)
		return -1;
	return static_cast<int>(seconds);
}


HttpResponse
performWithRetry(HttpTransport& transport, const HttpRequest& request,
	const RetryPolicy& policy)
{
	for (int attempt = 0;; attempt++) {
		HttpResponse response = transport.perform(request);
		if (response.status != 429 || attempt >= policy.maxRateLimitRetries)
			return response;

		int wait = response.retryAfterSeconds();
		if (wait < 0)
			wait = policy.defaultWaitSeconds;
		if (wait > policy.maxWaitSeconds)
			return response;
		if (policy.onRateLimited)
			policy.onRateLimited(request.url, wait);
		log(LogLevel::Info, "HTTP 429 for " + request.url + ", retrying in "
			+ std::to_string(wait) + " s");
		if (policy.sleeper) {
			if (!policy.sleeper(wait))
				return response;
		} else {
			std::this_thread::sleep_for(std::chrono::seconds(wait));
		}
	}
}

// ---- CurlTransport ---------------------------------------------------------------

namespace {

std::once_flag sCurlInit;

struct TransferState {
	const HttpRequest* request = nullptr;
	HttpResponse* response = nullptr;
	bool sawStatusLine = false;
};


size_t
writeCallback(char* data, size_t size, size_t count, void* user)
{
	auto* state = static_cast<TransferState*>(user);
	size_t bytes = size * count;
	if (state->request->sink) {
		if (!state->request->sink(data, bytes)) {
			state->response->aborted = true;
			return 0;
		}
		return bytes;
	}
	state->response->body.append(data, bytes);
	return bytes;
}


size_t
headerCallback(char* data, size_t size, size_t count, void* user)
{
	auto* state = static_cast<TransferState*>(user);
	size_t bytes = size * count;
	std::string_view line(data, bytes);
	while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
		line.remove_suffix(1);
	if (startsWith(line, "HTTP/")) {
		// A new response starts (redirects, 100-continue): keep only the
		// headers of the final one.
		state->response->headers.clear();
		return bytes;
	}
	size_t colon = line.find(':');
	if (colon != std::string_view::npos) {
		state->response->headers.emplace_back(
			trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
	}
	return bytes;
}


int
progressCallback(void* user, curl_off_t downloadTotal, curl_off_t downloadNow,
	curl_off_t uploadTotal, curl_off_t uploadNow)
{
	auto* state = static_cast<TransferState*>(user);
	if (!state->request->progress)
		return 0;
	bool upload = uploadTotal > 0 && downloadTotal == 0;
	uint64_t done = static_cast<uint64_t>(upload ? uploadNow : downloadNow);
	uint64_t total = static_cast<uint64_t>(upload ? uploadTotal : downloadTotal);
	if (!state->request->progress(done, total)) {
		state->response->aborted = true;
		return 1;
	}
	return 0;
}


std::string
urlHost(const std::string& url)
{
	size_t start = url.find("://");
	start = start == std::string::npos ? 0 : start + 3;
	size_t end = url.find_first_of(":/?#", start);
	std::string host = url.substr(start, end == std::string::npos ? std::string::npos
		: end - start);
	size_t at = host.rfind('@');
	if (at != std::string::npos)
		host = host.substr(at + 1);
	return toLower(host);
}


// Netscape cookie-file lines for "a=1; b=2", scoped like a browser would
// scope a cookie set by the parent domain (".slack.com" for
// "team.slack.com"), or host-only for IP addresses and short names.
std::vector<std::string>
cookieJarLines(const std::string& url, const std::string& cookieHeader)
{
	std::string host = urlHost(url);
	bool numeric = host.find_first_not_of("0123456789.") == std::string::npos
		|| host.find(':') != std::string::npos;
	size_t dots = static_cast<size_t>(std::count(host.begin(), host.end(), '.'));
	std::string domain = host;
	std::string subdomains = "FALSE";
	if (!numeric && dots >= 2) {
		domain = host.substr(host.find('.'));
		subdomains = "TRUE";
	}
	bool secure = startsWith(url, "https://");

	std::vector<std::string> lines;
	size_t pos = 0;
	while (pos < cookieHeader.size()) {
		size_t end = cookieHeader.find(';', pos);
		if (end == std::string::npos)
			end = cookieHeader.size();
		std::string pair = trim(std::string_view(cookieHeader).substr(pos, end - pos));
		pos = end + 1;
		size_t equals = pair.find('=');
		if (pair.empty() || equals == std::string::npos)
			continue;
		lines.push_back(domain + "\t" + subdomains + "\t/\t"
			+ (secure ? "TRUE" : "FALSE") + "\t0\t" + pair.substr(0, equals)
			+ "\t" + pair.substr(equals + 1));
	}
	return lines;
}

}  // namespace


CurlTransport::CurlTransport()
	:
	CurlTransport(Options())
{
}


CurlTransport::CurlTransport(Options options)
	:
	fOptions(std::move(options))
{
	std::call_once(sCurlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
	if (fOptions.userAgent.empty())
		fOptions.userAgent = std::string("Natter/") + natterVersion();
}


CurlTransport::~CurlTransport()
{
	std::lock_guard<std::mutex> lock(fPoolLock);
	for (void* handle : fPool)
		curl_easy_cleanup(static_cast<CURL*>(handle));
	fPool.clear();
}


void*
CurlTransport::acquireHandle()
{
	{
		std::lock_guard<std::mutex> lock(fPoolLock);
		if (!fPool.empty()) {
			void* handle = fPool.back();
			fPool.pop_back();
			curl_easy_reset(static_cast<CURL*>(handle));
			return handle;
		}
	}
	return curl_easy_init();
}


void
CurlTransport::releaseHandle(void* handle)
{
	std::lock_guard<std::mutex> lock(fPoolLock);
	if (fPool.size() < 8)
		fPool.push_back(handle);
	else
		curl_easy_cleanup(static_cast<CURL*>(handle));
}


HttpResponse
CurlTransport::perform(const HttpRequest& request)
{
	HttpResponse response;
	CURL* curl = static_cast<CURL*>(acquireHandle());
	if (curl == nullptr) {
		response.transportError = "curl_easy_init failed";
		return response;
	}

	TransferState state;
	state.request = &request;
	state.response = &response;

	char errorBuffer[CURL_ERROR_SIZE] = {0};
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
	curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, fOptions.userAgent.c_str());
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, request.timeoutMs);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, request.connectTimeoutMs);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, request.followRedirects ? 1L : 0L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
	if (request.progress) {
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
	}
	if (!fOptions.caFile.empty())
		curl_easy_setopt(curl, CURLOPT_CAINFO, fOptions.caFile.c_str());
	if (!fOptions.caPath.empty())
		curl_easy_setopt(curl, CURLOPT_CAPATH, fOptions.caPath.c_str());
	if (!fOptions.proxy.empty())
		curl_easy_setopt(curl, CURLOPT_PROXY, fOptions.proxy.c_str());
	if (!fOptions.verifyPeer) {
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	if (fOptions.verbose)
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

	struct curl_slist* headerList = nullptr;
	std::string cookie;
	for (const auto& [name, value] : request.headers) {
		if (equalsIgnoreCase(name, "Cookie")) {
			// CURLOPT_COOKIE rides every redirect hop; a raw Cookie header
			// may not. Slack's workspace pages redirect before the page that
			// carries the token.
			if (!cookie.empty())
				cookie += "; ";
			cookie += value;
			continue;
		}
		headerList = curl_slist_append(headerList, (name + ": " + value).c_str());
	}
	bool cookieEngine = false;
	if (!cookie.empty()) {
		if (request.followRedirects) {
			// Load the cookies into curl's engine scoped to the request's
			// domain, so they follow the redirect chain within that domain
			// but never leak to another host (CURLOPT_COOKIE would).
			cookieEngine = true;
			curl_easy_setopt(curl, CURLOPT_COOKIELIST, "ALL");
			for (const std::string& line : cookieJarLines(request.url, cookie))
				curl_easy_setopt(curl, CURLOPT_COOKIELIST, line.c_str());
		} else {
			curl_easy_setopt(curl, CURLOPT_COOKIE, cookie.c_str());
		}
	}

	std::string formBody;
	curl_mime* mime = nullptr;
	switch (request.bodyKind) {
		case BodyKind::None:
			break;
		case BodyKind::Form:
			formBody = formEncode(request.form);
			headerList = curl_slist_append(headerList,
				"Content-Type: application/x-www-form-urlencoded");
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
				static_cast<curl_off_t>(formBody.size()));
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, formBody.c_str());
			break;
		case BodyKind::Json:
			headerList = curl_slist_append(headerList,
				"Content-Type: application/json; charset=utf-8");
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
				static_cast<curl_off_t>(request.body.size()));
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
			break;
		case BodyKind::Raw: {
			std::string type = "Content-Type: "
				+ (request.contentType.empty() ? std::string("application/octet-stream")
					: request.contentType);
			headerList = curl_slist_append(headerList, type.c_str());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
				static_cast<curl_off_t>(request.body.size()));
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
			break;
		}
		case BodyKind::Multipart: {
			mime = curl_mime_init(curl);
			for (const MultipartPart& part : request.parts) {
				curl_mimepart* field = curl_mime_addpart(mime);
				curl_mime_name(field, part.name.c_str());
				if (!part.filePath.empty())
					curl_mime_filedata(field, part.filePath.c_str());
				else
					curl_mime_data(field, part.data.data(), part.data.size());
				if (!part.fileName.empty())
					curl_mime_filename(field, part.fileName.c_str());
				if (!part.contentType.empty())
					curl_mime_type(field, part.contentType.c_str());
			}
			curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
			break;
		}
	}
	// Posting with an explicit empty body still needs POST semantics.
	if (request.method == "POST" && request.bodyKind == BodyKind::None) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
	} else if (request.method != "GET" && request.method != "POST") {
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
	}
	// Slack's upload endpoints do not want "Expect: 100-continue" pauses.
	headerList = curl_slist_append(headerList, "Expect:");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

	CURLcode code = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	response.status = static_cast<int>(status);
	char* effective = nullptr;
	if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK
		&& effective != nullptr)
		response.effectiveUrl = effective;

	if (code != CURLE_OK && !(response.aborted && response.status != 0)) {
		response.transportError = errorBuffer[0] != '\0' ? errorBuffer
			: curl_easy_strerror(code);
		if (!response.aborted)
			response.status = 0;
	}

	curl_slist_free_all(headerList);
	if (mime != nullptr)
		curl_mime_free(mime);
	// Detach per-request pointers before the handle goes back to the pool.
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, nullptr);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
	curl_easy_setopt(curl, CURLOPT_MIMEPOST, nullptr);
	if (cookieEngine)
		curl_easy_setopt(curl, CURLOPT_COOKIELIST, "ALL");
	releaseHandle(curl);
	return response;
}


std::shared_ptr<HttpTransport>
defaultTransport()
{
	static std::shared_ptr<HttpTransport> sTransport
		= std::make_shared<CurlTransport>();
	return sTransport;
}

}  // namespace natter
