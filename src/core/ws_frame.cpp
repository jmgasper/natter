// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT

#include "natter/ws_frame.h"
#include "natter/util.h"

#include <cstring>

namespace natter::ws {

std::string
encodeFrame(Opcode opcode, std::string_view payload, bool fin, const uint8_t* maskKey)
{
	std::string out;
	out.reserve(payload.size() + 14);
	out += static_cast<char>((fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode));
	uint8_t maskBit = maskKey != nullptr ? 0x80 : 0x00;
	uint64_t length = payload.size();
	if (length < 126) {
		out += static_cast<char>(maskBit | length);
	} else if (length <= 0xFFFF) {
		out += static_cast<char>(maskBit | 126);
		out += static_cast<char>((length >> 8) & 0xFF);
		out += static_cast<char>(length & 0xFF);
	} else {
		out += static_cast<char>(maskBit | 127);
		for (int shift = 56; shift >= 0; shift -= 8)
			out += static_cast<char>((length >> shift) & 0xFF);
	}
	if (maskKey == nullptr) {
		out.append(payload);
		return out;
	}
	out.append(reinterpret_cast<const char*>(maskKey), 4);
	size_t start = out.size();
	out.append(payload);
	for (size_t i = 0; i < payload.size(); i++)
		out[start + i] = static_cast<char>(out[start + i] ^ maskKey[i & 3]);
	return out;
}


std::string
encodeClientFrame(Opcode opcode, std::string_view payload, bool fin)
{
	std::string key = randomBytes(4);
	return encodeFrame(opcode, payload, fin, reinterpret_cast<const uint8_t*>(key.data()));
}


std::string
makeClosePayload(uint16_t code, std::string_view reason)
{
	std::string out;
	out += static_cast<char>(code >> 8);
	out += static_cast<char>(code & 0xFF);
	// Control frames carry at most 125 bytes.
	out.append(reason.substr(0, 123));
	return out;
}


bool
parseClosePayload(std::string_view payload, uint16_t& code, std::string& reason)
{
	reason.clear();
	if (payload.empty()) {
		code = 1005;
		return true;
	}
	if (payload.size() == 1)
		return false;
	code = static_cast<uint16_t>((static_cast<uint8_t>(payload[0]) << 8)
		| static_cast<uint8_t>(payload[1]));
	reason.assign(payload.substr(2));
	return true;
}

// ---- FrameDecoder ------------------------------------------------------------------

FrameDecoder::FrameDecoder(size_t maxPayload)
	:
	fMaxPayload(maxPayload)
{
}


void
FrameDecoder::feed(const char* data, size_t size)
{
	if (!fError.empty())
		return;
	// Drop consumed bytes now and then so the buffer does not grow forever.
	if (fOffset > 0 && (fOffset == fBuffer.size() || fOffset > 64 * 1024)) {
		fBuffer.erase(0, fOffset);
		fOffset = 0;
	}
	fBuffer.append(data, size);
}


FrameDecoder::Status
FrameDecoder::next(Frame& frame)
{
	if (!fError.empty())
		return Status::Error;
	size_t available = fBuffer.size() - fOffset;
	if (available < 2)
		return Status::NeedMore;
	const auto* p = reinterpret_cast<const uint8_t*>(fBuffer.data() + fOffset);

	bool fin = (p[0] & 0x80) != 0;
	uint8_t rsv = (p[0] >> 4) & 0x07;
	uint8_t opcodeValue = p[0] & 0x0F;
	bool masked = (p[1] & 0x80) != 0;
	uint64_t length = p[1] & 0x7F;
	size_t header = 2;

	if (length == 126) {
		if (available < 4)
			return Status::NeedMore;
		length = (uint64_t(p[2]) << 8) | p[3];
		header = 4;
	} else if (length == 127) {
		if (available < 10)
			return Status::NeedMore;
		length = 0;
		for (int i = 0; i < 8; i++)
			length = (length << 8) | p[2 + i];
		header = 10;
		if ((length >> 63) != 0) {
			fError = "64-bit length with the high bit set";
			fErrorCode = 1002;
			return Status::Error;
		}
	}

	bool known = opcodeValue <= 0x2 || (opcodeValue >= 0x8 && opcodeValue <= 0xA);
	if (!known) {
		fError = "unknown opcode " + std::to_string(opcodeValue);
		fErrorCode = 1002;
		return Status::Error;
	}
	if (rsv != 0) {
		fError = "reserved bits set without an extension";
		fErrorCode = 1002;
		return Status::Error;
	}
	if (opcodeValue >= 0x8 && (!fin || length > 125)) {
		fError = "fragmented or oversized control frame";
		fErrorCode = 1002;
		return Status::Error;
	}
	if (length > fMaxPayload) {
		fError = "frame too large";
		fErrorCode = 1009;
		return Status::Error;
	}

	size_t maskBytes = masked ? 4 : 0;
	if (available < header + maskBytes + length)
		return Status::NeedMore;

	frame.fin = fin;
	frame.rsv = rsv;
	frame.opcode = static_cast<Opcode>(opcodeValue);
	frame.masked = masked;
	const char* data = fBuffer.data() + fOffset + header + maskBytes;
	frame.payload.assign(data, static_cast<size_t>(length));
	if (masked) {
		const uint8_t* key = p + header;
		for (size_t i = 0; i < frame.payload.size(); i++)
			frame.payload[i] = static_cast<char>(frame.payload[i] ^ key[i & 3]);
	}
	fOffset += header + maskBytes + static_cast<size_t>(length);
	return Status::Ready;
}

// ---- MessageAssembler ------------------------------------------------------------------

MessageAssembler::MessageAssembler(size_t maxMessage)
	:
	fMaxMessage(maxMessage)
{
}


MessageAssembler::Status
MessageAssembler::add(Frame&& frame, DataMessage& message)
{
	if (frame.opcode == Opcode::Continuation) {
		if (!fInMessage) {
			fError = "continuation frame without a message";
			return Status::Error;
		}
		if (fData.size() + frame.payload.size() > fMaxMessage) {
			fError = "message too large";
			return Status::Error;
		}
		fData += frame.payload;
	} else if (frame.opcode == Opcode::Text || frame.opcode == Opcode::Binary) {
		if (fInMessage) {
			fError = "new data frame inside a fragmented message";
			return Status::Error;
		}
		fInMessage = true;
		fOpcode = frame.opcode;
		fData = std::move(frame.payload);
	} else {
		fError = "control frame passed to the assembler";
		return Status::Error;
	}

	if (!frame.fin)
		return Status::NeedMore;
	message.opcode = fOpcode;
	message.data = std::move(fData);
	fData.clear();
	fInMessage = false;
	return Status::Ready;
}

// ---- handshake ------------------------------------------------------------------------

std::optional<Url>
parseUrl(std::string_view text)
{
	Url url;
	std::string_view rest;
	if (startsWith(text, "wss://")) {
		url.secure = true;
		rest = text.substr(6);
	} else if (startsWith(text, "ws://")) {
		rest = text.substr(5);
	} else if (startsWith(text, "https://")) {
		url.secure = true;
		rest = text.substr(8);
	} else if (startsWith(text, "http://")) {
		rest = text.substr(7);
	} else {
		return std::nullopt;
	}
	size_t slash = rest.find_first_of("/?");
	std::string_view authority = rest.substr(0, slash);
	if (slash != std::string_view::npos) {
		url.resource = std::string(rest.substr(slash));
		if (url.resource[0] == '?')
			url.resource.insert(0, "/");
	}
	if (authority.empty())
		return std::nullopt;

	url.port = url.secure ? 443 : 80;
	if (authority[0] == '[') {
		size_t close = authority.find(']');
		if (close == std::string_view::npos)
			return std::nullopt;
		url.host = std::string(authority.substr(1, close - 1));
		std::string_view after = authority.substr(close + 1);
		if (startsWith(after, ":"))
			url.port = std::atoi(std::string(after.substr(1)).c_str());
	} else {
		size_t colon = authority.rfind(':');
		if (colon != std::string_view::npos) {
			url.host = std::string(authority.substr(0, colon));
			url.port = std::atoi(std::string(authority.substr(colon + 1)).c_str());
		} else {
			url.host = std::string(authority);
		}
	}
	if (url.host.empty() || url.port <= 0 || url.port > 65535)
		return std::nullopt;
	return url;
}


std::string
makeKey()
{
	return base64Encode(randomBytes(16));
}


std::string
acceptForKey(std::string_view key)
{
	std::string input(key);
	input += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	return base64Encode(sha1(input));
}


std::string
buildHandshakeRequest(const Url& url, const std::string& key,
	const Headers& extraHeaders, const std::string& userAgent)
{
	std::string host = url.host.find(':') != std::string::npos
		? "[" + url.host + "]" : url.host;
	bool defaultPort = (url.secure && url.port == 443) || (!url.secure && url.port == 80);
	if (!defaultPort)
		host += ":" + std::to_string(url.port);

	std::string request = "GET " + url.resource + " HTTP/1.1\r\n";
	request += "Host: " + host + "\r\n";
	request += "Upgrade: websocket\r\n";
	request += "Connection: Upgrade\r\n";
	request += "Sec-WebSocket-Key: " + key + "\r\n";
	request += "Sec-WebSocket-Version: 13\r\n";
	if (!userAgent.empty())
		request += "User-Agent: " + userAgent + "\r\n";
	for (const auto& [name, value] : extraHeaders)
		request += name + ": " + value + "\r\n";
	request += "\r\n";
	return request;
}


std::optional<HandshakeResponse>
parseHandshakeResponse(std::string_view head)
{
	HandshakeResponse response;
	size_t lineEnd = head.find("\r\n");
	if (lineEnd == std::string_view::npos)
		return std::nullopt;
	std::string_view statusLine = head.substr(0, lineEnd);
	if (!startsWith(statusLine, "HTTP/1."))
		return std::nullopt;
	size_t space = statusLine.find(' ');
	if (space == std::string_view::npos)
		return std::nullopt;
	std::string_view rest = statusLine.substr(space + 1);
	response.status = std::atoi(std::string(rest.substr(0, 3)).c_str());
	if (rest.size() > 4)
		response.reason = std::string(rest.substr(4));

	size_t pos = lineEnd + 2;
	while (pos < head.size()) {
		size_t end = head.find("\r\n", pos);
		if (end == std::string_view::npos)
			end = head.size();
		std::string_view line = head.substr(pos, end - pos);
		pos = end + 2;
		if (line.empty())
			break;
		size_t colon = line.find(':');
		if (colon == std::string_view::npos)
			continue;
		response.headers.emplace_back(trim(line.substr(0, colon)),
			trim(line.substr(colon + 1)));
	}
	return response;
}


Status
checkHandshakeResponse(const HandshakeResponse& response, const std::string& key)
{
	if (response.status != 101) {
		ErrorKind kind = ErrorKind::Http;
		if (response.status == 401 || response.status == 403)
			kind = ErrorKind::Auth;
		else if (response.status == 429)
			kind = ErrorKind::RateLimited;
		return Error(kind, "handshake_status",
			"HTTP " + std::to_string(response.status) + " " + response.reason,
			response.status);
	}
	auto upgrade = findHeader(response.headers, "Upgrade");
	if (!upgrade || !equalsIgnoreCase(trim(*upgrade), "websocket"))
		return Error(ErrorKind::Transport, "handshake_upgrade", "no Upgrade: websocket");
	auto connection = findHeader(response.headers, "Connection");
	if (!connection || toLower(*connection).find("upgrade") == std::string::npos)
		return Error(ErrorKind::Transport, "handshake_connection", "no Connection: Upgrade");
	auto accept = findHeader(response.headers, "Sec-WebSocket-Accept");
	if (!accept || trim(*accept) != acceptForKey(key))
		return Error(ErrorKind::Transport, "handshake_accept", "bad Sec-WebSocket-Accept");
	auto extensions = findHeader(response.headers, "Sec-WebSocket-Extensions");
	if (extensions && !trim(*extensions).empty())
		return Error(ErrorKind::Transport, "handshake_extension", "unrequested extension");
	return {};
}

}  // namespace natter::ws
