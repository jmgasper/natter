// Natter - credentials, sign-in helpers and small utilities.
// SPDX-License-Identifier: MIT

#include "fake_transport.h"
#include "natter/credentials.h"
#include "natter/http.h"
#include "natter/util.h"

#include <sys/stat.h>

#include <cstdio>

using namespace natter;

// ---- utilities -----------------------------------------------------------------------

TEST(util_url_and_form_encoding)
{
	CHECK_EQ(urlEncode("a b&c=d/é"), std::string("a%20b%26c%3Dd%2F%C3%A9"));
	CHECK_EQ(urlDecode("a%20b+c%2Fd%zz"), std::string("a b c/d%zz"));
	CHECK_EQ(formEncode({{"token", "xoxc-1"}, {"text", "hi there"}}),
		std::string("token=xoxc-1&text=hi%20there"));
}


TEST(util_base64_sha1)
{
	CHECK_EQ(base64Encode(""), std::string(""));
	CHECK_EQ(base64Encode("f"), std::string("Zg=="));
	CHECK_EQ(base64Encode("fo"), std::string("Zm8="));
	CHECK_EQ(base64Encode("foo"), std::string("Zm9v"));
	CHECK_EQ(base64Encode(sha1("abc")), std::string("qZk+NkcGgWq6PiVxeFDCbJzQ2J0="));
	CHECK_EQ(randomBytes(16).size(), size_t(16));
}


TEST(util_ts_ordering)
{
	CHECK(compareTs("1727000100.000200", "1727000100.000300") < 0);
	CHECK(compareTs("1727000100.0002", "1727000100.000200") == 0);
	CHECK(compareTs("999999999.9", "1000000000.0") < 0);
	CHECK(compareTs("1727000200", "1727000100.999999") > 0);
	CHECK(compareTs("", "1") < 0);
	CHECK_EQ(tsSeconds("1727000100.000200"), int64_t(1727000100));
	std::map<std::string, int, TsLess> ordered{{"10.5", 1}, {"9.9", 2}, {"10.10", 3}};
	CHECK_EQ(ordered.begin()->second, 2);
	CHECK_EQ(ordered.rbegin()->second, 1);   // .5 > .10 as a decimal fraction
}


TEST(util_tolerant_json)
{
	json j = parseJson(R"({"s":"x","n":5,"ns":"12","f":2.5,"b":true,"bs":"true","nul":null,
		"o":{"k":1},"a":[1]})");
	CHECK_EQ(jStr(j, "s"), std::string("x"));
	CHECK_EQ(jStr(j, "n"), std::string("5"));
	CHECK_EQ(jStr(j, "missing", "dflt"), std::string("dflt"));
	CHECK_EQ(jStr(j, "nul", "dflt"), std::string("dflt"));
	CHECK_EQ(jInt(j, "ns"), int64_t(12));
	CHECK_EQ(jInt(j, "f"), int64_t(2));
	CHECK(jBool(j, "b"));
	CHECK(jBool(j, "bs"));
	CHECK(jObj(j, "o").is_object());
	CHECK(jObj(j, "a").is_null());
	CHECK(jArr(j, "a").is_array());
	CHECK(!jHas(j, "nul"));
	CHECK(parseJson("{broken").is_discarded());
	CHECK_EQ(jStr(json("not an object"), "s", "d"), std::string("d"));
}


TEST(util_http_response_helpers)
{
	HttpResponse response;
	response.status = 429;
	response.headers = {{"retry-after", " 30 "}, {"Content-Type", "application/json"}};
	CHECK_EQ(response.retryAfterSeconds(), 30);
	CHECK_EQ(response.header("CONTENT-TYPE").value_or(""), std::string("application/json"));
	response.headers.clear();
	CHECK_EQ(response.retryAfterSeconds(), -1);

	FakeTransport fake;
	fake.addJson("https://x/", "{}", 429);
	fake.addJson("https://x/", "{}", 200);
	std::vector<int> waits;
	RetryPolicy policy;
	policy.defaultWaitSeconds = 4;
	policy.sleeper = [&waits](int seconds) {
		waits.push_back(seconds);
		return true;
	};
	HttpResponse result = performWithRetry(fake, HttpRequest::get("https://x/"), policy);
	CHECK_EQ(result.status, 200);
	REQUIRE(waits.size() == 1);
	CHECK_EQ(waits[0], 4);

	// A wait longer than the policy allows is not attempted.
	FakeTransport slow;
	slow.addJson("https://y/", "{}", 429, {{"Retry-After", "3600"}});
	waits.clear();
	result = performWithRetry(slow, HttpRequest::get("https://y/"), policy);
	CHECK_EQ(result.status, 429);
	CHECK(waits.empty());
}

// ---- credentials ------------------------------------------------------------------------

TEST(credentials_modes_and_hosts)
{
	Credentials credentials;
	credentials.token = "xoxc-1";
	CHECK(credentials.mode() == AuthMode::Session);
	credentials.token = "xoxp-1";
	CHECK(credentials.mode() == AuthMode::UserToken);
	credentials.token = "xoxb-1";
	CHECK(credentials.mode() == AuthMode::BotToken);
	credentials.token = "nonsense";
	CHECK(credentials.mode() == AuthMode::None);

	CHECK_EQ(normalizeWorkspaceHost("acme"), std::string("acme.slack.com"));
	CHECK_EQ(normalizeWorkspaceHost(" Acme.Slack.com "), std::string("acme.slack.com"));
	CHECK_EQ(normalizeWorkspaceHost("https://acme.slack.com/client/T1/C1"),
		std::string("acme.slack.com"));
	CHECK_EQ(normalizeWorkspaceHost("127.0.0.1:8080"), std::string("127.0.0.1:8080"));
	CHECK_EQ(normalizeWorkspaceHost(""), std::string(""));

	CHECK_EQ(normalizeCookie("xoxd-a%2Fb"), std::string("xoxd-a%2Fb"));
	CHECK_EQ(normalizeCookie("d=xoxd-a/b+c="), std::string("xoxd-a%2Fb%2Bc%3D"));

	credentials.token = "xoxc-1";
	credentials.cookie = "xoxd-a/b";
	credentials.workspace = "acme";
	CHECK_EQ(credentials.cookieHeader(), std::string("d=xoxd-a%2Fb"));
	CHECK_EQ(credentials.workspaceUrl(), std::string("https://acme.slack.com/"));
	CHECK(!credentials.hasAppToken());
	credentials.appToken = "xapp-1-A";
	CHECK(credentials.hasAppToken());
}


TEST(credentials_json_round_trip_and_file)
{
	Credentials credentials;
	credentials.token = "xoxc-1";
	credentials.cookie = "xoxd-2";
	credentials.workspace = "acme";
	credentials.appToken = "xapp-3";
	credentials.teamId = "T1";
	credentials.teamName = "Acme";
	credentials.userId = "U1";
	credentials.userName = "me";
	credentials.url = "https://acme.slack.com/";
	credentials.apiBase = "http://127.0.0.1:1/api/";
	json j = credentials.toJson();
	CHECK_EQ(j["app_token"].get<std::string>(), std::string("xapp-3"));
	Credentials copy = Credentials::fromJson(j);
	CHECK_EQ(copy.toJson().dump(), j.dump());

	std::string path = std::string(NATTER_TEST_TMP_DIR) + "/credentials.json";
	REQUIRE(credentials.save(path).ok());
	struct stat st;
	REQUIRE(::stat(path.c_str(), &st) == 0);
	CHECK_EQ(st.st_mode & 0777, 0600u);
	Result<Credentials> loaded = Credentials::load(path);
	REQUIRE(loaded.ok());
	CHECK_EQ(loaded->token, std::string("xoxc-1"));
	CHECK_EQ(loaded->apiBase, std::string("http://127.0.0.1:1/api/"));
	std::remove(path.c_str());
	CHECK(!Credentials::load(path).ok());
}

// ---- sign-in helpers ----------------------------------------------------------------------

TEST(signin_extract_tokens_from_html)
{
	std::vector<std::string> tokens = signin::extractTokensFromHtml(
		testing::fixture("workspace_page.html"));
	REQUIRE(tokens.size() == 2);
	CHECK_EQ(tokens[0], std::string("xoxc-1111-2222-3333-4444-5555abcdef"));
	CHECK_EQ(tokens[1], std::string("xoxc-9999-8888-7777-ffff"));

	CHECK(signin::extractTokensFromHtml("<html>\"api_token\":null</html>").empty());
	std::vector<std::string> escaped = signin::extractTokensFromHtml(
		R"(x = "{\"api_token\":\"xoxc-55-66-77-88\"}")");
	REQUIRE(escaped.size() == 1);
	CHECK_EQ(escaped[0], std::string("xoxc-55-66-77-88"));
}


TEST(signin_parse_local_config)
{
	Result<std::vector<signin::LocalConfigTeam>> teams
		= signin::parseLocalConfig(testing::fixture("local_config.json"));
	REQUIRE(teams.ok());
	REQUIRE(teams->size() == 2);   // the signed-out team has no token
	CHECK_EQ(teams->at(0).id, std::string("T0OTHER"));   // lastActiveTeamId first
	CHECK_EQ(teams->at(0).url, std::string("https://other-place.slack.com/"));
	CHECK_EQ(teams->at(0).enterpriseId, std::string("E01"));
	CHECK_EQ(teams->at(1).token, std::string("xoxc-1111-2222-3333-aaaa"));
	CHECK_EQ(teams->at(1).name, std::string("Natter Test"));

	// The value as a JSON string, the way some storage viewers copy it.
	json wrapped = testing::fixture("local_config.json");
	Result<std::vector<signin::LocalConfigTeam>> again
		= signin::parseLocalConfig(wrapped.dump());
	CHECK(again.ok() && again->size() == 2);

	CHECK(!signin::parseLocalConfig("{}").ok());
	CHECK(!signin::parseLocalConfig("not json").ok());
	Result<std::vector<signin::LocalConfigTeam>> none
		= signin::parseLocalConfig(R"({"teams":{"T1":{"name":"x"}}})");
	REQUIRE(!none.ok());
	CHECK(none.error().kind == ErrorKind::NotFound);
}


TEST(signin_fetch_token_ignores_status)
{
	FakeTransport fake;
	HttpResponse page;
	page.status = 403;   // Slack does this for the logged-in boot page
	page.body = testing::fixture("workspace_page.html");
	fake.add("https://natter-test.slack.com/", page);

	Credentials credentials;
	credentials.workspace = "natter-test";
	credentials.cookie = "xoxd-cookie/value";
	Result<std::string> token = signin::fetchSessionToken(fake, credentials);
	REQUIRE(token.ok());
	CHECK_EQ(token.value(), std::string("xoxc-1111-2222-3333-4444-5555abcdef"));
	const HttpRequest& request = fake.requests.at(0);
	CHECK_EQ(request.method, std::string("GET"));
	CHECK(request.followRedirects);
	CHECK_EQ(findHeader(request.headers, "Cookie").value_or(""),
		std::string("d=xoxd-cookie%2Fvalue"));

	FakeTransport stale;
	HttpResponse loggedOut;
	loggedOut.status = 200;
	loggedOut.body = "<html>\"api_token\":null</html>";
	stale.add("https://natter-test.slack.com/", loggedOut);
	Result<std::string> none = signin::fetchSessionToken(stale, credentials);
	REQUIRE(!none.ok());
	CHECK_EQ(none.error().code, std::string("token_not_found"));

	credentials.cookie.clear();
	CHECK(!signin::fetchSessionToken(fake, credentials).ok());
}


TEST(signin_with_cookie_validates)
{
	FakeTransport fake;
	HttpResponse page;
	page.status = 200;
	page.body = testing::fixture("workspace_page.html");
	fake.add("https://natter-test.slack.com/", page);
	fake.addFixture("auth.test", "auth.test.json");

	Result<Credentials> credentials = signin::signInWithCookie(fake, "natter-test",
		"xoxd-abc");
	REQUIRE(credentials.ok());
	CHECK_EQ(credentials->token, std::string("xoxc-1111-2222-3333-4444-5555abcdef"));
	CHECK_EQ(credentials->teamId, std::string("T0NATTER"));
	CHECK_EQ(credentials->userId, std::string("U03SELF"));
	CHECK_EQ(credentials->url, std::string("https://natter-test.slack.com/"));
	const HttpRequest check = fake.calls("auth.test").at(0);
	CHECK_EQ(check.url, std::string("https://natter-test.slack.com/api/auth.test"));
	CHECK_EQ(FakeTransport::field(check, "token"),
		std::string("xoxc-1111-2222-3333-4444-5555abcdef"));
	CHECK_EQ(findHeader(check.headers, "Cookie").value_or(""), std::string("d=xoxd-abc"));

	FakeTransport rejected;
	rejected.add("https://natter-test.slack.com/", page);
	rejected.addError("auth.test", "invalid_auth");
	Result<Credentials> bad = signin::signInWithCookie(rejected, "natter-test", "xoxd-old");
	REQUIRE(!bad.ok());
	CHECK(bad.error().isAuth());
}
