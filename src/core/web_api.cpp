// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT

#include "natter/web_api.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

namespace natter {

namespace {

std::string
stripColons(const std::string& name)
{
	std::string text = name;
	if (startsWith(text, ":"))
		text.erase(0, 1);
	if (endsWith(text, ":"))
		text.pop_back();
	return text;
}


std::string
fileNameOf(const std::string& path)
{
	size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}


HistoryPage
parseHistoryPage(const json& body, const std::string& channel)
{
	HistoryPage page;
	for (const json& item : jArr(body, "messages"))
		page.messages.push_back(Message::fromJson(item, channel));
	page.hasMore = jBool(body, "has_more");
	page.nextCursor = jStr(jObj(body, "response_metadata"), "next_cursor");
	return page;
}


void
addCountsEntries(const json& array, UnreadCounts& counts, bool isDirect)
{
	for (const json& item : array) {
		UnreadInfo info;
		info.channel = jStr(item, "id");
		if (info.channel.empty())
			continue;
		info.lastRead = jStr(item, "last_read");
		const json& latest = item.contains("latest") ? item["latest"] : json();
		if (latest.is_string())
			info.latestTs = latest.get<std::string>();
		else if (latest.is_object())
			info.latestTs = jStr(latest, "ts");
		info.mentionCount = static_cast<int>(jInt(item, "mention_count_display",
			jInt(item, "mention_count")));
		int dmCount = static_cast<int>(jInt(item, "dm_count"));
		info.unreadCount = static_cast<int>(jInt(item, "unread_count_display",
			jInt(item, "unread_count")));
		if (dmCount > info.unreadCount)
			info.unreadCount = dmCount;
		info.hasUnreads = jBool(item, "has_unreads", info.unreadCount > 0);
		if (info.hasUnreads && info.unreadCount == 0) {
			// client.counts only says "some" for channels; a DM's count is
			// its mention count.
			info.unreadCount = isDirect ? std::max(1, info.mentionCount) : 1;
		}
		if (isDirect && info.mentionCount < dmCount)
			info.mentionCount = dmCount;
		counts.channels.push_back(info);
	}
}

}  // namespace


WebApi::WebApi(std::shared_ptr<HttpTransport> transport, Credentials credentials,
	ApiOptions options)
	:
	fTransport(std::move(transport)),
	fCredentials(std::move(credentials)),
	fOptions(std::move(options))
{
	if (!fTransport)
		fTransport = defaultTransport();
}


void
WebApi::cancel()
{
	fCancelled = true;
}


void
WebApi::resetCancel()
{
	fCancelled = false;
}


void
WebApi::addAuth(HttpRequest& request, bool useAppToken) const
{
	if (useAppToken) {
		request.headers.emplace_back("Authorization", "Bearer " + fCredentials.appToken);
		return;
	}
	if (fCredentials.mode() == AuthMode::Session) {
		// The web client's scheme: the token as a form field, the d cookie
		// as the browser would send it.
		if (request.bodyKind == BodyKind::Form)
			request.form.insert(request.form.begin(), {"token", fCredentials.token});
		else
			request.headers.emplace_back("Authorization", "Bearer " + fCredentials.token);
		std::string cookie = fCredentials.cookieHeader();
		if (!cookie.empty())
			request.headers.emplace_back("Cookie", cookie);
		return;
	}
	if (!fCredentials.token.empty())
		request.headers.emplace_back("Authorization", "Bearer " + fCredentials.token);
}


HttpRequest
WebApi::buildRequest(const std::string& method, const Params& params,
	bool useAppToken) const
{
	HttpRequest request = HttpRequest::postForm(fCredentials.apiUrl() + method, params);
	request.timeoutMs = fOptions.timeoutMs;
	addAuth(request, useAppToken);
	return request;
}


RetryPolicy
WebApi::retryPolicy(const std::string& method) const
{
	RetryPolicy policy;
	policy.maxRateLimitRetries = fOptions.maxRateLimitRetries;
	policy.maxWaitSeconds = fOptions.maxRetryWaitSeconds;
	if (fOptions.onRateLimited) {
		auto hook = fOptions.onRateLimited;
		policy.onRateLimited = [hook, method](const std::string&, int seconds) {
			hook(method, seconds);
		};
	}
	if (fOptions.sleeper) {
		auto sleeper = fOptions.sleeper;
		policy.sleeper = [this, sleeper](int seconds) {
			return !fCancelled && sleeper(seconds);
		};
	} else {
		policy.sleeper = [this](int seconds) {
			auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
			while (std::chrono::steady_clock::now() < until) {
				if (fCancelled)
					return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			return !fCancelled.load();
		};
	}
	return policy;
}


Result<json>
parseApiResponse(const HttpResponse& response)
{
	if (!response.transportOk())
		return Error(ErrorKind::Transport, "network", response.transportError);

	json body = parseJson(response.body);
	if (!body.is_object()) {
		if (response.status == 429) {
			Error error(ErrorKind::RateLimited, "ratelimited", {}, 429);
			error.retryAfterSeconds = std::max(0, response.retryAfterSeconds());
			return error;
		}
		if (!response.success()) {
			return Error(ErrorKind::Http, "http_" + std::to_string(response.status),
				response.body.substr(0, 200), response.status);
		}
		return Error(ErrorKind::Parse, "invalid_json", response.body.substr(0, 200),
			response.status);
	}
	if (!jBool(body, "ok", false)) {
		std::string code = jStr(body, "error", "unknown_error");
		Error error(classifySlackError(code), code, {}, response.status);
		std::string needed = jStr(body, "needed");
		if (!needed.empty())
			error.message = "needs " + needed;
		const json& metadata = jObj(body, "response_metadata");
		const json& messages = jArr(metadata, "messages");
		if (messages.is_array() && !messages.empty() && messages[0].is_string()) {
			if (!error.message.empty())
				error.message += "; ";
			error.message += messages[0].get<std::string>();
		}
		if (response.status == 429 || error.kind == ErrorKind::RateLimited)
			error.retryAfterSeconds = std::max(0, response.retryAfterSeconds());
		return error;
	}
	return body;
}


Result<json>
WebApi::perform(const std::string& method, const HttpRequest& request) const
{
	if (fCancelled)
		return Error(ErrorKind::Cancelled, "cancelled");
	HttpResponse response = performWithRetry(*fTransport, request, retryPolicy(method));
	Result<json> result = parseApiResponse(response);
	if (!result) {
		log(result.error().kind == ErrorKind::Transport ? LogLevel::Warning
				: LogLevel::Debug,
			method + " failed: " + result.error().describe());
	}
	return result;
}


Result<json>
WebApi::call(const std::string& method, Params params) const
{
	return perform(method, buildRequest(method, params));
}


Result<json>
WebApi::callWithAppToken(const std::string& method, Params params) const
{
	if (!fCredentials.hasAppToken())
		return Error(ErrorKind::InvalidArgument, "missing_app_token");
	return perform(method, buildRequest(method, params, true));
}


Status
WebApi::paginate(const std::string& method, Params params,
	const std::function<bool(const json& page)>& onPage) const
{
	bool hasLimit = false;
	for (const auto& [key, value] : params) {
		if (key == "limit")
			hasLimit = true;
	}
	if (!hasLimit)
		params.emplace_back("limit", std::to_string(fOptions.pageLimit));

	std::string cursor;
	for (int page = 0; page < fOptions.maxPages; page++) {
		Params pageParams = params;
		if (!cursor.empty())
			pageParams.emplace_back("cursor", cursor);
		Result<json> body = call(method, pageParams);
		if (!body)
			return body.error();
		if (!onPage(body.value()))
			return {};
		cursor = jStr(jObj(body.value(), "response_metadata"), "next_cursor");
		if (cursor.empty())
			return {};
	}
	log(LogLevel::Warning, method + ": stopped after maxPages pages");
	return {};
}


Result<json>
WebApi::collect(const std::string& method, Params params, const char* key) const
{
	json all = json::array();
	Status status = paginate(method, std::move(params), [&all, key](const json& page) {
		for (const json& item : jArr(page, key))
			all.push_back(item);
		return true;
	});
	if (!status)
		return status.error();
	return all;
}

// ---- auth, team, users ----------------------------------------------------------

Result<AuthInfo>
WebApi::authTest() const
{
	Result<json> body = call("auth.test");
	if (!body)
		return body.error();
	return AuthInfo::fromJson(body.value());
}


Result<Team>
WebApi::teamInfo(const std::string& teamId) const
{
	Params params;
	if (!teamId.empty())
		params.emplace_back("team", teamId);
	Result<json> body = call("team.info", params);
	if (!body)
		return body.error();
	return Team::fromJson(jObj(body.value(), "team"));
}


Result<std::vector<User>>
WebApi::usersList() const
{
	Result<json> members = collect("users.list", {}, "members");
	if (!members)
		return members.error();
	std::vector<User> users;
	users.reserve(members->size());
	for (const json& item : members.value())
		users.push_back(User::fromJson(item));
	return users;
}


Result<User>
WebApi::usersInfo(const std::string& userId) const
{
	Result<json> body = call("users.info", {{"user", userId}});
	if (!body)
		return body.error();
	return User::fromJson(jObj(body.value(), "user"));
}


Result<Presence>
WebApi::usersGetPresence(const std::string& userId) const
{
	Params params;
	if (!userId.empty())
		params.emplace_back("user", userId);
	Result<json> body = call("users.getPresence", params);
	if (!body)
		return body.error();
	return Presence::fromJson(body.value());
}

// ---- conversations ------------------------------------------------------------------

Result<std::vector<Channel>>
WebApi::conversationsList(const std::string& types, bool excludeArchived) const
{
	Params params = {{"types", types},
		{"exclude_archived", excludeArchived ? "true" : "false"}};
	if (!fCredentials.teamId.empty() && fCredentials.mode() != AuthMode::Session)
		params.emplace_back("team_id", fCredentials.teamId);
	Result<json> items = collect("conversations.list", params, "channels");
	if (!items)
		return items.error();
	std::vector<Channel> channels;
	channels.reserve(items->size());
	for (const json& item : items.value()) {
		Channel channel = Channel::fromJson(item);
		if (excludeArchived && channel.isArchived)
			continue;
		channels.push_back(std::move(channel));
	}
	return channels;
}


Result<Channel>
WebApi::conversationsInfo(const std::string& channel) const
{
	Result<json> body = call("conversations.info",
		{{"channel", channel}, {"include_num_members", "true"}});
	if (!body)
		return body.error();
	return Channel::fromJson(jObj(body.value(), "channel"));
}


Result<HistoryPage>
WebApi::conversationsHistory(const std::string& channel,
	const HistoryOptions& options) const
{
	Params params = {{"channel", channel}, {"limit", std::to_string(options.limit)}};
	if (!options.cursor.empty())
		params.emplace_back("cursor", options.cursor);
	if (!options.latest.empty())
		params.emplace_back("latest", options.latest);
	if (!options.oldest.empty())
		params.emplace_back("oldest", options.oldest);
	if (options.inclusive)
		params.emplace_back("inclusive", "true");
	Result<json> body = call("conversations.history", params);
	if (!body)
		return body.error();
	return parseHistoryPage(body.value(), channel);
}


Result<HistoryPage>
WebApi::conversationsReplies(const std::string& channel, const std::string& threadTs,
	const HistoryOptions& options) const
{
	Params params = {{"channel", channel}, {"ts", threadTs},
		{"limit", std::to_string(options.limit)}};
	if (!options.cursor.empty())
		params.emplace_back("cursor", options.cursor);
	if (!options.latest.empty())
		params.emplace_back("latest", options.latest);
	if (!options.oldest.empty())
		params.emplace_back("oldest", options.oldest);
	if (options.inclusive)
		params.emplace_back("inclusive", "true");
	Result<json> body = call("conversations.replies", params);
	if (!body)
		return body.error();
	return parseHistoryPage(body.value(), channel);
}


Status
WebApi::conversationsMark(const std::string& channel, const std::string& ts) const
{
	Result<json> body = call("conversations.mark", {{"channel", channel}, {"ts", ts}});
	if (!body)
		return body.error();
	return {};
}


Result<std::vector<std::string>>
WebApi::conversationsMembers(const std::string& channel) const
{
	Result<json> items = collect("conversations.members", {{"channel", channel}},
		"members");
	if (!items)
		return items.error();
	std::vector<std::string> members;
	for (const json& item : items.value()) {
		if (item.is_string())
			members.push_back(item.get<std::string>());
	}
	return members;
}


Result<Channel>
WebApi::conversationsOpen(const std::vector<std::string>& users) const
{
	if (users.empty())
		return Error(ErrorKind::InvalidArgument, "no_users");
	std::string list;
	for (const std::string& user : users) {
		if (!list.empty())
			list += ',';
		list += user;
	}
	Result<json> body = call("conversations.open",
		{{"users", list}, {"return_im", "true"}});
	if (!body)
		return body.error();
	Channel channel = Channel::fromJson(jObj(body.value(), "channel"));
	// A bare {"id": "D.."} still means a direct message.
	if (!channel.isIm && !channel.isMpim && startsWith(channel.id, "D")) {
		channel.isIm = users.size() == 1;
		channel.isMpim = users.size() > 1;
	}
	if (channel.isIm && channel.imUser.empty() && users.size() == 1)
		channel.imUser = users.front();
	channel.isMember = true;
	return channel;
}

// ---- messages ------------------------------------------------------------------------

Result<Message>
WebApi::chatPostMessage(const std::string& channel, const std::string& text,
	const PostOptions& options) const
{
	Params params = {{"channel", channel}, {"text", text}};
	if (!options.threadTs.empty()) {
		params.emplace_back("thread_ts", options.threadTs);
		if (options.replyBroadcast)
			params.emplace_back("reply_broadcast", "true");
	}
	if (!options.blocks.is_null())
		params.emplace_back("blocks", options.blocks.dump());
	if (options.unfurlLinks)
		params.emplace_back("unfurl_links", *options.unfurlLinks ? "true" : "false");
	if (options.unfurlMedia)
		params.emplace_back("unfurl_media", *options.unfurlMedia ? "true" : "false");
	if (options.linkNames)
		params.emplace_back("link_names", "true");
	Result<json> body = call("chat.postMessage", params);
	if (!body)
		return body.error();
	std::string realChannel = jStr(body.value(), "channel", channel);
	Message message = Message::fromJson(jObj(body.value(), "message"), realChannel);
	message.channel = realChannel;
	if (message.ts.empty())
		message.ts = jStr(body.value(), "ts");
	if (message.text.empty() && message.blocks.is_null())
		message.text = text;
	if (message.threadTs.empty())
		message.threadTs = options.threadTs;
	return message;
}


Result<Message>
WebApi::chatUpdate(const std::string& channel, const std::string& ts,
	const std::string& text, const json& blocks) const
{
	Params params = {{"channel", channel}, {"ts", ts}, {"text", text}};
	if (!blocks.is_null())
		params.emplace_back("blocks", blocks.dump());
	Result<json> body = call("chat.update", params);
	if (!body)
		return body.error();
	std::string realChannel = jStr(body.value(), "channel", channel);
	Message message = Message::fromJson(jObj(body.value(), "message"), realChannel);
	message.channel = realChannel;
	message.ts = jStr(body.value(), "ts", ts);
	if (message.text.empty())
		message.text = jStr(body.value(), "text", text);
	return message;
}


Status
WebApi::chatDelete(const std::string& channel, const std::string& ts) const
{
	Result<json> body = call("chat.delete", {{"channel", channel}, {"ts", ts}});
	if (!body)
		return body.error();
	return {};
}


Status
WebApi::reactionsAdd(const std::string& channel, const std::string& ts,
	const std::string& name) const
{
	Result<json> body = call("reactions.add",
		{{"channel", channel}, {"timestamp", ts}, {"name", stripColons(name)}});
	if (!body)
		return body.error();
	return {};
}


Status
WebApi::reactionsRemove(const std::string& channel, const std::string& ts,
	const std::string& name) const
{
	Result<json> body = call("reactions.remove",
		{{"channel", channel}, {"timestamp", ts}, {"name", stripColons(name)}});
	if (!body)
		return body.error();
	return {};
}

// ---- emoji and search -------------------------------------------------------------------

Result<std::map<std::string, std::string>>
WebApi::emojiList() const
{
	Result<json> body = call("emoji.list");
	if (!body)
		return body.error();
	std::map<std::string, std::string> emoji;
	const json& map = jObj(body.value(), "emoji");
	for (auto it = map.begin(); it != map.end(); ++it) {
		if (it->is_string())
			emoji[it.key()] = it->get<std::string>();
	}
	return emoji;
}


Result<SearchPage>
WebApi::searchMessages(const std::string& query, const SearchOptions& options) const
{
	Params params = {{"query", query}, {"count", std::to_string(options.count)},
		{"page", std::to_string(options.page)}, {"sort", options.sort},
		{"sort_dir", options.sortDir},
		{"highlight", options.highlight ? "true" : "false"}};
	Result<json> body = call("search.messages", params);
	if (!body)
		return body.error();

	SearchPage page;
	const json& messages = jObj(body.value(), "messages");
	const json& paging = jObj(messages, "paging");
	page.total = static_cast<int>(jInt(messages, "total", jInt(paging, "total")));
	page.page = static_cast<int>(jInt(paging, "page", options.page));
	page.pages = static_cast<int>(jInt(paging, "pages"));
	for (const json& item : jArr(messages, "matches")) {
		SearchMatch match;
		const json& channel = jObj(item, "channel");
		std::string channelId = jStr(channel, "id");
		match.message = Message::fromJson(item, channelId);
		match.message.channel = channelId;
		match.channelName = jStr(channel, "name");
		match.teamId = jStr(item, "team");
		page.matches.push_back(std::move(match));
	}
	return page;
}

// ---- files ---------------------------------------------------------------------------------

Result<UploadedFile>
WebApi::uploadFile(const std::string& channel, const std::string& fileName,
	const std::string& data, const UploadOptions& options) const
{
	Params params = {{"filename", fileName}, {"length", std::to_string(data.size())}};
	if (!options.altText.empty())
		params.emplace_back("alt_txt", options.altText);
	Result<json> ticket = call("files.getUploadURLExternal", params);
	if (!ticket)
		return ticket.error();
	std::string uploadUrl = jStr(ticket.value(), "upload_url");
	std::string fileId = jStr(ticket.value(), "file_id");
	if (uploadUrl.empty() || fileId.empty())
		return Error(ErrorKind::Parse, "no_upload_url");

	// The upload URL is pre-signed: no token, no cookie.
	HttpRequest upload;
	upload.method = "POST";
	upload.url = uploadUrl;
	upload.bodyKind = BodyKind::Raw;
	upload.body = data;
	upload.timeoutMs = fOptions.uploadTimeoutMs;
	upload.progress = options.progress;
	HttpResponse response = performWithRetry(*fTransport, upload,
		retryPolicy("files.upload"));
	if (!response.transportOk())
		return Error(ErrorKind::Transport, "upload_failed", response.transportError);
	if (!response.success()) {
		return Error(ErrorKind::Http, "upload_failed", response.body.substr(0, 200),
			response.status);
	}

	json files = json::array();
	files.push_back(json{{"id", fileId},
		{"title", options.title.empty() ? fileName : options.title}});
	Params complete = {{"files", files.dump()}};
	if (!channel.empty())
		complete.emplace_back("channel_id", channel);
	if (!options.initialComment.empty())
		complete.emplace_back("initial_comment", options.initialComment);
	if (!options.threadTs.empty())
		complete.emplace_back("thread_ts", options.threadTs);
	Result<json> done = call("files.completeUploadExternal", complete);
	if (!done)
		return done.error();

	UploadedFile uploaded;
	uploaded.id = fileId;
	uploaded.title = options.title.empty() ? fileName : options.title;
	const json& list = jArr(done.value(), "files");
	if (list.is_array() && !list.empty()) {
		uploaded.raw = list[0];
		uploaded.title = jStr(list[0], "title", uploaded.title);
	}
	return uploaded;
}


Result<UploadedFile>
WebApi::uploadFileFromPath(const std::string& channel, const std::string& path,
	const UploadOptions& options) const
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return Error(ErrorKind::InvalidArgument, "cannot_open", path);
	std::stringstream buffer;
	buffer << in.rdbuf();
	return uploadFile(channel, fileNameOf(path), buffer.str(), options);
}


Result<std::string>
WebApi::download(const std::string& url) const
{
	HttpRequest request = HttpRequest::get(url);
	request.followRedirects = true;
	request.timeoutMs = fOptions.uploadTimeoutMs;
	if (!fCredentials.token.empty())
		request.headers.emplace_back("Authorization", "Bearer " + fCredentials.token);
	std::string cookie = fCredentials.cookieHeader();
	if (!cookie.empty())
		request.headers.emplace_back("Cookie", cookie);
	HttpResponse response = performWithRetry(*fTransport, request,
		retryPolicy("download"));
	if (!response.transportOk())
		return Error(ErrorKind::Transport, "network", response.transportError);
	if (response.status == 401 || response.status == 403)
		return Error(ErrorKind::Auth, "download_denied", {}, response.status);
	if (!response.success())
		return Error(ErrorKind::Http, "http_" + std::to_string(response.status), {},
			response.status);
	// Slack serves its sign-in page, not an error, when the auth is wrong.
	std::string type = toLower(response.header("Content-Type").value_or(""));
	if (startsWith(type, "text/html") && !endsWith(toLower(url), ".html"))
		return Error(ErrorKind::Auth, "download_login_page", {}, response.status);
	return std::move(response.body);
}


Status
WebApi::downloadToFile(const std::string& url, const std::string& path,
	std::function<bool(uint64_t, uint64_t)> progress) const
{
	std::string temporary = path + ".part";
	std::FILE* file = std::fopen(temporary.c_str(), "wb");
	if (file == nullptr)
		return Error(ErrorKind::InvalidArgument, "cannot_write", path);

	HttpRequest request = HttpRequest::get(url);
	request.followRedirects = true;
	request.timeoutMs = 0;
	request.progress = std::move(progress);
	bool writeFailed = false;
	request.sink = [file, &writeFailed](const char* data, size_t size) {
		if (std::fwrite(data, 1, size, file) != size) {
			writeFailed = true;
			return false;
		}
		return true;
	};
	if (!fCredentials.token.empty())
		request.headers.emplace_back("Authorization", "Bearer " + fCredentials.token);
	std::string cookie = fCredentials.cookieHeader();
	if (!cookie.empty())
		request.headers.emplace_back("Cookie", cookie);

	HttpResponse response = fTransport->perform(request);
	std::fclose(file);
	auto fail = [&temporary](Error error) -> Status {
		std::remove(temporary.c_str());
		return error;
	};
	if (writeFailed)
		return fail(Error(ErrorKind::InvalidArgument, "cannot_write", path));
	if (response.aborted)
		return fail(Error(ErrorKind::Cancelled, "cancelled"));
	if (!response.transportOk())
		return fail(Error(ErrorKind::Transport, "network", response.transportError));
	if (!response.success()) {
		return fail(Error(response.status == 401 || response.status == 403
				? ErrorKind::Auth : ErrorKind::Http,
			"http_" + std::to_string(response.status), {}, response.status));
	}
	if (std::rename(temporary.c_str(), path.c_str()) != 0)
		return fail(Error(ErrorKind::InvalidArgument, "cannot_write", path));
	return {};
}

// ---- unread counts ------------------------------------------------------------------------

Result<UnreadCounts>
WebApi::countsFromClientCounts() const
{
	Result<json> body = call("client.counts", {{"thread_counts_by_channel", "true"}});
	if (!body)
		return body.error();
	UnreadCounts counts;
	counts.source = CountsSource::ClientCounts;
	addCountsEntries(jArr(body.value(), "channels"), counts, false);
	addCountsEntries(jArr(body.value(), "mpims"), counts, true);
	addCountsEntries(jArr(body.value(), "ims"), counts, true);
	return counts;
}


Result<UnreadCounts>
WebApi::countsFromUsersCounts() const
{
	Result<json> body = call("users.counts",
		{{"mpim_aware", "true"}, {"only_relevant_ims", "true"}, {"simple_unreads", "true"}});
	if (!body)
		return body.error();
	UnreadCounts counts;
	counts.source = CountsSource::UsersCounts;
	addCountsEntries(jArr(body.value(), "channels"), counts, false);
	addCountsEntries(jArr(body.value(), "groups"), counts, false);
	addCountsEntries(jArr(body.value(), "mpims"), counts, true);
	addCountsEntries(jArr(body.value(), "ims"), counts, true);
	return counts;
}


Result<UnreadCounts>
WebApi::unreadCounts(const std::vector<std::string>& fallbackChannels) const
{
	auto giveUp = [](const Error& error) {
		return error.kind == ErrorKind::Unsupported || error.kind == ErrorKind::Api
			|| error.kind == ErrorKind::NotFound || error.kind == ErrorKind::Http;
	};

	if (mode() == AuthMode::Session && !fClientCountsUnavailable) {
		Result<UnreadCounts> counts = countsFromClientCounts();
		if (counts)
			return counts;
		if (!giveUp(counts.error()))
			return counts.error();
		fClientCountsUnavailable = true;
		log(LogLevel::Info, "client.counts unavailable: " + counts.error().describe());
	}
	if (!fUsersCountsUnavailable) {
		Result<UnreadCounts> counts = countsFromUsersCounts();
		if (counts)
			return counts;
		if (!giveUp(counts.error()))
			return counts.error();
		fUsersCountsUnavailable = true;
		log(LogLevel::Info, "users.counts unavailable: " + counts.error().describe());
	}

	UnreadCounts counts;
	counts.source = CountsSource::ConversationsInfo;
	for (const std::string& id : fallbackChannels) {
		Result<Channel> channel = conversationsInfo(id);
		if (!channel) {
			if (channel.error().kind == ErrorKind::Auth
				|| channel.error().kind == ErrorKind::Transport
				|| channel.error().kind == ErrorKind::Cancelled)
				return channel.error();
			continue;
		}
		UnreadInfo info;
		info.channel = id;
		info.lastRead = channel->lastRead;
		info.latestTs = channel->latestTs;
		info.unreadCount = channel->unreadCount;
		info.mentionCount = channel->isDirect() ? channel->unreadCount
			: channel->mentionCount;
		info.hasUnreads = channel->unreadCount > 0
			|| (!channel->latestTs.empty() && !channel->lastRead.empty()
				&& compareTs(channel->latestTs, channel->lastRead) > 0);
		counts.channels.push_back(info);
	}
	return counts;
}

// ---- real time ------------------------------------------------------------------------------

Result<ConnectInfo>
WebApi::rtmConnect() const
{
	Result<json> body = call("rtm.connect",
		{{"batch_presence_aware", "1"}, {"presence_sub", "true"}});
	if (!body)
		return body.error();
	ConnectInfo info;
	info.url = jStr(body.value(), "url");
	const json& self = jObj(body.value(), "self");
	info.selfId = jStr(self, "id");
	info.selfName = jStr(self, "name");
	const json& team = jObj(body.value(), "team");
	info.teamId = jStr(team, "id");
	info.teamDomain = jStr(team, "domain");
	if (info.url.empty())
		return Error(ErrorKind::Parse, "no_url");
	return info;
}


Result<ConnectInfo>
WebApi::appsConnectionsOpen() const
{
	Result<json> body = callWithAppToken("apps.connections.open");
	if (!body)
		return body.error();
	ConnectInfo info;
	info.url = jStr(body.value(), "url");
	if (info.url.empty())
		return Error(ErrorKind::Parse, "no_url");
	return info;
}

}  // namespace natter
