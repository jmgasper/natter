// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "natter/credentials.h"
#include "natter/events.h"
#include "natter/realtime.h"
#include "natter/store.h"
#include "natter/web_api.h"

namespace natter {

// A fixed pool of threads running jobs in FIFO order.
class WorkQueue {
public:
	explicit WorkQueue(int threads = 2);
	~WorkQueue();   // drops queued jobs, waits for running ones

	void post(std::function<void()> job);
	void stop();
	size_t pending() const;

private:
	void threadMain();

	std::vector<std::thread> fThreads;
	mutable std::mutex fLock;
	std::condition_variable fWake;
	std::deque<std::function<void()>> fJobs;
	bool fStopping = false;
};

struct SessionOptions {
	std::string cacheDirectory;            // empty: no disk cache
	size_t cacheMessagesPerConversation = 50;
	RealtimeOptions realtime;
	ApiOptions api;
	int workerThreads = 2;
	// Conversations whose counts come from conversations.info when neither
	// counts method works (each costs one Tier 3 call per counts poll).
	size_t maxInfoCountsChannels = 20;
};

struct BootstrapOptions {
	bool team = true;
	bool users = true;
	bool channels = true;
	bool emoji = true;
	bool counts = true;
};

// What listeners receive: the event, where it came from, and what it changed
// in the store (already merged when the listener runs).
struct SessionEvent {
	Event event;
	EventSource source = EventSource::Local;
	StoreChange change;
};

// One signed-in workspace: a WebApi, a Store kept up to date, and the
// real-time connection feeding it.
//
// Threading:
//  * The blocking methods (bootstrap, loadHistory, send, ...) run on the
//    calling thread; call them from a worker, e.g. via post().
//  * Listeners run on whichever thread produced the event: the RTM/Socket
//    Mode thread, the poller thread, or the thread that called send() and
//    friends (source Local). Dispatch is serialised: one listener call at a
//    time, in order. A listener must not block waiting for the UI thread;
//    post a message to it instead (BMessenger::SendMessage without a reply).
//  * All other methods are thread-safe.
class Session {
public:
	using Listener = std::function<void(const SessionEvent& event)>;

	explicit Session(Credentials credentials, SessionOptions options = {},
		std::shared_ptr<HttpTransport> transport = nullptr);
	~Session();

	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;

	const Credentials& credentials() const { return fApi->credentials(); }
	std::shared_ptr<WebApi> api() const { return fApi; }
	Store& store() { return fStore; }
	const Store& store() const { return fStore; }

	// ---- listeners ----------------------------------------------------------------
	int addListener(Listener listener);
	// Once this returns the listener will not be called again (unless it is
	// called from inside that listener, which is allowed).
	void removeListener(int id);

	// ---- startup (blocking) ------------------------------------------------------------
	Status loadCache();
	Status saveCache() const;
	// auth.test, then team, users, conversations, emoji and counts. Only the
	// auth and conversation-list failures are fatal.
	Status bootstrap(const BootstrapOptions& options = {});

	// ---- conversations (blocking) ---------------------------------------------------------
	Result<HistoryPage> loadHistory(const std::string& channel,
		const HistoryOptions& options = {});
	Result<HistoryPage> loadThread(const std::string& channel, const std::string& threadTs,
		const HistoryOptions& options = {});
	Result<Message> send(const std::string& channel, const std::string& text,
		const PostOptions& options = {});
	Result<Message> edit(const std::string& channel, const std::string& ts,
		const std::string& text);
	Status remove(const std::string& channel, const std::string& ts);
	Status react(const std::string& channel, const std::string& ts,
		const std::string& name, bool add = true);
	Status markRead(const std::string& channel, const std::string& ts);
	Result<Channel> openDirectMessage(const std::vector<std::string>& users);
	Status refreshCounts();

	// ---- real time -------------------------------------------------------------------------
	void startRealtime();
	void stopRealtime();
	RealtimeMode realtimeMode() const;
	// The conversation (and thread) on screen: polled first in polling mode.
	void setActiveConversation(const std::string& channel, const std::string& threadTs = {});
	bool sendTyping(const std::string& channel, const std::string& threadTs = {});

	// ---- helpers ---------------------------------------------------------------------------
	// Run a job on the session's worker threads.
	void post(std::function<void()> job);
	FormatContext formatContext() const { return fStore.formatContext(); }
	FormattedText format(const Message& message) const
	{
		return formatMessage(message, fStore.formatContext());
	}

private:
	void handleEvent(const Event& event, EventSource source);
	void deliver(const Event& event, EventSource source, const StoreChange& change);
	std::vector<std::string> countsChannels() const;
	std::string selfMessageUser() const;

	SessionOptions fOptions;
	std::shared_ptr<WebApi> fApi;
	Store fStore;

	std::recursive_mutex fDispatchLock;   // serialises listener calls
	std::mutex fListenerLock;
	std::map<int, std::shared_ptr<Listener>> fListeners;
	int fNextListener = 1;

	mutable std::mutex fRealtimeLock;
	std::unique_ptr<Realtime> fRealtime;
	std::string fActiveChannel;
	std::string fActiveThread;

	std::unique_ptr<WorkQueue> fWorkers;
};

}  // namespace natter
