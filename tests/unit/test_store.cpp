// Natter - store merge rules and the disk cache.
// SPDX-License-Identifier: MIT

#include "testing.h"
#include "natter/store.h"

#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <thread>

using namespace natter;

namespace {

const std::string kGeneral = "C01GENERAL";
const std::string kRoot = "1727000200.000400";

json
fixtureJson(const std::string& name)
{
	return parseJson(testing::fixture(name));
}


// A copy of one member, safe to loop over (a range-for over a member of a
// temporary would dangle before C++23).
json
fixtureArray(const std::string& name, const char* key)
{
	return fixtureJson(name)[key];
}


std::vector<Message>
historyMessages()
{
	std::vector<Message> messages;
	for (const json& item : fixtureArray("conversations.history.json", "messages"))
		messages.push_back(Message::fromJson(item, kGeneral));
	return messages;
}


// A store filled the way Session::bootstrap would.
void
fill(Store& store)
{
	store.setSelf("U03SELF", "T0NATTER");
	std::vector<User> users;
	for (const char* page : {"users.list.page1.json", "users.list.page2.json"}) {
		for (const json& item : fixtureArray(page, "members"))
			users.push_back(User::fromJson(item));
	}
	store.setUsers(users);
	std::vector<Channel> channels;
	for (const char* page : {"conversations.list.page1.json", "conversations.list.page2.json"}) {
		for (const json& item : fixtureArray(page, "channels"))
			channels.push_back(Channel::fromJson(item));
	}
	store.setChannels(channels);
	Channel general = *store.channel(kGeneral);
	general.lastRead = "1727000100.000200";
	store.upsertChannel(general);
	store.mergeHistory(kGeneral, historyMessages());
}


Message
newMessage(const std::string& ts, const std::string& user, const std::string& text,
	const std::string& channel = kGeneral)
{
	Message message;
	message.channel = channel;
	message.ts = ts;
	message.user = user;
	message.text = text;
	return message;
}


std::string
tempDirectory(const std::string& name)
{
	return std::string(NATTER_TEST_TMP_DIR) + "/" + name;
}

}  // namespace

TEST(store_history_is_ordered_oldest_first)
{
	Store store;
	fill(store);
	std::vector<Message> messages = store.messages(kGeneral);
	REQUIRE(messages.size() == 5);
	CHECK_EQ(messages.front().ts, std::string("1727000050.000100"));
	CHECK_EQ(messages.back().ts, std::string("1727000300.000500"));
	CHECK_EQ(store.messages(kGeneral, 2).front().ts, std::string("1727000250.000300"));
	CHECK_EQ(store.newestTs(kGeneral), std::string("1727000300.000500"));
	CHECK_EQ(store.oldestTs(kGeneral), std::string("1727000050.000100"));
	std::vector<Message> older = store.messagesBefore(kGeneral, "1727000200.000400", 10);
	REQUIRE(older.size() == 2);
	CHECK_EQ(older.back().ts, std::string("1727000100.000200"));
	CHECK_EQ(store.channel(kGeneral)->latestTs, std::string("1727000300.000500"));
	// Merging the same page again changes nothing.
	store.mergeHistory(kGeneral, historyMessages());
	CHECK_EQ(store.messages(kGeneral).size(), size_t(5));
	CHECK(store.messages("C0NOTHING").empty());
}


TEST(store_new_messages_count_unread_once)
{
	Store store;
	fill(store);
	StoreChange change = store.apply(MessageEvent{newMessage("1727000600.000700", "U01ALICE",
		"hello")});
	CHECK(change.kind == StoreChange::Messages);
	CHECK(change.newUnread);
	CHECK(!change.mentionsSelf);
	Channel general = *store.channel(kGeneral);
	CHECK_EQ(general.unreadCount, 1);
	CHECK(general.hasUnreads);
	CHECK_EQ(general.latestTs, std::string("1727000600.000700"));

	// The same message again (RTM plus polling) is not counted twice.
	change = store.apply(MessageEvent{newMessage("1727000600.000700", "U01ALICE", "hello")});
	CHECK(!change.newUnread);
	CHECK_EQ(store.channel(kGeneral)->unreadCount, 1);

	// Our own messages and joins do not count; mentions do.
	store.apply(MessageEvent{newMessage("1727000610.000000", "U03SELF", "mine")});
	Message join = newMessage("1727000615.000000", "U02BOB", "joined");
	join.subtype = "channel_join";
	store.apply(MessageEvent{join});
	change = store.apply(MessageEvent{newMessage("1727000620.000000", "U02BOB",
		"hey <@U03SELF>")});
	CHECK(change.mentionsSelf);
	general = *store.channel(kGeneral);
	CHECK_EQ(general.unreadCount, 2);
	CHECK_EQ(general.mentionCount, 1);

	// Every DM message is a mention.
	change = store.apply(MessageEvent{newMessage("1727000630.000000", "U01ALICE", "psst",
		"D04ALICE")});
	CHECK(change.mentionsSelf);
	CHECK_EQ(store.channel("D04ALICE")->mentionCount, 1);
	CHECK_EQ(store.messages(kGeneral).size(), size_t(9));
}


TEST(store_thread_replies_update_the_parent)
{
	Store store;
	fill(store);
	std::vector<Message> replies;
	for (const json& item : fixtureArray("conversations.replies.json", "messages"))
		replies.push_back(Message::fromJson(item, kGeneral));
	store.mergeReplies(kGeneral, kRoot, replies);
	std::vector<Message> thread = store.thread(kGeneral, kRoot);
	REQUIRE(thread.size() == 3);
	CHECK_EQ(thread.front().ts, kRoot);

	Message reply = newMessage("1727000610.000710", "U02BOB", "a reply");
	reply.threadTs = kRoot;
	StoreChange change = store.apply(MessageEvent{reply});
	CHECK(change.kind == StoreChange::Thread);
	CHECK_EQ(change.threadTs, kRoot);
	CHECK(!change.newUnread);   // replies do not make the channel unread
	CHECK_EQ(store.thread(kGeneral, kRoot).size(), size_t(4));
	Message parent = *store.message(kGeneral, kRoot);
	CHECK_EQ(parent.replyCount, 3);
	CHECK_EQ(parent.latestReply, std::string("1727000610.000710"));
	CHECK(std::find(parent.replyUsers.begin(), parent.replyUsers.end(), "U02BOB")
		!= parent.replyUsers.end());
	CHECK_EQ(store.thread(kGeneral, kRoot).front().replyCount, 3);   // thread copy too
	CHECK_EQ(store.messages(kGeneral).size(), size_t(5));   // not in the channel

	store.apply(MessageEvent{reply});
	CHECK_EQ(store.message(kGeneral, kRoot)->replyCount, 3);

	// A reply older than latest_reply was already counted by Slack.
	Message late = newMessage("1727000230.000000", "U01ALICE", "late");
	late.threadTs = kRoot;
	store.apply(MessageEvent{late});
	CHECK_EQ(store.message(kGeneral, kRoot)->replyCount, 3);

	// "Also send to channel" replies appear in both places.
	Message broadcast = newMessage("1727000700.000800", "U01ALICE", "to everyone");
	broadcast.threadTs = kRoot;
	broadcast.subtype = "thread_broadcast";
	change = store.apply(MessageEvent{broadcast});
	CHECK(change.newUnread);
	CHECK_EQ(store.messages(kGeneral).size(), size_t(6));
	CHECK_EQ(store.thread(kGeneral, kRoot).size(), size_t(6));

	// Thread of a parent that is not loaded: only the replies.
	Message orphan = newMessage("5.0", "U01ALICE", "x");
	orphan.threadTs = "4.0";
	store.apply(MessageEvent{orphan});
	CHECK_EQ(store.thread(kGeneral, "4.0").size(), size_t(1));
}


TEST(store_edits_and_deletes)
{
	Store store;
	fill(store);
	Message edited = newMessage("1727000100.000200", "U03SELF", "hello (fixed)");
	edited.edited = Edited{"U03SELF", "1727000500.000000"};
	StoreChange change = store.apply(MessageChangedEvent{kGeneral, edited, std::nullopt});
	CHECK(change.kind == StoreChange::Messages);
	CHECK_EQ(store.message(kGeneral, "1727000100.000200")->text, std::string("hello (fixed)"));
	CHECK_EQ(store.message(kGeneral, "1727000100.000200")->channel, kGeneral);

	// An edit to a message older than the loaded window is ignored.
	change = store.apply(MessageChangedEvent{kGeneral, newMessage("1000.0", "U1", "x"),
		std::nullopt});
	CHECK(change.kind == StoreChange::None);
	// One inside the window we somehow missed is filled in.
	change = store.apply(MessageChangedEvent{kGeneral, newMessage("1727000150.000000", "U1",
		"missed"), std::nullopt});
	CHECK(change.kind == StoreChange::Messages);
	CHECK(store.message(kGeneral, "1727000150.000000").has_value());

	change = store.apply(MessageDeletedEvent{kGeneral, "1727000250.000300", ""});
	CHECK(change.kind == StoreChange::Messages);
	CHECK(!store.message(kGeneral, "1727000250.000300").has_value());
	change = store.apply(MessageDeletedEvent{kGeneral, "999.0", ""});
	CHECK(change.kind == StoreChange::None);

	// Deleting a reply shrinks the parent's count.
	std::vector<Message> replies;
	for (const json& item : fixtureArray("conversations.replies.json", "messages"))
		replies.push_back(Message::fromJson(item, kGeneral));
	store.mergeReplies(kGeneral, kRoot, replies);
	change = store.apply(MessageDeletedEvent{kGeneral, "1727000240.000460", kRoot});
	CHECK(change.kind == StoreChange::Thread);
	Message parent = *store.message(kGeneral, kRoot);
	CHECK_EQ(parent.replyCount, 1);
	CHECK_EQ(parent.latestReply, std::string("1727000220.000410"));
	CHECK_EQ(store.thread(kGeneral, kRoot).size(), size_t(2));

	// Deleting the parent drops the thread.
	store.apply(MessageDeletedEvent{kGeneral, kRoot, ""});
	CHECK(store.thread(kGeneral, kRoot).empty());
}


TEST(store_reactions)
{
	Store store;
	fill(store);
	const std::string ts = "1727000300.000500";
	auto reactions = [&]() { return store.message(kGeneral, ts)->reactions; };
	REQUIRE(reactions().size() == 2);

	ReactionEvent add{true, "U01ALICE", "tada", kGeneral, ts, "U01ALICE", "1.0"};
	store.apply(add);
	CHECK_EQ(reactions()[0].count, 3);
	store.apply(add);   // duplicate
	CHECK_EQ(reactions()[0].count, 3);

	ReactionEvent fresh{true, "U02BOB", "eyes", kGeneral, ts, "", ""};
	StoreChange change = store.apply(fresh);
	CHECK(change.kind == StoreChange::Messages);
	REQUIRE(reactions().size() == 3);
	CHECK_EQ(reactions()[2].name, std::string("eyes"));
	CHECK_EQ(reactions()[2].count, 1);

	ReactionEvent remove{false, "U02BOB", "eyes", kGeneral, ts, "", ""};
	store.apply(remove);
	CHECK_EQ(reactions().size(), size_t(2));
	store.apply(remove);   // nothing left to remove
	CHECK_EQ(reactions().size(), size_t(2));

	// Removing someone missing from a truncated users list still counts down.
	ReactionEvent other{false, "U09OTHER", "tada", kGeneral, ts, "", ""};
	Message message = *store.message(kGeneral, ts);
	message.reactions[0].count = 5;
	store.apply(MessageChangedEvent{kGeneral, message, std::nullopt});
	store.apply(other);
	CHECK_EQ(reactions()[0].count, 4);

	CHECK(store.apply(ReactionEvent{true, "U1", "x", kGeneral, "1.0", "", ""}).kind
		== StoreChange::None);
}


TEST(store_marks_and_counts)
{
	Store store;
	fill(store);
	store.apply(MessageEvent{newMessage("1727000600.000700", "U01ALICE", "one")});
	store.apply(MessageEvent{newMessage("1727000610.000710", "U02BOB", "<@U03SELF> two")});
	CHECK_EQ(store.channel(kGeneral)->unreadCount, 2);

	// Marked half way: recounted from the timeline.
	MarkedEvent partial;
	partial.channel = kGeneral;
	partial.ts = "1727000600.000700";
	partial.kind = "channel";
	StoreChange change = store.apply(partial);
	CHECK(change.kind == StoreChange::ReadState);
	CHECK_EQ(store.channel(kGeneral)->unreadCount, 1);
	CHECK_EQ(store.channel(kGeneral)->mentionCount, 1);

	// Marked with Slack's own figures.
	MarkedEvent all = partial;
	all.ts = "1727000610.000710";
	all.unreadCount = 0;
	all.mentionCount = 0;
	store.apply(all);
	Channel general = *store.channel(kGeneral);
	CHECK_EQ(general.unreadCount, 0);
	CHECK(!general.hasUnreads);
	CHECK_EQ(general.lastRead, std::string("1727000610.000710"));

	store.markRead(kGeneral, "1727000000.000000");
	CHECK_EQ(store.channel(kGeneral)->unreadCount, 6);   // all but our own

	// Counts snapshot from client.counts.
	UnreadCounts counts;
	counts.channels.push_back(UnreadInfo{"D04ALICE", "1.0", "2.0", 2, 2, true});
	counts.channels.push_back(UnreadInfo{"C0UNKNOWN", "1.0", "2.0", 1, 0, true});
	change = store.apply(CountsEvent{counts});
	CHECK(change.kind == StoreChange::ReadState);
	Channel im = *store.channel("D04ALICE");
	CHECK_EQ(im.unreadCount, 2);
	CHECK_EQ(im.mentionCount, 2);
	CHECK_EQ(im.lastRead, std::string("1.0"));
	CHECK(!store.channel("C0UNKNOWN").has_value());
}


TEST(store_channel_list_merges)
{
	Store store;
	fill(store);
	CHECK_EQ(store.channels().size(), size_t(7));
	// A fresh conversations.list has no read state: what we had is kept.
	Channel bare = Channel::fromJson(fixtureJson("conversations.list.page1.json")["channels"][0]);
	store.setChannels({bare});
	CHECK_EQ(store.channels().size(), size_t(1));
	CHECK_EQ(store.channel(kGeneral)->lastRead, std::string("1727000100.000200"));
	fill(store);

	json joined = fixtureJson("rtm_events.json")[11];
	store.apply(*parseEvent(joined));
	CHECK(store.channel("C07NOTMINE")->isMember);
	store.apply(ChannelLeftEvent{"C02RANDOM"});
	CHECK(!store.channel("C02RANDOM")->isMember);
	CHECK(store.apply(ChannelLeftEvent{"C0NOPE"}).kind == StoreChange::None);

	ChannelUpdatedEvent rename;
	rename.type = "channel_rename";
	rename.channel.id = kGeneral;
	rename.channel.name = "town-square";
	store.apply(rename);
	Channel general = *store.channel(kGeneral);
	CHECK_EQ(general.name, std::string("town-square"));
	CHECK(general.isMember);   // not clobbered by the partial update
	CHECK(general.isGeneral);
	ChannelUpdatedEvent archive;
	archive.type = "channel_archive";
	archive.channel.id = kGeneral;
	store.apply(archive);
	CHECK(store.channel(kGeneral)->isArchived);

	store.apply(MemberJoinedChannelEvent{false, "U03SELF", "G03SECRET", "G", ""});
	CHECK(!store.channel("G03SECRET")->isMember);
	CHECK_EQ(store.channel("G03SECRET")->numMembers, 1);
	store.apply(MemberJoinedChannelEvent{true, "U03SELF", "G03SECRET", "G", ""});
	CHECK(store.channel("G03SECRET")->isMember);

	ChannelJoinedEvent bareJoin;
	bareJoin.channel.id = "C02RANDOM";
	store.apply(bareJoin);
	CHECK(store.channel("C02RANDOM")->isMember);
	CHECK_EQ(store.channel("C02RANDOM")->name, std::string("random"));
	store.removeChannel("C02RANDOM");
	CHECK(!store.channel("C02RANDOM").has_value());
}


TEST(store_users_presence_emoji)
{
	Store store;
	fill(store);
	CHECK_EQ(store.users().size(), size_t(5));
	CHECK_EQ(store.userName("U01ALICE"), std::string("ali"));
	CHECK_EQ(store.userName("U0NOBODY"), std::string("U0NOBODY"));
	CHECK_EQ(store.user("U01ALICE")->presence, std::string("active"));

	store.apply(PresenceEvent{{"U01ALICE", "U02BOB"}, "away"});
	CHECK_EQ(store.user("U01ALICE")->presence, std::string("away"));
	UserChangeEvent change;
	change.user = *store.user("U02BOB");
	change.user.displayName = "bobby";
	change.user.presence.clear();
	StoreChange result = store.apply(change);
	CHECK(result.kind == StoreChange::Users);
	CHECK_EQ(result.id, std::string("U02BOB"));
	CHECK_EQ(store.userName("U02BOB"), std::string("bobby"));
	CHECK_EQ(store.user("U02BOB")->presence, std::string("away"));   // kept

	store.setEmoji({{"partyparrot", "https://e/p.gif"}, {"parrot", "alias:partyparrot"},
		{"loop", "alias:loop2"}, {"loop2", "alias:loop"}});
	CHECK_EQ(store.resolveCustomEmoji("parrot"), std::string("partyparrot"));
	CHECK_EQ(store.resolveCustomEmoji("loop"), std::string(""));   // cycles give up
	EmojiChangedEvent add;
	add.subtype = "add";
	add.name = "natter";
	add.value = "https://e/n.png";
	store.apply(add);
	CHECK_EQ(store.customEmojiUrl("natter"), std::string("https://e/n.png"));
	EmojiChangedEvent rename;
	rename.subtype = "rename";
	rename.oldName = "natter";
	rename.name = "natter2";
	store.apply(rename);
	CHECK_EQ(store.customEmojiUrl("natter2"), std::string("https://e/n.png"));
	CHECK_EQ(store.customEmojiUrl("natter"), std::string(""));
	EmojiChangedEvent remove;
	remove.subtype = "remove";
	remove.names = {"natter2", "partyparrot"};
	store.apply(remove);
	CHECK_EQ(store.emoji().size(), size_t(3));
	CHECK_EQ(store.customEmojiUrl("parrot"), std::string(""));
}


TEST(store_names_and_lookup)
{
	Store store;
	fill(store);
	CHECK_EQ(store.findChannel("general")->id, kGeneral);
	CHECK_EQ(store.findChannel("#general")->id, kGeneral);
	CHECK_EQ(store.findChannel(kGeneral)->id, kGeneral);
	CHECK_EQ(store.findChannel("@alice")->id, std::string("D04ALICE"));
	CHECK_EQ(store.findChannel("ali")->id, std::string("D04ALICE"));
	CHECK(!store.findChannel("nope").has_value());
	CHECK_EQ(store.channelDisplayName(kGeneral), std::string("#general"));
	CHECK_EQ(store.channelDisplayName("D04ALICE"), std::string("@ali"));
	CHECK_EQ(store.channelDisplayName("G05MPIM"), std::string("alice, bob, natter"));
	CHECK_EQ(store.channelDisplayName("C0NOPE"), std::string("C0NOPE"));
}


TEST(store_cache_round_trip)
{
	std::string directory = tempDirectory("cache/nested");
	Store store;
	fill(store);
	Team team;
	team.id = "T0NATTER";
	team.name = "Natter Test";
	team.iconUrl = "https://icons.example/132.png";
	store.setTeam(team);
	store.setEmoji({{"partyparrot", "https://e/p.gif"}});
	REQUIRE(store.saveCache(directory, 3).ok());
	struct stat st;
	CHECK(::stat((directory + "/users.json").c_str(), &st) == 0);

	Store loaded;
	REQUIRE(loaded.loadCache(directory).ok());
	CHECK_EQ(loaded.selfUserId(), std::string("U03SELF"));
	CHECK_EQ(loaded.team().name, std::string("Natter Test"));
	CHECK_EQ(loaded.team().iconUrl, std::string("https://icons.example/132.png"));
	CHECK_EQ(loaded.users().size(), size_t(5));
	CHECK_EQ(loaded.channels().size(), store.channels().size());
	CHECK_EQ(loaded.channel(kGeneral)->lastRead, std::string("1727000100.000200"));
	CHECK_EQ(loaded.channel("D04ALICE")->imUser, std::string("U01ALICE"));
	std::vector<Message> messages = loaded.messages(kGeneral);
	REQUIRE(messages.size() == 3);   // the newest three
	CHECK_EQ(messages.back().ts, std::string("1727000300.000500"));
	CHECK_EQ(messages.back().reactions.size(), size_t(2));
	CHECK_EQ(messages.front().replyCount, 2);
	CHECK_EQ(loaded.customEmojiUrl("partyparrot"), std::string("https://e/p.gif"));

	Store empty;
	CHECK(empty.loadCache(tempDirectory("no-such-cache")).ok());
	CHECK(empty.channels().empty());
	store.clear();
	CHECK(store.users().empty());
}


TEST(store_trims_long_timelines)
{
	Store store;
	store.setMaxMessagesPerConversation(3);
	for (int i = 1; i <= 10; i++)
		store.apply(MessageEvent{newMessage(std::to_string(1000 + i) + ".0", "U1", "m")});
	std::vector<Message> messages = store.messages(kGeneral);
	REQUIRE(messages.size() == 3);
	CHECK_EQ(messages.front().ts, std::string("1008.0"));
}


TEST(store_concurrent_access)
{
	Store store;
	fill(store);
	std::atomic<bool> stop{false};
	std::thread reader([&] {
		while (!stop) {
			store.messages(kGeneral);
			store.channels();
			formatMrkdwn("<@U01ALICE>", store.formatContext());
		}
	});
	std::vector<std::thread> writers;
	for (int w = 0; w < 4; w++) {
		writers.emplace_back([&store, w] {
			for (int i = 0; i < 250; i++) {
				std::string ts = "1800000" + std::to_string(w) + std::to_string(1000 + i) + ".0";
				store.apply(MessageEvent{newMessage(ts, "U01ALICE", "x")});
				store.apply(ReactionEvent{true, "U02BOB", "tada", kGeneral, ts, "", ""});
			}
		});
	}
	for (std::thread& writer : writers)
		writer.join();
	stop = true;
	reader.join();
	CHECK_EQ(store.messages(kGeneral).size(), size_t(1005));
	CHECK_EQ(store.channel(kGeneral)->unreadCount, 1000);
}
