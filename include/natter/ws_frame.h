// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

// RFC 6455 framing and the client handshake, free of any I/O so they can be
// unit tested.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "natter/http.h"
#include "natter/result.h"

namespace natter::ws {

enum class Opcode : uint8_t {
	Continuation = 0x0,
	Text = 0x1,
	Binary = 0x2,
	Close = 0x8,
	Ping = 0x9,
	Pong = 0xA,
};

inline bool isControl(Opcode opcode) { return static_cast<uint8_t>(opcode) >= 0x8; }

struct Frame {
	bool fin = true;
	uint8_t rsv = 0;          // RSV1-3 bits; must be 0 without extensions
	Opcode opcode = Opcode::Text;
	bool masked = false;
	std::string payload;      // unmasked
};

// Encode one frame. With a mask key the payload is masked (clients must
// mask); without one it is sent in the clear (servers, tests).
std::string encodeFrame(Opcode opcode, std::string_view payload, bool fin = true,
	const uint8_t* maskKey = nullptr);
// A masked client frame with a fresh random key.
std::string encodeClientFrame(Opcode opcode, std::string_view payload, bool fin = true);

std::string makeClosePayload(uint16_t code, std::string_view reason);
// Returns false for a malformed payload (length 1). An empty payload is
// valid and yields code 1005 (no status).
bool parseClosePayload(std::string_view payload, uint16_t& code, std::string& reason);

// Incremental frame parser.
class FrameDecoder {
public:
	enum class Status { NeedMore, Ready, Error };

	explicit FrameDecoder(size_t maxPayload = 64 * 1024 * 1024);

	void feed(const char* data, size_t size);
	void feed(std::string_view data) { feed(data.data(), data.size()); }
	// Ready: `frame` holds the next frame. NeedMore: feed more bytes.
	// Error: see error(); the stream is unusable afterwards.
	Status next(Frame& frame);
	const std::string& error() const { return fError; }
	uint16_t errorCloseCode() const { return fErrorCode; }
	size_t buffered() const { return fBuffer.size() - fOffset; }

private:
	std::string fBuffer;
	size_t fOffset = 0;
	size_t fMaxPayload;
	std::string fError;
	uint16_t fErrorCode = 0;
};

struct DataMessage {
	Opcode opcode = Opcode::Text;   // Text or Binary
	std::string data;
};

// Joins fragmented data frames. Feed it data and continuation frames only;
// control frames may arrive between fragments and are handled by the caller.
class MessageAssembler {
public:
	enum class Status { NeedMore, Ready, Error };

	explicit MessageAssembler(size_t maxMessage = 64 * 1024 * 1024);

	Status add(Frame&& frame, DataMessage& message);
	const std::string& error() const { return fError; }

private:
	bool fInMessage = false;
	Opcode fOpcode = Opcode::Text;
	std::string fData;
	size_t fMaxMessage;
	std::string fError;
};

// ---- handshake --------------------------------------------------------------------

struct Url {
	bool secure = false;
	std::string host;
	int port = 0;
	std::string resource = "/";   // path and query
};

std::optional<Url> parseUrl(std::string_view url);

// A random 16-byte key, base64 encoded.
std::string makeKey();
// base64(SHA-1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
std::string acceptForKey(std::string_view key);

std::string buildHandshakeRequest(const Url& url, const std::string& key,
	const Headers& extraHeaders, const std::string& userAgent = {});

struct HandshakeResponse {
	int status = 0;
	std::string reason;
	Headers headers;
};

// Parse the response head (up to and including the blank line).
std::optional<HandshakeResponse> parseHandshakeResponse(std::string_view head);
// 101, Upgrade: websocket, Connection: Upgrade and the right accept value.
Status checkHandshakeResponse(const HandshakeResponse& response,
	const std::string& key);

}  // namespace natter::ws
