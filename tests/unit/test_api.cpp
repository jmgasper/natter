// Natter - Web API client tests against canned responses.
// SPDX-License-Identifier: MIT

#include "fake_transport.h"
#include "natter/web_api.h"

#include <cstdio>
#include <fstream>
#include <memory>

using namespace natter;

namespace {

Credentials
sessionCredentials()
{
	Credentials credentials;
	credentials.token = "xoxc-test-token";
	credentials.cookie = "xoxd-abc%2Fdef";
	credentials.workspace = "natter-test";
	return credentials;
}


Credentials
userCredentials()
{
	Credentials credentials;
	credentials.token = "xoxp-user-token";
	credentials.appToken = "xapp-app-token";
	return credentials;
}


struct Fixture {
	std::shared_ptr<FakeTransport> fake = std::make_shared<FakeTransport>();
	std::vector<int> sleeps;
	std::unique_ptr<WebApi> api;

	explicit Fixture(Credentials credentials = sessionCredentials())
	{
		ApiOptions options;
		options.sleeper = [this](int seconds) {
			sleeps.push_back(seconds);
			return true;
		};
		api = std::make_unique<WebApi>(fake, std::move(credentials), options);
	}
};

}  // namespace

// ---- auth placement ----------------------------------------------------------------

TEST(api_session_mode_sends_token_field_and_cookie)
{
	Fixture f;
	f.fake->addFixture("auth.test", "auth.test.json");
	REQUIRE(f.api->authTest().ok());
	const HttpRequest& request = f.fake->requests.at(0);
	CHECK_EQ(request.url, std::string("https://natter-test.slack.com/api/auth.test"));
	CHECK_EQ(request.method, std::string("POST"));
	CHECK(request.bodyKind == BodyKind::Form);
	REQUIRE(!request.form.empty());
	CHECK_EQ(request.form[0].first, std::string("token"));
	CHECK_EQ(request.form[0].second, std::string("xoxc-test-token"));
	CHECK_EQ(findHeader(request.headers, "cookie").value_or(""),
		std::string("d=xoxd-abc%2Fdef"));
	CHECK(!findHeader(request.headers, "Authorization").has_value());
}


TEST(api_user_token_uses_bearer_on_slack_com)
{
	Fixture f(userCredentials());
	f.fake->addFixture("auth.test", "auth.test.json");
	REQUIRE(f.api->authTest().ok());
	const HttpRequest& request = f.fake->requests.at(0);
	CHECK_EQ(request.url, std::string("https://slack.com/api/auth.test"));
	CHECK_EQ(findHeader(request.headers, "Authorization").value_or(""),
		std::string("Bearer xoxp-user-token"));
	CHECK(!FakeTransport::hasField(request, "token"));
	CHECK(!findHeader(request.headers, "Cookie").has_value());
}


TEST(api_base_override_and_enterprise_host)
{
	Credentials credentials = sessionCredentials();
	credentials.apiBase = "http://127.0.0.1:9999/api";
	CHECK_EQ(credentials.apiUrl(), std::string("http://127.0.0.1:9999/api/"));
	credentials.apiBase.clear();
	credentials.workspace = "https://acme-corp.enterprise.slack.com/messages/C1";
	CHECK_EQ(credentials.apiUrl(), std::string("https://acme-corp.enterprise.slack.com/api/"));
	Credentials user = userCredentials();
	user.workspace = "acme";
	CHECK_EQ(user.apiUrl(), std::string("https://slack.com/api/"));
}

// ---- auth, team, users ----------------------------------------------------------------

TEST(api_auth_test_parses)
{
	Fixture f;
	f.fake->addFixture("auth.test", "auth.test.json");
	Result<AuthInfo> info = f.api->authTest();
	REQUIRE(info.ok());
	CHECK_EQ(info->teamId, std::string("T0NATTER"));
	CHECK_EQ(info->userId, std::string("U03SELF"));
	CHECK_EQ(info->team, std::string("Natter Test"));
	CHECK_EQ(info->url, std::string("https://natter-test.slack.com/"));
}


TEST(api_team_info_parses)
{
	Fixture f;
	f.fake->addFixture("team.info", "team.info.json");
	Result<Team> team = f.api->teamInfo();
	REQUIRE(team.ok());
	CHECK_EQ(team->id, std::string("T0NATTER"));
	CHECK_EQ(team->domain, std::string("natter-test"));
	CHECK_EQ(team->iconUrl, std::string("https://icons.example/132.png"));
}


TEST(api_users_list_paginates)
{
	Fixture f;
	f.fake->addFixture("users.list", "users.list.page1.json");
	f.fake->addFixture("users.list", "users.list.page2.json");
	Result<std::vector<User>> users = f.api->usersList();
	REQUIRE(users.ok());
	REQUIRE(users->size() == 5);
	std::vector<HttpRequest> calls = f.fake->calls("users.list");
	REQUIRE(calls.size() == 2);
	CHECK_EQ(FakeTransport::field(calls[0], "limit"), std::string("200"));
	CHECK(!FakeTransport::hasField(calls[0], "cursor"));
	CHECK_EQ(FakeTransport::field(calls[1], "cursor"), std::string("dXNlcjpVMDNTRUxG"));

	const User& alice = users->at(0);
	CHECK_EQ(alice.id, std::string("U01ALICE"));
	CHECK_EQ(alice.name, std::string("alice"));
	CHECK_EQ(alice.realName, std::string("Alice Anderson"));
	CHECK_EQ(alice.displayName, std::string("ali"));
	CHECK_EQ(alice.bestName(), std::string("ali"));
	CHECK_EQ(alice.tz, std::string("Australia/Brisbane"));
	CHECK_EQ(alice.tzOffset, int64_t(36000));
	CHECK_EQ(alice.avatarUrl(48), std::string("https://avatars.example/U01ALICE_48.png"));
	CHECK_EQ(alice.avatarUrl(50), std::string("https://avatars.example/U01ALICE_72.png"));
	CHECK_EQ(alice.avatarUrl(1000), std::string("https://avatars.example/U01ALICE_192.png"));
	CHECK(!alice.deleted);
	CHECK(!alice.isBot);
	CHECK_EQ(users->at(1).bestName(), std::string("Bob Brown"));
	CHECK(users->at(3).isBot);
	CHECK_EQ(users->at(3).botId, std::string("B01DEPLOY"));
	CHECK(users->at(4).deleted);
}


TEST(api_pagination_error_on_second_page)
{
	Fixture f;
	f.fake->addFixture("users.list", "users.list.page1.json");
	f.fake->addError("users.list", "invalid_cursor");
	Result<std::vector<User>> users = f.api->usersList();
	REQUIRE(!users.ok());
	CHECK_EQ(users.error().code, std::string("invalid_cursor"));
	CHECK(users.error().kind == ErrorKind::Api);
}


TEST(api_users_info_and_presence)
{
	Fixture f;
	f.fake->addFixture("users.info", "users.info.json");
	f.fake->addFixture("users.getPresence", "users.getPresence.json");
	Result<User> user = f.api->usersInfo("U01ALICE");
	REQUIRE(user.ok());
	CHECK_EQ(user->id, std::string("U01ALICE"));
	CHECK_EQ(user->presence, std::string("active"));
	CHECK_EQ(FakeTransport::field(f.fake->calls("users.info").at(0), "user"),
		std::string("U01ALICE"));
	Result<Presence> presence = f.api->usersGetPresence("U01ALICE");
	REQUIRE(presence.ok());
	CHECK_EQ(presence->presence, std::string("away"));
	CHECK(presence->autoAway);
	CHECK(!presence->online);
	CHECK_EQ(presence->lastActivity, int64_t(1727000000));
}

// ---- conversations ------------------------------------------------------------------

TEST(api_conversations_list_all_types_without_archived)
{
	Fixture f;
	f.fake->addFixture("conversations.list", "conversations.list.page1.json");
	f.fake->addFixture("conversations.list", "conversations.list.page2.json");
	Result<std::vector<Channel>> channels = f.api->conversationsList();
	REQUIRE(channels.ok());
	std::vector<HttpRequest> calls = f.fake->calls("conversations.list");
	REQUIRE(calls.size() == 2);
	CHECK_EQ(FakeTransport::field(calls[0], "types"),
		std::string("public_channel,private_channel,mpim,im"));
	CHECK_EQ(FakeTransport::field(calls[0], "exclude_archived"), std::string("true"));
	CHECK_EQ(FakeTransport::field(calls[1], "cursor"), std::string("dGVhbTpDMDZPTEQ="));

	REQUIRE(channels->size() == 6);   // C06OLD is archived
	for (const Channel& channel : channels.value())
		CHECK(channel.id != "C06OLD");
	const Channel& general = channels->at(0);
	CHECK(general.kind() == ChannelKind::Public);
	CHECK(general.isMember);
	CHECK(general.isGeneral);
	CHECK_EQ(general.topic.value, std::string("Company-wide *news*"));
	CHECK_EQ(general.numMembers, 4);
	const Channel& secret = channels->at(2);
	CHECK(secret.kind() == ChannelKind::Private);
	const Channel& im = channels->at(3);
	CHECK(im.kind() == ChannelKind::Im);
	CHECK(im.isMember);
	CHECK_EQ(im.imUser, std::string("U01ALICE"));
	const Channel& mpim = channels->at(4);
	CHECK(mpim.kind() == ChannelKind::Mpim);
	CHECK(!channels->at(5).isMember);
}


TEST(api_conversations_info_read_state)
{
	Fixture f;
	f.fake->addFixture("conversations.info", "conversations.info.json");
	Result<Channel> channel = f.api->conversationsInfo("C01GENERAL");
	REQUIRE(channel.ok());
	CHECK_EQ(channel->lastRead, std::string("1727000100.000200"));
	CHECK_EQ(channel->latestTs, std::string("1727000300.000500"));
	CHECK_EQ(channel->unreadCount, 2);
	CHECK(channel->hasUnreads);
	CHECK_EQ(FakeTransport::field(f.fake->requests.at(0), "channel"),
		std::string("C01GENERAL"));
}


TEST(api_conversations_history_parses_messages)
{
	Fixture f;
	f.fake->addFixture("conversations.history", "conversations.history.json");
	HistoryOptions options;
	options.limit = 50;
	options.cursor = "abc";
	options.oldest = "1700000000.000000";
	options.latest = "1800000000.000000";
	options.inclusive = true;
	Result<HistoryPage> page = f.api->conversationsHistory("C01GENERAL", options);
	REQUIRE(page.ok());
	const HttpRequest& request = f.fake->requests.at(0);
	CHECK_EQ(FakeTransport::field(request, "channel"), std::string("C01GENERAL"));
	CHECK_EQ(FakeTransport::field(request, "limit"), std::string("50"));
	CHECK_EQ(FakeTransport::field(request, "cursor"), std::string("abc"));
	CHECK_EQ(FakeTransport::field(request, "oldest"), std::string("1700000000.000000"));
	CHECK_EQ(FakeTransport::field(request, "latest"), std::string("1800000000.000000"));
	CHECK_EQ(FakeTransport::field(request, "inclusive"), std::string("true"));

	CHECK(page->hasMore);
	CHECK_EQ(page->nextCursor, std::string("YmVmb3JlOjE3MjcwMDAwNTA="));
	REQUIRE(page->messages.size() == 5);
	const Message& welcome = page->messages[0];
	CHECK_EQ(welcome.channel, std::string("C01GENERAL"));
	CHECK_EQ(welcome.user, std::string("U01ALICE"));
	CHECK_EQ(welcome.ts, std::string("1727000300.000500"));
	REQUIRE(welcome.reactions.size() == 2);
	CHECK_EQ(welcome.reactions[0].name, std::string("tada"));
	CHECK_EQ(welcome.reactions[0].count, 2);
	CHECK_EQ(welcome.reactions[0].users.size(), size_t(2));
	CHECK_EQ(welcome.reactions[1].name, std::string("+1::skin-tone-3"));

	const Message& bot = page->messages[1];
	CHECK_EQ(bot.subtype, std::string("bot_message"));
	CHECK_EQ(bot.botId, std::string("B01DEPLOY"));
	CHECK_EQ(bot.username, std::string("deploybot"));
	CHECK(bot.attachments.is_array());
	CHECK_EQ(bot.attachments[0]["title"].get<std::string>(), std::string("Deployed v1.2"));
	CHECK(bot.botProfile.is_object());

	const Message& root = page->messages[2];
	CHECK(root.isThreadParent());
	CHECK(!root.isThreadReply());
	CHECK_EQ(root.replyCount, 2);
	CHECK_EQ(root.latestReply, std::string("1727000240.000460"));
	CHECK_EQ(root.replyUsers.size(), size_t(2));

	const Message& mine = page->messages[3];
	REQUIRE(mine.edited.has_value());
	CHECK_EQ(mine.edited->ts, std::string("1727000110.000000"));
	REQUIRE(mine.files.size() == 1);
	CHECK_EQ(mine.files[0].id, std::string("F01SHOT"));
	CHECK(mine.files[0].isImage());
	CHECK_EQ(mine.files[0].size, int64_t(12345));
	CHECK_EQ(mine.files[0].originalWidth, 800);
	CHECK_EQ(mine.files[0].thumbUrl(300), std::string("https://files.slack.com/thumb_360.png"));
	CHECK(mine.files[0].urlPrivate.find("files-pri") != std::string::npos);

	const Message& rich = page->messages[4];
	CHECK(rich.blocks.is_array());
	CHECK_EQ(rich.blocks[0]["type"].get<std::string>(), std::string("rich_text"));
	CHECK(rich.attachments.is_null());
}


TEST(api_message_json_round_trip)
{
	json body = parseJson(testing::fixture("conversations.history.json"));
	for (const json& item : body["messages"]) {
		Message original = Message::fromJson(item, "C01GENERAL");
		Message copy = Message::fromJson(original.toJson());
		CHECK_EQ(copy.toJson().dump(), original.toJson().dump());
		CHECK_EQ(copy.channel, std::string("C01GENERAL"));
	}
	json users = parseJson(testing::fixture("users.list.page1.json"));
	for (const json& item : users["members"]) {
		User original = User::fromJson(item);
		CHECK_EQ(User::fromJson(original.toJson()).toJson().dump(), original.toJson().dump());
	}
	json channels = parseJson(testing::fixture("conversations.list.page2.json"));
	for (const json& item : channels["channels"]) {
		Channel original = Channel::fromJson(item);
		CHECK_EQ(Channel::fromJson(original.toJson()).toJson().dump(),
			original.toJson().dump());
	}
}


TEST(api_conversations_replies)
{
	Fixture f;
	f.fake->addFixture("conversations.replies", "conversations.replies.json");
	Result<HistoryPage> page = f.api->conversationsReplies("C01GENERAL", "1727000200.000400");
	REQUIRE(page.ok());
	CHECK_EQ(FakeTransport::field(f.fake->requests.at(0), "ts"),
		std::string("1727000200.000400"));
	REQUIRE(page->messages.size() == 3);
	CHECK(page->messages[0].isThreadParent());
	CHECK(page->messages[1].isThreadReply());
	CHECK_EQ(page->messages[1].parentUserId, std::string("U02BOB"));
	CHECK(!page->hasMore);
}


TEST(api_conversations_mark_members_open)
{
	Fixture f;
	f.fake->addFixture("conversations.mark", "conversations.mark.json");
	f.fake->addFixture("conversations.members", "conversations.members.page1.json");
	f.fake->addFixture("conversations.members", "conversations.members.page2.json");
	f.fake->addFixture("conversations.open", "conversations.open.json");

	CHECK(f.api->conversationsMark("C01GENERAL", "1727000300.000500").ok());
	const HttpRequest mark = f.fake->calls("conversations.mark").at(0);
	CHECK_EQ(FakeTransport::field(mark, "ts"), std::string("1727000300.000500"));

	Result<std::vector<std::string>> members = f.api->conversationsMembers("G03SECRET");
	REQUIRE(members.ok());
	CHECK_EQ(members->size(), size_t(3));
	CHECK_EQ(members->at(2), std::string("U03SELF"));
	CHECK_EQ(f.fake->calls("conversations.members").size(), size_t(2));

	Result<Channel> dm = f.api->conversationsOpen({"U01ALICE"});
	REQUIRE(dm.ok());
	CHECK_EQ(dm->id, std::string("D04ALICE"));
	CHECK(dm->isIm);
	CHECK_EQ(dm->imUser, std::string("U01ALICE"));
	const HttpRequest open = f.fake->calls("conversations.open").at(0);
	CHECK_EQ(FakeTransport::field(open, "users"), std::string("U01ALICE"));

	f.api->conversationsOpen({"U01ALICE", "U02BOB"});
	CHECK_EQ(FakeTransport::field(f.fake->calls("conversations.open").at(1), "users"),
		std::string("U01ALICE,U02BOB"));
	CHECK(!f.api->conversationsOpen({}).ok());
}

// ---- messages -----------------------------------------------------------------------

TEST(api_chat_post_message_with_thread)
{
	Fixture f;
	f.fake->addFixture("chat.postMessage", "chat.postMessage.json");
	PostOptions options;
	options.threadTs = "1727000200.000400";
	options.replyBroadcast = true;
	options.blocks = json::array({json{{"type", "section"}}});
	Result<Message> message = f.api->chatPostMessage("C01GENERAL", "posted from natter",
		options);
	REQUIRE(message.ok());
	const HttpRequest& request = f.fake->requests.at(0);
	CHECK_EQ(FakeTransport::field(request, "channel"), std::string("C01GENERAL"));
	CHECK_EQ(FakeTransport::field(request, "text"), std::string("posted from natter"));
	CHECK_EQ(FakeTransport::field(request, "thread_ts"), std::string("1727000200.000400"));
	CHECK_EQ(FakeTransport::field(request, "reply_broadcast"), std::string("true"));
	CHECK_EQ(FakeTransport::field(request, "blocks"), std::string("[{\"type\":\"section\"}]"));
	CHECK_EQ(message->ts, std::string("1727000400.000600"));
	CHECK_EQ(message->channel, std::string("C01GENERAL"));
	CHECK_EQ(message->user, std::string("U03SELF"));
	CHECK_EQ(message->threadTs, std::string("1727000200.000400"));
}


TEST(api_chat_update_and_delete)
{
	Fixture f;
	f.fake->addFixture("chat.update", "chat.update.json");
	f.fake->addFixture("chat.delete", "chat.delete.json");
	Result<Message> updated = f.api->chatUpdate("C01GENERAL", "1727000100.000200", "hello again");
	REQUIRE(updated.ok());
	CHECK_EQ(updated->text, std::string("hello again"));
	CHECK_EQ(updated->ts, std::string("1727000100.000200"));
	REQUIRE(updated->edited.has_value());
	const HttpRequest update = f.fake->calls("chat.update").at(0);
	CHECK_EQ(FakeTransport::field(update, "ts"), std::string("1727000100.000200"));
	CHECK_EQ(FakeTransport::field(update, "text"), std::string("hello again"));
	CHECK(f.api->chatDelete("C01GENERAL", "1727000100.000200").ok());
	CHECK_EQ(FakeTransport::field(f.fake->calls("chat.delete").at(0), "ts"),
		std::string("1727000100.000200"));
}


TEST(api_reactions_strip_colons)
{
	Fixture f;
	f.fake->addFixture("reactions.add", "reactions.add.json");
	f.fake->addFixture("reactions.remove", "reactions.remove.json");
	CHECK(f.api->reactionsAdd("C01GENERAL", "1727000300.000500", ":thumbsup:").ok());
	CHECK(f.api->reactionsRemove("C01GENERAL", "1727000300.000500", "tada").ok());
	const HttpRequest add = f.fake->calls("reactions.add").at(0);
	CHECK_EQ(FakeTransport::field(add, "name"), std::string("thumbsup"));
	CHECK_EQ(FakeTransport::field(add, "timestamp"), std::string("1727000300.000500"));
	CHECK_EQ(FakeTransport::field(f.fake->calls("reactions.remove").at(0), "name"),
		std::string("tada"));
}

// ---- emoji, search ------------------------------------------------------------------

TEST(api_emoji_list)
{
	Fixture f;
	f.fake->addFixture("emoji.list", "emoji.list.json");
	Result<std::map<std::string, std::string>> emoji = f.api->emojiList();
	REQUIRE(emoji.ok());
	CHECK_EQ(emoji->size(), size_t(4));
	CHECK_EQ(emoji->at("parrot"), std::string("alias:partyparrot"));
	CHECK_EQ(emoji->at("shipit"), std::string("https://emoji.example/shipit.png"));
}


TEST(api_search_messages)
{
	Fixture f;
	f.fake->addFixture("search.messages", "search.messages.json");
	SearchOptions options;
	options.count = 5;
	options.page = 2;
	Result<SearchPage> page = f.api->searchMessages("deploy", options);
	REQUIRE(page.ok());
	const HttpRequest& request = f.fake->requests.at(0);
	CHECK_EQ(FakeTransport::field(request, "query"), std::string("deploy"));
	CHECK_EQ(FakeTransport::field(request, "count"), std::string("5"));
	CHECK_EQ(FakeTransport::field(request, "page"), std::string("2"));
	CHECK_EQ(FakeTransport::field(request, "sort"), std::string("timestamp"));
	CHECK_EQ(page->total, 2);
	CHECK_EQ(page->pages, 1);
	REQUIRE(page->matches.size() == 2);
	CHECK_EQ(page->matches[0].channelName, std::string("general"));
	CHECK_EQ(page->matches[0].message.channel, std::string("C01GENERAL"));
	CHECK_EQ(page->matches[0].message.text, std::string("when do we deploy?"));
	CHECK(page->matches[0].message.permalink.find("/archives/C01GENERAL/") != std::string::npos);
	CHECK_EQ(page->matches[1].message.channel, std::string("D04ALICE"));
}

// ---- files ---------------------------------------------------------------------------

TEST(api_upload_file_three_steps)
{
	Fixture f;
	f.fake->addFixture("files.getUploadURLExternal", "files.getUploadURLExternal.json");
	f.fake->addJson("https://files.slack.com/upload/v1/ABC123", "OK - 11");
	f.fake->addFixture("files.completeUploadExternal", "files.completeUploadExternal.json");
	UploadOptions options;
	options.initialComment = "see attached";
	options.threadTs = "1727000200.000400";
	Result<UploadedFile> file = f.api->uploadFile("C01GENERAL", "notes.txt", "hello world",
		options);
	REQUIRE(file.ok());
	CHECK_EQ(file->id, std::string("F02UPLOAD"));
	CHECK_EQ(file->title, std::string("notes.txt"));
	REQUIRE(f.fake->requests.size() == 3);

	const HttpRequest& ticket = f.fake->requests[0];
	CHECK_EQ(FakeTransport::field(ticket, "filename"), std::string("notes.txt"));
	CHECK_EQ(FakeTransport::field(ticket, "length"), std::string("11"));

	const HttpRequest& upload = f.fake->requests[1];
	CHECK_EQ(upload.url, std::string("https://files.slack.com/upload/v1/ABC123"));
	CHECK_EQ(upload.body, std::string("hello world"));
	CHECK(upload.bodyKind == BodyKind::Raw);
	CHECK(!findHeader(upload.headers, "Cookie").has_value());
	CHECK(!findHeader(upload.headers, "Authorization").has_value());

	const HttpRequest& complete = f.fake->requests[2];
	json files = parseJson(FakeTransport::field(complete, "files"));
	REQUIRE(files.is_array());
	CHECK_EQ(files[0]["id"].get<std::string>(), std::string("F02UPLOAD"));
	CHECK_EQ(files[0]["title"].get<std::string>(), std::string("notes.txt"));
	CHECK_EQ(FakeTransport::field(complete, "channel_id"), std::string("C01GENERAL"));
	CHECK_EQ(FakeTransport::field(complete, "initial_comment"), std::string("see attached"));
	CHECK_EQ(FakeTransport::field(complete, "thread_ts"), std::string("1727000200.000400"));
}


TEST(api_upload_failure_stops_before_complete)
{
	Fixture f;
	f.fake->addFixture("files.getUploadURLExternal", "files.getUploadURLExternal.json");
	HttpResponse broken;
	broken.status = 500;
	broken.body = "upload failed";
	f.fake->add("https://files.slack.com/upload/v1/ABC123", broken);
	Result<UploadedFile> file = f.api->uploadFile("C01GENERAL", "a.txt", "x");
	REQUIRE(!file.ok());
	CHECK(file.error().kind == ErrorKind::Http);
	CHECK_EQ(f.fake->calls("files.completeUploadExternal").size(), size_t(0));
}


TEST(api_download_sends_auth_and_detects_login_page)
{
	Fixture f;
	const std::string url = "https://files.slack.com/files-pri/T0NATTER-F01SHOT/shot.png";
	HttpResponse image;
	image.status = 200;
	image.body = std::string("\x89PNG", 4);
	image.headers = {{"Content-Type", "image/png"}};
	f.fake->add(url, image);
	HttpResponse login;
	login.status = 200;
	login.body = "<html>sign in</html>";
	login.headers = {{"Content-Type", "text/html; charset=utf-8"}};
	f.fake->add(url, login);

	Result<std::string> data = f.api->download(url);
	REQUIRE(data.ok());
	CHECK_EQ(data->size(), size_t(4));
	const HttpRequest& request = f.fake->requests.at(0);
	CHECK(request.followRedirects);
	CHECK_EQ(findHeader(request.headers, "Authorization").value_or(""),
		std::string("Bearer xoxc-test-token"));
	CHECK_EQ(findHeader(request.headers, "Cookie").value_or(""),
		std::string("d=xoxd-abc%2Fdef"));

	Result<std::string> denied = f.api->download(url);
	REQUIRE(!denied.ok());
	CHECK(denied.error().kind == ErrorKind::Auth);
}


TEST(api_download_to_file)
{
	Fixture f(userCredentials());
	const std::string url = "https://files.slack.com/files-pri/T0NATTER-F01SHOT/download/shot.png";
	HttpResponse image;
	image.status = 200;
	image.body = "file-bytes";
	f.fake->add(url, image);
	std::string path = std::string(NATTER_TEST_TMP_DIR) + "/download.bin";
	Status status = f.api->downloadToFile(url, path);
	REQUIRE(status.ok());
	std::ifstream in(path, std::ios::binary);
	std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	CHECK_EQ(contents, std::string("file-bytes"));
	std::remove(path.c_str());
	CHECK_EQ(findHeader(f.fake->requests.at(0).headers, "Authorization").value_or(""),
		std::string("Bearer xoxp-user-token"));
}

// ---- unread counts -------------------------------------------------------------------

TEST(api_counts_from_client_counts)
{
	Fixture f;
	f.fake->addFixture("client.counts", "client.counts.json");
	Result<UnreadCounts> counts = f.api->unreadCounts();
	REQUIRE(counts.ok());
	CHECK(counts->source == CountsSource::ClientCounts);
	REQUIRE(counts->channels.size() == 4);
	const UnreadInfo& general = counts->channels[0];
	CHECK_EQ(general.channel, std::string("C01GENERAL"));
	CHECK_EQ(general.lastRead, std::string("1727000100.000200"));
	CHECK_EQ(general.latestTs, std::string("1727000300.000500"));
	CHECK_EQ(general.mentionCount, 1);
	CHECK(general.hasUnreads);
	CHECK(general.unreadCount >= 1);
	CHECK(!counts->channels[1].hasUnreads);
	CHECK_EQ(counts->channels[1].unreadCount, 0);
	const UnreadInfo& im = counts->channels[3];
	CHECK_EQ(im.channel, std::string("D04ALICE"));
	CHECK_EQ(im.mentionCount, 2);
	CHECK_EQ(im.unreadCount, 2);
}


TEST(api_counts_fall_back_to_users_counts)
{
	Fixture f;
	f.fake->addError("client.counts", "unknown_method");
	f.fake->addFixture("users.counts", "users.counts.json");
	Result<UnreadCounts> counts = f.api->unreadCounts();
	REQUIRE(counts.ok());
	CHECK(counts->source == CountsSource::UsersCounts);
	REQUIRE(counts->channels.size() == 3);
	CHECK_EQ(counts->channels[0].unreadCount, 3);
	CHECK_EQ(counts->channels[0].mentionCount, 1);
	CHECK_EQ(counts->channels[2].channel, std::string("D04ALICE"));
	CHECK_EQ(counts->channels[2].unreadCount, 2);
	// The refused method is not asked again.
	f.api->unreadCounts();
	CHECK_EQ(f.fake->calls("client.counts").size(), size_t(1));
	CHECK_EQ(f.fake->calls("users.counts").size(), size_t(2));
}


TEST(api_counts_fall_back_to_conversations_info)
{
	Fixture f(userCredentials());
	f.fake->addError("users.counts", "not_allowed_token_type");
	f.fake->addFixture("conversations.info", "conversations.info.json");
	Result<UnreadCounts> counts = f.api->unreadCounts({"C01GENERAL", "C02RANDOM"});
	REQUIRE(counts.ok());
	CHECK(counts->source == CountsSource::ConversationsInfo);
	CHECK_EQ(f.fake->calls("client.counts").size(), size_t(0));   // not for xoxp
	REQUIRE(counts->channels.size() == 2);
	CHECK_EQ(counts->channels[0].unreadCount, 2);
	CHECK(counts->channels[0].hasUnreads);
	CHECK_EQ(counts->channels[1].channel, std::string("C02RANDOM"));
	CHECK_EQ(f.fake->calls("conversations.info").size(), size_t(2));
}


TEST(api_counts_auth_failure_is_returned)
{
	Fixture f;
	f.fake->addError("client.counts", "invalid_auth");
	Result<UnreadCounts> counts = f.api->unreadCounts();
	REQUIRE(!counts.ok());
	CHECK(counts.error().isAuth());
	CHECK_EQ(f.fake->calls("users.counts").size(), size_t(0));
}

// ---- real time bootstrap ---------------------------------------------------------------

TEST(api_rtm_connect_and_socket_mode_open)
{
	Fixture f(userCredentials());
	f.fake->addFixture("rtm.connect", "rtm.connect.json");
	f.fake->addFixture("apps.connections.open", "apps.connections.open.json");
	Result<ConnectInfo> rtm = f.api->rtmConnect();
	REQUIRE(rtm.ok());
	CHECK_EQ(rtm->url, std::string("wss://wss-primary.slack.com/websocket/AbCdEf"));
	CHECK_EQ(rtm->selfId, std::string("U03SELF"));
	CHECK_EQ(rtm->teamDomain, std::string("natter-test"));
	CHECK_EQ(FakeTransport::field(f.fake->calls("rtm.connect").at(0), "batch_presence_aware"),
		std::string("1"));

	Result<ConnectInfo> socket = f.api->appsConnectionsOpen();
	REQUIRE(socket.ok());
	CHECK(socket->url.find("ticket=abc") != std::string::npos);
	CHECK_EQ(findHeader(f.fake->calls("apps.connections.open").at(0).headers,
		"Authorization").value_or(""), std::string("Bearer xapp-app-token"));

	Fixture noApp;
	Result<ConnectInfo> refused = noApp.api->appsConnectionsOpen();
	REQUIRE(!refused.ok());
	CHECK(refused.error().kind == ErrorKind::InvalidArgument);
}

// ---- errors and rate limits --------------------------------------------------------------

TEST(api_error_mapping)
{
	Fixture f;
	f.fake->addError("auth.test", "invalid_auth");
	f.fake->addError("conversations.info", "channel_not_found");
	f.fake->addJson("users.info",
		R"({"ok":false,"error":"missing_scope","needed":"users:read","provided":"chat:write"})");
	f.fake->addJson("team.info", "<html>bad gateway</html>", 502);
	f.fake->addJson("emoji.list", "not json at all", 200);
	HttpResponse offline;
	offline.transportError = "Could not resolve host";
	f.fake->add("users.getPresence", offline);

	Result<AuthInfo> auth = f.api->authTest();
	REQUIRE(!auth.ok());
	CHECK(auth.error().kind == ErrorKind::Auth);
	CHECK_EQ(auth.error().code, std::string("invalid_auth"));

	Result<Channel> channel = f.api->conversationsInfo("C404");
	CHECK(!channel.ok() && channel.error().kind == ErrorKind::NotFound);

	Result<User> user = f.api->usersInfo("U1");
	REQUIRE(!user.ok());
	CHECK(user.error().kind == ErrorKind::Unsupported);
	CHECK(user.error().message.find("users:read") != std::string::npos);

	Result<Team> team = f.api->teamInfo();
	REQUIRE(!team.ok());
	CHECK(team.error().kind == ErrorKind::Http);
	CHECK_EQ(team.error().httpStatus, 502);

	Result<std::map<std::string, std::string>> emoji = f.api->emojiList();
	CHECK(!emoji.ok() && emoji.error().kind == ErrorKind::Parse);

	Result<Presence> presence = f.api->usersGetPresence("U1");
	CHECK(!presence.ok() && presence.error().kind == ErrorKind::Transport);

	CHECK(classifySlackError("not_authed") == ErrorKind::Auth);
	CHECK(classifySlackError("token_revoked") == ErrorKind::Auth);
	CHECK(classifySlackError("ratelimited") == ErrorKind::RateLimited);
	CHECK(classifySlackError("message_not_found") == ErrorKind::NotFound);
	CHECK(classifySlackError("not_allowed_token_type") == ErrorKind::Unsupported);
	CHECK(classifySlackError("msg_too_long") == ErrorKind::Api);
	CHECK(!auth.error().describe().empty());
}


TEST(api_retry_after_is_honoured)
{
	Fixture f;
	f.fake->addJson("auth.test", R"({"ok":false,"error":"ratelimited"})", 429,
		{{"Retry-After", "7"}});
	f.fake->addFixture("auth.test", "auth.test.json");
	Result<AuthInfo> info = f.api->authTest();
	REQUIRE(info.ok());
	REQUIRE(f.sleeps.size() == 1);
	CHECK_EQ(f.sleeps[0], 7);
	CHECK_EQ(f.fake->calls("auth.test").size(), size_t(2));
}


TEST(api_rate_limit_gives_up_after_retries)
{
	Fixture f;
	f.fake->addJson("auth.test", R"({"ok":false,"error":"ratelimited"})", 429,
		{{"retry-after", "2"}});
	Result<AuthInfo> info = f.api->authTest();
	REQUIRE(!info.ok());
	CHECK(info.error().kind == ErrorKind::RateLimited);
	CHECK_EQ(info.error().retryAfterSeconds, 2);
	CHECK_EQ(f.sleeps.size(), size_t(3));
	CHECK_EQ(f.fake->calls("auth.test").size(), size_t(4));
}


TEST(api_cancel_stops_calls)
{
	Fixture f;
	f.fake->addFixture("auth.test", "auth.test.json");
	f.api->cancel();
	Result<AuthInfo> info = f.api->authTest();
	CHECK(!info.ok() && info.error().kind == ErrorKind::Cancelled);
	f.api->resetCancel();
	CHECK(f.api->authTest().ok());
}
