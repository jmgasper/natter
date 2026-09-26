// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "natter/util.h"

namespace natter {

// Every model parses from the JSON Slack sends (fromJson) and writes back the
// same shape (toJson), so the disk cache reuses the API parsers.

struct Team {
	std::string id;
	std::string name;
	std::string domain;
	std::string url;
	std::string enterpriseId;
	std::string iconUrl;   // the largest of image_132/88/68/44

	static Team fromJson(const json& j);
	json toJson() const;
};

struct User {
	std::string id;
	std::string teamId;
	std::string name;          // the legacy handle
	std::string realName;
	std::string displayName;
	std::string title;
	std::string color;
	std::string tz;
	int64_t tzOffset = 0;
	bool deleted = false;
	bool isBot = false;
	bool isAppUser = false;
	std::string botId;         // profile.bot_id for bot users
	std::string statusText;
	std::string statusEmoji;
	std::string presence;      // "active"/"away"/"" when unknown
	// profile.image_24 .. image_512 and image_original, keyed by size.
	std::map<int, std::string> avatars;

	// display_name, else real_name, else name, else the id.
	std::string bestName() const;
	// The smallest avatar at least minSize pixels wide (or the largest).
	std::string avatarUrl(int minSize = 48) const;

	static User fromJson(const json& j);
	json toJson() const;
};

struct Reaction {
	std::string name;                 // "thumbsup", "+1::skin-tone-2", custom
	int count = 0;
	std::vector<std::string> users;   // may be a prefix of all reactors

	static Reaction fromJson(const json& j);
	json toJson() const;
};

struct File {
	std::string id;
	std::string name;
	std::string title;
	std::string mimetype;
	std::string filetype;
	std::string prettyType;
	int64_t size = 0;
	std::string urlPrivate;
	std::string urlPrivateDownload;
	std::string permalink;
	std::string mode;                 // "hosted", "external", "tombstone", ...
	int originalWidth = 0;
	int originalHeight = 0;
	std::map<int, std::string> thumbs;   // thumb_64 .. thumb_1024

	bool isImage() const { return mimetype.rfind("image/", 0) == 0; }
	std::string thumbUrl(int minSize = 360) const;

	static File fromJson(const json& j);
	json toJson() const;
};

struct Edited {
	std::string user;
	std::string ts;
};

struct Message {
	std::string channel;       // filled in by the client when Slack omits it
	std::string ts;
	std::string user;
	std::string botId;
	std::string username;      // bot or integration display name
	std::string text;
	std::string subtype;
	std::string threadTs;
	std::string parentUserId;
	int replyCount = 0;
	int replyUsersCount = 0;
	std::vector<std::string> replyUsers;
	std::string latestReply;
	std::optional<Edited> edited;
	std::vector<Reaction> reactions;
	std::vector<File> files;
	json attachments;          // raw, null when absent
	json blocks;               // raw, null when absent
	json botProfile;           // raw bot_profile, null when absent
	bool hidden = false;
	bool isStarred = false;
	std::string permalink;     // only from search results

	// A reply inside a thread (thread_ts set and different from ts).
	bool isThreadReply() const { return !threadTs.empty() && threadTs != ts; }
	// The first message of a thread that has replies.
	bool isThreadParent() const
	{
		return (!threadTs.empty() && threadTs == ts) || replyCount > 0;
	}
	// Replies with subtype thread_broadcast also show in the channel.
	bool isBroadcastReply() const { return subtype == "thread_broadcast"; }

	static Message fromJson(const json& j, const std::string& channel = {});
	json toJson() const;
};

struct Topic {
	std::string value;
	std::string creator;
	int64_t lastSet = 0;
};

enum class ChannelKind { Public, Private, Im, Mpim };

struct Channel {
	std::string id;
	std::string name;
	std::string nameNormalized;
	bool isChannel = false;
	bool isGroup = false;
	bool isIm = false;
	bool isMpim = false;
	bool isPrivate = false;
	bool isMember = false;
	bool isArchived = false;
	bool isGeneral = false;
	bool isExtShared = false;
	std::string imUser;        // the other user of a direct message
	Topic topic;
	Topic purpose;
	int64_t created = 0;
	int numMembers = 0;
	std::string lastRead;      // ts of the last message marked read
	std::string latestTs;      // ts of the newest message, when known
	int unreadCount = 0;       // unread_count_display
	int mentionCount = 0;      // mentions (DMs count every message)
	bool hasUnreads = false;
	int64_t priority = 0;

	ChannelKind kind() const;
	bool isDirect() const { return isIm || isMpim; }

	static Channel fromJson(const json& j);
	json toJson() const;
};

struct AuthInfo {
	std::string url;
	std::string team;
	std::string user;
	std::string teamId;
	std::string userId;
	std::string botId;
	std::string enterpriseId;
	bool isEnterpriseInstall = false;

	static AuthInfo fromJson(const json& j);
};

struct Presence {
	std::string presence;   // "active" or "away"
	bool online = false;
	bool autoAway = false;
	bool manualAway = false;
	int64_t lastActivity = 0;
	int connectionCount = 0;

	static Presence fromJson(const json& j);
};

// One page of conversations.history or conversations.replies.
struct HistoryPage {
	std::vector<Message> messages;   // newest first for history, oldest first
	                                 // for replies - as Slack returns them
	bool hasMore = false;
	std::string nextCursor;
};

struct SearchMatch {
	Message message;          // message.channel and message.permalink set
	std::string channelName;
	std::string teamId;
};

struct SearchPage {
	int total = 0;
	int page = 1;
	int pages = 0;
	std::vector<SearchMatch> matches;
};

// Where an unread figure came from; client.counts only knows "some unread"
// for channels, while DMs and mentions are exact.
enum class CountsSource { ClientCounts, UsersCounts, ConversationsInfo };

struct UnreadInfo {
	std::string channel;
	std::string lastRead;
	std::string latestTs;
	int unreadCount = 0;
	int mentionCount = 0;
	bool hasUnreads = false;
};

struct UnreadCounts {
	CountsSource source = CountsSource::ClientCounts;
	std::vector<UnreadInfo> channels;
};

struct UploadedFile {
	std::string id;
	std::string title;
	json raw;                 // the files[] entry from completeUploadExternal
};

struct ConnectInfo {
	std::string url;          // wss:// URL to open
	std::string selfId;       // rtm.connect self.id
	std::string selfName;
	std::string teamId;
	std::string teamDomain;
};

}  // namespace natter
