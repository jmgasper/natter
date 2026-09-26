// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT

#include "natter/store.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

namespace natter {

namespace {

constexpr int kCacheVersion = 1;

// Fields that only some sources know are kept when fresh data lacks them.
Channel
mergeChannelData(const Channel& old, Channel fresh)
{
	if (fresh.lastRead.empty()) {
		fresh.lastRead = old.lastRead;
		fresh.unreadCount = old.unreadCount;
		fresh.mentionCount = old.mentionCount;
		fresh.hasUnreads = old.hasUnreads;
	}
	if (fresh.latestTs.empty())
		fresh.latestTs = old.latestTs;
	if (fresh.name.empty())
		fresh.name = old.name;
	if (fresh.imUser.empty())
		fresh.imUser = old.imUser;
	if (fresh.topic.value.empty())
		fresh.topic = old.topic;
	if (fresh.purpose.value.empty())
		fresh.purpose = old.purpose;
	if (fresh.numMembers == 0)
		fresh.numMembers = old.numMembers;
	if (fresh.created == 0)
		fresh.created = old.created;
	return fresh;
}


// Keep what a thinner copy of the same message does not carry.
void
preserveExtras(const Message& old, Message& fresh)
{
	if (fresh.reactions.empty() && !old.reactions.empty())
		fresh.reactions = old.reactions;
	if (fresh.replyCount == 0 && old.replyCount > 0) {
		fresh.replyCount = old.replyCount;
		fresh.replyUsers = old.replyUsers;
		fresh.replyUsersCount = old.replyUsersCount;
		fresh.latestReply = old.latestReply;
		if (fresh.threadTs.empty())
			fresh.threadTs = old.threadTs;
	}
	if (fresh.channel.empty())
		fresh.channel = old.channel;
}


bool
countsAsUnread(const Message& message)
{
	if (message.hidden)
		return false;
	static const char* kQuiet[] = {"channel_join", "channel_leave", "group_join",
		"group_leave", "channel_topic", "channel_purpose", "message_replied"};
	for (const char* subtype : kQuiet) {
		if (message.subtype == subtype)
			return false;
	}
	return !message.isThreadReply() || message.isBroadcastReply();
}


Status
writeFileAtomically(const std::string& path, const std::string& text)
{
	std::string temporary = path + ".tmp";
	{
		std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
		if (!out)
			return Error(ErrorKind::InvalidArgument, "cannot_write", temporary);
		out << text;
		if (!out)
			return Error(ErrorKind::InvalidArgument, "cannot_write", temporary);
	}
	if (std::rename(temporary.c_str(), path.c_str()) != 0) {
		std::remove(temporary.c_str());
		return Error(ErrorKind::InvalidArgument, "cannot_write", path);
	}
	return {};
}


json
readJsonFile(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return json();
	std::stringstream buffer;
	buffer << in.rdbuf();
	json value = parseJson(buffer.str());
	return value.is_discarded() ? json() : value;
}


bool
makeDirectories(const std::string& path)
{
	std::string partial;
	size_t pos = 0;
	while (pos != std::string::npos) {
		pos = path.find('/', pos + 1);
		partial = path.substr(0, pos);
		if (partial.empty())
			continue;
		if (::mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST)
			return false;
	}
	return true;
}

}  // namespace

// ---- identity -----------------------------------------------------------------------

void
Store::setSelf(const std::string& userId, const std::string& teamId)
{
	std::unique_lock lock(fLock);
	fSelfId = userId;
	if (!teamId.empty())
		fTeamId = teamId;
}


std::string
Store::selfUserId() const
{
	std::shared_lock lock(fLock);
	return fSelfId;
}


void
Store::setTeam(const natter::Team& team)
{
	std::unique_lock lock(fLock);
	fTeam = team;
	if (!team.id.empty())
		fTeamId = team.id;
}


natter::Team
Store::team() const
{
	std::shared_lock lock(fLock);
	return fTeam;
}

// ---- users --------------------------------------------------------------------------

void
Store::setUsers(const std::vector<User>& users)
{
	std::unique_lock lock(fLock);
	std::unordered_map<std::string, User> fresh;
	fresh.reserve(users.size());
	for (const User& user : users) {
		User copy = user;
		auto old = fUsers.find(user.id);
		if (copy.presence.empty() && old != fUsers.end())
			copy.presence = old->second.presence;
		fresh[user.id] = std::move(copy);
	}
	fUsers = std::move(fresh);
}


void
Store::upsertUser(const User& user)
{
	std::unique_lock lock(fLock);
	User copy = user;
	auto old = fUsers.find(user.id);
	if (old != fUsers.end() && copy.presence.empty())
		copy.presence = old->second.presence;
	fUsers[user.id] = std::move(copy);
}


std::optional<User>
Store::user(const std::string& id) const
{
	std::shared_lock lock(fLock);
	auto it = fUsers.find(id);
	if (it == fUsers.end())
		return std::nullopt;
	return it->second;
}


std::vector<User>
Store::users() const
{
	std::shared_lock lock(fLock);
	std::vector<User> out;
	out.reserve(fUsers.size());
	for (const auto& [id, user] : fUsers)
		out.push_back(user);
	std::sort(out.begin(), out.end(),
		[](const User& a, const User& b) { return a.id < b.id; });
	return out;
}


std::string
Store::userNameLocked(const std::string& id) const
{
	auto it = fUsers.find(id);
	return it == fUsers.end() ? id : it->second.bestName();
}


std::string
Store::userName(const std::string& id) const
{
	std::shared_lock lock(fLock);
	return userNameLocked(id);
}


void
Store::setPresence(const std::vector<std::string>& ids, const std::string& presence)
{
	std::unique_lock lock(fLock);
	for (const std::string& id : ids) {
		auto it = fUsers.find(id);
		if (it != fUsers.end())
			it->second.presence = presence;
	}
}

// ---- channels -----------------------------------------------------------------------

void
Store::setChannels(const std::vector<Channel>& channels)
{
	std::unique_lock lock(fLock);
	std::unordered_map<std::string, Channel> fresh;
	fresh.reserve(channels.size());
	for (const Channel& channel : channels) {
		auto old = fChannels.find(channel.id);
		fresh[channel.id] = old == fChannels.end() ? channel
			: mergeChannelData(old->second, channel);
	}
	fChannels = std::move(fresh);
}


void
Store::upsertChannel(const Channel& channel)
{
	std::unique_lock lock(fLock);
	auto old = fChannels.find(channel.id);
	fChannels[channel.id] = old == fChannels.end() ? channel
		: mergeChannelData(old->second, channel);
}


void
Store::removeChannel(const std::string& id)
{
	std::unique_lock lock(fLock);
	fChannels.erase(id);
	fTimelines.erase(id);
	fThreads.erase(id);
}


std::optional<Channel>
Store::channel(const std::string& id) const
{
	std::shared_lock lock(fLock);
	auto it = fChannels.find(id);
	if (it == fChannels.end())
		return std::nullopt;
	return it->second;
}


std::vector<Channel>
Store::channels() const
{
	std::shared_lock lock(fLock);
	std::vector<Channel> out;
	out.reserve(fChannels.size());
	for (const auto& [id, channel] : fChannels)
		out.push_back(channel);
	std::sort(out.begin(), out.end(), [](const Channel& a, const Channel& b) {
		if (a.kind() != b.kind())
			return a.kind() < b.kind();
		if (a.name != b.name)
			return a.name < b.name;
		return a.id < b.id;
	});
	return out;
}


std::optional<Channel>
Store::findChannel(const std::string& nameOrId) const
{
	std::string key = nameOrId;
	if (startsWith(key, "#") || startsWith(key, "@"))
		key.erase(0, 1);
	std::shared_lock lock(fLock);
	auto exact = fChannels.find(key);
	if (exact != fChannels.end())
		return exact->second;
	for (const auto& [id, channel] : fChannels) {
		if (!channel.isIm && (channel.name == key || channel.nameNormalized == key))
			return channel;
	}
	for (const auto& [id, channel] : fChannels) {
		if (!channel.isIm)
			continue;
		auto user = fUsers.find(channel.imUser);
		if (channel.imUser == key || (user != fUsers.end()
				&& (user->second.name == key || user->second.displayName == key
					|| user->second.realName == key)))
			return channel;
	}
	return std::nullopt;
}


std::string
Store::channelDisplayName(const std::string& id) const
{
	std::shared_lock lock(fLock);
	auto it = fChannels.find(id);
	if (it == fChannels.end())
		return id;
	const Channel& channel = it->second;
	if (channel.isIm)
		return "@" + userNameLocked(channel.imUser);
	if (channel.isMpim && startsWith(channel.name, "mpdm-")) {
		// mpdm-alice--bob--carol-1
		std::string names = channel.name.substr(5);
		size_t dash = names.rfind('-');
		if (dash != std::string::npos)
			names.resize(dash);
		std::string out;
		size_t pos = 0;
		while (pos <= names.size()) {
			size_t next = names.find("--", pos);
			if (!out.empty())
				out += ", ";
			out += names.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
			if (next == std::string::npos)
				break;
			pos = next + 2;
		}
		return out;
	}
	return "#" + channel.name;
}

// ---- messages --------------------------------------------------------------------------

void
Store::trimLocked(Timeline& timeline)
{
	while (timeline.size() > fMaxMessages)
		timeline.erase(timeline.begin());
}


template <typename Fn>
bool
Store::forEachCopyLocked(const std::string& channel, const std::string& ts, Fn&& fn)
{
	bool found = false;
	auto timeline = fTimelines.find(channel);
	if (timeline != fTimelines.end()) {
		auto it = timeline->second.find(ts);
		if (it != timeline->second.end()) {
			fn(it->second);
			found = true;
		}
	}
	auto threads = fThreads.find(channel);
	if (threads != fThreads.end()) {
		for (auto& [threadTs, thread] : threads->second) {
			auto it = thread.find(ts);
			if (it != thread.end()) {
				fn(it->second);
				found = true;
			}
		}
	}
	return found;
}


void
Store::upsertMessageLocked(Message message, StoreChange& change)
{
	Timeline& timeline = fTimelines[message.channel];
	auto it = timeline.find(message.ts);
	if (it != timeline.end())
		preserveExtras(it->second, message);
	// Keep the parent copy of an open thread in step.
	auto threads = fThreads.find(message.channel);
	if (threads != fThreads.end()) {
		auto thread = threads->second.find(message.ts);
		if (thread != threads->second.end()) {
			auto parent = thread->second.find(message.ts);
			if (parent != thread->second.end()) {
				Message copy = message;
				preserveExtras(parent->second, copy);
				parent->second = copy;
			}
		}
	}
	change.kind = StoreChange::Messages;
	change.channel = message.channel;
	change.ts = message.ts;
	std::string ts = message.ts;
	timeline[ts] = std::move(message);
	trimLocked(timeline);
}


void
Store::insertReplyLocked(const Message& reply, bool& isNew)
{
	Timeline& thread = fThreads[reply.channel][reply.threadTs];
	auto it = thread.find(reply.ts);
	isNew = it == thread.end();
	Message copy = reply;
	if (!isNew)
		preserveExtras(it->second, copy);
	thread[reply.ts] = std::move(copy);
	if (!isNew)
		return;

	forEachCopyLocked(reply.channel, reply.threadTs, [&reply](Message& parent) {
		if (parent.ts != reply.threadTs)
			return;
		if (parent.threadTs.empty())
			parent.threadTs = parent.ts;
		// A parent fetched after this reply was posted already counts it.
		if (!parent.latestReply.empty() && compareTs(parent.latestReply, reply.ts) >= 0)
			return;
		parent.replyCount++;
		parent.latestReply = reply.ts;
		std::string author = reply.user.empty() ? reply.botId : reply.user;
		if (!author.empty() && std::find(parent.replyUsers.begin(),
				parent.replyUsers.end(), author) == parent.replyUsers.end()) {
			parent.replyUsers.push_back(author);
			parent.replyUsersCount = static_cast<int>(parent.replyUsers.size());
		}
	});
}


void
Store::mergeHistory(const std::string& channel, const std::vector<Message>& messages)
{
	std::unique_lock lock(fLock);
	StoreChange ignored;
	for (Message message : messages) {
		if (message.ts.empty())
			continue;
		message.channel = channel;
		if (message.isThreadReply() && !message.isBroadcastReply()) {
			bool isNew;
			insertReplyLocked(message, isNew);
			continue;
		}
		upsertMessageLocked(std::move(message), ignored);
	}
	auto it = fChannels.find(channel);
	auto timeline = fTimelines.find(channel);
	if (it != fChannels.end() && timeline != fTimelines.end() && !timeline->second.empty()) {
		const std::string& newest = timeline->second.rbegin()->first;
		if (it->second.latestTs.empty() || compareTs(newest, it->second.latestTs) > 0)
			it->second.latestTs = newest;
	}
}


void
Store::mergeReplies(const std::string& channel, const std::string& threadTs,
	const std::vector<Message>& messages)
{
	std::unique_lock lock(fLock);
	Timeline& thread = fThreads[channel][threadTs];
	for (Message message : messages) {
		if (message.ts.empty())
			continue;
		message.channel = channel;
		auto it = thread.find(message.ts);
		if (it != thread.end())
			preserveExtras(it->second, message);
		if (message.ts == threadTs) {
			// The parent: refresh the timeline copy too.
			auto timeline = fTimelines.find(channel);
			if (timeline != fTimelines.end()) {
				auto parent = timeline->second.find(threadTs);
				if (parent != timeline->second.end())
					parent->second = message;
			}
		}
		std::string ts = message.ts;
		thread[ts] = std::move(message);
	}
	// The parent counts at least the replies we hold: its reply_count can lag
	// behind replies that arrived in real time.
	int replies = 0;
	std::string latest;
	for (const auto& [ts, reply] : thread) {
		if (ts == threadTs || reply.hidden)
			continue;
		replies++;
		latest = ts;
	}
	forEachCopyLocked(channel, threadTs, [&](Message& parent) {
		if (parent.ts != threadTs || parent.replyCount >= replies)
			return;
		parent.replyCount = replies;
		if (parent.latestReply.empty() || compareTs(parent.latestReply, latest) < 0)
			parent.latestReply = latest;
	});
}


std::vector<Message>
Store::messages(const std::string& channel, size_t limit) const
{
	std::shared_lock lock(fLock);
	std::vector<Message> out;
	auto it = fTimelines.find(channel);
	if (it == fTimelines.end())
		return out;
	const Timeline& timeline = it->second;
	size_t skip = limit > 0 && timeline.size() > limit ? timeline.size() - limit : 0;
	out.reserve(timeline.size() - skip);
	size_t index = 0;
	for (const auto& [ts, message] : timeline) {
		if (index++ < skip)
			continue;
		out.push_back(message);
	}
	return out;
}


std::vector<Message>
Store::messagesBefore(const std::string& channel, const std::string& ts,
	size_t limit) const
{
	std::shared_lock lock(fLock);
	std::vector<Message> out;
	auto it = fTimelines.find(channel);
	if (it == fTimelines.end())
		return out;
	auto end = it->second.lower_bound(ts);
	auto begin = end;
	for (size_t i = 0; i < limit && begin != it->second.begin(); i++)
		--begin;
	for (auto cursor = begin; cursor != end; ++cursor)
		out.push_back(cursor->second);
	return out;
}


std::optional<Message>
Store::message(const std::string& channel, const std::string& ts) const
{
	std::shared_lock lock(fLock);
	auto timeline = fTimelines.find(channel);
	if (timeline != fTimelines.end()) {
		auto it = timeline->second.find(ts);
		if (it != timeline->second.end())
			return it->second;
	}
	auto threads = fThreads.find(channel);
	if (threads != fThreads.end()) {
		for (const auto& [threadTs, thread] : threads->second) {
			auto it = thread.find(ts);
			if (it != thread.end())
				return it->second;
		}
	}
	return std::nullopt;
}


std::vector<Message>
Store::thread(const std::string& channel, const std::string& threadTs) const
{
	std::shared_lock lock(fLock);
	std::vector<Message> out;
	auto threads = fThreads.find(channel);
	const Timeline* thread = nullptr;
	if (threads != fThreads.end()) {
		auto it = threads->second.find(threadTs);
		if (it != threads->second.end())
			thread = &it->second;
	}
	bool haveParent = thread != nullptr && thread->count(threadTs) != 0;
	if (!haveParent) {
		auto timeline = fTimelines.find(channel);
		if (timeline != fTimelines.end()) {
			auto parent = timeline->second.find(threadTs);
			if (parent != timeline->second.end())
				out.push_back(parent->second);
		}
	}
	if (thread != nullptr) {
		for (const auto& [ts, message] : *thread)
			out.push_back(message);
	}
	return out;
}


std::string
Store::newestTs(const std::string& channel) const
{
	std::shared_lock lock(fLock);
	auto it = fTimelines.find(channel);
	if (it == fTimelines.end() || it->second.empty())
		return {};
	return it->second.rbegin()->first;
}


std::string
Store::oldestTs(const std::string& channel) const
{
	std::shared_lock lock(fLock);
	auto it = fTimelines.find(channel);
	if (it == fTimelines.end() || it->second.empty())
		return {};
	return it->second.begin()->first;
}


void
Store::setMaxMessagesPerConversation(size_t count)
{
	std::unique_lock lock(fLock);
	fMaxMessages = std::max<size_t>(count, 1);
	for (auto& [id, timeline] : fTimelines)
		trimLocked(timeline);
}

// ---- emoji ---------------------------------------------------------------------------

void
Store::setEmoji(const std::map<std::string, std::string>& emoji)
{
	std::unique_lock lock(fLock);
	fEmoji = emoji;
}


std::map<std::string, std::string>
Store::emoji() const
{
	std::shared_lock lock(fLock);
	return fEmoji;
}


std::string
Store::resolveEmojiLocked(const std::string& name) const
{
	std::string current = name;
	for (int depth = 0; depth < 10; depth++) {
		auto it = fEmoji.find(current);
		if (it == fEmoji.end())
			return depth == 0 ? std::string() : current;   // alias of a standard one
		if (!startsWith(it->second, "alias:"))
			return current;
		current = it->second.substr(6);
	}
	return {};
}


std::string
Store::resolveCustomEmoji(const std::string& name) const
{
	std::shared_lock lock(fLock);
	return resolveEmojiLocked(name);
}


std::string
Store::customEmojiUrl(const std::string& name) const
{
	std::shared_lock lock(fLock);
	std::string resolved = resolveEmojiLocked(name);
	auto it = fEmoji.find(resolved);
	if (it == fEmoji.end() || startsWith(it->second, "alias:"))
		return {};
	return it->second;
}

// ---- read state ------------------------------------------------------------------------

bool
Store::mentionsSelfLocked(const Message& message, const Channel* channel) const
{
	if (channel != nullptr && channel->isIm)
		return true;
	const std::string& text = message.text;
	if (!fSelfId.empty() && text.find("<@" + fSelfId) != std::string::npos)
		return true;
	return text.find("<!here") != std::string::npos
		|| text.find("<!channel") != std::string::npos
		|| text.find("<!everyone") != std::string::npos;
}


void
Store::recountLocked(Channel& channel) const
{
	if (!channel.latestTs.empty() && !channel.lastRead.empty()
		&& compareTs(channel.lastRead, channel.latestTs) >= 0) {
		channel.unreadCount = 0;
		channel.mentionCount = 0;
		channel.hasUnreads = false;
		return;
	}
	int unread = 0;
	int mentions = 0;
	auto timeline = fTimelines.find(channel.id);
	if (timeline != fTimelines.end()) {
		for (auto it = timeline->second.upper_bound(channel.lastRead);
				it != timeline->second.end(); ++it) {
			const Message& message = it->second;
			if (message.user == fSelfId || !countsAsUnread(message))
				continue;
			unread++;
			if (mentionsSelfLocked(message, &channel))
				mentions++;
		}
	}
	channel.unreadCount = unread;
	channel.mentionCount = mentions;
	channel.hasUnreads = unread > 0;
}


void
Store::applyCounts(const UnreadCounts& counts)
{
	std::unique_lock lock(fLock);
	for (const UnreadInfo& info : counts.channels) {
		auto it = fChannels.find(info.channel);
		if (it == fChannels.end())
			continue;
		Channel& channel = it->second;
		if (!info.lastRead.empty())
			channel.lastRead = info.lastRead;
		if (!info.latestTs.empty())
			channel.latestTs = info.latestTs;
		channel.unreadCount = info.unreadCount;
		channel.mentionCount = info.mentionCount;
		channel.hasUnreads = info.hasUnreads || info.unreadCount > 0;
	}
}


void
Store::markRead(const std::string& id, const std::string& ts)
{
	std::unique_lock lock(fLock);
	auto it = fChannels.find(id);
	if (it == fChannels.end())
		return;
	it->second.lastRead = ts;
	recountLocked(it->second);
}

// ---- events -------------------------------------------------------------------------------

StoreChange
Store::apply(const Event& event)
{
	std::unique_lock lock(fLock);
	StoreChange change;

	if (const auto* e = std::get_if<MessageEvent>(&event)) {
		Message message = e->message;
		if (message.channel.empty() || message.ts.empty())
			return change;
		bool isNew = false;
		if (message.isThreadReply()) {
			insertReplyLocked(message, isNew);
			change.kind = StoreChange::Thread;
			change.channel = message.channel;
			change.ts = message.ts;
			change.threadTs = message.threadTs;
			if (message.isBroadcastReply()) {
				StoreChange ignored;
				upsertMessageLocked(message, ignored);
			}
		} else {
			const Timeline& timeline = fTimelines[message.channel];
			isNew = timeline.find(message.ts) == timeline.end();
			upsertMessageLocked(message, change);
		}

		auto channel = fChannels.find(message.channel);
		if (channel != fChannels.end()) {
			Channel& c = channel->second;
			if (isNew && countsAsUnread(message) && message.user != fSelfId
				&& (c.lastRead.empty() || compareTs(message.ts, c.lastRead) > 0)) {
				c.unreadCount++;
				c.hasUnreads = true;
				change.newUnread = true;
				if (mentionsSelfLocked(message, &c)) {
					c.mentionCount++;
					change.mentionsSelf = true;
				}
			}
			if (countsAsUnread(message)
				&& (c.latestTs.empty() || compareTs(message.ts, c.latestTs) > 0))
				c.latestTs = message.ts;
		}
		return change;
	}

	if (const auto* e = std::get_if<MessageChangedEvent>(&event)) {
		Message message = e->message;
		message.channel = e->channel;
		bool found = forEachCopyLocked(e->channel, message.ts, [&message](Message& copy) {
			copy = message;
		});
		if (!found && !message.isThreadReply()) {
			// Only fill in messages inside the loaded window; older ones
			// would leave a hole in the timeline.
			auto timeline = fTimelines.find(e->channel);
			if (timeline != fTimelines.end() && !timeline->second.empty()
				&& compareTs(message.ts, timeline->second.begin()->first) > 0) {
				StoreChange ignored;
				upsertMessageLocked(message, ignored);
				found = true;
			}
		}
		if (!found)
			return change;
		change.kind = message.isThreadReply() ? StoreChange::Thread : StoreChange::Messages;
		change.channel = e->channel;
		change.ts = message.ts;
		change.threadTs = message.isThreadReply() ? message.threadTs : std::string();
		return change;
	}

	if (const auto* e = std::get_if<MessageDeletedEvent>(&event)) {
		std::string threadTs = e->threadTs;
		bool removed = false;
		auto timeline = fTimelines.find(e->channel);
		if (timeline != fTimelines.end())
			removed = timeline->second.erase(e->ts) > 0;
		auto threads = fThreads.find(e->channel);
		if (threads != fThreads.end()) {
			threads->second.erase(e->ts);   // a deleted parent takes its thread
			for (auto& [key, thread] : threads->second) {
				if (thread.erase(e->ts) > 0) {
					removed = true;
					threadTs = key;
				}
			}
		}
		if (!threadTs.empty() && threadTs != e->ts) {
			Timeline* thread = nullptr;
			if (threads != fThreads.end()) {
				auto it = threads->second.find(threadTs);
				if (it != threads->second.end())
					thread = &it->second;
			}
			forEachCopyLocked(e->channel, threadTs, [&](Message& parent) {
				if (parent.ts != threadTs)
					return;
				if (parent.replyCount > 0)
					parent.replyCount--;
				if (thread != nullptr) {
					parent.latestReply.clear();
					for (auto it = thread->rbegin(); it != thread->rend(); ++it) {
						if (it->first != threadTs) {
							parent.latestReply = it->first;
							break;
						}
					}
				}
			});
			removed = true;
		}
		if (!removed)
			return change;
		change.kind = threadTs.empty() || threadTs == e->ts ? StoreChange::Messages
			: StoreChange::Thread;
		change.channel = e->channel;
		change.ts = e->ts;
		change.threadTs = threadTs;
		return change;
	}

	if (const auto* e = std::get_if<ReactionEvent>(&event)) {
		bool found = forEachCopyLocked(e->channel, e->ts, [e](Message& message) {
			auto it = std::find_if(message.reactions.begin(), message.reactions.end(),
				[e](const Reaction& r) { return r.name == e->reaction; });
			if (e->added) {
				if (it == message.reactions.end()) {
					message.reactions.push_back(Reaction{e->reaction, 1, {e->user}});
				} else if (std::find(it->users.begin(), it->users.end(), e->user)
						== it->users.end()) {
					it->users.push_back(e->user);
					it->count++;
				}
				return;
			}
			if (it == message.reactions.end())
				return;
			auto user = std::find(it->users.begin(), it->users.end(), e->user);
			if (user != it->users.end()) {
				it->users.erase(user);
				it->count--;
			} else if (it->count > static_cast<int>(it->users.size())) {
				it->count--;   // the users list was truncated
			}
			if (it->count <= 0)
				message.reactions.erase(it);
		});
		if (!found)
			return change;
		auto timeline = fTimelines.find(e->channel);
		bool inTimeline = timeline != fTimelines.end() && timeline->second.count(e->ts) != 0;
		change.kind = inTimeline ? StoreChange::Messages : StoreChange::Thread;
		change.channel = e->channel;
		change.ts = e->ts;
		return change;
	}

	if (const auto* e = std::get_if<MarkedEvent>(&event)) {
		auto it = fChannels.find(e->channel);
		if (it == fChannels.end())
			return change;
		Channel& channel = it->second;
		channel.lastRead = e->ts;
		recountLocked(channel);
		if (e->unreadCount >= 0) {
			channel.unreadCount = e->unreadCount;
			channel.hasUnreads = e->unreadCount > 0;
		}
		if (e->mentionCount >= 0)
			channel.mentionCount = e->mentionCount;
		if (channel.unreadCount == 0)
			channel.mentionCount = 0;
		change.kind = StoreChange::ReadState;
		change.channel = e->channel;
		change.ts = e->ts;
		return change;
	}

	if (const auto* e = std::get_if<CountsEvent>(&event)) {
		lock.unlock();
		applyCounts(e->counts);
		change.kind = StoreChange::ReadState;
		return change;
	}

	if (const auto* e = std::get_if<ChannelJoinedEvent>(&event)) {
		Channel channel = e->channel;
		channel.isMember = true;
		auto old = fChannels.find(channel.id);
		if (old != fChannels.end()) {
			Channel merged = mergeChannelData(old->second, channel);
			if (channel.name.empty()) {
				// A bare id: keep what we knew and only flip membership.
				merged = old->second;
				merged.isMember = true;
			}
			old->second = merged;
		} else {
			fChannels[channel.id] = channel;
		}
		change.kind = StoreChange::Channels;
		change.channel = channel.id;
		return change;
	}

	if (const auto* e = std::get_if<ChannelLeftEvent>(&event)) {
		auto it = fChannels.find(e->channel);
		if (it == fChannels.end())
			return change;
		it->second.isMember = false;
		change.kind = StoreChange::Channels;
		change.channel = e->channel;
		return change;
	}

	if (const auto* e = std::get_if<ChannelUpdatedEvent>(&event)) {
		const Channel& update = e->channel;
		auto it = fChannels.find(update.id);
		if (it == fChannels.end()) {
			if (e->type == "channel_created" && !update.name.empty()) {
				Channel channel = update;
				channel.isChannel = true;
				fChannels[update.id] = channel;
			} else {
				return change;
			}
		} else {
			Channel& channel = it->second;
			if (e->type == "channel_rename" || e->type == "group_rename") {
				if (!update.name.empty())
					channel.name = update.name;
				if (!update.nameNormalized.empty())
					channel.nameNormalized = update.nameNormalized;
			} else if (e->type == "channel_archive" || e->type == "group_archive") {
				channel.isArchived = true;
			} else if (e->type == "channel_unarchive" || e->type == "group_unarchive") {
				channel.isArchived = false;
			}
		}
		change.kind = StoreChange::Channels;
		change.channel = update.id;
		return change;
	}

	if (const auto* e = std::get_if<MemberJoinedChannelEvent>(&event)) {
		auto it = fChannels.find(e->channel);
		if (it == fChannels.end())
			return change;
		Channel& channel = it->second;
		if (e->user == fSelfId)
			channel.isMember = e->joined;
		if (channel.numMembers > 0 || e->joined)
			channel.numMembers = std::max(0, channel.numMembers + (e->joined ? 1 : -1));
		change.kind = StoreChange::Channels;
		change.channel = e->channel;
		return change;
	}

	if (const auto* e = std::get_if<UserChangeEvent>(&event)) {
		User user = e->user;
		auto old = fUsers.find(user.id);
		if (old != fUsers.end() && user.presence.empty())
			user.presence = old->second.presence;
		fUsers[user.id] = user;
		change.kind = StoreChange::Users;
		change.id = user.id;
		return change;
	}

	if (const auto* e = std::get_if<PresenceEvent>(&event)) {
		for (const std::string& id : e->users) {
			auto it = fUsers.find(id);
			if (it != fUsers.end())
				it->second.presence = e->presence;
		}
		change.kind = StoreChange::Users;
		if (e->users.size() == 1)
			change.id = e->users.front();
		return change;
	}

	if (const auto* e = std::get_if<EmojiChangedEvent>(&event)) {
		if (e->subtype == "add") {
			fEmoji[e->name] = e->value;
		} else if (e->subtype == "remove") {
			for (const std::string& name : e->names)
				fEmoji.erase(name);
		} else if (e->subtype == "rename") {
			auto it = fEmoji.find(e->oldName);
			std::string value = e->value;
			if (it != fEmoji.end()) {
				if (value.empty())
					value = it->second;
				fEmoji.erase(it);
			}
			fEmoji[e->name] = value;
		}
		change.kind = StoreChange::Emoji;
		change.id = e->name;
		return change;
	}

	return change;
}

// ---- formatting --------------------------------------------------------------------------

FormatContext
Store::formatContext() const
{
	FormatContext context;
	context.selfUserId = selfUserId();
	context.userName = [this](const std::string& id) {
		std::shared_lock lock(fLock);
		auto it = fUsers.find(id);
		return it == fUsers.end() ? std::string() : it->second.bestName();
	};
	context.channelName = [this](const std::string& id) {
		std::shared_lock lock(fLock);
		auto it = fChannels.find(id);
		if (it == fChannels.end())
			return std::string();
		if (it->second.isIm)
			return userNameLocked(it->second.imUser);
		return it->second.name;
	};
	context.customEmoji = [this](const std::string& name) {
		return resolveCustomEmoji(name);
	};
	return context;
}

EncodeContext
Store::encodeContext() const
{
	EncodeContext context;
	context.matchUser = [this](std::string_view text) {
		auto lower = [](std::string_view value) {
			std::string out(value);
			for (char& c : out)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return out;
		};
		std::string rest = lower(text.substr(0, 128));
		std::pair<std::string, size_t> best{ {}, 0 };
		std::shared_lock lock(fLock);
		for (const auto& [id, user] : fUsers) {
			if (user.deleted)
				continue;
			for (const std::string* name : { &user.displayName, &user.realName, &user.name }) {
				if (name->empty() || name->size() <= best.second || !startsWith(rest, lower(*name)))
					continue;
				best = { id, name->size() };
			}
		}
		return best;
	};
	context.channelId = [this](const std::string& name) {
		std::shared_lock lock(fLock);
		for (const auto& [id, channel] : fChannels) {
			if (!channel.isIm && !channel.isMpim && !channel.name.empty()
					&& (channel.name == name || channel.nameNormalized == name))
				return id;
		}
		return std::string();
	};
	return context;
}

// ---- cache ---------------------------------------------------------------------------------

Status
Store::saveCache(const std::string& directory, size_t messagesPerConversation) const
{
	if (!makeDirectories(directory))
		return Error(ErrorKind::InvalidArgument, "cannot_create", directory);

	json meta;
	json users = json::array();
	json channels = json::array();
	json emoji = json::object();
	json messages = json::object();
	{
		std::shared_lock lock(fLock);
		meta = {{"version", kCacheVersion}, {"self_id", fSelfId}, {"team_id", fTeamId},
			{"team", fTeam.toJson()}};
		for (const auto& [id, user] : fUsers)
			users.push_back(user.toJson());
		for (const auto& [id, channel] : fChannels)
			channels.push_back(channel.toJson());
		for (const auto& [name, value] : fEmoji)
			emoji[name] = value;
		for (const auto& [id, timeline] : fTimelines) {
			if (timeline.empty() || messagesPerConversation == 0)
				continue;
			json list = json::array();
			size_t skip = timeline.size() > messagesPerConversation
				? timeline.size() - messagesPerConversation : 0;
			size_t index = 0;
			for (const auto& [ts, message] : timeline) {
				if (index++ < skip)
					continue;
				list.push_back(message.toJson());
			}
			messages[id] = list;
		}
	}

	std::string base = directory;
	if (!endsWith(base, "/"))
		base += '/';
	for (const auto& [name, value] : std::vector<std::pair<std::string, const json*>>{
			{"meta.json", &meta}, {"users.json", &users}, {"channels.json", &channels},
			{"emoji.json", &emoji}, {"messages.json", &messages}}) {
		Status status = writeFileAtomically(base + name, value->dump());
		if (!status)
			return status;
	}
	return {};
}


Status
Store::loadCache(const std::string& directory)
{
	std::string base = directory;
	if (!endsWith(base, "/"))
		base += '/';
	json meta = readJsonFile(base + "meta.json");
	if (!meta.is_object())
		return {};   // nothing cached yet
	if (jInt(meta, "version") != kCacheVersion)
		return Error(ErrorKind::Parse, "cache_version", "ignoring an old cache");

	json users = readJsonFile(base + "users.json");
	json channels = readJsonFile(base + "channels.json");
	json emoji = readJsonFile(base + "emoji.json");
	json messages = readJsonFile(base + "messages.json");

	std::unique_lock lock(fLock);
	fSelfId = jStr(meta, "self_id");
	fTeamId = jStr(meta, "team_id");
	fTeam = natter::Team::fromJson(jObj(meta, "team"));
	if (users.is_array()) {
		fUsers.clear();
		for (const json& item : users) {
			User user = User::fromJson(item);
			if (!user.id.empty())
				fUsers[user.id] = user;
		}
	}
	if (channels.is_array()) {
		fChannels.clear();
		for (const json& item : channels) {
			Channel channel = Channel::fromJson(item);
			if (!channel.id.empty())
				fChannels[channel.id] = channel;
		}
	}
	if (emoji.is_object()) {
		fEmoji.clear();
		for (auto it = emoji.begin(); it != emoji.end(); ++it) {
			if (it->is_string())
				fEmoji[it.key()] = it->get<std::string>();
		}
	}
	if (messages.is_object()) {
		fTimelines.clear();
		for (auto it = messages.begin(); it != messages.end(); ++it) {
			Timeline& timeline = fTimelines[it.key()];
			for (const json& item : it.value()) {
				Message message = Message::fromJson(item, it.key());
				if (!message.ts.empty()) {
					std::string ts = message.ts;
					timeline[ts] = std::move(message);
				}
			}
		}
	}
	return {};
}


void
Store::clear()
{
	std::unique_lock lock(fLock);
	fSelfId.clear();
	fTeamId.clear();
	fTeam = natter::Team();
	fUsers.clear();
	fChannels.clear();
	fTimelines.clear();
	fThreads.clear();
	fEmoji.clear();
}

}  // namespace natter
