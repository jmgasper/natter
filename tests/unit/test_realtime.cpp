// Natter - polling fallback, real-time mode selection and Session.
// SPDX-License-Identifier: MIT

#include "fake_transport.h"
#include "natter/realtime.h"
#include "natter/session.h"

#include <chrono>
#include <condition_variable>
#include <thread>

using namespace natter;

namespace {

std::string
historyBody(const std::vector<json>& messages, bool hasMore = false)
{
	json body = {{"ok", true}, {"messages", messages}, {"has_more", hasMore}};
	return body.dump();
}


json
message(const std::string& ts, const std::string& text, const std::string& user = "U01ALICE")
{
	return json{{"type", "message"}, {"ts", ts}, {"user", user}, {"text", text}};
}


struct Collector {
	std::mutex lock;
	std::condition_variable changed;
	std::vector<Event> events;

	EventHandler handler()
	{
		return [this](const Event& event, EventSource) {
			std::lock_guard<std::mutex> guard(lock);
			events.push_back(event);
			changed.notify_all();
		};
	}

	template <typename T>
	std::vector<T> all()
	{
		std::lock_guard<std::mutex> guard(lock);
		std::vector<T> out;
		for (const Event& event : events) {
			if (const T* typed = std::get_if<T>(&event))
				out.push_back(*typed);
		}
		return out;
	}

	bool waitFor(const std::function<bool(const Event&)>& predicate, int ms = 3000)
	{
		std::unique_lock<std::mutex> guard(lock);
		return changed.wait_for(guard, std::chrono::milliseconds(ms), [&] {
			for (const Event& event : events) {
				if (predicate(event))
					return true;
			}
			return false;
		});
	}
};


Credentials
sessionCredentials()
{
	Credentials credentials;
	credentials.token = "xoxc-test";
	credentials.cookie = "xoxd-test";
	credentials.workspace = "natter-test";
	return credentials;
}

}  // namespace

TEST(poller_reports_new_changed_and_deleted_messages)
{
	auto fake = std::make_shared<FakeTransport>();
	fake->addJson("conversations.history", historyBody({message("3.0", "three"),
		message("2.0", "two"), message("1.0", "one")}, true));
	json edited = message("2.0", "two (edited)");
	edited["edited"] = {{"user", "U01ALICE"}, {"ts", "4.5"}};
	fake->addJson("conversations.history", historyBody({message("4.0", "four"), edited,
		message("1.0", "one")}, true));
	auto api = std::make_shared<WebApi>(fake, sessionCredentials());
	Collector collector;
	Poller poller(api, collector.handler());
	poller.setActiveConversation("C01GENERAL");

	poller.pollHistoryOnce();   // baseline: no events
	CHECK(collector.events.empty());
	poller.pollHistoryOnce();

	std::vector<MessageEvent> added = collector.all<MessageEvent>();
	REQUIRE(added.size() == 1);
	CHECK_EQ(added[0].message.ts, std::string("4.0"));
	CHECK_EQ(added[0].message.channel, std::string("C01GENERAL"));
	std::vector<MessageChangedEvent> changed = collector.all<MessageChangedEvent>();
	REQUIRE(changed.size() == 1);
	CHECK_EQ(changed[0].message.text, std::string("two (edited)"));
	REQUIRE(changed[0].previous.has_value());
	CHECK_EQ(changed[0].previous->text, std::string("two"));
	std::vector<MessageDeletedEvent> deleted = collector.all<MessageDeletedEvent>();
	REQUIRE(deleted.size() == 1);
	CHECK_EQ(deleted[0].ts, std::string("3.0"));
	CHECK_EQ(FakeTransport::field(fake->requests.at(0), "limit"), std::string("30"));
}


TEST(poller_fills_gaps_and_polls_threads)
{
	auto fake = std::make_shared<FakeTransport>();
	fake->addJson("conversations.history", historyBody({message("2.0", "b"),
		message("1.0", "a")}));
	// Two new messages, but the page only has room for them: a gap follows.
	fake->addJson("conversations.history", historyBody({message("5.0", "e"),
		message("4.0", "d")}, true));
	fake->addJson("conversations.history", historyBody({message("5.0", "e"),
		message("4.0", "d"), message("3.0", "c")}));
	json reply = message("10.1", "reply one");
	reply["thread_ts"] = "10.0";
	json root = message("10.0", "root");
	root["thread_ts"] = "10.0";
	json reply2 = message("10.2", "reply two");
	reply2["thread_ts"] = "10.0";
	fake->addJson("conversations.replies", historyBody({root, reply}));
	fake->addJson("conversations.replies", historyBody({root, reply, reply2}));

	auto api = std::make_shared<WebApi>(fake, sessionCredentials());
	Collector collector;
	Poller poller(api, collector.handler());
	poller.setActiveConversation("C01GENERAL", "10.0");
	poller.pollHistoryOnce();
	poller.pollHistoryOnce();

	std::vector<HttpRequest> history = fake->calls("conversations.history");
	REQUIRE(history.size() == 3);
	CHECK_EQ(FakeTransport::field(history[2], "oldest"), std::string("2.0"));
	std::vector<MessageEvent> added = collector.all<MessageEvent>();
	REQUIRE(added.size() == 4);
	std::set<std::string> ts;
	for (const MessageEvent& event : added)
		ts.insert(event.message.ts);
	CHECK(ts.count("3.0") == 1 && ts.count("4.0") == 1 && ts.count("5.0") == 1);
	CHECK(ts.count("10.2") == 1);
	CHECK(collector.all<MessageDeletedEvent>().empty());
	CHECK_EQ(FakeTransport::field(fake->calls("conversations.replies").at(0), "ts"),
		std::string("10.0"));
}


TEST(poller_counts_and_thread)
{
	auto fake = std::make_shared<FakeTransport>();
	fake->addFixture("client.counts", "client.counts.json");
	fake->addJson("conversations.history", historyBody({message("1.0", "a")}));
	auto api = std::make_shared<WebApi>(fake, sessionCredentials());
	Collector collector;
	PollOptions options;
	options.historyIntervalMs = 20;
	options.countsIntervalMs = 50;
	Poller poller(api, collector.handler(), options);
	poller.setActiveConversation("C01GENERAL");
	poller.start();
	CHECK(collector.waitFor([](const Event& event) {
		return std::holds_alternative<CountsEvent>(event);
	}));
	CHECK(collector.waitFor([](const Event& event) {
		const ConnectionEvent* connection = std::get_if<ConnectionEvent>(&event);
		return connection != nullptr && connection->state == ConnectionState::Polling;
	}));
	std::this_thread::sleep_for(std::chrono::milliseconds(120));
	poller.stop();
	CHECK(!poller.running());
	CHECK(fake->calls("conversations.history").size() >= 3);
	std::vector<CountsEvent> counts = collector.all<CountsEvent>();
	REQUIRE(!counts.empty());
	CHECK_EQ(counts[0].counts.channels.size(), size_t(4));
}


TEST(realtime_auto_falls_back_to_polling)
{
	auto fake = std::make_shared<FakeTransport>();
	fake->addError("rtm.connect", "not_allowed_token_type");
	fake->addJson("conversations.history", historyBody({message("1.0", "a")}));
	fake->addFixture("client.counts", "client.counts.json");
	auto api = std::make_shared<WebApi>(fake, sessionCredentials());
	Collector collector;
	RealtimeOptions options;
	options.poll.historyIntervalMs = 50;
	Realtime realtime(api, collector.handler(), options);
	realtime.setActiveConversation("C01GENERAL");
	realtime.start();
	CHECK(collector.waitFor([](const Event& event) {
		const ConnectionEvent* connection = std::get_if<ConnectionEvent>(&event);
		return connection != nullptr && connection->state == ConnectionState::Polling;
	}));
	CHECK(realtime.activeMode() == RealtimeMode::Polling);
	CHECK(collector.waitFor([&fake](const Event&) {
		return !fake->calls("conversations.history").empty();
	}));
	realtime.stop();
	CHECK(realtime.activeMode() == RealtimeMode::Off);
	CHECK_EQ(fake->calls("rtm.connect").size(), size_t(1));

	std::vector<ConnectionEvent> states = collector.all<ConnectionEvent>();
	REQUIRE(states.size() >= 3);
	CHECK(states[0].state == ConnectionState::Connecting);
	CHECK(states[1].state == ConnectionState::Disconnected);
	CHECK(states[1].detail.find("not_allowed_token_type") != std::string::npos);
}


TEST(realtime_auto_prefers_socket_mode_with_app_token)
{
	auto fake = std::make_shared<FakeTransport>();
	fake->addError("apps.connections.open", "invalid_auth");
	fake->addFixture("client.counts", "client.counts.json");
	Credentials credentials = sessionCredentials();
	credentials.appToken = "xapp-1";
	auto api = std::make_shared<WebApi>(fake, credentials);
	Collector collector;
	Realtime realtime(api, collector.handler());
	realtime.start();
	CHECK(collector.waitFor([](const Event& event) {
		const ConnectionEvent* connection = std::get_if<ConnectionEvent>(&event);
		return connection != nullptr && connection->state == ConnectionState::Polling;
	}));
	realtime.stop();
	CHECK_EQ(fake->calls("apps.connections.open").size(), size_t(1));
	CHECK(fake->calls("rtm.connect").empty());
}

// ---- Session --------------------------------------------------------------------------

namespace {

std::shared_ptr<FakeTransport>
workspace()
{
	auto fake = std::make_shared<FakeTransport>();
	fake->addFixture("auth.test", "auth.test.json");
	fake->addFixture("team.info", "team.info.json");
	fake->addFixture("users.list", "users.list.page1.json");
	fake->addFixture("users.list", "users.list.page2.json");
	fake->addFixture("conversations.list", "conversations.list.page1.json");
	fake->addFixture("conversations.list", "conversations.list.page2.json");
	fake->addFixture("emoji.list", "emoji.list.json");
	fake->addFixture("client.counts", "client.counts.json");
	fake->addFixture("conversations.history", "conversations.history.json");
	fake->addFixture("conversations.replies", "conversations.replies.json");
	fake->addFixture("chat.postMessage", "chat.postMessage.json");
	fake->addFixture("chat.update", "chat.update.json");
	fake->addFixture("chat.delete", "chat.delete.json");
	fake->addFixture("reactions.add", "reactions.add.json");
	fake->addJson("reactions.remove", R"({"ok":false,"error":"no_reaction"})");
	fake->addFixture("conversations.mark", "conversations.mark.json");
	fake->addFixture("conversations.open", "conversations.open.json");
	return fake;
}

}  // namespace


TEST(session_bootstrap_fills_the_store)
{
	auto fake = workspace();
	SessionOptions options;
	options.cacheDirectory = std::string(NATTER_TEST_TMP_DIR) + "/session-cache";
	Session session(sessionCredentials(), options, fake);
	REQUIRE(session.bootstrap().ok());
	Store& store = session.store();
	CHECK_EQ(store.selfUserId(), std::string("U03SELF"));
	CHECK_EQ(store.team().name, std::string("Natter Test"));
	CHECK_EQ(store.team().url, std::string("https://natter-test.slack.com/"));
	CHECK_EQ(store.users().size(), size_t(5));
	CHECK_EQ(store.channels().size(), size_t(6));
	CHECK_EQ(store.emoji().size(), size_t(4));
	Channel im = *store.channel("D04ALICE");
	CHECK_EQ(im.mentionCount, 2);
	CHECK(im.hasUnreads);
	CHECK_EQ(store.channel("C01GENERAL")->lastRead, std::string("1727000100.000200"));

	REQUIRE(session.loadHistory("C01GENERAL").ok());
	CHECK_EQ(store.messages("C01GENERAL").size(), size_t(5));
	REQUIRE(session.loadThread("C01GENERAL", "1727000200.000400").ok());
	CHECK_EQ(store.thread("C01GENERAL", "1727000200.000400").size(), size_t(3));
	FormattedText text = session.format(*store.message("C01GENERAL", "1727000100.000200"));
	CHECK_EQ(text.plainText(), std::string("hello @ali & friends"));

	REQUIRE(session.saveCache().ok());
	Session again(sessionCredentials(), options, fake);
	REQUIRE(again.loadCache().ok());
	CHECK_EQ(again.store().channels().size(), size_t(6));
	CHECK_EQ(again.store().messages("C01GENERAL").size(), size_t(5));
}


TEST(session_bootstrap_auth_failure)
{
	auto fake = std::make_shared<FakeTransport>();
	fake->addError("auth.test", "invalid_auth");
	Session session(sessionCredentials(), {}, fake);
	Status status = session.bootstrap();
	REQUIRE(!status.ok());
	CHECK(status.error().isAuth());
}


TEST(session_actions_update_store_and_notify)
{
	auto fake = workspace();
	Session session(sessionCredentials(), {}, fake);
	REQUIRE(session.bootstrap().ok());
	REQUIRE(session.loadHistory("C01GENERAL").ok());

	std::vector<SessionEvent> seen;
	int listener = session.addListener([&seen](const SessionEvent& event) {
		seen.push_back(event);
	});

	Result<Message> sent = session.send("C01GENERAL", "posted from natter");
	REQUIRE(sent.ok());
	REQUIRE(seen.size() == 1);
	CHECK(seen[0].source == EventSource::Local);
	CHECK(std::holds_alternative<MessageEvent>(seen[0].event));
	CHECK(seen[0].change.kind == StoreChange::Messages);
	CHECK(!seen[0].change.newUnread);   // our own message
	CHECK(session.store().message("C01GENERAL", "1727000400.000600").has_value());

	REQUIRE(session.edit("C01GENERAL", "1727000100.000200", "hello again").ok());
	Message edited = *session.store().message("C01GENERAL", "1727000100.000200");
	CHECK_EQ(edited.text, std::string("hello again"));
	CHECK(edited.edited.has_value());
	CHECK_EQ(edited.files.size(), size_t(1));   // kept from what we had

	REQUIRE(session.react("C01GENERAL", "1727000300.000500", ":eyes:").ok());
	CHECK_EQ(session.store().message("C01GENERAL", "1727000300.000500")->reactions.size(),
		size_t(3));
	// no_reaction from Slack is not an error for the UI.
	CHECK(session.react("C01GENERAL", "1727000300.000500", "eyes", false).ok());
	CHECK_EQ(session.store().message("C01GENERAL", "1727000300.000500")->reactions.size(),
		size_t(2));

	REQUIRE(session.markRead("C01GENERAL", "1727000400.000600").ok());
	CHECK_EQ(session.store().channel("C01GENERAL")->lastRead,
		std::string("1727000400.000600"));
	CHECK_EQ(session.store().channel("C01GENERAL")->unreadCount, 0);

	REQUIRE(session.remove("C01GENERAL", "1727000100.000200").ok());
	CHECK(!session.store().message("C01GENERAL", "1727000100.000200").has_value());

	REQUIRE(session.openDirectMessage({"U01ALICE"}).ok());
	size_t before = seen.size();
	CHECK_EQ(before, size_t(7));
	session.removeListener(listener);
	session.send("C01GENERAL", "unheard");
	CHECK_EQ(seen.size(), before);
}


TEST(session_listener_can_remove_itself_and_post_runs_jobs)
{
	auto fake = workspace();
	Session session(sessionCredentials(), {}, fake);
	int calls = 0;
	int id = 0;
	id = session.addListener([&](const SessionEvent&) {
		calls++;
		session.removeListener(id);
	});
	session.send("C01GENERAL", "one");
	session.send("C01GENERAL", "two");
	CHECK_EQ(calls, 1);

	std::mutex lock;
	std::condition_variable done;
	bool ran = false;
	session.post([&] {
		std::lock_guard<std::mutex> guard(lock);
		ran = true;
		done.notify_all();
	});
	std::unique_lock<std::mutex> guard(lock);
	CHECK(done.wait_for(guard, std::chrono::seconds(3), [&] { return ran; }));
}


TEST(session_realtime_polling_feeds_the_store)
{
	auto fake = workspace();
	fake->addError("rtm.connect", "not_allowed_token_type");
	SessionOptions options;
	options.realtime.poll.historyIntervalMs = 30;
	Session session(sessionCredentials(), options, fake);
	REQUIRE(session.bootstrap().ok());
	REQUIRE(session.loadHistory("C01GENERAL").ok());

	std::mutex lock;
	std::condition_variable changed;
	std::vector<SessionEvent> seen;
	session.addListener([&](const SessionEvent& event) {
		std::lock_guard<std::mutex> guard(lock);
		seen.push_back(event);
		changed.notify_all();
	});
	session.setActiveConversation("C01GENERAL");
	session.startRealtime();
	{
		std::unique_lock<std::mutex> guard(lock);
		CHECK(changed.wait_for(guard, std::chrono::seconds(3), [&] {
			for (const SessionEvent& event : seen) {
				if (event.source == EventSource::Polling)
					return true;
			}
			return false;
		}));
	}
	// A new message appears on the server.
	json body = parseJson(testing::fixture("conversations.history.json"));
	body["messages"].insert(body["messages"].begin(),
		message("1727000900.000900", "fresh from the poller", "U02BOB"));
	fake->addJson("conversations.history", body.dump());
	{
		std::unique_lock<std::mutex> guard(lock);
		CHECK(changed.wait_for(guard, std::chrono::seconds(3), [&] {
			for (const SessionEvent& event : seen) {
				const MessageEvent* message = std::get_if<MessageEvent>(&event.event);
				if (message != nullptr && message->message.ts == "1727000900.000900")
					return event.change.newUnread;
			}
			return false;
		}));
	}
	CHECK(session.realtimeMode() == RealtimeMode::Polling);
	session.stopRealtime();
	CHECK(session.realtimeMode() == RealtimeMode::Off);
	CHECK(session.store().message("C01GENERAL", "1727000900.000900").has_value());
}
