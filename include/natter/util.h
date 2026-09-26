// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace natter {

using json = nlohmann::json;

// ---- logging ---------------------------------------------------------------

enum class LogLevel { Debug, Info, Warning, Error };

using LogHandler = std::function<void(LogLevel, const std::string&)>;

// Replace the log sink. The default writes Warning and Error to stderr, and
// Debug/Info too when NATTER_DEBUG is set in the environment. The handler may
// be called from any thread.
void setLogHandler(LogHandler handler);
void log(LogLevel level, const std::string& message);

// ---- strings ---------------------------------------------------------------

std::string urlEncode(std::string_view text);
std::string urlDecode(std::string_view text);
std::string formEncode(const std::vector<std::pair<std::string, std::string>>& fields);
std::string toLower(std::string_view text);
bool startsWith(std::string_view text, std::string_view prefix);
bool endsWith(std::string_view text, std::string_view suffix);
bool equalsIgnoreCase(std::string_view a, std::string_view b);
std::string trim(std::string_view text);

std::string base64Encode(const uint8_t* data, size_t size);
std::string base64Encode(std::string_view data);
std::string sha1(std::string_view data);   // raw 20 bytes
std::string randomBytes(size_t count);      // OpenSSL RAND_bytes

// Append a code point to a UTF-8 string.
void appendUtf8(std::string& out, uint32_t codePoint);

// ---- Slack timestamps --------------------------------------------------------

// Slack message timestamps ("1712345678.000100") are ids that also order
// messages. Compare them numerically without floating point.
int compareTs(std::string_view a, std::string_view b);
struct TsLess {
	bool operator()(const std::string& a, const std::string& b) const
	{
		return compareTs(a, b) < 0;
	}
};
// Seconds part as an integer, 0 when the ts is malformed.
int64_t tsSeconds(std::string_view ts);

// ---- tolerant JSON access ----------------------------------------------------
// Slack omits fields, sends null, and sometimes sends numbers as strings. These
// never throw.

std::string jStr(const json& object, const char* key, std::string fallback = {});
int64_t jInt(const json& object, const char* key, int64_t fallback = 0);
bool jBool(const json& object, const char* key, bool fallback = false);
double jDouble(const json& object, const char* key, double fallback = 0);
const json& jObj(const json& object, const char* key);  // null json when absent
const json& jArr(const json& object, const char* key);  // null json when absent
bool jHas(const json& object, const char* key);

// Parse without exceptions; returns a discarded value on bad input.
json parseJson(std::string_view text);

}  // namespace natter
