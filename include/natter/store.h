// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "natter/events.h"
#include "natter/models.h"
#include "natter/mrkdwn.h"
#include "natter/result.h"

namespace natter {

// What an update touched, so a UI can refresh only that.
struct StoreChange {
	enum Kind {
		None,
		Team,
		Users,        // users (and presence); `id` is set for a single user
		Channels,     // the channel list or one channel (`channel`)
		Messages,     // a conversation's timeline (`channel`, `ts`)
		Thread,       // a thread (`channel`, `threadTs`, `ts`)
		ReadState,    // unread counts / last_read of `channel` (or all)
		Emoji,
	};
	Kind kind = None;
	std::string channel;
	std::string ts;
	std::string threadTs;
	std::string id;
	// A new message counted as unread (for notifications).
	bool newUnread = false;
	bool mentionsSelf = false;
};

// In-memory model of one workspace. Every method is thread-safe (a
// reader/writer lock); getters return copies, so callers never hold
// references into the store.
class Store {
public:
	Store() = default;

	// ---- identity ----------------------------------------------------------------
	void setSelf(const std::string& userId, const std::string& teamId = {});
	std::string selfUserId() const;
	void setTeam(const Team& team);
	natter::Team team() const;

	// ---- users -------------------------------------------------------------------
	void setUsers(const std::vector<User>& users);     // replaces the set
	void upsertUser(const User& user);
	std::optional<User> user(const std::string& id) const;
	std::vector<User> users() const;
	// Best display name, or the id when unknown.
	std::string userName(const std::string& id) const;
	void setPresence(const std::vector<std::string>& ids, const std::string& presence);

	// ---- channels -------------------------------------------------------------------
	// Replaces the list; read state already known is kept when the new data
	// lacks it (conversations.list carries no unread figures).
	void setChannels(const std::vector<Channel>& channels);
	void upsertChannel(const Channel& channel);
	void removeChannel(const std::string& id);
	std::optional<Channel> channel(const std::string& id) const;
	std::vector<Channel> channels() const;
	// "general", "#general" or an id; IMs match the other user's name.
	std::optional<Channel> findChannel(const std::string& nameOrId) const;
	// "#general", "@alice", or "alice, bob" for group DMs.
	std::string channelDisplayName(const std::string& id) const;

	// ---- messages ------------------------------------------------------------------------
	// Insert or replace by ts. History pages and thread replies alike.
	void mergeHistory(const std::string& channel, const std::vector<Message>& messages);
	// A thread from conversations.replies (the parent is the first message).
	void mergeReplies(const std::string& channel, const std::string& threadTs,
		const std::vector<Message>& messages);
	// Oldest first; `limit` > 0 keeps only the newest `limit`.
	std::vector<Message> messages(const std::string& channel, size_t limit = 0) const;
	// Up to `limit` messages older than `ts`, oldest first (scroll back).
	std::vector<Message> messagesBefore(const std::string& channel,
		const std::string& ts, size_t limit) const;
	std::optional<Message> message(const std::string& channel, const std::string& ts) const;
	// The parent followed by its replies, oldest first.
	std::vector<Message> thread(const std::string& channel, const std::string& threadTs) const;
	std::string newestTs(const std::string& channel) const;
	std::string oldestTs(const std::string& channel) const;
	void setMaxMessagesPerConversation(size_t count);

	// ---- emoji ---------------------------------------------------------------------------
	void setEmoji(const std::map<std::string, std::string>& emoji);
	std::map<std::string, std::string> emoji() const;
	// Follow "alias:" chains: the final name (a standard emoji name when an
	// alias points at one), or "" when the workspace has no such emoji.
	std::string resolveCustomEmoji(const std::string& name) const;
	// Image URL of a custom emoji (aliases followed), or "".
	std::string customEmojiUrl(const std::string& name) const;

	// ---- read state -------------------------------------------------------------------------
	void applyCounts(const UnreadCounts& counts);
	// Local mark (after conversations.mark succeeded).
	void markRead(const std::string& channel, const std::string& ts);

	// ---- events ---------------------------------------------------------------------------------
	// Merge a real-time event. Idempotent for duplicates (RTM and polling
	// may report the same message).
	StoreChange apply(const Event& event);

	// ---- formatting ----------------------------------------------------------------------------
	// Lookups for formatMrkdwn/formatMessage bound to this store. The store
	// must outlive the returned context.
	FormatContext formatContext() const;

	// ---- disk cache ----------------------------------------------------------------------------
	// Write users, channels, emoji, team and the newest `messagesPerConversation`
	// messages of each conversation as JSON files under `directory`
	// (created if missing).
	Status saveCache(const std::string& directory, size_t messagesPerConversation = 50) const;
	// Load what saveCache wrote; missing files are not an error.
	Status loadCache(const std::string& directory);
	void clear();

private:
	using Timeline = std::map<std::string, Message, TsLess>;

	void upsertMessageLocked(Message message, StoreChange& change);
	void insertReplyLocked(const Message& reply, bool& isNew);
	template <typename Fn> bool forEachCopyLocked(const std::string& channel,
		const std::string& ts, Fn&& fn);
	void recountLocked(Channel& channel) const;
	bool mentionsSelfLocked(const Message& message, const Channel* channel) const;
	void trimLocked(Timeline& timeline);
	std::string userNameLocked(const std::string& id) const;
	std::string resolveEmojiLocked(const std::string& name) const;

	mutable std::shared_mutex fLock;
	std::string fSelfId;
	std::string fTeamId;
	natter::Team fTeam;
	std::unordered_map<std::string, User> fUsers;
	std::unordered_map<std::string, Channel> fChannels;
	std::unordered_map<std::string, Timeline> fTimelines;
	// channel -> thread ts -> parent and replies
	std::unordered_map<std::string, std::unordered_map<std::string, Timeline>> fThreads;
	std::map<std::string, std::string> fEmoji;
	size_t fMaxMessages = 5000;
};

}  // namespace natter
