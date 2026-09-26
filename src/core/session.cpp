// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT

#include "natter/session.h"

#include <algorithm>

namespace natter {

// ---- WorkQueue -------------------------------------------------------------------------

WorkQueue::WorkQueue(int threads)
{
	for (int i = 0; i < std::max(1, threads); i++)
		fThreads.emplace_back(&WorkQueue::threadMain, this);
}


WorkQueue::~WorkQueue()
{
	stop();
}


void
WorkQueue::post(std::function<void()> job)
{
	{
		std::lock_guard<std::mutex> guard(fLock);
		if (fStopping)
			return;
		fJobs.push_back(std::move(job));
	}
	fWake.notify_one();
}


void
WorkQueue::stop()
{
	{
		std::lock_guard<std::mutex> guard(fLock);
		fStopping = true;
		fJobs.clear();
	}
	fWake.notify_all();
	for (std::thread& thread : fThreads) {
		if (thread.joinable() && thread.get_id() != std::this_thread::get_id())
			thread.join();
		else if (thread.joinable())
			thread.detach();
	}
	fThreads.clear();
}


size_t
WorkQueue::pending() const
{
	std::lock_guard<std::mutex> guard(fLock);
	return fJobs.size();
}


void
WorkQueue::threadMain()
{
	for (;;) {
		std::function<void()> job;
		{
			std::unique_lock<std::mutex> guard(fLock);
			fWake.wait(guard, [this] { return fStopping || !fJobs.empty(); });
			if (fStopping)
				return;
			job = std::move(fJobs.front());
			fJobs.pop_front();
		}
		try {
			job();
		} catch (const std::exception& exception) {
			log(LogLevel::Error, std::string("job threw: ") + exception.what());
		} catch (...) {
			log(LogLevel::Error, "job threw");
		}
	}
}

// ---- Session ---------------------------------------------------------------------------

Session::Session(Credentials credentials, SessionOptions options,
	std::shared_ptr<HttpTransport> transport)
	:
	fOptions(std::move(options)),
	fApi(std::make_shared<WebApi>(transport ? transport : defaultTransport(),
		std::move(credentials), fOptions.api)),
	fWorkers(std::make_unique<WorkQueue>(fOptions.workerThreads))
{
}


Session::~Session()
{
	stopRealtime();
	fApi->cancel();
	fWorkers->stop();
}


int
Session::addListener(Listener listener)
{
	std::lock_guard<std::mutex> guard(fListenerLock);
	int id = fNextListener++;
	fListeners[id] = std::make_shared<Listener>(std::move(listener));
	return id;
}


void
Session::removeListener(int id)
{
	{
		std::lock_guard<std::mutex> guard(fListenerLock);
		fListeners.erase(id);
	}
	// Wait for a dispatch in progress on another thread to finish; from
	// inside a listener the recursive lock is already ours.
	std::lock_guard<std::recursive_mutex> dispatch(fDispatchLock);
}


void
Session::deliver(const Event& event, EventSource source, const StoreChange& change)
{
	std::lock_guard<std::recursive_mutex> dispatch(fDispatchLock);
	std::vector<std::pair<int, std::shared_ptr<Listener>>> listeners;
	{
		std::lock_guard<std::mutex> guard(fListenerLock);
		listeners.assign(fListeners.begin(), fListeners.end());
	}
	SessionEvent sessionEvent{event, source, change};
	for (const auto& [id, listener] : listeners) {
		{
			// Skip listeners removed by an earlier listener in this round.
			std::lock_guard<std::mutex> guard(fListenerLock);
			if (fListeners.count(id) == 0)
				continue;
		}
		(*listener)(sessionEvent);
	}
}


void
Session::handleEvent(const Event& event, EventSource source)
{
	StoreChange change = fStore.apply(event);
	deliver(event, source, change);
}


Status
Session::loadCache()
{
	if (fOptions.cacheDirectory.empty())
		return {};
	return fStore.loadCache(fOptions.cacheDirectory);
}


Status
Session::saveCache() const
{
	if (fOptions.cacheDirectory.empty())
		return {};
	return fStore.saveCache(fOptions.cacheDirectory, fOptions.cacheMessagesPerConversation);
}


std::vector<std::string>
Session::countsChannels() const
{
	std::vector<Channel> channels = fStore.channels();
	std::vector<std::string> ids;
	// Direct messages first: they are the ones people expect badges on.
	std::stable_sort(channels.begin(), channels.end(),
		[](const Channel& a, const Channel& b) { return a.isDirect() > b.isDirect(); });
	for (const Channel& channel : channels) {
		if (!channel.isMember || channel.isArchived)
			continue;
		ids.push_back(channel.id);
		if (ids.size() >= fOptions.maxInfoCountsChannels)
			break;
	}
	return ids;
}


Status
Session::bootstrap(const BootstrapOptions& options)
{
	Result<AuthInfo> auth = fApi->authTest();
	if (!auth)
		return auth.error();
	fStore.setSelf(auth->userId, auth->teamId);

	if (options.team) {
		Result<Team> team = fApi->teamInfo();
		if (team) {
			Team value = team.value();
			if (value.url.empty())
				value.url = auth->url;
			fStore.setTeam(value);
		} else {
			Team fallback;
			fallback.id = auth->teamId;
			fallback.name = auth->team;
			fallback.url = auth->url;
			fStore.setTeam(fallback);
			log(LogLevel::Info, "team.info: " + team.error().describe());
		}
	}
	if (options.users) {
		Result<std::vector<User>> users = fApi->usersList();
		if (users)
			fStore.setUsers(users.value());
		else
			log(LogLevel::Warning, "users.list: " + users.error().describe());
	}
	if (options.channels) {
		Result<std::vector<Channel>> channels = fApi->conversationsList();
		if (!channels)
			return channels.error();
		fStore.setChannels(channels.value());
	}
	if (options.emoji) {
		Result<std::map<std::string, std::string>> emoji = fApi->emojiList();
		if (emoji)
			fStore.setEmoji(emoji.value());
		else
			log(LogLevel::Info, "emoji.list: " + emoji.error().describe());
	}
	if (options.counts)
		refreshCounts();

	std::lock_guard<std::mutex> guard(fRealtimeLock);
	if (fRealtime)
		fRealtime->setCountsChannels(countsChannels());
	return {};
}


Status
Session::refreshCounts()
{
	Result<UnreadCounts> counts = fApi->unreadCounts(countsChannels());
	if (!counts)
		return counts.error();
	handleEvent(Event(CountsEvent{counts.value()}), EventSource::Local);
	return {};
}


Result<HistoryPage>
Session::loadHistory(const std::string& channel, const HistoryOptions& options)
{
	Result<HistoryPage> page = fApi->conversationsHistory(channel, options);
	if (page)
		fStore.mergeHistory(channel, page->messages);
	return page;
}


Result<HistoryPage>
Session::loadThread(const std::string& channel, const std::string& threadTs,
	const HistoryOptions& options)
{
	Result<HistoryPage> page = fApi->conversationsReplies(channel, threadTs, options);
	if (page)
		fStore.mergeReplies(channel, threadTs, page->messages);
	return page;
}


std::string
Session::selfMessageUser() const
{
	std::string self = fStore.selfUserId();
	return self.empty() ? fApi->credentials().userId : self;
}


Result<Message>
Session::send(const std::string& channel, const std::string& text,
	const PostOptions& options)
{
	Result<Message> message = fApi->chatPostMessage(channel, text, options);
	if (message) {
		Message copy = message.value();
		if (copy.user.empty() && copy.botId.empty())
			copy.user = selfMessageUser();
		handleEvent(Event(MessageEvent{copy}), EventSource::Local);
	}
	return message;
}


Result<Message>
Session::edit(const std::string& channel, const std::string& ts, const std::string& text)
{
	Result<Message> message = fApi->chatUpdate(channel, ts, text);
	if (message) {
		// Merge onto what we have: chat.update answers with a thin message.
		Message merged = fStore.message(channel, ts).value_or(message.value());
		merged.text = message->text;
		if (!message->blocks.is_null())
			merged.blocks = message->blocks;
		merged.edited = Edited{selfMessageUser(), message->ts};
		handleEvent(Event(MessageChangedEvent{channel, merged, std::nullopt}),
			EventSource::Local);
	}
	return message;
}


Status
Session::remove(const std::string& channel, const std::string& ts)
{
	Status status = fApi->chatDelete(channel, ts);
	if (status) {
		MessageDeletedEvent event;
		event.channel = channel;
		event.ts = ts;
		std::optional<Message> old = fStore.message(channel, ts);
		if (old && old->isThreadReply())
			event.threadTs = old->threadTs;
		handleEvent(Event(event), EventSource::Local);
	}
	return status;
}


Status
Session::react(const std::string& channel, const std::string& ts, const std::string& name,
	bool add)
{
	Status status = add ? fApi->reactionsAdd(channel, ts, name)
		: fApi->reactionsRemove(channel, ts, name);
	// Adding what is already there is not worth an error for the UI.
	bool harmless = !status && (status.error().code == "already_reacted"
		|| status.error().code == "no_reaction");
	if (status || harmless) {
		ReactionEvent event;
		event.added = add;
		event.user = selfMessageUser();
		event.reaction = name;
		if (startsWith(event.reaction, ":"))
			event.reaction.erase(0, 1);
		if (endsWith(event.reaction, ":"))
			event.reaction.pop_back();
		event.channel = channel;
		event.ts = ts;
		handleEvent(Event(event), EventSource::Local);
		return {};
	}
	return status;
}


Status
Session::markRead(const std::string& channel, const std::string& ts)
{
	Status status = fApi->conversationsMark(channel, ts);
	if (status) {
		MarkedEvent event;
		event.channel = channel;
		event.ts = ts;
		event.kind = "channel";
		handleEvent(Event(event), EventSource::Local);
	}
	return status;
}


Result<Channel>
Session::openDirectMessage(const std::vector<std::string>& users)
{
	Result<Channel> channel = fApi->conversationsOpen(users);
	if (channel)
		handleEvent(Event(ChannelJoinedEvent{channel.value()}), EventSource::Local);
	return channel;
}


void
Session::startRealtime()
{
	std::lock_guard<std::mutex> guard(fRealtimeLock);
	if (fRealtime)
		return;
	fRealtime = std::make_unique<Realtime>(fApi,
		[this](const Event& event, EventSource source) { handleEvent(event, source); },
		fOptions.realtime);
	fRealtime->setCountsChannels(countsChannels());
	fRealtime->setActiveConversation(fActiveChannel, fActiveThread);
	fRealtime->start();
}


void
Session::stopRealtime()
{
	std::unique_ptr<Realtime> realtime;
	{
		std::lock_guard<std::mutex> guard(fRealtimeLock);
		realtime = std::move(fRealtime);
	}
	if (realtime)
		realtime->stop();
}


RealtimeMode
Session::realtimeMode() const
{
	std::lock_guard<std::mutex> guard(fRealtimeLock);
	return fRealtime ? fRealtime->activeMode() : RealtimeMode::Off;
}


void
Session::setActiveConversation(const std::string& channel, const std::string& threadTs)
{
	std::lock_guard<std::mutex> guard(fRealtimeLock);
	fActiveChannel = channel;
	fActiveThread = threadTs;
	if (fRealtime)
		fRealtime->setActiveConversation(channel, threadTs);
}


bool
Session::sendTyping(const std::string& channel, const std::string& threadTs)
{
	std::lock_guard<std::mutex> guard(fRealtimeLock);
	return fRealtime && fRealtime->sendTyping(channel, threadTs);
}


void
Session::post(std::function<void()> job)
{
	fWorkers->post(std::move(job));
}

}  // namespace natter
