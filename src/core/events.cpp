// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT

#include "natter/events.h"

namespace natter {

const char*
connectionStateName(ConnectionState state)
{
	switch (state) {
		case ConnectionState::Disconnected: return "disconnected";
		case ConnectionState::Connecting: return "connecting";
		case ConnectionState::Connected: return "connected";
		case ConnectionState::Polling: return "polling";
	}
	return "?";
}


namespace {

struct NameVisitor {
	const char* operator()(const HelloEvent&) const { return "hello"; }
	const char* operator()(const GoodbyeEvent&) const { return "goodbye"; }
	const char* operator()(const MessageEvent&) const { return "message"; }
	const char* operator()(const MessageChangedEvent&) const { return "message_changed"; }
	const char* operator()(const MessageDeletedEvent&) const { return "message_deleted"; }
	const char* operator()(const ReactionEvent& e) const
	{
		return e.added ? "reaction_added" : "reaction_removed";
	}
	const char* operator()(const TypingEvent&) const { return "user_typing"; }
	const char* operator()(const PresenceEvent&) const { return "presence_change"; }
	const char* operator()(const MarkedEvent&) const { return "marked"; }
	const char* operator()(const ChannelJoinedEvent&) const { return "channel_joined"; }
	const char* operator()(const ChannelLeftEvent&) const { return "channel_left"; }
	const char* operator()(const ChannelUpdatedEvent&) const { return "channel_updated"; }
	const char* operator()(const MemberJoinedChannelEvent& e) const
	{
		return e.joined ? "member_joined_channel" : "member_left_channel";
	}
	const char* operator()(const UserChangeEvent&) const { return "user_change"; }
	const char* operator()(const EmojiChangedEvent&) const { return "emoji_changed"; }
	const char* operator()(const CountsEvent&) const { return "counts"; }
	const char* operator()(const ConnectionEvent&) const { return "connection"; }
	const char* operator()(const UnknownEvent&) const { return "unknown"; }
};


std::optional<Event>
parseMessage(const json& payload)
{
	std::string subtype = jStr(payload, "subtype");
	std::string channel = jStr(payload, "channel");

	if (subtype == "message_changed" || subtype == "message_replied") {
		MessageChangedEvent event;
		event.channel = channel;
		event.message = Message::fromJson(jObj(payload, "message"), channel);
		event.message.channel = channel;
		const json& previous = jObj(payload, "previous_message");
		if (previous.is_object()) {
			event.previous = Message::fromJson(previous, channel);
			event.previous->channel = channel;
		}
		if (event.message.ts.empty())
			return std::nullopt;
		return Event(std::move(event));
	}
	if (subtype == "message_deleted") {
		MessageDeletedEvent event;
		event.channel = channel;
		event.ts = jStr(payload, "deleted_ts");
		const json& previous = jObj(payload, "previous_message");
		if (event.ts.empty())
			event.ts = jStr(previous, "ts");
		std::string threadTs = jStr(previous, "thread_ts");
		if (threadTs != event.ts)
			event.threadTs = threadTs;
		if (event.ts.empty())
			return std::nullopt;
		return Event(std::move(event));
	}

	MessageEvent event;
	event.message = Message::fromJson(payload, channel);
	// Our own echo of a chat.postMessage arrives without "hidden"; RTM also
	// sends hidden bookkeeping messages we should not show.
	if (event.message.ts.empty())
		return std::nullopt;
	return Event(std::move(event));
}


std::string
channelOf(const json& payload)
{
	const json& value = payload.contains("channel") ? payload["channel"] : json();
	if (value.is_string())
		return value.get<std::string>();
	if (value.is_object())
		return jStr(value, "id");
	return jStr(payload, "channel_id");
}

}  // namespace


const char*
eventName(const Event& event)
{
	return std::visit(NameVisitor(), event);
}


std::optional<Event>
parseEvent(const json& payload)
{
	if (!payload.is_object())
		return std::nullopt;
	// Replies to our own RTM messages ({"ok":true,"reply_to":1}) and pongs.
	if (payload.contains("reply_to") && !payload.contains("type"))
		return std::nullopt;

	std::string type = jStr(payload, "type");
	if (type.empty() || type == "pong")
		return std::nullopt;

	if (type == "hello")
		return Event(HelloEvent());
	if (type == "goodbye")
		return Event(GoodbyeEvent());
	if (type == "message")
		return parseMessage(payload);

	if (type == "reaction_added" || type == "reaction_removed") {
		ReactionEvent event;
		event.added = type == "reaction_added";
		event.user = jStr(payload, "user");
		event.reaction = jStr(payload, "reaction");
		const json& item = jObj(payload, "item");
		event.channel = jStr(item, "channel");
		event.ts = jStr(item, "ts");
		event.itemUser = jStr(payload, "item_user");
		event.eventTs = jStr(payload, "event_ts");
		if (jStr(item, "type", "message") != "message")
			return Event(UnknownEvent{type, jStr(item, "type"), payload});
		return Event(std::move(event));
	}
	if (type == "user_typing" || type == "typing") {
		TypingEvent event;
		event.channel = channelOf(payload);
		event.user = jStr(payload, "user");
		event.threadTs = jStr(payload, "thread_ts");
		return Event(std::move(event));
	}
	if (type == "presence_change" || type == "manual_presence_change") {
		PresenceEvent event;
		event.presence = jStr(payload, "presence");
		std::string user = jStr(payload, "user");
		if (!user.empty())
			event.users.push_back(user);
		for (const json& item : jArr(payload, "users")) {
			if (item.is_string())
				event.users.push_back(item.get<std::string>());
		}
		return Event(std::move(event));
	}
	if (type == "channel_marked" || type == "group_marked" || type == "im_marked"
		|| type == "mpim_marked") {
		MarkedEvent event;
		event.channel = channelOf(payload);
		event.ts = jStr(payload, "ts");
		event.kind = type.substr(0, type.find('_'));
		if (jHas(payload, "unread_count_display"))
			event.unreadCount = static_cast<int>(jInt(payload, "unread_count_display"));
		else if (jHas(payload, "unread_count"))
			event.unreadCount = static_cast<int>(jInt(payload, "unread_count"));
		if (jHas(payload, "num_mentions_display"))
			event.mentionCount = static_cast<int>(jInt(payload, "num_mentions_display"));
		else if (jHas(payload, "mention_count_display"))
			event.mentionCount = static_cast<int>(jInt(payload, "mention_count_display"));
		return Event(std::move(event));
	}
	if (type == "channel_joined" || type == "group_joined" || type == "im_created"
		|| type == "mpim_joined") {
		ChannelJoinedEvent event;
		const json& channel = jObj(payload, "channel");
		if (channel.is_object())
			event.channel = Channel::fromJson(channel);
		else
			event.channel.id = channelOf(payload);
		event.channel.isMember = true;
		if (type == "group_joined" && !event.channel.isMpim)
			event.channel.isPrivate = true;
		if (type == "im_created")
			event.channel.isIm = true;
		if (type == "mpim_joined")
			event.channel.isMpim = true;
		return Event(std::move(event));
	}
	if (type == "channel_left" || type == "group_left" || type == "im_close"
		|| type == "channel_deleted" || type == "group_close") {
		return Event(ChannelLeftEvent{channelOf(payload)});
	}
	if (type == "channel_created" || type == "channel_rename"
		|| type == "group_rename" || type == "channel_archive"
		|| type == "group_archive" || type == "channel_unarchive"
		|| type == "group_unarchive") {
		ChannelUpdatedEvent event;
		event.type = type;
		const json& channel = jObj(payload, "channel");
		if (channel.is_object())
			event.channel = Channel::fromJson(channel);
		event.channel.id = channelOf(payload);
		return Event(std::move(event));
	}
	if (type == "member_joined_channel" || type == "member_left_channel") {
		MemberJoinedChannelEvent event;
		event.joined = type == "member_joined_channel";
		event.user = jStr(payload, "user");
		event.channel = channelOf(payload);
		event.channelType = jStr(payload, "channel_type");
		event.inviter = jStr(payload, "inviter");
		return Event(std::move(event));
	}
	if (type == "user_change" || type == "team_join" || type == "user_profile_changed") {
		UserChangeEvent event;
		event.user = User::fromJson(jObj(payload, "user"));
		if (event.user.id.empty())
			return std::nullopt;
		return Event(std::move(event));
	}
	if (type == "emoji_changed") {
		EmojiChangedEvent event;
		event.subtype = jStr(payload, "subtype");
		event.name = jStr(payload, "name");
		event.value = jStr(payload, "value");
		event.oldName = jStr(payload, "old_name");
		if (event.subtype == "rename")
			event.name = jStr(payload, "new_name", event.name);
		for (const json& item : jArr(payload, "names")) {
			if (item.is_string())
				event.names.push_back(item.get<std::string>());
		}
		return Event(std::move(event));
	}
	return Event(UnknownEvent{type, jStr(payload, "subtype"), payload});
}


namespace {

std::string
shortText(const std::string& text)
{
	std::string out;
	for (char c : text) {
		out += c == '\n' ? ' ' : c;
		if (out.size() >= 160) {
			out += "...";
			break;
		}
	}
	return out;
}


struct DescribeVisitor {
	std::string operator()(const HelloEvent&) const { return "hello"; }
	std::string operator()(const GoodbyeEvent&) const { return "goodbye"; }
	std::string operator()(const MessageEvent& e) const
	{
		const Message& m = e.message;
		std::string text = "message " + m.channel + " " + m.ts;
		if (m.isThreadReply())
			text += " thread=" + m.threadTs;
		if (!m.subtype.empty())
			text += " subtype=" + m.subtype;
		text += " <" + (m.user.empty() ? m.username : m.user) + "> " + shortText(m.text);
		return text;
	}
	std::string operator()(const MessageChangedEvent& e) const
	{
		return "message_changed " + e.channel + " " + e.message.ts + " "
			+ shortText(e.message.text);
	}
	std::string operator()(const MessageDeletedEvent& e) const
	{
		return "message_deleted " + e.channel + " " + e.ts;
	}
	std::string operator()(const ReactionEvent& e) const
	{
		return std::string(e.added ? "reaction_added " : "reaction_removed ")
			+ e.channel + " " + e.ts + " :" + e.reaction + ": by " + e.user;
	}
	std::string operator()(const TypingEvent& e) const
	{
		return "user_typing " + e.channel + " " + e.user;
	}
	std::string operator()(const PresenceEvent& e) const
	{
		std::string text = "presence_change " + e.presence;
		for (const std::string& user : e.users)
			text += " " + user;
		return text;
	}
	std::string operator()(const MarkedEvent& e) const
	{
		return e.kind + "_marked " + e.channel + " " + e.ts;
	}
	std::string operator()(const ChannelJoinedEvent& e) const
	{
		return "channel_joined " + e.channel.id + " " + e.channel.name;
	}
	std::string operator()(const ChannelLeftEvent& e) const
	{
		return "channel_left " + e.channel;
	}
	std::string operator()(const ChannelUpdatedEvent& e) const
	{
		return e.type + " " + e.channel.id + " " + e.channel.name;
	}
	std::string operator()(const MemberJoinedChannelEvent& e) const
	{
		return std::string(e.joined ? "member_joined_channel " : "member_left_channel ")
			+ e.channel + " " + e.user;
	}
	std::string operator()(const UserChangeEvent& e) const
	{
		return "user_change " + e.user.id + " " + e.user.bestName();
	}
	std::string operator()(const EmojiChangedEvent& e) const
	{
		std::string text = "emoji_changed " + e.subtype + " " + e.name;
		for (const std::string& name : e.names)
			text += " " + name;
		return text;
	}
	std::string operator()(const CountsEvent& e) const
	{
		int unread = 0;
		for (const UnreadInfo& info : e.counts.channels)
			unread += info.hasUnreads ? 1 : 0;
		return "counts " + std::to_string(e.counts.channels.size()) + " conversations, "
			+ std::to_string(unread) + " unread";
	}
	std::string operator()(const ConnectionEvent& e) const
	{
		static const char* kSources[] = {"rtm", "socket-mode", "polling", "local"};
		std::string text = std::string("connection ") + connectionStateName(e.state)
			+ " via " + kSources[static_cast<int>(e.source)];
		if (!e.detail.empty())
			text += " (" + e.detail + ")";
		if (e.retryInSeconds > 0)
			text += " retry in " + std::to_string(e.retryInSeconds) + " s";
		return text;
	}
	std::string operator()(const UnknownEvent& e) const
	{
		return "event " + e.type + (e.subtype.empty() ? "" : "/" + e.subtype);
	}
};

}  // namespace


std::string
describeEvent(const Event& event)
{
	return std::visit(DescribeVisitor(), event);
}

}  // namespace natter
