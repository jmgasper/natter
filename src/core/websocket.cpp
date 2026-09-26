// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT

#include "natter/websocket.h"
#include "natter/util.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace natter {

namespace {

using Clock = std::chrono::steady_clock;

int64_t
nowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		Clock::now().time_since_epoch()).count();
}


bool
setNonBlocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return false;
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}


std::string
sslErrorText()
{
	std::string text;
	unsigned long code;
	while ((code = ERR_get_error()) != 0) {
		char buffer[256];
		ERR_error_string_n(code, buffer, sizeof(buffer));
		if (!text.empty())
			text += "; ";
		text += buffer;
	}
	return text.empty() ? std::string("TLS error") : text;
}


bool
isIpLiteral(const std::string& host)
{
	unsigned char buffer[sizeof(struct in6_addr)];
	return inet_pton(AF_INET, host.c_str(), buffer) == 1
		|| inet_pton(AF_INET6, host.c_str(), buffer) == 1;
}


bool
fileExists(const char* path)
{
	struct stat st;
	return ::stat(path, &st) == 0;
}

enum class Io { Ok, WouldBlock, Eof, Error };

struct PendingFrame {
	std::string bytes;        // an encoded frame
	bool isClose = false;
	uint16_t closeCode = 0;
	std::string closeReason;
};

}  // namespace


struct WebSocketConnection::Impl {
	int fd = -1;
	int wakeRead = -1;
	int wakeWrite = -1;
	SSL_CTX* ctx = nullptr;
	SSL* ssl = nullptr;
	BIO* rbio = nullptr;   // network -> TLS (owned by ssl)
	BIO* wbio = nullptr;   // TLS -> network (owned by ssl)

	std::string out;       // bytes waiting for the socket
	size_t outOffset = 0;
	std::string plain;     // decrypted bytes not yet parsed
	std::string error;

	ws::FrameDecoder decoder;
	ws::MessageAssembler assembler;

	std::mutex lock;
	std::deque<PendingFrame> pending;
	bool closeQueued = false;
	bool abortRequested = false;

	Impl(size_t maxMessage)
		:
		decoder(maxMessage),
		assembler(maxMessage)
	{
	}

	~Impl()
	{
		if (ssl != nullptr)
			SSL_free(ssl);
		if (ctx != nullptr)
			SSL_CTX_free(ctx);
		if (fd >= 0)
			::close(fd);
		if (wakeRead >= 0)
			::close(wakeRead);
		if (wakeWrite >= 0)
			::close(wakeWrite);
	}

	void wake()
	{
		if (wakeWrite >= 0) {
			char byte = 1;
			ssize_t ignored = ::write(wakeWrite, &byte, 1);
			(void)ignored;
		}
	}

	void drainWake()
	{
		char buffer[64];
		while (::read(wakeRead, buffer, sizeof(buffer)) > 0) {
		}
	}

	bool aborted()
	{
		std::lock_guard<std::mutex> guard(lock);
		return abortRequested;
	}

	// Wait for the socket (and the wake pipe). Returns the socket's revents,
	// 0 on timeout, -1 when woken by abort.
	int waitSocket(short events, int timeoutMs)
	{
		struct pollfd fds[2];
		fds[0].fd = fd;
		fds[0].events = events;
		fds[0].revents = 0;
		fds[1].fd = wakeRead;
		fds[1].events = POLLIN;
		fds[1].revents = 0;
		int result = ::poll(fds, 2, std::max(0, timeoutMs));
		if (result < 0)
			return errno == EINTR ? 0 : POLLERR;
		if ((fds[1].revents & POLLIN) != 0) {
			drainWake();
			if (aborted())
				return -1;
		}
		return fds[0].revents;
	}

	void drainTls()
	{
		if (wbio == nullptr)
			return;
		char buffer[16384];
		while (BIO_ctrl_pending(wbio) > 0) {
			int n = BIO_read(wbio, buffer, sizeof(buffer));
			if (n <= 0)
				break;
			out.append(buffer, static_cast<size_t>(n));
		}
	}

	bool queuePlain(std::string_view data)
	{
		if (ssl == nullptr) {
			out.append(data);
			return true;
		}
		size_t done = 0;
		while (done < data.size()) {
			int n = SSL_write(ssl, data.data() + done,
				static_cast<int>(std::min<size_t>(data.size() - done, 1 << 20)));
			if (n <= 0) {
				error = "SSL_write: " + sslErrorText();
				return false;
			}
			done += static_cast<size_t>(n);
		}
		drainTls();
		return true;
	}

	Io pumpSend()
	{
		while (outOffset < out.size()) {
			ssize_t n = ::send(fd, out.data() + outOffset, out.size() - outOffset,
				MSG_NOSIGNAL);
			if (n > 0) {
				outOffset += static_cast<size_t>(n);
				continue;
			}
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				return Io::WouldBlock;
			error = std::string("send: ") + std::strerror(errno);
			return Io::Error;
		}
		out.clear();
		outOffset = 0;
		return Io::Ok;
	}

	// Read everything the socket has; decrypted bytes land in `plain`.
	Io pumpReceive(bool& gotData)
	{
		char buffer[16384];
		bool eof = false;
		for (;;) {
			ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
			if (n > 0) {
				gotData = true;
				if (ssl != nullptr)
					BIO_write(rbio, buffer, static_cast<int>(n));
				else
					plain.append(buffer, static_cast<size_t>(n));
				continue;
			}
			if (n == 0) {
				eof = true;
				break;
			}
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			error = std::string("recv: ") + std::strerror(errno);
			return Io::Error;
		}
		if (ssl != nullptr) {
			for (;;) {
				int n = SSL_read(ssl, buffer, sizeof(buffer));
				if (n > 0) {
					plain.append(buffer, static_cast<size_t>(n));
					continue;
				}
				int code = SSL_get_error(ssl, n);
				if (code == SSL_ERROR_WANT_READ)
					break;
				if (code == SSL_ERROR_ZERO_RETURN) {
					eof = true;
					break;
				}
				error = "SSL_read: " + sslErrorText();
				return Io::Error;
			}
			// Reads can produce output (key updates, alerts).
			drainTls();
		}
		return eof ? Io::Eof : Io::Ok;
	}

	// Blocking helpers for the connect phase.
	Status flushUntil(int64_t deadline)
	{
		for (;;) {
			Io io = pumpSend();
			if (io == Io::Ok)
				return {};
			if (io == Io::Error)
				return Error(ErrorKind::Transport, "send_failed", error);
			int64_t left = deadline - nowMs();
			if (left <= 0)
				return Error(ErrorKind::Transport, "timeout", "sending timed out");
			int revents = waitSocket(POLLOUT, static_cast<int>(left));
			if (revents < 0)
				return Error(ErrorKind::Cancelled, "cancelled");
			if ((revents & (POLLERR | POLLNVAL)) != 0)
				return Error(ErrorKind::Transport, "socket_error", "socket error");
		}
	}

	Status receiveUntil(int64_t deadline)
	{
		int64_t left = deadline - nowMs();
		if (left <= 0)
			return Error(ErrorKind::Transport, "timeout", "connection timed out");
		int revents = waitSocket(POLLIN, static_cast<int>(left));
		if (revents < 0)
			return Error(ErrorKind::Cancelled, "cancelled");
		if (revents == 0)
			return Error(ErrorKind::Transport, "timeout", "connection timed out");
		bool gotData = false;
		Io io = pumpReceive(gotData);
		if (io == Io::Error)
			return Error(ErrorKind::Transport, "receive_failed", error);
		if (io == Io::Eof)
			return Error(ErrorKind::Transport, "closed", "server closed the connection");
		return {};
	}

	Status tcpConnect(const std::string& host, int port, int64_t deadline)
	{
		struct addrinfo hints;
		std::memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		struct addrinfo* list = nullptr;
		std::string service = std::to_string(port);
		int status = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &list);
		if (status != 0 || list == nullptr) {
			return Error(ErrorKind::Transport, "dns_failed",
				host + ": " + gai_strerror(status));
		}

		std::string lastError = "no address";
		for (struct addrinfo* ai = list; ai != nullptr; ai = ai->ai_next) {
			int s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
			if (s < 0) {
				lastError = std::strerror(errno);
				continue;
			}
			setNonBlocking(s);
			int result = ::connect(s, ai->ai_addr, ai->ai_addrlen);
			if (result != 0 && errno != EINPROGRESS) {
				lastError = std::strerror(errno);
				::close(s);
				continue;
			}
			if (result != 0) {
				fd = s;
				int64_t left = deadline - nowMs();
				int revents = left > 0 ? waitSocket(POLLOUT, static_cast<int>(left)) : 0;
				fd = -1;
				if (revents < 0) {
					::close(s);
					::freeaddrinfo(list);
					return Error(ErrorKind::Cancelled, "cancelled");
				}
				int socketError = 0;
				socklen_t length = sizeof(socketError);
				if (revents == 0) {
					lastError = "connect timed out";
					::close(s);
					continue;
				}
				if (::getsockopt(s, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0
					|| socketError != 0) {
					lastError = std::strerror(socketError != 0 ? socketError : errno);
					::close(s);
					continue;
				}
			}
			int one = 1;
			::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
			::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
			fd = s;
			::freeaddrinfo(list);
			return {};
		}
		::freeaddrinfo(list);
		return Error(ErrorKind::Transport, "connect_failed",
			host + ":" + service + ": " + lastError);
	}

	Status tlsConnect(const std::string& host, const WebSocketOptions& options,
		int64_t deadline)
	{
		ctx = SSL_CTX_new(TLS_client_method());
		if (ctx == nullptr)
			return Error(ErrorKind::Transport, "tls_init", sslErrorText());
		SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
		if (options.verifyPeer) {
			SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
			SSL_CTX_set_default_verify_paths(ctx);
			// Haiku keeps its bundle here; harmless elsewhere.
			static const char* kHaikuBundle
				= "/boot/system/data/ssl/CARootCertificates.pem";
			if (fileExists(kHaikuBundle))
				SSL_CTX_load_verify_locations(ctx, kHaikuBundle, nullptr);
			if (!options.caFile.empty() || !options.caPath.empty()) {
				if (SSL_CTX_load_verify_locations(ctx,
						options.caFile.empty() ? nullptr : options.caFile.c_str(),
						options.caPath.empty() ? nullptr : options.caPath.c_str()) != 1) {
					return Error(ErrorKind::Transport, "tls_ca",
						"cannot load CA from " + options.caFile + options.caPath);
				}
			}
			ERR_clear_error();
		} else {
			SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
		}

		ssl = SSL_new(ctx);
		rbio = BIO_new(BIO_s_mem());
		wbio = BIO_new(BIO_s_mem());
		if (ssl == nullptr || rbio == nullptr || wbio == nullptr)
			return Error(ErrorKind::Transport, "tls_init", sslErrorText());
		BIO_set_mem_eof_return(rbio, -1);
		BIO_set_mem_eof_return(wbio, -1);
		SSL_set_bio(ssl, rbio, wbio);
		SSL_set_connect_state(ssl);

		static const unsigned char kAlpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
		SSL_set_alpn_protos(ssl, kAlpn, sizeof(kAlpn));
		bool ip = isIpLiteral(host);
		if (!ip)
			SSL_set_tlsext_host_name(ssl, host.c_str());
		if (options.verifyPeer) {
			if (ip) {
				X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
				X509_VERIFY_PARAM_set1_ip_asc(param, host.c_str());
			} else {
				SSL_set1_host(ssl, host.c_str());
			}
		}

		for (;;) {
			int result = SSL_do_handshake(ssl);
			drainTls();
			Status flushed = flushUntil(deadline);
			if (!flushed)
				return flushed;
			if (result == 1)
				return {};
			int code = SSL_get_error(ssl, result);
			if (code != SSL_ERROR_WANT_READ) {
				std::string detail = sslErrorText();
				long verify = SSL_get_verify_result(ssl);
				if (verify != X509_V_OK)
					detail += std::string(" (") + X509_verify_cert_error_string(verify) + ")";
				return Error(ErrorKind::Transport, "tls_handshake", detail);
			}
			// Feed network bytes to the handshake.
			int64_t left = deadline - nowMs();
			if (left <= 0)
				return Error(ErrorKind::Transport, "timeout", "TLS handshake timed out");
			int revents = waitSocket(POLLIN, static_cast<int>(left));
			if (revents < 0)
				return Error(ErrorKind::Cancelled, "cancelled");
			if (revents == 0)
				return Error(ErrorKind::Transport, "timeout", "TLS handshake timed out");
			char buffer[16384];
			for (;;) {
				ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
				if (n > 0) {
					BIO_write(rbio, buffer, static_cast<int>(n));
					continue;
				}
				if (n == 0)
					return Error(ErrorKind::Transport, "tls_handshake",
						"server closed the connection during TLS");
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				return Error(ErrorKind::Transport, "tls_handshake", std::strerror(errno));
			}
		}
	}
};


WebSocketConnection::WebSocketConnection(WebSocketOptions options)
	:
	fOptions(std::move(options)),
	fImpl(std::make_unique<Impl>(fOptions.maxMessageBytes))
{
	int fds[2];
	if (::pipe(fds) == 0) {
		fImpl->wakeRead = fds[0];
		fImpl->wakeWrite = fds[1];
		setNonBlocking(fds[0]);
		setNonBlocking(fds[1]);
	}
}


WebSocketConnection::~WebSocketConnection() = default;


Status
WebSocketConnection::connect(const std::string& urlText, const Headers& headers)
{
	std::optional<ws::Url> url = ws::parseUrl(urlText);
	if (!url)
		return Error(ErrorKind::InvalidArgument, "bad_url", urlText);
	if (fImpl->wakeRead < 0)
		return Error(ErrorKind::Transport, "pipe_failed", std::strerror(errno));

	int64_t deadline = nowMs() + fOptions.connectTimeoutMs;
	Status status = fImpl->tcpConnect(url->host, url->port, deadline);
	if (!status)
		return status;
	if (url->secure) {
		status = fImpl->tlsConnect(url->host, fOptions, deadline);
		if (!status)
			return status;
	}

	std::string key = ws::makeKey();
	std::string request = ws::buildHandshakeRequest(*url, key, headers,
		fOptions.userAgent.empty() ? std::string("Natter/") + natterVersion()
			: fOptions.userAgent);
	if (!fImpl->queuePlain(request))
		return Error(ErrorKind::Transport, "send_failed", fImpl->error);
	status = fImpl->flushUntil(deadline);
	if (!status)
		return status;

	size_t headEnd;
	while ((headEnd = fImpl->plain.find("\r\n\r\n")) == std::string::npos) {
		if (fImpl->plain.size() > 64 * 1024)
			return Error(ErrorKind::Transport, "handshake_too_large");
		status = fImpl->receiveUntil(deadline);
		if (!status)
			return status;
	}
	std::string head = fImpl->plain.substr(0, headEnd + 4);
	fImpl->plain.erase(0, headEnd + 4);

	std::optional<ws::HandshakeResponse> response = ws::parseHandshakeResponse(head);
	if (!response)
		return Error(ErrorKind::Transport, "handshake_parse", head.substr(0, 200));
	status = ws::checkHandshakeResponse(*response, key);
	if (!status)
		return status;

	// Frames that arrived right behind the response head.
	if (!fImpl->plain.empty()) {
		fImpl->decoder.feed(fImpl->plain);
		fImpl->plain.clear();
	}
	fOpen = true;
	return {};
}


bool
WebSocketConnection::queueFrame(ws::Opcode opcode, std::string_view payload)
{
	{
		std::lock_guard<std::mutex> guard(fImpl->lock);
		if (!fOpen || fImpl->closeQueued || fImpl->abortRequested)
			return false;
		PendingFrame frame;
		frame.bytes = ws::encodeClientFrame(opcode, payload);
		fImpl->pending.push_back(std::move(frame));
	}
	fImpl->wake();
	return true;
}


bool
WebSocketConnection::sendText(std::string_view text)
{
	return queueFrame(ws::Opcode::Text, text);
}


bool
WebSocketConnection::sendBinary(std::string_view data)
{
	return queueFrame(ws::Opcode::Binary, data);
}


bool
WebSocketConnection::sendPing(std::string_view payload)
{
	return queueFrame(ws::Opcode::Ping, payload.substr(0, 125));
}


void
WebSocketConnection::close(uint16_t code, std::string_view reason)
{
	{
		std::lock_guard<std::mutex> guard(fImpl->lock);
		if (fImpl->closeQueued || fImpl->abortRequested)
			return;
		fImpl->closeQueued = true;
		PendingFrame frame;
		frame.bytes = ws::encodeClientFrame(ws::Opcode::Close,
			ws::makeClosePayload(code, reason));
		frame.isClose = true;
		frame.closeCode = code;
		frame.closeReason = std::string(reason);
		fImpl->pending.push_back(std::move(frame));
	}
	fImpl->wake();
}


void
WebSocketConnection::abort()
{
	{
		std::lock_guard<std::mutex> guard(fImpl->lock);
		fImpl->abortRequested = true;
	}
	fImpl->wake();
}


CloseInfo
WebSocketConnection::run(const Handlers& handlers)
{
	Impl& impl = *fImpl;
	CloseInfo info;
	if (!fOpen) {
		info.reason = "not connected";
		return info;
	}

	bool closeSent = false;
	int64_t closeSentAt = 0;
	int64_t lastReceive = nowMs();
	int64_t lastPing = nowMs();
	auto finish = [&](CloseInfo result) {
		fOpen = false;
		{
			std::lock_guard<std::mutex> guard(impl.lock);
			impl.closeQueued = true;
			impl.pending.clear();
		}
		if (impl.fd >= 0) {
			::shutdown(impl.fd, SHUT_RDWR);
			::close(impl.fd);
			impl.fd = -1;
		}
		return result;
	};
	// Best effort: push out what is queued (a close reply) before leaving.
	auto flushBriefly = [&]() {
		int64_t deadline = nowMs() + 1000;
		while (impl.outOffset < impl.out.size() && nowMs() < deadline) {
			if (impl.pumpSend() == Io::Error)
				break;
			if (impl.outOffset < impl.out.size())
				impl.waitSocket(POLLOUT, 100);
		}
	};

	for (;;) {
		// 1. Frames other threads queued.
		std::deque<PendingFrame> pending;
		bool abortNow;
		{
			std::lock_guard<std::mutex> guard(impl.lock);
			pending.swap(impl.pending);
			abortNow = impl.abortRequested;
		}
		if (abortNow) {
			info.code = 1006;
			info.reason = "aborted";
			return finish(info);
		}
		for (PendingFrame& frame : pending) {
			if (closeSent)
				break;
			if (!impl.queuePlain(frame.bytes)) {
				info.reason = impl.error;
				return finish(info);
			}
			if (frame.isClose) {
				closeSent = true;
				closeSentAt = nowMs();
				info.code = frame.closeCode;
				info.reason = frame.closeReason;
				info.byServer = false;
			}
		}

		// 2. Frames from the server.
		ws::Frame frame;
		for (;;) {
			ws::FrameDecoder::Status status = impl.decoder.next(frame);
			if (status == ws::FrameDecoder::Status::NeedMore)
				break;
			if (status == ws::FrameDecoder::Status::Error) {
				log(LogLevel::Warning, "websocket: " + impl.decoder.error());
				if (!closeSent) {
					impl.queuePlain(ws::encodeClientFrame(ws::Opcode::Close,
						ws::makeClosePayload(impl.decoder.errorCloseCode(), "protocol error")));
					flushBriefly();
				}
				info.code = impl.decoder.errorCloseCode();
				info.reason = impl.decoder.error();
				return finish(info);
			}
			if (frame.opcode == ws::Opcode::Ping) {
				if (!closeSent)
					impl.queuePlain(ws::encodeClientFrame(ws::Opcode::Pong, frame.payload));
				continue;
			}
			if (frame.opcode == ws::Opcode::Pong)
				continue;
			if (frame.opcode == ws::Opcode::Close) {
				uint16_t code = 1005;
				std::string reason;
				if (!ws::parseClosePayload(frame.payload, code, reason))
					code = 1002;
				if (!closeSent) {
					// Echo the status back, as RFC 6455 section 5.5.1 asks.
					std::string payload = code == 1005 ? std::string()
						: ws::makeClosePayload(code, {});
					impl.queuePlain(ws::encodeClientFrame(ws::Opcode::Close, payload));
					flushBriefly();
					info.code = code;
					info.reason = reason;
					info.byServer = true;
				}
				return finish(info);
			}
			ws::DataMessage message;
			ws::MessageAssembler::Status status2 = impl.assembler.add(std::move(frame), message);
			if (status2 == ws::MessageAssembler::Status::Error) {
				log(LogLevel::Warning, "websocket: " + impl.assembler.error());
				impl.queuePlain(ws::encodeClientFrame(ws::Opcode::Close,
					ws::makeClosePayload(1002, "protocol error")));
				flushBriefly();
				info.code = 1002;
				info.reason = impl.assembler.error();
				return finish(info);
			}
			if (status2 == ws::MessageAssembler::Status::Ready && !closeSent) {
				if (message.opcode == ws::Opcode::Text) {
					if (handlers.onText)
						handlers.onText(std::move(message.data));
				} else if (handlers.onBinary) {
					handlers.onBinary(std::move(message.data));
				}
			}
		}

		// 3. Timers.
		int64_t now = nowMs();
		int timeout = 1000;
		if (closeSent) {
			int64_t left = closeSentAt + fOptions.closeTimeoutMs - now;
			if (left <= 0)
				return finish(info);
			timeout = static_cast<int>(std::min<int64_t>(timeout, left));
		} else if (fOptions.pingIntervalMs > 0) {
			if (now - lastReceive > fOptions.pingIntervalMs + fOptions.pongTimeoutMs) {
				info.code = 1006;
				info.reason = "ping timeout";
				return finish(info);
			}
			if (now - lastPing >= fOptions.pingIntervalMs) {
				lastPing = now;
				impl.queuePlain(ws::encodeClientFrame(ws::Opcode::Ping,
					std::to_string(now)));
				if (fOptions.applicationPing) {
					std::string text = fOptions.applicationPing();
					if (!text.empty())
						impl.queuePlain(ws::encodeClientFrame(ws::Opcode::Text, text));
				}
			}
			int64_t untilPing = lastPing + fOptions.pingIntervalMs - now;
			timeout = static_cast<int>(std::clamp<int64_t>(untilPing, 10, 1000));
		}

		// 4. Send.
		if (impl.pumpSend() == Io::Error) {
			info.code = 1006;
			info.reason = impl.error;
			return finish(info);
		}

		// 5. Wait.
		short events = POLLIN;
		if (impl.outOffset < impl.out.size())
			events |= POLLOUT;
		int revents = impl.waitSocket(events, timeout);
		if (revents < 0)
			continue;   // abort: handled at the top of the loop
		if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			bool gotData = false;
			Io io = impl.pumpReceive(gotData);
			if (gotData)
				lastReceive = nowMs();
			if (!impl.plain.empty()) {
				impl.decoder.feed(impl.plain);
				impl.plain.clear();
			}
			if (io == Io::Error || io == Io::Eof) {
				// Parse whatever arrived with the EOF (a close frame) first.
				ws::Frame last;
				while (impl.decoder.next(last) == ws::FrameDecoder::Status::Ready) {
					if (last.opcode == ws::Opcode::Close) {
						uint16_t code = 1005;
						std::string reason;
						ws::parseClosePayload(last.payload, code, reason);
						if (!closeSent) {
							info.code = code;
							info.reason = reason;
							info.byServer = true;
						}
						return finish(info);
					}
					if (!closeSent && !ws::isControl(last.opcode)) {
						ws::DataMessage message;
						if (impl.assembler.add(std::move(last), message)
								== ws::MessageAssembler::Status::Ready) {
							if (message.opcode == ws::Opcode::Text && handlers.onText)
								handlers.onText(std::move(message.data));
							else if (message.opcode == ws::Opcode::Binary && handlers.onBinary)
								handlers.onBinary(std::move(message.data));
						}
					}
				}
				if (!closeSent) {
					info.code = 1006;
					info.reason = io == Io::Eof ? "connection closed" : impl.error;
				}
				return finish(info);
			}
		}
	}
}

// ---- Backoff -----------------------------------------------------------------------------

Backoff::Backoff(int initialMs, int maxMs, double factor, double jitter)
	:
	fInitialMs(initialMs),
	fMaxMs(maxMs),
	fFactor(factor),
	fJitter(jitter)
{
}


int
Backoff::nextDelayMs()
{
	double delay = fInitialMs;
	for (int i = 0; i < fAttempts && delay < fMaxMs; i++)
		delay *= fFactor;
	delay = std::min<double>(delay, fMaxMs);
	fAttempts++;
	if (fJitter > 0) {
		static thread_local std::mt19937 generator{std::random_device{}()};
		std::uniform_real_distribution<double> spread(1.0 - fJitter, 1.0 + fJitter);
		delay *= spread(generator);
	}
	return static_cast<int>(std::min<double>(delay, fMaxMs * (1.0 + fJitter)));
}


void
Backoff::reset()
{
	fAttempts = 0;
}

// ---- WebSocketClient ---------------------------------------------------------------------

WebSocketClient::WebSocketClient(Provider provider, Callbacks callbacks,
	WebSocketOptions options, Backoff backoff)
	:
	fProvider(std::move(provider)),
	fCallbacks(std::move(callbacks)),
	fOptions(std::move(options)),
	fBackoff(backoff)
{
}


WebSocketClient::~WebSocketClient()
{
	stop();
	if (fThread.joinable()) {
		if (fThread.get_id() == std::this_thread::get_id())
			fThread.detach();
		else
			fThread.join();
	}
}


void
WebSocketClient::start()
{
	std::lock_guard<std::mutex> guard(fLock);
	if (fRunning)
		return;
	if (fThread.joinable())
		fThread.join();
	fStopping = false;
	fRunning = true;
	fThread = std::thread(&WebSocketClient::threadMain, this);
}


void
WebSocketClient::stop()
{
	std::shared_ptr<WebSocketConnection> connection;
	{
		std::lock_guard<std::mutex> guard(fLock);
		fStopping = true;
		connection = fConnection;
	}
	fWake.notify_all();
	if (connection) {
		if (connection->isOpen())
			connection->close(1000, "client closing");
		else
			connection->abort();
	}
	if (fThread.joinable() && fThread.get_id() != std::this_thread::get_id())
		fThread.join();
}


bool
WebSocketClient::connected() const
{
	std::lock_guard<std::mutex> guard(fLock);
	return fConnection && fConnection->isOpen();
}


bool
WebSocketClient::send(std::string_view text)
{
	std::shared_ptr<WebSocketConnection> connection;
	{
		std::lock_guard<std::mutex> guard(fLock);
		connection = fConnection;
	}
	return connection && connection->sendText(text);
}


void
WebSocketClient::reconnectNow()
{
	std::shared_ptr<WebSocketConnection> connection;
	{
		std::lock_guard<std::mutex> guard(fLock);
		connection = fConnection;
	}
	fSkipDelay = true;
	if (connection)
		connection->close(1000, "reconnecting");
	fWake.notify_all();
}


bool
WebSocketClient::waitFor(int ms)
{
	std::unique_lock<std::mutex> guard(fLock);
	fWake.wait_for(guard, std::chrono::milliseconds(ms),
		[this] { return fStopping.load() || fSkipDelay.load(); });
	fSkipDelay = false;
	return !fStopping;
}


void
WebSocketClient::threadMain()
{
	int attempt = 0;
	while (!fStopping) {
		attempt++;
		if (fCallbacks.onConnecting)
			fCallbacks.onConnecting(attempt);

		Error error;
		CloseInfo close;
		bool opened = false;
		int64_t openedAt = 0;

		Result<WebSocketTarget> target = fProvider();
		if (!target) {
			error = target.error();
		} else if (!fStopping) {
			auto connection = std::make_shared<WebSocketConnection>(fOptions);
			{
				std::lock_guard<std::mutex> guard(fLock);
				fConnection = connection;
			}
			if (fStopping)
				connection->abort();
			Status status = connection->connect(target->url, target->headers);
			if (!status) {
				error = status.error();
			} else {
				opened = true;
				openedAt = nowMs();
				if (fCallbacks.onOpen)
					fCallbacks.onOpen();
				WebSocketConnection::Handlers handlers;
				handlers.onText = fCallbacks.onText;
				handlers.onBinary = fCallbacks.onBinary;
				close = connection->run(handlers);
			}
			std::lock_guard<std::mutex> guard(fLock);
			fConnection.reset();
		}

		if (opened && nowMs() - openedAt >= fStableAfterMs) {
			fBackoff.reset();
			attempt = 0;
		}
		bool retry = !fStopping;
		if (retry && !opened && error.kind != ErrorKind::None) {
			if (fCallbacks.shouldRetry) {
				retry = fCallbacks.shouldRetry(error);
			} else {
				retry = error.kind != ErrorKind::Auth
					&& error.kind != ErrorKind::Unsupported
					&& error.kind != ErrorKind::InvalidArgument;
			}
		}
		int delay = -1;
		if (retry) {
			delay = fSkipDelay.exchange(false) ? 0 : fBackoff.nextDelayMs();
			if (error.retryAfterSeconds > 0)
				delay = std::max(delay, error.retryAfterSeconds * 1000);
		}
		if (fCallbacks.onDisconnected)
			fCallbacks.onDisconnected(close, error, delay, attempt);
		if (!retry)
			break;
		if (delay > 0 && !waitFor(delay))
			break;
	}
	fRunning = false;
}

}  // namespace natter
