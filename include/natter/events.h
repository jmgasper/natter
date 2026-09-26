// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "natter/models.h"

namespace natter {

// Where an event came from.
enum class EventSource { Rtm, SocketMode, Polling, Local };

struct HelloEvent {};

// The server is about to close the socket; a reconnect follows.
struct GoodbyeEvent {};

// A new message, including thread replies (message.threadTs set and not equal
// to message.ts) and subtypes such as bot_message, me_message, file_share,
// channel_join and thread_broadcast.
struct MessageEvent {
	Message message;
};

// subtype message_changed (and message_replied, which carries the updated
// thread root).
struct MessageChangedEvent {
	std::string channel;
	Message message;
	std::optional<Message> previous;
};

// subtype message_deleted.
struct MessageDeletedEvent {
	std::string channel;
	std::string ts;
	std::string threadTs;    // from previous_message, when it was a reply
};

struct ReactionEvent {
	bool added = true;        // reaction_added or reaction_removed
	std::string user;
	std::string reaction;     // without colons, may carry "::skin-tone-N"
	std::string channel;
	std::string ts;           // the message's ts
	std::string itemUser;
	std::string eventTs;
};

struct TypingEvent {
	std::string channel;
	std::string user;
	std::string threadTs;
};

struct PresenceEvent {
	std::vector<std::string> users;
	std::string presence;     // "active" or "away"
};

// channel_marked, group_marked, im_marked, mpim_marked: read state moved,
// usually because the user read the conversation elsewhere.
struct MarkedEvent {
	std::string channel;
	std::string ts;
	std::string kind;         // "channel", "group", "im", "mpim"
	int unreadCount = -1;     // unread_count_display, -1 when absent
	int mentionCount = -1;    // num_mentions_display, -1 when absent
};

// channel_joined/group_joined carry the whole channel; *_left only the id.
struct ChannelJoinedEvent {
	Channel channel;
};

struct ChannelLeftEvent {
	std::string channel;
};

// Also channel_created/rename/archive, which update what we know.
struct ChannelUpdatedEvent {
	std::string type;         // the raw event type
	Channel channel;          // partial: at least the id
};

struct MemberJoinedChannelEvent {
	bool joined = true;       // member_joined_channel or member_left_channel
	std::string user;
	std::string channel;
	std::string channelType;  // "C" or "G"
	std::string inviter;
};

struct UserChangeEvent {
	User user;
};

struct EmojiChangedEvent {
	std::string subtype;      // "add", "remove", "rename"
	std::string name;         // add: the new name; rename: the new name
	std::string oldName;      // rename
	std::string value;        // add/rename: URL or "alias:..."
	std::vector<std::string> names;   // remove
};

// Unread figures from a counts poll (the polling fallback, or Session).
struct CountsEvent {
	UnreadCounts counts;
};

enum class ConnectionState { Disconnected, Connecting, Connected, Polling };

const char* connectionStateName(ConnectionState state);

// Synthesised by the real-time layer, never sent by Slack.
struct ConnectionEvent {
	ConnectionState state = ConnectionState::Disconnected;
	EventSource source = EventSource::Local;
	std::string detail;       // error or reason
	int attempt = 0;          // reconnect attempt number
	int retryInSeconds = 0;   // when Disconnected and a retry is scheduled
};

// Anything we do not model; `type` and the raw payload are kept.
struct UnknownEvent {
	std::string type;
	std::string subtype;
	json raw;
};

using Event = std::variant<HelloEvent, GoodbyeEvent, MessageEvent,
	MessageChangedEvent, MessageDeletedEvent, ReactionEvent, TypingEvent,
	PresenceEvent, MarkedEvent, ChannelJoinedEvent, ChannelLeftEvent,
	ChannelUpdatedEvent, MemberJoinedChannelEvent, UserChangeEvent,
	EmojiChangedEvent, CountsEvent, ConnectionEvent, UnknownEvent>;

// The name of the alternative ("message", "reaction", ...), for logs.
const char* eventName(const Event& event);

// Turn one RTM frame or Events API "event" object into an Event. Returns
// nothing for frames that are not events (replies to our pings, acks).
std::optional<Event> parseEvent(const json& payload);

// One line of text describing the event (natter-cli watch uses it).
std::string describeEvent(const Event& event);

}  // namespace natter
