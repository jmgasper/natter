# Natter core API

This is the guide for writing a UI on top of `natter_core`. Everything is in
namespace `natter`, and `#include "natter/natter.h"` pulls in all of it.

```
            +-----------------------------  Session  -----------------------------+
 UI  --->   |  WebApi (blocking calls)   Store (thread-safe model)   Realtime       |
 (BLooper)  |        |                        ^   ^                  RTM / Socket   |
    ^       |  HttpTransport (libcurl)        |   +-- merge ---------  Mode / Poller |
    |       +---------------------------------|----------------------------|-------+
    +------------ listeners (SessionEvent) ---+----------------------------+
```

For most UIs `Session` is the only class you construct. The other classes can
also be used on their own (tests and `natter-cli` do).

## Threading rules

| What | Which thread | Notes |
|---|---|---|
| `WebApi` methods | the caller's | Blocking. Thread-safe: one `WebApi` may serve many threads. |
| `Session::bootstrap`, `loadHistory`, `loadThread`, `send`, `edit`, `remove`, `react`, `markRead`, `openDirectMessage`, `refreshCounts`, `loadCache`, `saveCache` | the caller's | Blocking. Never call them on a window's thread; use `Session::post()` or your own worker. |
| `Session` listeners | the thread that produced the event | That is the RTM or Socket Mode socket thread, the poller thread, or the thread that called `send()` and similar (source `Local`). |
| `Store` getters and setters | any | A reader/writer lock is taken inside. Getters return copies. |
| `Session::post(job)` | a session worker (2 by default) | FIFO order. Jobs still queued at destruction are dropped. |
| `WebSocketClient` callbacks | its own thread | One callback runs at a time. |
| `natter::log` handler | any | |

Listener dispatch is serialised: one listener call at a time, in event
order, and the store has already been updated when a listener runs.

- **Listeners must not block on the UI thread.** Post a message instead;
  do not wait for a reply. On Haiku, send a `BMessage` through a
  `BMessenger` without asking for one:

  ```cpp
  session.addListener([messenger](const natter::SessionEvent& e) {
      BMessage message(kMsgSlackEvent);
      message.AddInt32("change", e.change.kind);
      message.AddString("channel", e.change.channel.c_str());
      message.AddString("ts", e.change.ts.c_str());
      messenger.SendMessage(&message);          // no reply, never blocks long
  });
  ```

  Then read what you need from `session.store()` in `MessageReceived`.
- `removeListener(id)` returns only once no call to that listener is still
  running. A listener may remove itself.
- Do not destroy a `Session`, or call `stopRealtime()`, from inside one of
  its listeners or jobs. Stopping joins the source threads, and one of them
  may be waiting to dispatch. Post the request to the UI thread instead.
- `Session::~Session` stops real time, cancels pending rate-limit waits and
  joins the workers. An HTTP request already in flight is not interrupted;
  it can take up to its timeout (30 s by default).

## Typical lifecycle

```cpp
using namespace natter;

Result<Credentials> creds = Credentials::load(settingsPath);  // or sign in, below
SessionOptions options;
options.cacheDirectory = "/boot/home/config/settings/Natter/cache/T0123";
auto session = std::make_unique<Session>(creds.value(), options);

session->loadCache();                 // instant, possibly stale, UI can draw now
session->post([s = session.get()] {
    Status status = s->bootstrap();   // auth.test, team, users, channels, emoji, counts
    if (!status && status.error().isAuth()) { /* ask the user to sign in again */ }
    s->startRealtime();
});

// When the user opens a conversation:
session->setActiveConversation(channelId);          // polled first in polling mode
session->post([s = session.get(), channelId] {      // merges into the store
    s->loadHistory(channelId);
});
// ...draw from session->store().messages(channelId, 200)

// When quitting:
session->saveCache();
session.reset();
```

## Credentials and sign-in (`credentials.h`)

`Credentials` holds `token`, `cookie`, `workspace`, `appToken`, the
informational `teamId`, `teamName`, `userId`, `userName` and `url`, and
`apiBase`, which is for tests and proxies only.

- `mode()` returns `Session` for `xoxc-`, `UserToken` for `xoxp-`,
  `BotToken` for `xoxb-`, and `None` otherwise.
- `apiUrl()` is `https://<workspace host>/api/` in session mode (this is
  what makes Enterprise Grid work) and `https://slack.com/api/` otherwise.
- `cookieHeader()` returns `d=<cookie>`. The value is percent-encoded if
  the user pasted it decoded.
- `toJson()`, `fromJson()`, `load(path)` and `save(path)` handle storage.
  `save()` writes atomically with mode 0600.

Helpers in namespace `natter::signin`:

| Function | Does |
|---|---|
| `extractTokensFromHtml(html)` | Every `xoxc-` token in a workspace page, the `"api_token"` one first. |
| `parseLocalConfig(text)` | The web client's `localStorage.localConfig_v2`: a list of `LocalConfigTeam` (id, name, domain, url, token), the last active team first. |
| `fetchSessionToken(transport, creds)` | GETs the workspace page with the d cookie, following redirects, and scrapes the token. The HTTP status is ignored, because Slack returns 403 with the logged-in page. |
| `validate(transport, creds)` | Runs `auth.test` and fills in the team, user and url fields. |
| `signInWithCookie(transport, workspace, cookie)` | Combines the two: from a d cookie to validated credentials. |

A sign-in window only needs a workspace field and a cookie field, and then
calls `signInWithCookie` on a worker thread.

## Results and errors (`result.h`)

Fallible calls return `Result<T>` (or `Status`, which is `Result<void>`).
Test `ok()` (or use it as a bool), then use `value()` or `->`. On failure,
`error()` is an `Error` with `kind`, `code` (Slack's `error` string),
`message`, `httpStatus` and `retryAfterSeconds`.

| `ErrorKind` | Meaning, and what a UI should do |
|---|---|
| `Auth` | `not_authed`, `invalid_auth`, `token_revoked` and so on. The session is dead; ask the user to sign in again. |
| `RateLimited` | Still limited after the automatic `Retry-After` retries. Try later. |
| `Transport` | DNS, TCP, TLS or timeout. A write may or may not have landed. |
| `NotFound` | `channel_not_found`, `message_not_found` and similar. |
| `Unsupported` | The method is not available to this token (`missing_scope`, `not_allowed_token_type`, ...). |
| `Api`, `Http`, `Parse` | Any other Slack error code, a non-JSON error page, or a malformed body. |
| `Cancelled`, `InvalidArgument` | Local. |

## `WebApi` (`web_api.h`)

This is a typed, synchronous Web API client. All methods are `const` and
thread-safe. A 429 response is retried after `Retry-After`, up to three
times (see `ApiOptions`), and `ApiOptions::onRateLimited` is told about
each wait.

| Area | Methods |
|---|---|
| Generic | `call(method, params)`, `callWithAppToken`, `paginate(method, params, onPage)`, `collect(method, params, key)` |
| Auth, team, users | `authTest`, `teamInfo`, `usersList` (all pages), `usersInfo`, `usersGetPresence` |
| Conversations | `conversationsList` (all four types, archived excluded, all pages), `conversationsInfo`, `conversationsHistory(channel, HistoryOptions)`, `conversationsReplies(channel, ts, ...)`, `conversationsMark`, `conversationsMembers` (all pages), `conversationsOpen(users)` |
| Messages | `chatPostMessage(channel, text, PostOptions{threadTs, replyBroadcast, blocks, ...})`, `chatUpdate`, `chatDelete`, `reactionsAdd`, `reactionsRemove` (names with or without colons) |
| Emoji, search | `emojiList` (name → URL or `alias:other`), `searchMessages(query, SearchOptions)` (pages, not cursors) |
| Files | `uploadFile(channel, name, data, UploadOptions)` and `uploadFileFromPath` (`getUploadURLExternal`, then POST, then `completeUploadExternal`); `download(url)` and `downloadToFile(url, path, progress)` for `url_private`, with auth, which report Slack's sign-in page as an `Auth` error |
| Unread counts | `unreadCounts(fallbackChannels)`: `client.counts` in session mode, else `users.counts`, else `conversations.info` for each listed channel. A method that says "not for this token" is not tried again. |
| Real time | `rtmConnect`, `appsConnectionsOpen` |

Pagination is applied automatically where a method returns the whole
set. `conversationsHistory`, `conversationsReplies` and `searchMessages`
return one page (`HistoryPage::nextCursor`, `SearchPage::pages`) because a UI
pages them on scroll.

`cancel()` aborts rate-limit waits and fails later calls with `Cancelled`.
It is for shutdown.

## Models (`models.h`)

These are plain structs. Each has `fromJson`, and `toJson` writes Slack's own
shape back, so the cache reuses the parsers.

- `Team`: id, name, domain, url, iconUrl.
- `User`: id, name, realName, displayName, avatars (`avatarUrl(size)`), tz,
  tzOffset, deleted, isBot, botId, status, presence. `bestName()` gives the
  name to show.
- `Channel`: id, name, the `is*` flags, imUser (for IMs), topic, purpose,
  lastRead, latestTs, unreadCount, mentionCount, hasUnreads. `kind()` is
  Public, Private, Im or Mpim.
- `Message`: channel, ts, user, botId, username, text, subtype, threadTs,
  replyCount, replyUsers, latestReply, edited, reactions (`Reaction`),
  files (`File`), and raw `attachments`, `blocks` and `botProfile` json.
  Helpers: `isThreadReply()`, `isThreadParent()`, `isBroadcastReply()`.
- `File`: id, name, title, mimetype, size, urlPrivate(Download), permalink,
  thumbs (`thumbUrl(size)`) and the original size.
- `HistoryPage`, `SearchPage`, `UnreadCounts`, `AuthInfo`, `Presence`,
  `ConnectInfo`, `UploadedFile`.

A timestamp (`ts`) is both an id and an order. Compare timestamps with
`compareTs` or `TsLess`, never as doubles.

## Events (`events.h`)

`Event` is a `std::variant` of:

| Alternative | From |
|---|---|
| `MessageEvent{message}` | A new message, including thread replies (`message.isThreadReply()`) and subtypes such as `bot_message`, `file_share` and `thread_broadcast`. |
| `MessageChangedEvent{channel, message, previous}` | `message_changed` and `message_replied`. |
| `MessageDeletedEvent{channel, ts, threadTs}` | `message_deleted`. |
| `ReactionEvent{added, user, reaction, channel, ts, ...}` | `reaction_added` and `reaction_removed`. |
| `TypingEvent`, `PresenceEvent{users, presence}` | `user_typing`, `presence_change` (single or batch). |
| `MarkedEvent{channel, ts, kind, unreadCount, mentionCount}` | `channel_marked`, `group_marked`, `im_marked` and `mpim_marked`. The counts are -1 when absent. |
| `ChannelJoinedEvent`, `ChannelLeftEvent`, `ChannelUpdatedEvent` | Joining and leaving; `channel_created`, renames and archiving. |
| `MemberJoinedChannelEvent{joined, user, channel}` | `member_joined_channel` and `member_left_channel`. |
| `UserChangeEvent{user}` | `user_change` and `team_join`. |
| `EmojiChangedEvent{subtype, name, oldName, value, names}` | `emoji_changed` (add, remove, rename). |
| `HelloEvent`, `GoodbyeEvent` | RTM housekeeping. |
| `CountsEvent{counts}` | A counts poll (Natter itself, not Slack). |
| `ConnectionEvent{state, source, detail, attempt, retryInSeconds}` | Natter itself: Connecting, Connected, Disconnected or Polling. Use it for a status indicator. |
| `UnknownEvent{type, subtype, raw}` | Everything else, kept raw. |

`parseEvent(json)` turns an RTM frame or an Events API `event` object into an
`Event`. `describeEvent` gives one line of text for logs.

## Store (`store.h`)

The store is an in-memory model of one workspace. Every method is
thread-safe, and getters return copies.

- Users: `users()`, `user(id)`, `userName(id)`, `setPresence`.
- Channels: `channels()` (sorted by kind, then name), `channel(id)`,
  `findChannel("general" | "#general" | "@alice" | id)`, and
  `channelDisplayName(id)` (`#general`, `@ali`, or `alice, bob` for group
  DMs).
- Messages: `messages(channel, limit)` (oldest first), `messagesBefore`
  (scroll-back), `message(channel, ts)`, `thread(channel, threadTs)` (the
  parent, then the replies), `newestTs` and `oldestTs`.
- Emoji: `emoji()`, `resolveCustomEmoji(name)` (follows `alias:` chains)
  and `customEmojiUrl(name)`.
- `apply(event)` returns a `StoreChange` (`kind`: Users, Channels, Messages,
  Thread, ReadState, Emoji; plus `channel`, `ts`, `threadTs`, `id`,
  `newUnread` and `mentionsSelf`), so the UI can refresh only what changed
  and decide on notifications.

Merge rules:

- A new message is inserted by ts. A duplicate, for example the same
  message from RTM and from polling, is merged, not counted twice. A new
  top-level message from someone else, newer than `lastRead`, increments
  `unreadCount`, and `mentionCount` too if it mentions you (`<@me>`,
  `<!here>`, `<!channel>` or `<!everyone>`, or any DM). Joins and leaves
  do not count.
- A thread reply goes to the thread and updates the parent's `replyCount`,
  `latestReply` and `replyUsers`. It does not do this if the parent already
  counted it (its `latestReply` is newer). A `thread_broadcast` reply also
  appears in the channel.
- An edit replaces every copy of the message, in the channel and in threads.
  An edit to a message older than the loaded window is ignored, so it
  cannot leave a hole in the timeline.
- A delete removes the message. Deleting a reply shrinks its parent's
  count; deleting a parent drops its thread.
- Reactions are added and removed per user, and applying one twice has no
  further effect. When the users list was truncated, the count still goes
  down.
- A mark sets `lastRead` and takes Slack's counts when the event has them;
  otherwise it recounts from the loaded timeline. `CountsEvent` and
  `applyCounts` overwrite the unread figures.
- `setChannels` replaces the list but keeps read state that the new data
  lacks. `conversations.list` carries no unread figures.
- Each timeline keeps at most 5000 messages
  (`setMaxMessagesPerConversation`).

The disk cache: `saveCache(dir, messagesPerConversation = 50)` writes
`meta.json`, `users.json`, `channels.json`, `emoji.json` and
`messages.json` atomically. `loadCache(dir)` reads them back; a missing
cache is not an error.

## Real time (`realtime.h`, `websocket.h`)

`Realtime` picks the source:

| `RealtimeMode` | Behaviour |
|---|---|
| `Auto` (default) | Socket Mode if there is an app token, else RTM. If RTM is refused (for example `not_allowed_token_type`) or fails three times in a row without connecting, it falls back to polling. If Socket Mode gives up, it also falls back to polling. |
| `Rtm` | `rtm.connect`, then wss with `Cookie: d=...` in session mode. It sends JSON and WebSocket pings every 30 s. `goodbye` reconnects at once. |
| `SocketMode` | `apps.connections.open` with the xapp token. Every envelope is acknowledged by `envelope_id` before it is dispatched. Retried events are de-duplicated by `event_id`. A `disconnect` envelope reconnects at once. |
| `Polling` | `conversations.history` for the active conversation every 5 s (and `conversations.replies` for an open thread), and the counts every 30 s. The first poll of a conversation only records a baseline. After that, differences become `MessageEvent`, `MessageChangedEvent` and `MessageDeletedEvent`. |
| `Off` | Nothing. |

All sources reconnect with exponential backoff and jitter (1 s up to 60 s).
The backoff is reset after a connection has stayed up for 20 s. Auth and
unsupported-method errors stop the retries.

`Session::setActiveConversation(channel, threadTs)` tells the poller what to
watch. Call it whenever the visible conversation changes. It costs nothing
in RTM or Socket Mode.

The lower-level pieces can be used directly:

- `WebSocketConnection`: one connection. `connect(url, headers)` and
  `run(handlers)` block; `sendText`, `sendPing`, `close` and `abort` are
  thread-safe. It handles `ws://` and `wss://` (with SNI and hostname or IP
  verification against the system CA store, Haiku's bundle, or
  `WebSocketOptions::caFile`), masking, fragmentation, ping/pong and the
  closing handshake.
- `WebSocketClient`: keeps a connection alive on its own thread, asking a
  provider for a fresh URL on every attempt.
- `ws::encodeFrame`, `ws::FrameDecoder`, `ws::MessageAssembler`,
  `ws::parseUrl` and `ws::acceptForKey` are the pure RFC 6455 parts.

## Formatting messages (`mrkdwn.h`, `emoji.h`)

```cpp
FormattedText text = session.format(message);   // or formatMessage(message, store.formatContext())
for (const Run& run : text.runs) { ... }
```

`formatMessage` renders `rich_text` blocks when the message has them, and
its `mrkdwn` text otherwise. The result is a flat list of `Run`s:

| `RunKind` | `text` | `target` |
|---|---|---|
| `Text` | the text | |
| `Link` | the label | the URL |
| `UserMention`, `ChannelMention`, `UserGroupMention` | `@name`, `#name` or `@group` | the id |
| `Broadcast` | `@here`, `@channel` or `@everyone` | `here`, `channel` or `everyone` |
| `Emoji` | the Unicode sequence, skin tone applied | the shortcode |
| `CustomEmoji` | `:name:` as a fallback | the workspace emoji name, aliases resolved: draw the image from `store.customEmojiUrl(target)` |
| `Date` | Slack's fallback text | unix seconds |
| `ListMarker` | `• ` or `1. ` | |
| `LineBreak` | `\n` | |

`style` is a bit set: `kStyleBold`, `kStyleItalic`, `kStyleStrike`,
`kStyleCode` (inline), `kStyleCodeBlock` and `kStyleQuote`. Draw a quote bar
beside lines whose runs carry `kStyleQuote`, and a box behind consecutive
`kStyleCodeBlock` runs. `indent` is the list nesting level. `highlight` marks
mentions of the signed-in user (and broadcasts), and
`FormattedText::mentionsSelf` says whether any run has it.
`plainText()` joins the runs, for notifications and previews.

`emoji::lookup("thumbsup")`, `lookup("+1::skin-tone-3")` and
`lookup(name, tone)` give Unicode for standard shortcodes. For reaction names
use `emoji::splitSkinTone`, then `lookup`, then fall back to
`store.customEmojiUrl`.

## Logging

`setLogHandler(fn)` receives `(LogLevel, message)` from any thread. By
default, warnings and errors go to stderr, and everything else too when
`NATTER_DEBUG` is set.
