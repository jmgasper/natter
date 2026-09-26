// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <utility>
#include <variant>

namespace natter {

// Broad classes of failure, so a UI can decide what to do without string
// matching on Slack's error codes.
enum class ErrorKind {
	None,
	Transport,    // DNS, TCP, TLS, timeout - the request may not have arrived
	Http,         // a non-2xx HTTP status with no Slack error envelope
	Api,          // Slack answered ok:false with some other code
	Auth,         // not_authed, invalid_auth, token_revoked, ... - sign in again
	RateLimited,  // HTTP 429 or ratelimited, even after the retries
	NotFound,     // channel_not_found, message_not_found, ...
	Parse,        // the body was not the JSON we expected
	Unsupported,  // the method is not available for this token type
	Cancelled,
	InvalidArgument,
};

const char* errorKindName(ErrorKind kind);

struct Error {
	ErrorKind kind = ErrorKind::None;
	std::string code;      // Slack's error string, or a short local code
	std::string message;   // human readable detail
	int httpStatus = 0;
	int retryAfterSeconds = 0;

	Error() = default;
	Error(ErrorKind k, std::string c, std::string m = {}, int status = 0)
		: kind(k), code(std::move(c)), message(std::move(m)), httpStatus(status) {}

	// "invalid_auth (Auth)" style text for logs and dialogs.
	std::string describe() const;
	bool isAuth() const { return kind == ErrorKind::Auth; }
};

// Map a Slack `error` string from an ok:false envelope to an ErrorKind.
ErrorKind classifySlackError(const std::string& code);

// A value or an Error. std::expected is C++23; this is the part we need.
template <typename T>
class Result {
public:
	Result(T value) : fData(std::in_place_index<0>, std::move(value)) {}
	Result(Error error) : fData(std::in_place_index<1>, std::move(error)) {}

	bool ok() const { return fData.index() == 0; }
	explicit operator bool() const { return ok(); }

	T& value() & { return std::get<0>(fData); }
	const T& value() const& { return std::get<0>(fData); }
	T&& value() && { return std::get<0>(std::move(fData)); }
	T* operator->() { return &value(); }
	const T* operator->() const { return &value(); }
	T& operator*() & { return value(); }
	const T& operator*() const& { return value(); }

	const Error& error() const { return std::get<1>(fData); }

	T valueOr(T fallback) const { return ok() ? value() : std::move(fallback); }

private:
	std::variant<T, Error> fData;
};

template <>
class Result<void> {
public:
	Result() = default;
	Result(Error error) : fError(std::move(error)), fFailed(true) {}

	bool ok() const { return !fFailed; }
	explicit operator bool() const { return ok(); }
	const Error& error() const { return fError; }

private:
	Error fError;
	bool fFailed = false;
};

using Status = Result<void>;

}  // namespace natter
