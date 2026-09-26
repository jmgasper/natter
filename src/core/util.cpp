// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT

#include "natter/util.h"
#include "natter/result.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <unordered_set>

namespace natter {

// ---- errors -----------------------------------------------------------------

const char*
errorKindName(ErrorKind kind)
{
	switch (kind) {
		case ErrorKind::None: return "None";
		case ErrorKind::Transport: return "Transport";
		case ErrorKind::Http: return "Http";
		case ErrorKind::Api: return "Api";
		case ErrorKind::Auth: return "Auth";
		case ErrorKind::RateLimited: return "RateLimited";
		case ErrorKind::NotFound: return "NotFound";
		case ErrorKind::Parse: return "Parse";
		case ErrorKind::Unsupported: return "Unsupported";
		case ErrorKind::Cancelled: return "Cancelled";
		case ErrorKind::InvalidArgument: return "InvalidArgument";
	}
	return "?";
}


std::string
Error::describe() const
{
	std::string text = code.empty() ? std::string(errorKindName(kind)) : code;
	text += " (";
	text += errorKindName(kind);
	if (httpStatus != 0)
		text += ", HTTP " + std::to_string(httpStatus);
	text += ")";
	if (!message.empty())
		text += ": " + message;
	return text;
}


ErrorKind
classifySlackError(const std::string& code)
{
	static const std::unordered_set<std::string> kAuth = {
		"not_authed", "invalid_auth", "account_inactive", "token_revoked",
		"token_expired", "no_permission_for_auth", "org_login_required",
		"invalid_token", "user_removed_from_team", "team_disabled",
		"missing_cookie", "two_factor_setup_required"};
	static const std::unordered_set<std::string> kNotFound = {
		"channel_not_found", "user_not_found", "message_not_found",
		"thread_not_found", "file_not_found", "no_item_specified",
		"not_found", "users_not_found", "team_not_found", "bot_not_found",
		"no_reaction", "not_in_channel", "cant_update_message",
		"cant_delete_message"};
	static const std::unordered_set<std::string> kUnsupported = {
		"unknown_method", "method_deprecated", "not_allowed_token_type",
		"missing_scope", "method_not_supported_for_channel_type",
		"invalid_arguments", "enterprise_is_restricted", "not_supported",
		"no_permission", "rtm_connect_not_allowed"};

	if (code == "ratelimited" || code == "rate_limited")
		return ErrorKind::RateLimited;
	if (kAuth.count(code) != 0)
		return ErrorKind::Auth;
	if (kNotFound.count(code) != 0)
		return ErrorKind::NotFound;
	if (kUnsupported.count(code) != 0)
		return ErrorKind::Unsupported;
	return ErrorKind::Api;
}

// ---- logging ------------------------------------------------------------------

namespace {

std::mutex sLogLock;
LogHandler sLogHandler;

bool
debugEnabled()
{
	static const bool enabled = std::getenv("NATTER_DEBUG") != nullptr;
	return enabled;
}

}  // namespace


void
setLogHandler(LogHandler handler)
{
	std::lock_guard<std::mutex> lock(sLogLock);
	sLogHandler = std::move(handler);
}


void
log(LogLevel level, const std::string& message)
{
	LogHandler handler;
	{
		std::lock_guard<std::mutex> lock(sLogLock);
		handler = sLogHandler;
	}
	if (handler) {
		handler(level, message);
		return;
	}
	if (level < LogLevel::Warning && !debugEnabled())
		return;
	static const char* kNames[] = {"debug", "info", "warning", "error"};
	std::fprintf(stderr, "natter %s: %s\n", kNames[static_cast<int>(level)],
		message.c_str());
}

// ---- strings --------------------------------------------------------------------

std::string
urlEncode(std::string_view text)
{
	static const char* kHex = "0123456789ABCDEF";
	std::string out;
	out.reserve(text.size() * 3 / 2);
	for (unsigned char c : text) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'
			|| c == '~') {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += kHex[c >> 4];
			out += kHex[c & 15];
		}
	}
	return out;
}


std::string
urlDecode(std::string_view text)
{
	auto hexValue = [](char c) -> int {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); i++) {
		char c = text[i];
		if (c == '%' && i + 2 < text.size()) {
			int high = hexValue(text[i + 1]);
			int low = hexValue(text[i + 2]);
			if (high >= 0 && low >= 0) {
				out += static_cast<char>(high * 16 + low);
				i += 2;
				continue;
			}
		} else if (c == '+') {
			out += ' ';
			continue;
		}
		out += c;
	}
	return out;
}


std::string
formEncode(const std::vector<std::pair<std::string, std::string>>& fields)
{
	std::string out;
	for (const auto& [key, value] : fields) {
		if (!out.empty())
			out += '&';
		out += urlEncode(key);
		out += '=';
		out += urlEncode(value);
	}
	return out;
}


std::string
toLower(std::string_view text)
{
	std::string out(text);
	for (char& c : out) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return out;
}


bool
startsWith(std::string_view text, std::string_view prefix)
{
	return text.size() >= prefix.size()
		&& text.compare(0, prefix.size(), prefix) == 0;
}


bool
endsWith(std::string_view text, std::string_view suffix)
{
	return text.size() >= suffix.size()
		&& text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}


bool
equalsIgnoreCase(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++) {
		char x = a[i];
		char y = b[i];
		if (x >= 'A' && x <= 'Z')
			x = static_cast<char>(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z')
			y = static_cast<char>(y - 'A' + 'a');
		if (x != y)
			return false;
	}
	return true;
}


std::string
trim(std::string_view text)
{
	size_t start = 0;
	size_t end = text.size();
	while (start < end && (text[start] == ' ' || text[start] == '\t'
		|| text[start] == '\r' || text[start] == '\n'))
		start++;
	while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t'
		|| text[end - 1] == '\r' || text[end - 1] == '\n'))
		end--;
	return std::string(text.substr(start, end - start));
}


std::string
base64Encode(const uint8_t* data, size_t size)
{
	static const char* kAlphabet
		= "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((size + 2) / 3 * 4);
	size_t i = 0;
	for (; i + 2 < size; i += 3) {
		uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8)
			| data[i + 2];
		out += kAlphabet[(n >> 18) & 63];
		out += kAlphabet[(n >> 12) & 63];
		out += kAlphabet[(n >> 6) & 63];
		out += kAlphabet[n & 63];
	}
	if (i + 1 == size) {
		uint32_t n = uint32_t(data[i]) << 16;
		out += kAlphabet[(n >> 18) & 63];
		out += kAlphabet[(n >> 12) & 63];
		out += "==";
	} else if (i + 2 == size) {
		uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
		out += kAlphabet[(n >> 18) & 63];
		out += kAlphabet[(n >> 12) & 63];
		out += kAlphabet[(n >> 6) & 63];
		out += '=';
	}
	return out;
}


std::string
base64Encode(std::string_view data)
{
	return base64Encode(reinterpret_cast<const uint8_t*>(data.data()),
		data.size());
}


std::string
sha1(std::string_view data)
{
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int length = 0;
	EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha1(), nullptr);
	return std::string(reinterpret_cast<char*>(digest), length);
}


std::string
randomBytes(size_t count)
{
	std::string out(count, '\0');
	if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()),
			static_cast<int>(count)) != 1) {
		// RAND_bytes only fails when the PRNG cannot be seeded; fall back to
		// something unpredictable enough for WebSocket masks and keys.
		std::random_device device;
		for (char& c : out)
			c = static_cast<char>(device());
	}
	return out;
}


void
appendUtf8(std::string& out, uint32_t cp)
{
	if (cp < 0x80) {
		out += static_cast<char>(cp);
	} else if (cp < 0x800) {
		out += static_cast<char>(0xC0 | (cp >> 6));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += static_cast<char>(0xE0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (cp >> 18));
		out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	}
}

// ---- timestamps ------------------------------------------------------------------

namespace {

void
splitTs(std::string_view ts, std::string_view& whole, std::string_view& fraction)
{
	size_t dot = ts.find('.');
	whole = ts.substr(0, dot);
	fraction = dot == std::string_view::npos ? std::string_view() : ts.substr(dot + 1);
	while (whole.size() > 1 && whole[0] == '0')
		whole.remove_prefix(1);
}

}  // namespace


int
compareTs(std::string_view a, std::string_view b)
{
	std::string_view aWhole, aFraction, bWhole, bFraction;
	splitTs(a, aWhole, aFraction);
	splitTs(b, bWhole, bFraction);
	if (aWhole.size() != bWhole.size())
		return aWhole.size() < bWhole.size() ? -1 : 1;
	int c = aWhole.compare(bWhole);
	if (c != 0)
		return c < 0 ? -1 : 1;
	// Fractions compare digit by digit, missing digits being zero.
	size_t length = std::max(aFraction.size(), bFraction.size());
	for (size_t i = 0; i < length; i++) {
		char x = i < aFraction.size() ? aFraction[i] : '0';
		char y = i < bFraction.size() ? bFraction[i] : '0';
		if (x != y)
			return x < y ? -1 : 1;
	}
	return 0;
}


int64_t
tsSeconds(std::string_view ts)
{
	int64_t value = 0;
	for (char c : ts) {
		if (c == '.')
			break;
		if (c < '0' || c > '9')
			return 0;
		value = value * 10 + (c - '0');
	}
	return value;
}

// ---- JSON ---------------------------------------------------------------------------

std::string
jStr(const json& object, const char* key, std::string fallback)
{
	if (!object.is_object())
		return fallback;
	auto it = object.find(key);
	if (it == object.end())
		return fallback;
	if (it->is_string())
		return it->get<std::string>();
	if (it->is_number_integer())
		return std::to_string(it->get<int64_t>());
	if (it->is_number_float()) {
		std::string text = it->dump();
		return text;
	}
	if (it->is_boolean())
		return it->get<bool>() ? "true" : "false";
	return fallback;
}


int64_t
jInt(const json& object, const char* key, int64_t fallback)
{
	if (!object.is_object())
		return fallback;
	auto it = object.find(key);
	if (it == object.end())
		return fallback;
	if (it->is_number_integer())
		return it->get<int64_t>();
	if (it->is_number_float())
		return static_cast<int64_t>(it->get<double>());
	if (it->is_boolean())
		return it->get<bool>() ? 1 : 0;
	if (it->is_string()) {
		const std::string& text = it->get_ref<const std::string&>();
		char* end = nullptr;
		long long value = std::strtoll(text.c_str(), &end, 10);
		if (end != text.c_str())
			return value;
	}
	return fallback;
}


bool
jBool(const json& object, const char* key, bool fallback)
{
	if (!object.is_object())
		return fallback;
	auto it = object.find(key);
	if (it == object.end())
		return fallback;
	if (it->is_boolean())
		return it->get<bool>();
	if (it->is_number())
		return it->get<double>() != 0;
	if (it->is_string()) {
		const std::string& text = it->get_ref<const std::string&>();
		return text == "true" || text == "1";
	}
	return fallback;
}


double
jDouble(const json& object, const char* key, double fallback)
{
	if (!object.is_object())
		return fallback;
	auto it = object.find(key);
	if (it == object.end())
		return fallback;
	if (it->is_number())
		return it->get<double>();
	if (it->is_string())
		return std::strtod(it->get_ref<const std::string&>().c_str(), nullptr);
	return fallback;
}


const json&
jObj(const json& object, const char* key)
{
	static const json kNull;
	if (!object.is_object())
		return kNull;
	auto it = object.find(key);
	if (it == object.end() || !it->is_object())
		return kNull;
	return *it;
}


const json&
jArr(const json& object, const char* key)
{
	static const json kNull;
	if (!object.is_object())
		return kNull;
	auto it = object.find(key);
	if (it == object.end() || !it->is_array())
		return kNull;
	return *it;
}


bool
jHas(const json& object, const char* key)
{
	if (!object.is_object())
		return false;
	auto it = object.find(key);
	return it != object.end() && !it->is_null();
}


json
parseJson(std::string_view text)
{
	return json::parse(text.begin(), text.end(), nullptr, false);
}

}  // namespace natter
