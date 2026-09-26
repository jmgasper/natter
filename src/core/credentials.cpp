// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT

#include "natter/credentials.h"
#include "natter/web_api.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>

namespace natter {

const char*
authModeName(AuthMode mode)
{
	switch (mode) {
		case AuthMode::None: return "none";
		case AuthMode::Session: return "session";
		case AuthMode::UserToken: return "user-token";
		case AuthMode::BotToken: return "bot-token";
	}
	return "?";
}


std::string
normalizeWorkspaceHost(std::string_view workspace)
{
	std::string text = trim(workspace);
	size_t scheme = text.find("://");
	if (scheme != std::string::npos)
		text = text.substr(scheme + 3);
	size_t end = text.find_first_of("/?#");
	if (end != std::string::npos)
		text = text.substr(0, end);
	text = toLower(text);
	if (text.empty())
		return {};
	// A bare name means <name>.slack.com; anything with a dot or a port is
	// taken as a host (Enterprise Grid, or a local mock).
	if (text.find('.') == std::string::npos && text.find(':') == std::string::npos)
		text += ".slack.com";
	return text;
}


std::string
normalizeCookie(std::string_view cookie)
{
	std::string text = trim(cookie);
	if (startsWith(text, "d="))
		text = text.substr(2);
	if (text.find('%') != std::string::npos)
		return text;
	std::string out;
	for (char c : text) {
		switch (c) {
			case '+': out += "%2B"; break;
			case '/': out += "%2F"; break;
			case '=': out += "%3D"; break;
			default: out += c;
		}
	}
	return out;
}

// ---- Credentials -------------------------------------------------------------------

AuthMode
Credentials::mode() const
{
	if (startsWith(token, "xoxc-"))
		return AuthMode::Session;
	if (startsWith(token, "xoxp-"))
		return AuthMode::UserToken;
	if (startsWith(token, "xoxb-"))
		return AuthMode::BotToken;
	return AuthMode::None;
}


std::string
Credentials::workspaceHost() const
{
	std::string host = normalizeWorkspaceHost(workspace);
	if (host.empty())
		host = normalizeWorkspaceHost(url);
	return host;
}


std::string
Credentials::workspaceUrl() const
{
	std::string host = workspaceHost();
	if (host.empty())
		return "https://slack.com/";
	return "https://" + host + "/";
}


std::string
Credentials::apiUrl() const
{
	if (!apiBase.empty()) {
		std::string base = apiBase;
		if (!endsWith(base, "/"))
			base += '/';
		return base;
	}
	// Session tokens resolve in the context of the host they are sent to;
	// on Enterprise Grid slack.com/api lands in the org and refuses
	// workspace methods, so use the workspace's own host like the web client.
	if (mode() == AuthMode::Session) {
		std::string host = workspaceHost();
		if (!host.empty())
			return "https://" + host + "/api/";
	}
	return "https://slack.com/api/";
}


std::string
Credentials::cookieHeader() const
{
	if (cookie.empty())
		return {};
	return "d=" + normalizeCookie(cookie);
}


json
Credentials::toJson() const
{
	json j = json::object();
	j["token"] = token;
	if (!cookie.empty())
		j["cookie"] = cookie;
	if (!workspace.empty())
		j["workspace"] = workspace;
	if (!appToken.empty())
		j["app_token"] = appToken;
	if (!teamId.empty())
		j["team_id"] = teamId;
	if (!teamName.empty())
		j["team_name"] = teamName;
	if (!userId.empty())
		j["user_id"] = userId;
	if (!userName.empty())
		j["user_name"] = userName;
	if (!url.empty())
		j["url"] = url;
	if (!apiBase.empty())
		j["api_base"] = apiBase;
	return j;
}


Credentials
Credentials::fromJson(const json& j)
{
	Credentials credentials;
	credentials.token = jStr(j, "token");
	credentials.cookie = jStr(j, "cookie");
	credentials.workspace = jStr(j, "workspace");
	credentials.appToken = jStr(j, "app_token");
	credentials.teamId = jStr(j, "team_id");
	credentials.teamName = jStr(j, "team_name");
	credentials.userId = jStr(j, "user_id");
	credentials.userName = jStr(j, "user_name");
	credentials.url = jStr(j, "url");
	credentials.apiBase = jStr(j, "api_base");
	return credentials;
}


Result<Credentials>
Credentials::load(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return Error(ErrorKind::InvalidArgument, "cannot_open", path);
	std::stringstream buffer;
	buffer << in.rdbuf();
	json j = parseJson(buffer.str());
	if (!j.is_object())
		return Error(ErrorKind::Parse, "bad_credentials_file", path);
	return fromJson(j);
}


Status
Credentials::save(const std::string& path) const
{
	std::string text = toJson().dump(2) + "\n";
	std::string temporary = path + ".tmp";
	int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return Error(ErrorKind::InvalidArgument, "cannot_write", std::strerror(errno));
	size_t written = 0;
	while (written < text.size()) {
		ssize_t n = ::write(fd, text.data() + written, text.size() - written);
		if (n <= 0) {
			::close(fd);
			::unlink(temporary.c_str());
			return Error(ErrorKind::InvalidArgument, "cannot_write", std::strerror(errno));
		}
		written += static_cast<size_t>(n);
	}
	::close(fd);
	if (::rename(temporary.c_str(), path.c_str()) != 0) {
		::unlink(temporary.c_str());
		return Error(ErrorKind::InvalidArgument, "cannot_write", std::strerror(errno));
	}
	return {};
}

// ---- sign-in -----------------------------------------------------------------------

namespace signin {

namespace {

bool
isTokenChar(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		|| (c >= '0' && c <= '9') || c == '-';
}


// The token that starts at text[pos] ("xoxc-..."), or empty.
std::string
tokenAt(std::string_view text, size_t pos)
{
	size_t end = pos + 5;
	while (end < text.size() && isTokenChar(text[end]))
		end++;
	if (end - pos < 12)
		return {};
	return std::string(text.substr(pos, end - pos));
}

}  // namespace


std::vector<std::string>
extractTokensFromHtml(std::string_view html)
{
	std::vector<std::string> tokens;
	auto add = [&tokens](const std::string& token) {
		if (!token.empty()
			&& std::find(tokens.begin(), tokens.end(), token) == tokens.end())
			tokens.push_back(token);
	};

	// The boot data names it: "api_token":"xoxc-...". Allow whitespace and
	// the JS-escaped quote forms seen in inline scripts.
	static const std::regex keyed(
		R"re(\\?"api_token\\?"\s*:\s*\\?"(xoxc-[A-Za-z0-9-]+))re");
	std::string text(html);
	for (auto it = std::sregex_iterator(text.begin(), text.end(), keyed);
			it != std::sregex_iterator(); ++it)
		add((*it)[1].str());

	size_t pos = 0;
	while ((pos = html.find("xoxc-", pos)) != std::string_view::npos) {
		add(tokenAt(html, pos));
		pos += 5;
	}
	return tokens;
}


Result<std::vector<LocalConfigTeam>>
parseLocalConfig(std::string_view text)
{
	json root = parseJson(text);
	// Values copied out of a browser's storage viewer are sometimes a JSON
	// string holding the JSON.
	if (root.is_string())
		root = parseJson(root.get<std::string>());
	if (!root.is_object())
		return Error(ErrorKind::Parse, "bad_local_config", "not a JSON object");

	const json& teams = root.contains("teams") ? root["teams"] : json();
	std::vector<LocalConfigTeam> out;
	auto addTeam = [&out](const std::string& key, const json& t) {
		if (!t.is_object())
			return;
		LocalConfigTeam team;
		team.id = jStr(t, "id", key);
		team.name = jStr(t, "name");
		team.domain = jStr(t, "domain");
		team.url = jStr(t, "url");
		team.token = jStr(t, "token");
		team.enterpriseId = jStr(t, "enterprise_id");
		if (team.url.empty() && !team.domain.empty())
			team.url = "https://" + team.domain + ".slack.com/";
		if (startsWith(team.token, "xoxc-"))
			out.push_back(std::move(team));
	};
	if (teams.is_object()) {
		for (auto it = teams.begin(); it != teams.end(); ++it)
			addTeam(it.key(), it.value());
	} else if (teams.is_array()) {
		for (const json& t : teams)
			addTeam({}, t);
	} else {
		return Error(ErrorKind::Parse, "bad_local_config", "no teams");
	}
	if (out.empty())
		return Error(ErrorKind::NotFound, "token_not_found", "no xoxc- tokens");

	// The last active team first: that is the one the user expects.
	std::string last = jStr(root, "lastActiveTeamId");
	std::stable_sort(out.begin(), out.end(),
		[&last](const LocalConfigTeam& a, const LocalConfigTeam& b) {
			return (a.id == last) > (b.id == last);
		});
	return out;
}


Result<std::string>
fetchSessionToken(HttpTransport& transport, const Credentials& credentials)
{
	if (credentials.cookie.empty())
		return Error(ErrorKind::InvalidArgument, "missing_cookie");
	std::string url;
	if (!credentials.apiBase.empty()) {
		// A mock server: the page lives beside its /api/.
		url = credentials.apiUrl();
		if (endsWith(url, "api/"))
			url.resize(url.size() - 4);
	} else {
		if (credentials.workspaceHost().empty())
			return Error(ErrorKind::InvalidArgument, "missing_workspace");
		url = credentials.workspaceUrl();
	}

	HttpRequest request = HttpRequest::get(url);
	request.followRedirects = true;
	request.headers.emplace_back("Cookie", credentials.cookieHeader());
	request.headers.emplace_back("Accept", "text/html,application/xhtml+xml");
	HttpResponse response = transport.perform(request);
	if (!response.transportOk() && response.body.empty())
		return Error(ErrorKind::Transport, "network", response.transportError);

	std::vector<std::string> tokens = extractTokensFromHtml(response.body);
	if (tokens.empty()) {
		return Error(ErrorKind::Auth, "token_not_found",
			"the workspace page has no session token; the d cookie may be stale",
			response.status);
	}
	return tokens.front();
}


Result<AuthInfo>
validate(HttpTransport& transport, Credentials& credentials)
{
	WebApi api(std::shared_ptr<HttpTransport>(&transport, [](HttpTransport*) {}),
		credentials);
	Result<AuthInfo> info = api.authTest();
	if (!info)
		return info;
	credentials.teamId = info->teamId;
	credentials.teamName = info->team;
	credentials.userId = info->userId;
	credentials.userName = info->user;
	if (!info->url.empty()) {
		credentials.url = info->url;
		if (credentials.workspace.empty())
			credentials.workspace = normalizeWorkspaceHost(info->url);
	}
	return info;
}


Result<Credentials>
signInWithCookie(HttpTransport& transport, std::string workspace,
	std::string cookie, std::string apiBase)
{
	Credentials credentials;
	credentials.workspace = std::move(workspace);
	credentials.cookie = normalizeCookie(cookie);
	credentials.apiBase = std::move(apiBase);
	Result<std::string> token = fetchSessionToken(transport, credentials);
	if (!token)
		return token.error();
	credentials.token = token.value();
	Result<AuthInfo> info = validate(transport, credentials);
	if (!info)
		return info.error();
	return credentials;
}

}  // namespace signin

}  // namespace natter
