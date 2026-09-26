// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "natter/credentials.h"
#include "natter/http.h"
#include "natter/models.h"
#include "natter/result.h"

namespace natter {

using Params = FormFields;

struct ApiOptions {
	long timeoutMs = 30000;
	long uploadTimeoutMs = 600000;
	int pageLimit = 200;       // "limit" sent with cursor-paginated methods
	int maxPages = 200;        // safety cap for paginate()
	int maxRateLimitRetries = 3;
	int maxRetryWaitSeconds = 120;
	// Told about every 429 before waiting. Runs on the calling thread.
	std::function<void(const std::string& method, int seconds)> onRateLimited;
	// Replaces the sleep between 429 retries (tests). Return false to abort.
	std::function<bool(int seconds)> sleeper;
};

struct HistoryOptions {
	int limit = 100;
	std::string cursor;
	std::string latest;      // only messages before this ts
	std::string oldest;      // only messages after this ts
	bool inclusive = false;
};

struct PostOptions {
	std::string threadTs;
	bool replyBroadcast = false;
	json blocks;             // optional Block Kit array
	std::optional<bool> unfurlLinks;
	std::optional<bool> unfurlMedia;
	bool linkNames = true;   // turn @name and #channel into links
};

struct SearchOptions {
	int count = 20;
	int page = 1;
	std::string sort = "timestamp";   // or "score"
	std::string sortDir = "desc";
	bool highlight = false;
};

struct UploadOptions {
	std::string title;
	std::string initialComment;
	std::string threadTs;
	std::string altText;
	std::function<bool(uint64_t done, uint64_t total)> progress;
};

// Typed, synchronous Slack Web API client.
//
// Every method blocks until Slack answers, so call them from a worker thread
// (see Session::post or your own). All methods are const and thread-safe;
// one WebApi may be shared by any number of threads. Credentials are fixed at
// construction; build a new WebApi to switch accounts.
class WebApi {
public:
	WebApi(std::shared_ptr<HttpTransport> transport, Credentials credentials,
		ApiOptions options = {});

	const Credentials& credentials() const { return fCredentials; }
	AuthMode mode() const { return fCredentials.mode(); }
	HttpTransport& transport() const { return *fTransport; }

	// Abort pending rate-limit waits and fail further calls with Cancelled.
	void cancel();
	void resetCancel();
	bool cancelled() const { return fCancelled.load(); }

	// ---- generic ---------------------------------------------------------------

	// POST a method with form fields; ok:false becomes an Error.
	Result<json> call(const std::string& method, Params params = {}) const;
	// Same, authenticated with the app-level (xapp-) token.
	Result<json> callWithAppToken(const std::string& method, Params params = {}) const;

	// Follow response_metadata.next_cursor. onPage sees each page's body and
	// may return false to stop early.
	Status paginate(const std::string& method, Params params,
		const std::function<bool(const json& page)>& onPage) const;
	// Concatenate the array under `key` from every page.
	Result<json> collect(const std::string& method, Params params,
		const char* key) const;

	// ---- auth, team, users -----------------------------------------------------------

	Result<AuthInfo> authTest() const;
	Result<Team> teamInfo(const std::string& teamId = {}) const;
	Result<std::vector<User>> usersList() const;
	Result<User> usersInfo(const std::string& userId) const;
	Result<Presence> usersGetPresence(const std::string& userId) const;

	// ---- conversations -----------------------------------------------------------

	// All four types by default, archived ones excluded.
	Result<std::vector<Channel>> conversationsList(
		const std::string& types = "public_channel,private_channel,mpim,im",
		bool excludeArchived = true) const;
	Result<Channel> conversationsInfo(const std::string& channel) const;
	Result<HistoryPage> conversationsHistory(const std::string& channel,
		const HistoryOptions& options = {}) const;
	Result<HistoryPage> conversationsReplies(const std::string& channel,
		const std::string& threadTs, const HistoryOptions& options = {}) const;
	Status conversationsMark(const std::string& channel, const std::string& ts) const;
	Result<std::vector<std::string>> conversationsMembers(
		const std::string& channel) const;
	// Open (or find) a DM or group DM with these users.
	Result<Channel> conversationsOpen(const std::vector<std::string>& users) const;

	// ---- messages ------------------------------------------------------------------

	Result<Message> chatPostMessage(const std::string& channel,
		const std::string& text, const PostOptions& options = {}) const;
	Result<Message> chatUpdate(const std::string& channel, const std::string& ts,
		const std::string& text, const json& blocks = {}) const;
	Status chatDelete(const std::string& channel, const std::string& ts) const;
	// `name` may be given with or without colons.
	Status reactionsAdd(const std::string& channel, const std::string& ts,
		const std::string& name) const;
	Status reactionsRemove(const std::string& channel, const std::string& ts,
		const std::string& name) const;

	// ---- emoji, search -------------------------------------------------------------

	// name -> image URL, or "alias:<other name>".
	Result<std::map<std::string, std::string>> emojiList() const;
	Result<SearchPage> searchMessages(const std::string& query,
		const SearchOptions& options = {}) const;

	// ---- files ----------------------------------------------------------------------

	// files.getUploadURLExternal -> POST bytes -> files.completeUploadExternal.
	// An empty channel uploads privately.
	Result<UploadedFile> uploadFile(const std::string& channel,
		const std::string& fileName, const std::string& data,
		const UploadOptions& options = {}) const;
	Result<UploadedFile> uploadFileFromPath(const std::string& channel,
		const std::string& path, const UploadOptions& options = {}) const;

	// GET url_private(_download) with this account's auth.
	Result<std::string> download(const std::string& url) const;
	Status downloadToFile(const std::string& url, const std::string& path,
		std::function<bool(uint64_t, uint64_t)> progress = {}) const;

	// ---- unread counts ------------------------------------------------------------

	// Session mode asks client.counts first; any mode then tries users.counts,
	// then conversations.info for each of `fallbackChannels`. A method that
	// answers "not for this token" is not tried again by this WebApi.
	Result<UnreadCounts> unreadCounts(
		const std::vector<std::string>& fallbackChannels = {}) const;

	// ---- real time --------------------------------------------------------------------

	Result<ConnectInfo> rtmConnect() const;
	Result<ConnectInfo> appsConnectionsOpen() const;

	// Build the request for a method (exposed for tests and the CLI).
	HttpRequest buildRequest(const std::string& method, const Params& params,
		bool useAppToken = false) const;

private:
	Result<json> perform(const std::string& method, const HttpRequest& request) const;
	RetryPolicy retryPolicy(const std::string& method) const;
	void addAuth(HttpRequest& request, bool useAppToken) const;
	Result<UnreadCounts> countsFromClientCounts() const;
	Result<UnreadCounts> countsFromUsersCounts() const;

	std::shared_ptr<HttpTransport> fTransport;
	Credentials fCredentials;
	ApiOptions fOptions;
	std::atomic<bool> fCancelled{false};
	mutable std::atomic<bool> fClientCountsUnavailable{false};
	mutable std::atomic<bool> fUsersCountsUnavailable{false};
};

// Parse a whole Web API body: ok:false -> Error (with the kind from
// classifySlackError), not JSON -> Parse/Http error.
Result<json> parseApiResponse(const HttpResponse& response);

}  // namespace natter
