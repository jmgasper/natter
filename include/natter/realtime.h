// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "natter/events.h"
#include "natter/web_api.h"
#include "natter/websocket.h"

namespace natter {

// Receives every event. Runs on the thread of the source that produced it:
// the RTM or Socket Mode socket thread, or the poller thread. Keep it short
// and never block on the UI thread from here (post a message instead).
using EventHandler = std::function<void(const Event& event, EventSource source)>;

// ---- RTM -------------------------------------------------------------------------

// rtm.connect -> wss with the d cookie on the handshake (session mode needs
// it), then typed events. Reconnects with backoff; "goodbye" reconnects at once.
class RtmClient {
public:
	RtmClient(std::shared_ptr<const WebApi> api, EventHandler handler,
		WebSocketOptions options = {});
	~RtmClient();

	void start();
	void stop();
	bool connected() const;

	// Called on the RTM thread when the client stops retrying (fatal error:
	// the token may not use RTM). Used by Realtime to fall back to polling.
	void setOnGiveUp(std::function<void(const Error& error)> callback);
	// Give up after this many attempts in a row that never opened (0 = never).
	void setMaxFailedAttempts(int count) { fMaxFailedAttempts = count; }

	// {"type":"typing"} for a channel (and thread).
	bool sendTyping(const std::string& channel, const std::string& threadTs = {});
	// Ask for presence_change events for these users.
	bool subscribePresence(const std::vector<std::string>& users);

	std::optional<ConnectInfo> lastConnectInfo() const;

private:
	void handleText(std::string&& text);
	int nextId() { return ++fMessageId; }

	std::shared_ptr<const WebApi> fApi;
	EventHandler fHandler;
	std::unique_ptr<WebSocketClient> fClient;
	std::function<void(const Error&)> fOnGiveUp;
	std::atomic<int> fMessageId{0};
	std::atomic<int> fFailedAttempts{0};
	int fMaxFailedAttempts = 0;
	mutable std::mutex fLock;
	std::optional<ConnectInfo> fConnectInfo;
};

// ---- Socket Mode --------------------------------------------------------------------

// apps.connections.open (with the xapp- token) -> wss; every envelope is
// acknowledged by envelope_id before its event is dispatched. Events API
// payloads come out as the same typed events as RTM. Other envelope types
// (slash_commands, interactive) arrive as UnknownEvent with the envelope.
class SocketModeClient {
public:
	SocketModeClient(std::shared_ptr<const WebApi> api, EventHandler handler,
		WebSocketOptions options = {});
	~SocketModeClient();

	void start();
	void stop();
	bool connected() const;
	void setOnGiveUp(std::function<void(const Error& error)> callback);

	// Envelope ids acknowledged so far (tests and diagnostics).
	int ackCount() const { return fAcks.load(); }

private:
	void handleText(std::string&& text);

	std::shared_ptr<const WebApi> fApi;
	EventHandler fHandler;
	std::unique_ptr<WebSocketClient> fClient;
	std::function<void(const Error&)> fOnGiveUp;
	std::atomic<int> fAcks{0};
	std::mutex fLock;
	std::set<std::string> fSeenEventIds;
	std::vector<std::string> fSeenOrder;
};

// ---- polling fallback ------------------------------------------------------------------

struct PollOptions {
	int historyIntervalMs = 5000;   // the active conversation
	int countsIntervalMs = 30000;   // unread counts for everything
	int historyLimit = 30;          // messages per poll
};

// Polls conversations.history (and .replies for an open thread) of the
// active conversation, turning differences into MessageEvent,
// MessageChangedEvent and MessageDeletedEvent, and the unread counts into
// CountsEvent. Its thread delivers the events.
class Poller {
public:
	Poller(std::shared_ptr<const WebApi> api, EventHandler handler,
		PollOptions options = {});
	~Poller();

	void start();
	void stop();
	bool running() const { return fRunning.load(); }

	// Poll this conversation (and thread, if given). The first poll of a new
	// conversation only records a baseline; it does not emit events.
	void setActiveConversation(const std::string& channel,
		const std::string& threadTs = {});
	// Channels whose counts come from conversations.info when neither
	// client.counts nor users.counts is available.
	void setCountsChannels(std::vector<std::string> channels);
	// Poll now instead of waiting for the next tick.
	void pollNow();

	// One history round, synchronously (tests).
	void pollHistoryOnce();
	void pollCountsOnce();

private:
	struct Snapshot {
		std::string channel;
		std::string threadTs;
		std::map<std::string, Message, TsLess> messages;
		bool baseline = false;
	};

	void threadMain();
	void diff(Snapshot& snapshot, std::vector<Message> fetched, bool hasMore);

	std::shared_ptr<const WebApi> fApi;
	EventHandler fHandler;
	PollOptions fOptions;

	std::thread fThread;
	std::atomic<bool> fRunning{false};
	std::atomic<bool> fStopping{false};
	std::mutex fLock;
	std::condition_variable fWake;
	bool fPollRequested = false;
	std::string fChannel;
	std::string fThreadTs;
	std::vector<std::string> fCountsChannels;
	Snapshot fHistory;
	Snapshot fReplies;
	std::mutex fPollLock;   // one poll round at a time
};

// ---- the facade ----------------------------------------------------------------------------

enum class RealtimeMode {
	Auto,        // Socket Mode with an app token, else RTM, else polling
	Rtm,
	SocketMode,
	Polling,
	Off,
};

const char* realtimeModeName(RealtimeMode mode);

struct RealtimeOptions {
	RealtimeMode mode = RealtimeMode::Auto;
	WebSocketOptions websocket;
	PollOptions poll;
	// In Auto mode, RTM attempts in a row that never connect before falling
	// back to polling.
	int rtmFailuresBeforePolling = 3;
	// Keep polling the unread counts even while a socket is up.
	bool pollCountsWithSocket = false;
};

// Picks and runs one real-time source. Events are delivered to `handler` on
// the source's thread (see EventHandler).
class Realtime {
public:
	Realtime(std::shared_ptr<const WebApi> api, EventHandler handler,
		RealtimeOptions options = {});
	~Realtime();

	void start();
	void stop();

	// The source actually in use (Auto resolves to one of the others).
	RealtimeMode activeMode() const;
	bool connected() const;

	void setActiveConversation(const std::string& channel,
		const std::string& threadTs = {});
	void setCountsChannels(std::vector<std::string> channels);
	bool sendTyping(const std::string& channel, const std::string& threadTs = {});
	void pollNow();

private:
	void startPolling(const std::string& reason);
	void dispatch(const Event& event, EventSource source);

	std::shared_ptr<const WebApi> fApi;
	EventHandler fHandler;
	RealtimeOptions fOptions;

	mutable std::mutex fLock;
	RealtimeMode fActive = RealtimeMode::Off;
	std::unique_ptr<RtmClient> fRtm;
	std::unique_ptr<SocketModeClient> fSocket;
	std::unique_ptr<Poller> fPoller;
	std::string fChannel;
	std::string fThreadTs;
	std::vector<std::string> fCountsChannels;
	std::atomic<bool> fStopping{false};
};

}  // namespace natter
