// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "natter/http.h"
#include "natter/models.h"
#include "natter/result.h"
#include "natter/util.h"

namespace natter {

enum class AuthMode {
	None,
	Session,     // xoxc- token + the browser's d cookie (unofficial)
	UserToken,   // xoxp- OAuth user token
	BotToken,    // xoxb- bot token (works for most read calls)
};

const char* authModeName(AuthMode mode);

struct Credentials {
	std::string token;       // xoxc-, xoxp- or xoxb-
	std::string cookie;      // the d cookie value (xoxd-...), session mode
	std::string workspace;   // "acme", "acme.slack.com" or a URL
	std::string appToken;    // xapp- for Socket Mode (optional)

	// Filled in by validate()/auth.test; informational.
	std::string teamId;
	std::string teamName;
	std::string userId;
	std::string userName;
	std::string url;         // https://acme.slack.com/

	// Test and proxy hooks: override where API calls and the workspace page
	// go ("http://127.0.0.1:8080/api/"). Empty means the real Slack.
	std::string apiBase;

	AuthMode mode() const;
	bool hasAppToken() const { return startsWith(appToken, "xapp-"); }

	// "acme.slack.com" from whatever was entered, or from url.
	std::string workspaceHost() const;
	// https://acme.slack.com/ (session) or https://slack.com/ .
	std::string workspaceUrl() const;
	// Where Web API methods are posted, ending in '/'.
	std::string apiUrl() const;
	// The Cookie header value ("d=xoxd-..."), empty outside session mode.
	std::string cookieHeader() const;

	// Field names: token, cookie, workspace, app_token, team_id, team_name,
	// user_id, user_name, url, api_base.
	json toJson() const;
	static Credentials fromJson(const json& j);
	static Result<Credentials> load(const std::string& path);
	Status save(const std::string& path) const;   // written with mode 0600
};

// Slack stores the d cookie percent-encoded. People often paste the decoded
// form from a browser's cookie viewer; encode '+', '/' and '=' in that case.
std::string normalizeCookie(std::string_view cookie);

// "acme", "acme.slack.com", "https://acme.slack.com/messages" -> "acme.slack.com"
std::string normalizeWorkspaceHost(std::string_view workspace);

// ---- sign-in helpers ------------------------------------------------------------

namespace signin {

// All distinct xoxc- tokens in a workspace page, the "api_token" one first.
std::vector<std::string> extractTokensFromHtml(std::string_view html);

struct LocalConfigTeam {
	std::string id;
	std::string name;
	std::string domain;
	std::string url;
	std::string token;
	std::string enterpriseId;
};

// Parse the Slack web client's localStorage "localConfig_v2" value
// ({"teams": {"T..": {"token": "xoxc-..", "url": .., "name": ..}}}).
Result<std::vector<LocalConfigTeam>> parseLocalConfig(std::string_view text);

// GET the workspace page with the d cookie (following redirects) and scrape
// the session token. Slack answers the logged-in page with a 403 at times,
// so the status is ignored: only the body matters.
Result<std::string> fetchSessionToken(HttpTransport& transport,
	const Credentials& credentials);

// Check the credentials with auth.test and fill in team/user/url fields.
Result<AuthInfo> validate(HttpTransport& transport, Credentials& credentials);

// Cookie + workspace -> complete, validated session credentials.
Result<Credentials> signInWithCookie(HttpTransport& transport,
	std::string workspace, std::string cookie, std::string apiBase = {});

}  // namespace signin

}  // namespace natter
