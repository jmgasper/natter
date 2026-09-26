// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

// A WebSocket client written directly on BSD sockets and OpenSSL (RFC 6455).
// It does not use libcurl's WebSocket support, which Haiku builds may lack.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "natter/http.h"
#include "natter/result.h"
#include "natter/ws_frame.h"

namespace natter {

struct WebSocketOptions {
	int connectTimeoutMs = 15000;
	// A ping frame goes out after this much silence; 0 disables pings.
	int pingIntervalMs = 30000;
	// With no bytes from the server for pingIntervalMs + pongTimeoutMs the
	// connection is declared dead (close code 1006).
	int pongTimeoutMs = 15000;
	// How long close() waits for the server's close frame.
	int closeTimeoutMs = 3000;
	size_t maxMessageBytes = 64 * 1024 * 1024;
	std::string caFile;          // extra CA bundle (PEM), e.g. a test cert
	std::string caPath;
	bool verifyPeer = true;
	std::string userAgent;
	// Called at the ping interval; a non-empty result is sent as a text
	// message (Slack RTM expects {"type":"ping","id":N}).
	std::function<std::string()> applicationPing;
};

struct CloseInfo {
	uint16_t code = 1006;         // 1006: closed without a close frame
	std::string reason;
	bool byServer = false;
};

// One connection. connect() and run() block the calling thread; the send
// and close methods may be called from any thread while run() is active.
class WebSocketConnection {
public:
	struct Handlers {
		std::function<void(std::string&& text)> onText;
		std::function<void(std::string&& data)> onBinary;
	};

	explicit WebSocketConnection(WebSocketOptions options = {});
	~WebSocketConnection();

	WebSocketConnection(const WebSocketConnection&) = delete;
	WebSocketConnection& operator=(const WebSocketConnection&) = delete;

	// TCP (+ TLS for wss://) and the opening handshake. `headers` go into
	// the upgrade request (Slack session RTM needs "Cookie: d=...").
	Status connect(const std::string& url, const Headers& headers = {});

	// Pump frames until the connection closes; handlers run on this thread.
	CloseInfo run(const Handlers& handlers);

	// Thread-safe. False when the connection is closing or closed.
	bool sendText(std::string_view text);
	bool sendBinary(std::string_view data);
	bool sendPing(std::string_view payload = {});
	// Start the closing handshake (thread-safe).
	void close(uint16_t code = 1000, std::string_view reason = {});
	// Drop the connection at once (thread-safe); run() returns 1006.
	void abort();

	bool isOpen() const { return fOpen.load(); }

private:
	struct Impl;
	bool queueFrame(ws::Opcode opcode, std::string_view payload);

	WebSocketOptions fOptions;
	std::unique_ptr<Impl> fImpl;
	std::atomic<bool> fOpen{false};
};

// Exponential backoff with jitter.
class Backoff {
public:
	Backoff(int initialMs = 1000, int maxMs = 60000, double factor = 2.0,
		double jitter = 0.2);
	int nextDelayMs();       // grows with every call
	void reset();
	int attempts() const { return fAttempts; }

private:
	int fInitialMs;
	int fMaxMs;
	double fFactor;
	double fJitter;
	int fAttempts = 0;
};

struct WebSocketTarget {
	std::string url;
	Headers headers;
};

// Keeps one logical connection alive on its own thread: asks `provider` for
// a URL (rtm.connect / apps.connections.open hand out a fresh one each
// time), connects, runs, and reconnects with backoff.
//
// All callbacks run on the client's thread, one at a time. They may call
// send(), reconnectNow() and stop(), but must not block for long.
class WebSocketClient {
public:
	using Provider = std::function<Result<WebSocketTarget>()>;

	struct Callbacks {
		std::function<void()> onOpen;
		std::function<void(std::string&& text)> onText;
		std::function<void(std::string&& data)> onBinary;
		// Every attempt that ends: the close details, the error when the
		// connection could not be made, and the delay before the next try
		// (-1 when the client gives up).
		std::function<void(const CloseInfo& close, const Error& error,
			int retryInMs, int attempt)> onDisconnected;
		// Called before each connection attempt.
		std::function<void(int attempt)> onConnecting;
		// Decide whether an error from the provider is worth retrying.
		// Default: everything except Auth, Unsupported and InvalidArgument.
		std::function<bool(const Error& error)> shouldRetry;
	};

	WebSocketClient(Provider provider, Callbacks callbacks,
		WebSocketOptions options = {}, Backoff backoff = Backoff());
	~WebSocketClient();

	void start();
	// Close and join. Safe from any thread, including a callback (then it
	// only signals; the thread ends after the callback returns).
	void stop();
	bool running() const { return fRunning.load(); }
	bool connected() const;

	bool send(std::string_view text);
	// Drop the current connection and reconnect without waiting (for
	// server-requested disconnects).
	void reconnectNow();

	// A connection that stays up this long resets the backoff.
	void setStableAfterMs(int ms) { fStableAfterMs = ms; }

private:
	void threadMain();
	bool waitFor(int ms);   // false when stopping

	Provider fProvider;
	Callbacks fCallbacks;
	WebSocketOptions fOptions;
	Backoff fBackoff;
	int fStableAfterMs = 20000;

	std::thread fThread;
	std::atomic<bool> fRunning{false};
	std::atomic<bool> fStopping{false};
	std::atomic<bool> fSkipDelay{false};
	mutable std::mutex fLock;
	std::condition_variable fWake;
	std::shared_ptr<WebSocketConnection> fConnection;
};

}  // namespace natter
