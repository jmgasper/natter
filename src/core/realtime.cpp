// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT

#include "natter/realtime.h"

#include <algorithm>
#include <chrono>

namespace natter {

namespace {

ConnectionEvent
connectionEvent(ConnectionState state, EventSource source, std::string detail = {},
	int attempt = 0, int retryInMs = 0)
{
	ConnectionEvent event;
	event.state = state;
	event.source = source;
	event.detail = std::move(detail);
	event.attempt = attempt;
	event.retryInSeconds = retryInMs > 0 ? (retryInMs + 999) / 1000 : 0;
	return event;
}


std::string
closeDetail(const CloseInfo& close, const Error& error)
{
	if (error.kind != ErrorKind::None)
		return error.describe();
	std::string text = "close " + std::to_string(close.code);
	if (!close.reason.empty())
		text += " " + close.reason;
	return text;
}

}  // namespace


const char*
realtimeModeName(RealtimeMode mode)
{
	switch (mode) {
		case RealtimeMode::Auto: return "auto";
		case RealtimeMode::Rtm: return "rtm";
		case RealtimeMode::SocketMode: return "socket-mode";
		case RealtimeMode::Polling: return "polling";
		case RealtimeMode::Off: return "off";
	}
	return "?";
}

// ---- RtmClient ---------------------------------------------------------------------------

RtmClient::RtmClient(std::shared_ptr<const WebApi> api, EventHandler handler,
	WebSocketOptions options)
	:
	fApi(std::move(api)),
	fHandler(std::move(handler))
{
	options.applicationPing = [this] {
		return json{{"id", nextId()}, {"type", "ping"}}.dump();
	};

	WebSocketClient::Provider provider = [this]() -> Result<WebSocketTarget> {
		Result<ConnectInfo> info = fApi->rtmConnect();
		if (!info)
			return info.error();
		{
			std::lock_guard<std::mutex> guard(fLock);
			fConnectInfo = info.value();
		}
		WebSocketTarget target;
		target.url = info->url;
		// Session RTM authenticates the socket with the browser cookie; the
		// URL alone gets a connection that Slack drops within seconds.
		std::string cookie = fApi->credentials().cookieHeader();
		if (!cookie.empty())
			target.headers.emplace_back("Cookie", cookie);
		return target;
	};

	WebSocketClient::Callbacks callbacks;
	callbacks.onConnecting = [this](int attempt) {
		fHandler(connectionEvent(ConnectionState::Connecting, EventSource::Rtm, {},
			attempt), EventSource::Rtm);
	};
	callbacks.onOpen = [this] { fFailedAttempts = 0; };
	callbacks.onText = [this](std::string&& text) { handleText(std::move(text)); };
	callbacks.onDisconnected = [this](const CloseInfo& close, const Error& error,
			int retryInMs, int attempt) {
		bool neverOpened = error.kind != ErrorKind::None;
		int failures = neverOpened ? ++fFailedAttempts : fFailedAttempts.load();
		bool giveUp = retryInMs < 0
			|| (fMaxFailedAttempts > 0 && failures >= fMaxFailedAttempts);
		fHandler(connectionEvent(ConnectionState::Disconnected, EventSource::Rtm,
			closeDetail(close, error), attempt, giveUp ? 0 : retryInMs),
			EventSource::Rtm);
		if (giveUp) {
			if (retryInMs >= 0)
				fClient->stop();   // on our own thread: only signals
			std::function<void(const Error&)> callback;
			{
				std::lock_guard<std::mutex> guard(fLock);
				callback = fOnGiveUp;
			}
			if (callback)
				callback(error.kind != ErrorKind::None ? error
					: Error(ErrorKind::Transport, "rtm_unavailable", closeDetail(close, error)));
		}
	};

	fClient = std::make_unique<WebSocketClient>(std::move(provider),
		std::move(callbacks), std::move(options));
}


RtmClient::~RtmClient()
{
	stop();
}


void
RtmClient::start()
{
	fFailedAttempts = 0;
	fClient->start();
}


void
RtmClient::stop()
{
	fClient->stop();
}


bool
RtmClient::connected() const
{
	return fClient->connected();
}


void
RtmClient::setOnGiveUp(std::function<void(const Error& error)> callback)
{
	std::lock_guard<std::mutex> guard(fLock);
	fOnGiveUp = std::move(callback);
}


bool
RtmClient::sendTyping(const std::string& channel, const std::string& threadTs)
{
	json message = {{"id", nextId()}, {"type", "typing"}, {"channel", channel}};
	if (!threadTs.empty())
		message["thread_ts"] = threadTs;
	return fClient->send(message.dump());
}


bool
RtmClient::subscribePresence(const std::vector<std::string>& users)
{
	json message = {{"type", "presence_sub"}, {"ids", users}};
	return fClient->send(message.dump());
}


std::optional<ConnectInfo>
RtmClient::lastConnectInfo() const
{
	std::lock_guard<std::mutex> guard(fLock);
	return fConnectInfo;
}


void
RtmClient::handleText(std::string&& text)
{
	json payload = parseJson(text);
	if (!payload.is_object())
		return;
	std::string type = jStr(payload, "type");
	if (type == "hello") {
		fHandler(connectionEvent(ConnectionState::Connected, EventSource::Rtm),
			EventSource::Rtm);
	} else if (type == "goodbye") {
		fClient->reconnectNow();
	} else if (type == "error") {
		log(LogLevel::Warning, "rtm error: " + payload.dump());
		return;
	}
	std::optional<Event> event = parseEvent(payload);
	if (event)
		fHandler(*event, EventSource::Rtm);
}

// ---- SocketModeClient ---------------------------------------------------------------------

SocketModeClient::SocketModeClient(std::shared_ptr<const WebApi> api,
	EventHandler handler, WebSocketOptions options)
	:
	fApi(std::move(api)),
	fHandler(std::move(handler))
{
	WebSocketClient::Provider provider = [this]() -> Result<WebSocketTarget> {
		Result<ConnectInfo> info = fApi->appsConnectionsOpen();
		if (!info)
			return info.error();
		WebSocketTarget target;
		target.url = info->url;
		return target;
	};

	WebSocketClient::Callbacks callbacks;
	callbacks.onConnecting = [this](int attempt) {
		fHandler(connectionEvent(ConnectionState::Connecting, EventSource::SocketMode,
			{}, attempt), EventSource::SocketMode);
	};
	callbacks.onText = [this](std::string&& text) { handleText(std::move(text)); };
	callbacks.onDisconnected = [this](const CloseInfo& close, const Error& error,
			int retryInMs, int attempt) {
		fHandler(connectionEvent(ConnectionState::Disconnected, EventSource::SocketMode,
			closeDetail(close, error), attempt, retryInMs), EventSource::SocketMode);
		if (retryInMs < 0) {
			std::function<void(const Error&)> callback;
			{
				std::lock_guard<std::mutex> guard(fLock);
				callback = fOnGiveUp;
			}
			if (callback)
				callback(error);
		}
	};

	fClient = std::make_unique<WebSocketClient>(std::move(provider),
		std::move(callbacks), std::move(options));
}


SocketModeClient::~SocketModeClient()
{
	stop();
}


void
SocketModeClient::start()
{
	fClient->start();
}


void
SocketModeClient::stop()
{
	fClient->stop();
}


bool
SocketModeClient::connected() const
{
	return fClient->connected();
}


void
SocketModeClient::setOnGiveUp(std::function<void(const Error& error)> callback)
{
	std::lock_guard<std::mutex> guard(fLock);
	fOnGiveUp = std::move(callback);
}


void
SocketModeClient::handleText(std::string&& text)
{
	json envelope = parseJson(text);
	if (!envelope.is_object())
		return;
	std::string type = jStr(envelope, "type");
	std::string envelopeId = jStr(envelope, "envelope_id");

	// Acknowledge first: Slack retries unacknowledged envelopes, and an ack
	// must not wait for our own processing.
	if (!envelopeId.empty()) {
		if (fClient->send(json{{"envelope_id", envelopeId}}.dump()))
			fAcks++;
	}

	if (type == "hello") {
		fHandler(connectionEvent(ConnectionState::Connected, EventSource::SocketMode),
			EventSource::SocketMode);
		return;
	}
	if (type == "disconnect") {
		std::string reason = jStr(envelope, "reason");
		if (reason == "link_disabled") {
			fHandler(connectionEvent(ConnectionState::Disconnected,
				EventSource::SocketMode, "link_disabled"), EventSource::SocketMode);
			fClient->stop();
			std::function<void(const Error&)> callback;
			{
				std::lock_guard<std::mutex> guard(fLock);
				callback = fOnGiveUp;
			}
			if (callback)
				callback(Error(ErrorKind::Unsupported, "link_disabled"));
			return;
		}
		// "warning" and "refresh_requested": Slack is about to recycle us.
		fClient->reconnectNow();
		return;
	}
	if (type == "events_api") {
		const json& payload = jObj(envelope, "payload");
		std::string eventId = jStr(payload, "event_id");
		if (!eventId.empty()) {
			std::lock_guard<std::mutex> guard(fLock);
			if (!fSeenEventIds.insert(eventId).second)
				return;   // a retry of something we already handled
			fSeenOrder.push_back(eventId);
			if (fSeenOrder.size() > 512) {
				fSeenEventIds.erase(fSeenOrder.front());
				fSeenOrder.erase(fSeenOrder.begin());
			}
		}
		std::optional<Event> event = parseEvent(jObj(payload, "event"));
		if (event)
			fHandler(*event, EventSource::SocketMode);
		return;
	}
	if (!type.empty())
		fHandler(Event(UnknownEvent{type, {}, envelope}), EventSource::SocketMode);
}

// ---- Poller --------------------------------------------------------------------------------

namespace {

bool
reactionsEqual(const std::vector<Reaction>& a, const std::vector<Reaction>& b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++) {
		if (a[i].name != b[i].name || a[i].count != b[i].count || a[i].users != b[i].users)
			return false;
	}
	return true;
}


bool
messageChanged(const Message& before, const Message& after)
{
	if (before.text != after.text || before.subtype != after.subtype
		|| before.replyCount != after.replyCount
		|| before.latestReply != after.latestReply
		|| before.files.size() != after.files.size()
		|| !reactionsEqual(before.reactions, after.reactions))
		return true;
	std::string beforeEdit = before.edited ? before.edited->ts : std::string();
	std::string afterEdit = after.edited ? after.edited->ts : std::string();
	if (beforeEdit != afterEdit)
		return true;
	return before.blocks != after.blocks || before.attachments != after.attachments;
}

}  // namespace


Poller::Poller(std::shared_ptr<const WebApi> api, EventHandler handler,
	PollOptions options)
	:
	fApi(std::move(api)),
	fHandler(std::move(handler)),
	fOptions(options)
{
}


Poller::~Poller()
{
	stop();
}


void
Poller::start()
{
	std::lock_guard<std::mutex> guard(fLock);
	if (fRunning)
		return;
	if (fThread.joinable())
		fThread.join();
	fStopping = false;
	fRunning = true;
	fThread = std::thread(&Poller::threadMain, this);
}


void
Poller::stop()
{
	{
		std::lock_guard<std::mutex> guard(fLock);
		fStopping = true;
	}
	fWake.notify_all();
	if (fThread.joinable() && fThread.get_id() != std::this_thread::get_id())
		fThread.join();
}


void
Poller::setActiveConversation(const std::string& channel, const std::string& threadTs)
{
	{
		std::lock_guard<std::mutex> guard(fLock);
		fChannel = channel;
		fThreadTs = threadTs;
		fPollRequested = true;
	}
	fWake.notify_all();
}


void
Poller::setCountsChannels(std::vector<std::string> channels)
{
	std::lock_guard<std::mutex> guard(fLock);
	fCountsChannels = std::move(channels);
}


void
Poller::pollNow()
{
	{
		std::lock_guard<std::mutex> guard(fLock);
		fPollRequested = true;
	}
	fWake.notify_all();
}


void
Poller::diff(Snapshot& snapshot, std::vector<Message> fetched, bool hasMore)
{
	std::map<std::string, Message, TsLess> current;
	for (Message& message : fetched) {
		if (message.ts.empty())
			continue;
		message.channel = snapshot.channel;
		std::string ts = message.ts;
		current[ts] = std::move(message);
	}
	if (!snapshot.baseline) {
		snapshot.messages = std::move(current);
		snapshot.baseline = true;
		return;
	}

	std::vector<Event> events;
	std::string oldestKnown = snapshot.messages.empty() ? std::string()
		: snapshot.messages.begin()->first;
	for (const auto& [ts, message] : current) {
		auto known = snapshot.messages.find(ts);
		if (known == snapshot.messages.end()) {
			// Older messages sliding into the window are not new.
			if (oldestKnown.empty() || compareTs(ts, oldestKnown) > 0)
				events.push_back(MessageEvent{message});
		} else if (messageChanged(known->second, message)) {
			events.push_back(MessageChangedEvent{snapshot.channel, message, known->second});
		}
	}
	if (!current.empty()) {
		const std::string& oldestFetched = current.begin()->first;
		for (const auto& [ts, message] : snapshot.messages) {
			if (compareTs(ts, oldestFetched) >= 0 && current.count(ts) == 0) {
				MessageDeletedEvent deleted;
				deleted.channel = snapshot.channel;
				deleted.ts = ts;
				if (message.isThreadReply())
					deleted.threadTs = message.threadTs;
				events.push_back(std::move(deleted));
			}
		}
	}
	(void)hasMore;
	snapshot.messages = std::move(current);

	for (const Event& event : events) {
		if (fStopping)
			return;
		fHandler(event, EventSource::Polling);
	}
}


void
Poller::pollHistoryOnce()
{
	std::lock_guard<std::mutex> pollGuard(fPollLock);
	std::string channel;
	std::string threadTs;
	{
		std::lock_guard<std::mutex> guard(fLock);
		channel = fChannel;
		threadTs = fThreadTs;
	}

	if (channel != fHistory.channel) {
		fHistory = Snapshot();
		fHistory.channel = channel;
	}
	if (channel != fReplies.channel || threadTs != fReplies.threadTs) {
		fReplies = Snapshot();
		fReplies.channel = channel;
		fReplies.threadTs = threadTs;
	}
	if (channel.empty())
		return;

	HistoryOptions options;
	options.limit = fOptions.historyLimit;
	Result<HistoryPage> page = fApi->conversationsHistory(channel, options);
	if (page) {
		std::vector<Message> fetched = std::move(page->messages);
		// More new messages than one page: fetch the gap since the newest
		// message we know.
		if (fHistory.baseline && page->hasMore && !fHistory.messages.empty()) {
			const std::string& newest = fHistory.messages.rbegin()->first;
			bool overlap = std::any_of(fetched.begin(), fetched.end(),
				[&newest](const Message& m) { return compareTs(m.ts, newest) <= 0; });
			if (!overlap) {
				HistoryOptions gap;
				gap.limit = 200;
				gap.oldest = newest;
				Result<HistoryPage> more = fApi->conversationsHistory(channel, gap);
				if (more) {
					for (Message& message : more->messages)
						fetched.push_back(std::move(message));
				}
			}
		}
		diff(fHistory, std::move(fetched), page->hasMore);
	} else {
		log(LogLevel::Debug, "poll history: " + page.error().describe());
	}

	if (!threadTs.empty()) {
		HistoryOptions replyOptions;
		replyOptions.limit = 200;
		Result<HistoryPage> replies = fApi->conversationsReplies(channel, threadTs,
			replyOptions);
		if (replies)
			diff(fReplies, std::move(replies->messages), replies->hasMore);
	}
}


void
Poller::pollCountsOnce()
{
	std::vector<std::string> channels;
	{
		std::lock_guard<std::mutex> guard(fLock);
		channels = fCountsChannels;
	}
	Result<UnreadCounts> counts = fApi->unreadCounts(channels);
	if (!counts) {
		log(LogLevel::Debug, "poll counts: " + counts.error().describe());
		return;
	}
	if (!fStopping)
		fHandler(Event(CountsEvent{std::move(counts.value())}), EventSource::Polling);
}


void
Poller::threadMain()
{
	using Clock = std::chrono::steady_clock;
	fHandler(connectionEvent(ConnectionState::Polling, EventSource::Polling),
		EventSource::Polling);

	auto nextHistory = Clock::now();
	auto nextCounts = Clock::now();
	while (!fStopping) {
		bool forced = false;
		{
			std::unique_lock<std::mutex> guard(fLock);
			auto wakeAt = std::min(nextHistory, nextCounts);
			fWake.wait_until(guard, wakeAt,
				[this] { return fStopping.load() || fPollRequested; });
			forced = fPollRequested;
			fPollRequested = false;
		}
		if (fStopping)
			break;
		auto now = Clock::now();
		if (forced || now >= nextHistory) {
			pollHistoryOnce();
			nextHistory = Clock::now()
				+ std::chrono::milliseconds(fOptions.historyIntervalMs);
		}
		if (fStopping)
			break;
		if (fOptions.countsIntervalMs > 0 && Clock::now() >= nextCounts) {
			pollCountsOnce();
			nextCounts = Clock::now()
				+ std::chrono::milliseconds(fOptions.countsIntervalMs);
		} else if (fOptions.countsIntervalMs <= 0) {
			nextCounts = Clock::now() + std::chrono::hours(24);
		}
	}
	fRunning = false;
}

// ---- Realtime --------------------------------------------------------------------------------

Realtime::Realtime(std::shared_ptr<const WebApi> api, EventHandler handler,
	RealtimeOptions options)
	:
	fApi(std::move(api)),
	fHandler(std::move(handler)),
	fOptions(std::move(options))
{
}


Realtime::~Realtime()
{
	stop();
}


void
Realtime::dispatch(const Event& event, EventSource source)
{
	if (!fStopping)
		fHandler(event, source);
}


void
Realtime::startPolling(const std::string& reason)
{
	std::lock_guard<std::mutex> guard(fLock);
	if (fStopping)
		return;
	if (!reason.empty())
		log(LogLevel::Info, "real time falls back to polling: " + reason);
	fActive = RealtimeMode::Polling;
	if (!fPoller) {
		fPoller = std::make_unique<Poller>(fApi,
			[this](const Event& event, EventSource source) { dispatch(event, source); },
			fOptions.poll);
		fPoller->start();
	}
	fPoller->setCountsChannels(fCountsChannels);
	fPoller->setActiveConversation(fChannel, fThreadTs);
}


void
Realtime::start()
{
	RealtimeMode mode = fOptions.mode;
	if (mode == RealtimeMode::Auto) {
		mode = fApi->credentials().hasAppToken() ? RealtimeMode::SocketMode
			: RealtimeMode::Rtm;
	}
	fStopping = false;
	EventHandler forward = [this](const Event& event, EventSource source) {
		dispatch(event, source);
	};
	bool automatic = fOptions.mode == RealtimeMode::Auto;

	switch (mode) {
		case RealtimeMode::SocketMode: {
			std::lock_guard<std::mutex> guard(fLock);
			fActive = RealtimeMode::SocketMode;
			fSocket = std::make_unique<SocketModeClient>(fApi, forward, fOptions.websocket);
			if (automatic) {
				fSocket->setOnGiveUp([this](const Error& error) {
					startPolling(error.describe());
				});
			}
			fSocket->start();
			break;
		}
		case RealtimeMode::Rtm: {
			std::lock_guard<std::mutex> guard(fLock);
			fActive = RealtimeMode::Rtm;
			fRtm = std::make_unique<RtmClient>(fApi, forward, fOptions.websocket);
			if (automatic) {
				fRtm->setMaxFailedAttempts(fOptions.rtmFailuresBeforePolling);
				fRtm->setOnGiveUp([this](const Error& error) {
					startPolling(error.describe());
				});
			}
			fRtm->start();
			break;
		}
		case RealtimeMode::Polling:
			startPolling({});
			break;
		case RealtimeMode::Auto:
		case RealtimeMode::Off:
			break;
	}

	if (fOptions.pollCountsWithSocket && mode != RealtimeMode::Polling
		&& mode != RealtimeMode::Off) {
		std::lock_guard<std::mutex> guard(fLock);
		if (!fPoller) {
			// A counts-only poller: no active conversation is passed on.
			fPoller = std::make_unique<Poller>(fApi, forward, fOptions.poll);
			fPoller->setCountsChannels(fCountsChannels);
			fPoller->start();
		}
	}
}


void
Realtime::stop()
{
	std::unique_ptr<RtmClient> rtm;
	std::unique_ptr<SocketModeClient> socket;
	std::unique_ptr<Poller> poller;
	{
		std::lock_guard<std::mutex> guard(fLock);
		fStopping = true;
		rtm = std::move(fRtm);
		socket = std::move(fSocket);
		poller = std::move(fPoller);
		fActive = RealtimeMode::Off;
	}
	// Joining happens outside the lock: a source thread may be waiting for
	// it in startPolling().
	if (rtm)
		rtm->stop();
	if (socket)
		socket->stop();
	if (poller)
		poller->stop();
}


RealtimeMode
Realtime::activeMode() const
{
	std::lock_guard<std::mutex> guard(fLock);
	return fActive;
}


bool
Realtime::connected() const
{
	std::lock_guard<std::mutex> guard(fLock);
	switch (fActive) {
		case RealtimeMode::Rtm: return fRtm && fRtm->connected();
		case RealtimeMode::SocketMode: return fSocket && fSocket->connected();
		case RealtimeMode::Polling: return fPoller && fPoller->running();
		default: return false;
	}
}


void
Realtime::setActiveConversation(const std::string& channel, const std::string& threadTs)
{
	std::lock_guard<std::mutex> guard(fLock);
	fChannel = channel;
	fThreadTs = threadTs;
	if (fActive == RealtimeMode::Polling && fPoller)
		fPoller->setActiveConversation(channel, threadTs);
}


void
Realtime::setCountsChannels(std::vector<std::string> channels)
{
	std::lock_guard<std::mutex> guard(fLock);
	fCountsChannels = channels;
	if (fPoller)
		fPoller->setCountsChannels(std::move(channels));
}


bool
Realtime::sendTyping(const std::string& channel, const std::string& threadTs)
{
	std::lock_guard<std::mutex> guard(fLock);
	if (fActive == RealtimeMode::Rtm && fRtm)
		return fRtm->sendTyping(channel, threadTs);
	return false;
}


void
Realtime::pollNow()
{
	std::lock_guard<std::mutex> guard(fLock);
	if (fPoller)
		fPoller->pollNow();
}

}  // namespace natter
