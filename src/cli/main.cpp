// Natter - natter-cli, a command line driver for the core (manual testing).
// SPDX-License-Identifier: MIT

#include "natter/natter.h"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

using namespace natter;

namespace {

const char* kUsage = R"(usage: natter-cli [options] <command> [arguments]

Credentials (or put the same fields in a JSON file and pass --credentials):
  --token xoxc-...|xoxp-...   session or user token
  --cookie xoxd-...           the browser's d cookie (session tokens)
  --workspace NAME            acme, acme.slack.com or a URL
  --app-token xapp-...        app-level token for Socket Mode
  --credentials FILE          JSON: token, cookie, workspace, app_token, api_base
  --api-base URL              send API calls here instead of Slack (tests)
  --ca-file PEM               extra CA bundle for HTTPS and wss://
  --json                      print raw JSON where it applies
  --verbose                   log HTTP and real-time details

Commands:
  auth                                  check the credentials (auth.test)
  signin [--save FILE]                  derive the xoxc token from --cookie
                                        and --workspace, then validate it
  channels                              list conversations
  users                                 list users
  history <channel> [--limit N]         recent messages, oldest first
  thread <channel> <ts>                 a thread
  send <channel> <text> [--thread TS]   post a message
  edit <channel> <ts> <text>            change a message
  delete <channel> <ts>                 delete a message
  react <channel> <ts> <name> [--remove]
  search <query> [--count N] [--page N]
  counts                                unread counts
  emoji                                 custom emoji
  upload <channel> <file> [--comment TEXT]
  download <url> <file>
  watch [--mode auto|rtm|socket|poll] [--channel C] [--seconds N]
        [--until TEXT] [--poll-ms N]    print real-time events

<channel> is an id (C123, D456) or a name (general, #general, @alice).
)";

std::atomic<bool> sInterrupted{false};


void
onSignal(int)
{
	sInterrupted = true;
}


struct Options {
	Credentials credentials;
	std::string credentialsFile;
	std::string caFile;
	bool json = false;
	bool verbose = false;
	std::vector<std::string> positional;
	std::map<std::string, std::string> flags;   // command flags
	std::set<std::string> switches;
};


bool
takesValue(const std::string& flag)
{
	static const std::set<std::string> kSwitches = {"--json", "--verbose", "--remove",
		"--help", "-h"};
	return kSwitches.count(flag) == 0;
}


int
fail(const std::string& message)
{
	std::fprintf(stderr, "natter-cli: %s\n", message.c_str());
	return 1;
}


int
failError(const std::string& what, const Error& error)
{
	return fail(what + ": " + error.describe());
}


std::string
flag(const Options& options, const std::string& name, const std::string& fallback = {})
{
	auto it = options.flags.find(name);
	return it == options.flags.end() ? fallback : it->second;
}


bool
looksLikeId(const std::string& text)
{
	if (text.size() < 9 || (text[0] != 'C' && text[0] != 'G' && text[0] != 'D'))
		return false;
	for (char c : text) {
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
			return false;
	}
	return true;
}


// Resolve a channel argument, loading the directory when it is a name.
Result<std::string>
resolveChannel(Session& session, const std::string& argument)
{
	if (looksLikeId(argument))
		return argument;
	if (session.store().channels().empty()) {
		Result<std::vector<Channel>> channels = session.api()->conversationsList();
		if (!channels)
			return channels.error();
		session.store().setChannels(channels.value());
		if (startsWith(argument, "@")) {
			Result<std::vector<User>> users = session.api()->usersList();
			if (users)
				session.store().setUsers(users.value());
		}
	}
	std::optional<Channel> channel = session.store().findChannel(argument);
	if (!channel)
		return Error(ErrorKind::NotFound, "channel_not_found", argument);
	return channel->id;
}


void
loadUsers(Session& session)
{
	if (!session.store().users().empty())
		return;
	Result<std::vector<User>> users = session.api()->usersList();
	if (users)
		session.store().setUsers(users.value());
	Result<AuthInfo> auth = session.api()->authTest();
	if (auth)
		session.store().setSelf(auth->userId, auth->teamId);
}


void
printMessage(Session& session, const Message& message, bool asJson)
{
	if (asJson) {
		std::printf("%s\n", message.toJson().dump().c_str());
		return;
	}
	std::string author = !message.user.empty() ? session.store().userName(message.user)
		: !message.username.empty() ? message.username : message.botId;
	std::string text = session.format(message).plainText();
	if (text.empty() && message.attachments.is_array() && !message.attachments.empty())
		text = jStr(message.attachments[0], "fallback", jStr(message.attachments[0], "text"));
	std::string extras;
	if (message.replyCount > 0)
		extras += " [" + std::to_string(message.replyCount) + " replies]";
	for (const Reaction& reaction : message.reactions)
		extras += " :" + reaction.name + ":x" + std::to_string(reaction.count);
	for (const File& file : message.files)
		extras += " [file " + file.name + "]";
	if (message.edited)
		extras += " (edited)";
	std::printf("%s\t%s\t%s%s\n", message.ts.c_str(), author.c_str(), text.c_str(),
		extras.c_str());
}


int
cmdWatch(Session& session, const Options& options)
{
	std::string modeName = flag(options, "--mode", "auto");
	RealtimeMode mode = RealtimeMode::Auto;
	if (modeName == "rtm")
		mode = RealtimeMode::Rtm;
	else if (modeName == "socket")
		mode = RealtimeMode::SocketMode;
	else if (modeName == "poll")
		mode = RealtimeMode::Polling;
	else if (modeName != "auto")
		return fail("unknown --mode " + modeName);
	(void)mode;   // applied to SessionOptions in main()

	int seconds = std::atoi(flag(options, "--seconds", "0").c_str());
	std::string until = flag(options, "--until");
	std::string channelArgument = flag(options, "--channel");

	loadUsers(session);
	std::string channel;
	if (!channelArgument.empty()) {
		Result<std::string> resolved = resolveChannel(session, channelArgument);
		if (!resolved)
			return failError("channel", resolved.error());
		channel = resolved.value();
		// Baseline for polling and a quick look for the user.
		HistoryOptions recent;
		recent.limit = 20;
		session.loadHistory(channel, recent);
	}

	std::mutex lock;
	std::condition_variable done;
	bool matched = false;
	int listener = session.addListener([&](const SessionEvent& event) {
		std::string line = describeEvent(event.event);
		if (const auto* message = std::get_if<MessageEvent>(&event.event)) {
			std::string text = session.format(message->message).plainText();
			line += "\n  | " + text;
		}
		if (const auto* connection = std::get_if<ConnectionEvent>(&event.event)) {
			if (connection->state == ConnectionState::Connected
				|| connection->state == ConnectionState::Polling)
				line += "\nready";
		}
		std::printf("%s\n", line.c_str());
		std::fflush(stdout);
		if (!until.empty() && line.find(until) != std::string::npos) {
			std::lock_guard<std::mutex> guard(lock);
			matched = true;
			done.notify_all();
		}
	});

	session.setActiveConversation(channel);
	session.startRealtime();

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	{
		std::unique_lock<std::mutex> guard(lock);
		while (!matched && !sInterrupted) {
			if (seconds > 0 && std::chrono::steady_clock::now() >= deadline)
				break;
			done.wait_for(guard, std::chrono::milliseconds(200));
		}
	}
	session.removeListener(listener);
	session.stopRealtime();
	if (!until.empty() && !matched) {
		std::fprintf(stderr, "natter-cli: watch ended without seeing \"%s\"\n",
			until.c_str());
		return 2;
	}
	return 0;
}


int
run(const Options& options, Session& session)
{
	const std::vector<std::string>& args = options.positional;
	const std::string& command = args[0];
	auto need = [&args](size_t count) { return args.size() >= count + 1; };
	std::shared_ptr<WebApi> api = session.api();

	if (command == "auth") {
		Result<AuthInfo> info = api->authTest();
		if (!info)
			return failError("auth.test", info.error());
		std::printf("ok team=%s (%s) user=%s (%s) url=%s mode=%s\n", info->team.c_str(),
			info->teamId.c_str(), info->user.c_str(), info->userId.c_str(),
			info->url.c_str(), authModeName(api->mode()));
		return 0;
	}
	if (command == "channels") {
		loadUsers(session);
		Result<std::vector<Channel>> channels = api->conversationsList();
		if (!channels)
			return failError("conversations.list", channels.error());
		session.store().setChannels(channels.value());
		session.refreshCounts();
		for (const Channel& channel : session.store().channels()) {
			if (options.json) {
				std::printf("%s\n", channel.toJson().dump().c_str());
				continue;
			}
			std::printf("%s\t%s\t%s%s unread=%d mentions=%d\n", channel.id.c_str(),
				session.store().channelDisplayName(channel.id).c_str(),
				channel.isMember ? "member" : "-",
				channel.isPrivate ? " private" : "", channel.unreadCount,
				channel.mentionCount);
		}
		return 0;
	}
	if (command == "users") {
		Result<std::vector<User>> users = api->usersList();
		if (!users)
			return failError("users.list", users.error());
		for (const User& user : users.value()) {
			if (options.json)
				std::printf("%s\n", user.toJson().dump().c_str());
			else
				std::printf("%s\t%s\t%s%s%s\n", user.id.c_str(), user.name.c_str(),
					user.bestName().c_str(), user.isBot ? " [bot]" : "",
					user.deleted ? " [deleted]" : "");
		}
		return 0;
	}
	if (command == "history" || command == "thread") {
		if (!need(command == "thread" ? 2 : 1))
			return fail(command + " needs a channel" + (command == "thread" ? " and ts" : ""));
		Result<std::string> channel = resolveChannel(session, args[1]);
		if (!channel)
			return failError("channel", channel.error());
		loadUsers(session);
		HistoryOptions history;
		history.limit = std::atoi(flag(options, "--limit", "30").c_str());
		std::vector<Message> messages;
		if (command == "history") {
			Result<HistoryPage> page = session.loadHistory(channel.value(), history);
			if (!page)
				return failError("conversations.history", page.error());
			messages = session.store().messages(channel.value(),
				static_cast<size_t>(history.limit));
		} else {
			Result<HistoryPage> page = session.loadThread(channel.value(), args[2], history);
			if (!page)
				return failError("conversations.replies", page.error());
			messages = session.store().thread(channel.value(), args[2]);
		}
		for (const Message& message : messages)
			printMessage(session, message, options.json);
		return 0;
	}
	if (command == "send") {
		if (!need(2))
			return fail("send needs a channel and text");
		Result<std::string> channel = resolveChannel(session, args[1]);
		if (!channel)
			return failError("channel", channel.error());
		PostOptions post;
		post.threadTs = flag(options, "--thread");
		Result<Message> message = session.send(channel.value(), args[2], post);
		if (!message)
			return failError("chat.postMessage", message.error());
		std::printf("sent %s %s\n", message->channel.c_str(), message->ts.c_str());
		return 0;
	}
	if (command == "edit") {
		if (!need(3))
			return fail("edit needs a channel, ts and text");
		Result<std::string> channel = resolveChannel(session, args[1]);
		if (!channel)
			return failError("channel", channel.error());
		Result<Message> message = session.edit(channel.value(), args[2], args[3]);
		if (!message)
			return failError("chat.update", message.error());
		std::printf("edited %s %s\n", channel->c_str(), args[2].c_str());
		return 0;
	}
	if (command == "delete") {
		if (!need(2))
			return fail("delete needs a channel and ts");
		Result<std::string> channel = resolveChannel(session, args[1]);
		if (!channel)
			return failError("channel", channel.error());
		Status status = session.remove(channel.value(), args[2]);
		if (!status)
			return failError("chat.delete", status.error());
		std::printf("deleted %s %s\n", channel->c_str(), args[2].c_str());
		return 0;
	}
	if (command == "react") {
		if (!need(3))
			return fail("react needs a channel, ts and emoji name");
		Result<std::string> channel = resolveChannel(session, args[1]);
		if (!channel)
			return failError("channel", channel.error());
		bool remove = options.switches.count("--remove") != 0;
		Status status = session.react(channel.value(), args[2], args[3], !remove);
		if (!status)
			return failError(remove ? "reactions.remove" : "reactions.add", status.error());
		std::printf("%s :%s: on %s %s\n", remove ? "removed" : "added", args[3].c_str(),
			channel->c_str(), args[2].c_str());
		return 0;
	}
	if (command == "search") {
		if (!need(1))
			return fail("search needs a query");
		loadUsers(session);
		SearchOptions search;
		search.count = std::atoi(flag(options, "--count", "20").c_str());
		search.page = std::atoi(flag(options, "--page", "1").c_str());
		Result<SearchPage> page = api->searchMessages(args[1], search);
		if (!page)
			return failError("search.messages", page.error());
		std::printf("%d matches (page %d of %d)\n", page->total, page->page, page->pages);
		for (const SearchMatch& match : page->matches) {
			std::printf("#%s ", match.channelName.c_str());
			printMessage(session, match.message, options.json);
		}
		return 0;
	}
	if (command == "counts") {
		Result<UnreadCounts> counts = api->unreadCounts();
		if (!counts)
			return failError("counts", counts.error());
		static const char* kSources[] = {"client.counts", "users.counts",
			"conversations.info"};
		std::printf("source %s\n", kSources[static_cast<int>(counts->source)]);
		for (const UnreadInfo& info : counts->channels) {
			std::printf("%s\tunread=%d mentions=%d last_read=%s latest=%s\n",
				info.channel.c_str(), info.unreadCount, info.mentionCount,
				info.lastRead.c_str(), info.latestTs.c_str());
		}
		return 0;
	}
	if (command == "emoji") {
		Result<std::map<std::string, std::string>> emoji = api->emojiList();
		if (!emoji)
			return failError("emoji.list", emoji.error());
		for (const auto& [name, value] : emoji.value())
			std::printf(":%s:\t%s\n", name.c_str(), value.c_str());
		return 0;
	}
	if (command == "upload") {
		if (!need(2))
			return fail("upload needs a channel and a file");
		Result<std::string> channel = resolveChannel(session, args[1]);
		if (!channel)
			return failError("channel", channel.error());
		UploadOptions upload;
		upload.initialComment = flag(options, "--comment");
		upload.threadTs = flag(options, "--thread");
		Result<UploadedFile> file = api->uploadFileFromPath(channel.value(), args[2], upload);
		if (!file)
			return failError("upload", file.error());
		std::printf("uploaded %s %s\n", file->id.c_str(), file->title.c_str());
		return 0;
	}
	if (command == "download") {
		if (!need(2))
			return fail("download needs a URL and a file");
		Status status = api->downloadToFile(args[1], args[2]);
		if (!status)
			return failError("download", status.error());
		std::printf("saved %s\n", args[2].c_str());
		return 0;
	}
	if (command == "watch")
		return cmdWatch(session, options);
	return fail("unknown command " + command + "\n" + kUsage);
}

}  // namespace


int
main(int argc, char** argv)
{
	Options options;
	for (int i = 1; i < argc; i++) {
		std::string argument = argv[i];
		if (argument == "--help" || argument == "-h") {
			std::fputs(kUsage, stdout);
			return 0;
		}
		if (startsWith(argument, "--") && argument.size() > 2) {
			if (!takesValue(argument)) {
				options.switches.insert(argument);
				continue;
			}
			if (i + 1 >= argc)
				return fail(argument + " needs a value");
			std::string value = argv[++i];
			if (argument == "--token")
				options.credentials.token = value;
			else if (argument == "--cookie")
				options.credentials.cookie = value;
			else if (argument == "--workspace")
				options.credentials.workspace = value;
			else if (argument == "--app-token")
				options.credentials.appToken = value;
			else if (argument == "--api-base")
				options.credentials.apiBase = value;
			else if (argument == "--credentials")
				options.credentialsFile = value;
			else if (argument == "--ca-file")
				options.caFile = value;
			else
				options.flags[argument] = value;
			continue;
		}
		options.positional.push_back(argument);
	}
	if (options.switches.count("--json") != 0)
		options.json = true;
	if (options.switches.count("--verbose") != 0)
		options.verbose = true;
	if (options.positional.empty()) {
		std::fputs(kUsage, stderr);
		return 1;
	}

	if (!options.credentialsFile.empty()) {
		Result<Credentials> loaded = Credentials::load(options.credentialsFile);
		if (!loaded)
			return failError("credentials", loaded.error());
		// Flags given on the command line win over the file.
		Credentials merged = loaded.value();
		const Credentials& flags = options.credentials;
		if (!flags.token.empty())
			merged.token = flags.token;
		if (!flags.cookie.empty())
			merged.cookie = flags.cookie;
		if (!flags.workspace.empty())
			merged.workspace = flags.workspace;
		if (!flags.appToken.empty())
			merged.appToken = flags.appToken;
		if (!flags.apiBase.empty())
			merged.apiBase = flags.apiBase;
		options.credentials = merged;
	}

	if (options.verbose) {
		setLogHandler([](LogLevel level, const std::string& message) {
			static const char* kNames[] = {"debug", "info", "warning", "error"};
			std::fprintf(stderr, "[%s] %s\n", kNames[static_cast<int>(level)],
				message.c_str());
		});
	}
	std::signal(SIGINT, onSignal);
	std::signal(SIGTERM, onSignal);
	std::signal(SIGPIPE, SIG_IGN);

	CurlTransport::Options transportOptions;
	transportOptions.caFile = options.caFile;
	transportOptions.userAgent = std::string("natter-cli/") + natterVersion();
	auto transport = std::make_shared<CurlTransport>(transportOptions);

	if (options.positional[0] == "signin") {
		Result<Credentials> credentials = signin::signInWithCookie(*transport,
			options.credentials.workspace, options.credentials.cookie,
			options.credentials.apiBase);
		if (!credentials)
			return failError("sign-in", credentials.error());
		credentials->appToken = options.credentials.appToken;
		std::string save = flag(options, "--save");
		if (!save.empty()) {
			Status status = credentials->save(save);
			if (!status)
				return failError("save", status.error());
		}
		std::printf("ok team=%s user=%s url=%s token=%s...\n",
			credentials->teamName.c_str(), credentials->userName.c_str(),
			credentials->url.c_str(), credentials->token.substr(0, 12).c_str());
		return 0;
	}

	if (options.credentials.token.empty())
		return fail("no token: pass --token or --credentials");
	if (options.credentials.mode() == AuthMode::Session && options.credentials.cookie.empty())
		return fail("an xoxc- token needs --cookie (the d cookie)");

	SessionOptions sessionOptions;
	sessionOptions.workerThreads = 1;
	sessionOptions.realtime.websocket.caFile = options.caFile;
	std::string mode = flag(options, "--mode", "auto");
	if (mode == "rtm")
		sessionOptions.realtime.mode = RealtimeMode::Rtm;
	else if (mode == "socket")
		sessionOptions.realtime.mode = RealtimeMode::SocketMode;
	else if (mode == "poll")
		sessionOptions.realtime.mode = RealtimeMode::Polling;
	std::string pollMs = flag(options, "--poll-ms");
	if (!pollMs.empty()) {
		sessionOptions.realtime.poll.historyIntervalMs = std::atoi(pollMs.c_str());
		sessionOptions.realtime.poll.countsIntervalMs = std::atoi(pollMs.c_str()) * 6;
	}
	Session session(options.credentials, sessionOptions, transport);
	return run(options, session);
}
