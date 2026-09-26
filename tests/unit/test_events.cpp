// Natter - real-time event parsing.
// SPDX-License-Identifier: MIT

#include "testing.h"
#include "natter/events.h"

using namespace natter;

namespace {

json
rtmEvent(size_t index)
{
	static const json events = parseJson(testing::fixture("rtm_events.json"));
	return events.at(index);
}


// A copy of the parsed alternative, or nothing.
template <typename T>
std::optional<T>
parsed(const std::optional<Event>& event)
{
	if (!event)
		return std::nullopt;
	if (const T* value = std::get_if<T>(&*event))
		return *value;
	return std::nullopt;
}

}  // namespace

TEST(events_hello_goodbye_and_acks)
{
	CHECK(parsed<HelloEvent>(parseEvent(rtmEvent(0))).has_value());
	CHECK(parsed<GoodbyeEvent>(parseEvent(parseJson(R"({"type":"goodbye"})"))).has_value());
	CHECK(!parseEvent(rtmEvent(17)).has_value());   // pong
	CHECK(!parseEvent(rtmEvent(18)).has_value());   // reply_to ack
	CHECK(!parseEvent(json("not an object")).has_value());
	CHECK(!parseEvent(parseJson("{}")).has_value());
}


TEST(events_messages)
{
	std::optional<Event> event = parseEvent(rtmEvent(1));
	std::optional<MessageEvent> message = parsed<MessageEvent>(event);
	REQUIRE(message.has_value());
	CHECK_EQ(message->message.channel, std::string("C01GENERAL"));
	CHECK_EQ(message->message.user, std::string("U01ALICE"));
	CHECK_EQ(message->message.ts, std::string("1727000600.000700"));
	CHECK(!message->message.isThreadReply());
	CHECK_EQ(std::string(eventName(*event)), std::string("message"));

	std::optional<MessageEvent> reply = parsed<MessageEvent>(parseEvent(rtmEvent(2)));
	REQUIRE(reply.has_value());
	CHECK(reply->message.isThreadReply());
	CHECK_EQ(reply->message.threadTs, std::string("1727000200.000400"));

	std::optional<Event> changedEvent = parseEvent(rtmEvent(3));
	std::optional<MessageChangedEvent> changed = parsed<MessageChangedEvent>(changedEvent);
	REQUIRE(changed.has_value());
	CHECK_EQ(changed->channel, std::string("C01GENERAL"));
	CHECK_EQ(changed->message.ts, std::string("1727000600.000700"));
	CHECK_EQ(changed->message.text, std::string("rtm says *hello*"));
	CHECK_EQ(changed->message.channel, std::string("C01GENERAL"));
	CHECK(changed->message.edited.has_value());
	REQUIRE(changed->previous.has_value());
	CHECK(changed->previous->text.find("hi") != std::string::npos);

	std::optional<MessageDeletedEvent> deleted = parsed<MessageDeletedEvent>(parseEvent(rtmEvent(8)));
	REQUIRE(deleted.has_value());
	CHECK_EQ(deleted->ts, std::string("1727000250.000300"));
	CHECK(deleted->threadTs.empty());

	json replyDeleted = parseJson(R"({"type":"message","subtype":"message_deleted",
		"channel":"C1","deleted_ts":"2.0","previous_message":{"ts":"2.0","thread_ts":"1.0"}})");
	std::optional<MessageDeletedEvent> inThread = parsed<MessageDeletedEvent>(parseEvent(replyDeleted));
	REQUIRE(inThread.has_value());
	CHECK_EQ(inThread->threadTs, std::string("1.0"));

	json replied = parseJson(R"({"type":"message","subtype":"message_replied","channel":"C1",
		"message":{"ts":"1.0","thread_ts":"1.0","reply_count":4,"text":"root"}})");
	std::optional<MessageChangedEvent> root = parsed<MessageChangedEvent>(parseEvent(replied));
	REQUIRE(root.has_value());
	CHECK_EQ(root->message.replyCount, 4);
}


TEST(events_reactions_typing_presence)
{
	std::optional<ReactionEvent> added = parsed<ReactionEvent>(parseEvent(rtmEvent(4)));
	REQUIRE(added.has_value());
	CHECK(added->added);
	CHECK_EQ(added->reaction, std::string("thumbsup"));
	CHECK_EQ(added->channel, std::string("C01GENERAL"));
	CHECK_EQ(added->ts, std::string("1727000600.000700"));
	CHECK_EQ(added->user, std::string("U02BOB"));
	CHECK_EQ(added->itemUser, std::string("U01ALICE"));

	std::optional<ReactionEvent> removed = parsed<ReactionEvent>(parseEvent(rtmEvent(5)));
	REQUIRE(removed.has_value());
	CHECK(!removed->added);

	// Reactions on files are not message reactions.
	json onFile = parseJson(R"({"type":"reaction_added","user":"U1","reaction":"x",
		"item":{"type":"file","file":"F1"}})");
	CHECK(parsed<UnknownEvent>(parseEvent(onFile)).has_value());

	std::optional<TypingEvent> typing = parsed<TypingEvent>(parseEvent(rtmEvent(6)));
	REQUIRE(typing.has_value());
	CHECK_EQ(typing->user, std::string("U01ALICE"));

	std::optional<PresenceEvent> presence = parsed<PresenceEvent>(parseEvent(rtmEvent(7)));
	REQUIRE(presence.has_value());
	CHECK_EQ(presence->users.size(), size_t(2));
	CHECK_EQ(presence->presence, std::string("away"));
	std::optional<PresenceEvent> single = parsed<PresenceEvent>(parseEvent(parseJson(
		R"({"type":"presence_change","user":"U1","presence":"active"})")));
	REQUIRE(single.has_value());
	CHECK_EQ(single->users.at(0), std::string("U1"));
}


TEST(events_marks_channels_members)
{
	std::optional<MarkedEvent> marked = parsed<MarkedEvent>(parseEvent(rtmEvent(9)));
	REQUIRE(marked.has_value());
	CHECK_EQ(marked->kind, std::string("channel"));
	CHECK_EQ(marked->ts, std::string("1727000600.000700"));
	CHECK_EQ(marked->unreadCount, 0);
	CHECK_EQ(marked->mentionCount, 0);
	std::optional<MarkedEvent> im = parsed<MarkedEvent>(parseEvent(rtmEvent(10)));
	REQUIRE(im.has_value());
	CHECK_EQ(im->kind, std::string("im"));
	std::optional<MarkedEvent> bare = parsed<MarkedEvent>(parseEvent(parseJson(
		R"({"type":"group_marked","channel":"G1","ts":"1.0"})")));
	REQUIRE(bare.has_value());
	CHECK_EQ(bare->unreadCount, -1);

	std::optional<ChannelJoinedEvent> joined = parsed<ChannelJoinedEvent>(parseEvent(rtmEvent(11)));
	REQUIRE(joined.has_value());
	CHECK_EQ(joined->channel.id, std::string("C07NOTMINE"));
	CHECK(joined->channel.isMember);
	CHECK_EQ(joined->channel.numMembers, 41);

	std::optional<ChannelLeftEvent> left = parsed<ChannelLeftEvent>(parseEvent(rtmEvent(12)));
	REQUIRE(left.has_value());
	CHECK_EQ(left->channel, std::string("C02RANDOM"));

	std::optional<MemberJoinedChannelEvent> member = parsed<MemberJoinedChannelEvent>(parseEvent(rtmEvent(13)));
	REQUIRE(member.has_value());
	CHECK(member->joined);
	CHECK_EQ(member->user, std::string("U02BOB"));
	CHECK_EQ(member->channel, std::string("G03SECRET"));
	CHECK_EQ(member->inviter, std::string("U01ALICE"));

	std::optional<ChannelUpdatedEvent> renamed = parsed<ChannelUpdatedEvent>(parseEvent(parseJson(
		R"({"type":"channel_rename","channel":{"id":"C1","name":"new-name","created":1}})")));
	REQUIRE(renamed.has_value());
	CHECK_EQ(renamed->channel.name, std::string("new-name"));
	std::optional<ChannelUpdatedEvent> archived = parsed<ChannelUpdatedEvent>(parseEvent(parseJson(
		R"({"type":"channel_archive","channel":"C1","user":"U1"})")));
	REQUIRE(archived.has_value());
	CHECK_EQ(archived->channel.id, std::string("C1"));
}


TEST(events_users_emoji_unknown)
{
	std::optional<UserChangeEvent> user = parsed<UserChangeEvent>(parseEvent(rtmEvent(14)));
	REQUIRE(user.has_value());
	CHECK_EQ(user->user.id, std::string("U02BOB"));
	CHECK_EQ(user->user.displayName, std::string("bobby"));

	std::optional<EmojiChangedEvent> add = parsed<EmojiChangedEvent>(parseEvent(rtmEvent(15)));
	REQUIRE(add.has_value());
	CHECK_EQ(add->subtype, std::string("add"));
	CHECK_EQ(add->name, std::string("natter"));
	std::optional<EmojiChangedEvent> remove = parsed<EmojiChangedEvent>(parseEvent(rtmEvent(16)));
	REQUIRE(remove.has_value());
	CHECK_EQ(remove->names.at(0), std::string("shipit"));
	std::optional<EmojiChangedEvent> rename = parsed<EmojiChangedEvent>(parseEvent(parseJson(
		R"({"type":"emoji_changed","subtype":"rename","old_name":"a","new_name":"b","value":"u"})")));
	REQUIRE(rename.has_value());
	CHECK_EQ(rename->name, std::string("b"));
	CHECK_EQ(rename->oldName, std::string("a"));

	std::optional<Event> unknown = parseEvent(rtmEvent(19));
	std::optional<UnknownEvent> dnd = parsed<UnknownEvent>(unknown);
	REQUIRE(dnd.has_value());
	CHECK_EQ(dnd->type, std::string("dnd_updated_user"));
	CHECK(dnd->raw.is_object());
}


TEST(events_socket_mode_payloads_and_descriptions)
{
	json envelopes = parseJson(testing::fixture("socket_mode_envelopes.json"));
	std::optional<MessageEvent> message = parsed<MessageEvent>(
		parseEvent(envelopes[1]["payload"]["event"]));
	REQUIRE(message.has_value());
	CHECK_EQ(message->message.text, std::string("socket mode hello"));
	std::optional<ReactionEvent> reaction = parsed<ReactionEvent>(
		parseEvent(envelopes[2]["payload"]["event"]));
	REQUIRE(reaction.has_value());
	CHECK_EQ(reaction->reaction, std::string("eyes"));

	json all = parseJson(testing::fixture("rtm_events.json"));
	for (const json& item : all) {
		std::optional<Event> event = parseEvent(item);
		if (event) {
			CHECK(!describeEvent(*event).empty());
			CHECK(std::string(eventName(*event)).size() > 0);
		}
	}
	ConnectionEvent connection;
	connection.state = ConnectionState::Disconnected;
	connection.source = EventSource::Rtm;
	connection.detail = "close 1006";
	connection.retryInSeconds = 4;
	CHECK_EQ(describeEvent(Event(connection)),
		std::string("connection disconnected via rtm (close 1006) retry in 4 s"));
}
