# Natter

Natter is a native Slack client for [Haiku](https://www.haiku-os.org/),
written with the Interface Kit. It has:
- channels and direct messages, with unread and mention counts;
- threads in a side panel, reactions, files and images, and custom emoji;
- editing, deleting and uploads;
- message search (Command+F), which opens a result in its conversation;
- notifications and typing indicators, kept up to date in real time.

This repository holds the app (`src/ui`), its portable core (a UI-free C++20
library, `natter_core`), a command line driver (`natter-cli`) and the tests.
See [docs/CORE-API.md](docs/CORE-API.md) for the core's classes and threading
rules.

The core builds and runs on Linux (for development and tests) and on Haiku
(gcc 13). Its only system dependencies are libcurl, OpenSSL 3 and the C++
standard library.

<!-- airos-ci:latest-builds:start -->
## Latest builds

Built automatically by air/OS CI from commit `088ee71` on 2026-10-05 ([all files](https://github.com/jmgasper/natter/releases/tag/latest)).

| Architecture | Package |
|---|---|
| arm64 | [natter-0.1.0-1-arm64.hpkg](https://github.com/jmgasper/natter/releases/download/latest/natter-0.1.0-1-arm64.hpkg) |
| x86_64 | [natter-0.1.0-1-x86_64.hpkg](https://github.com/jmgasper/natter/releases/download/latest/natter-0.1.0-1-x86_64.hpkg) |

Install with `pkgman install <file>`, or copy the file into `/boot/system/packages`. Haiku SDK: x86_64 hrev60206-669-g9dc439ceeb, arm64 hrev60206-669-g9dc439ceeb.
<!-- airos-ci:latest-builds:end -->

## What is in the core

- **HTTP**: a synchronous libcurl client behind the `HttpTransport`
  interface (form, JSON, raw and multipart bodies, redirects, timeouts,
  `Retry-After` on HTTP 429). It is safe to call from several threads.
- **Sign-in**: session credentials (`xoxc-` token plus the browser's `d`
  cookie), user tokens (`xoxp-`) and app-level tokens (`xapp-`) for Socket
  Mode. It can derive the session token from a `d` cookie, or read tokens
  from the web client's `localConfig_v2` localStorage value.
- **Web API**: typed calls with cursor pagination and error mapping for
  the methods a client needs: users, conversations, history and threads,
  posting, editing and deleting, reactions, emoji, search, file upload and
  download, unread counts, and the real-time bootstrap calls.
- **Real time**: an RFC 6455 WebSocket client written on BSD sockets and
  OpenSSL (it does not use libcurl's WebSocket support, which Haiku builds
  may lack). On top of it are RTM, Socket Mode, and a polling fallback that
  is used when neither works.
- **Store**: an in-memory, thread-safe model of users, channels, message
  timelines, threads and emoji. It has merge rules for API pages and
  real-time events, and a small JSON disk cache so the UI starts fast.
- **Formatter**: turns Slack `mrkdwn` and `rich_text` blocks into styled
  runs. It covers bold, italic, strike, code, quotes, lists, links,
  mentions, broadcasts, dates, and emoji with skin tones. The emoji table is
  generated from iamcal/emoji-data.
- **Session**: the facade a UI drives. It ties the API, the store and the
  real-time connection together and delivers events to listeners.

## Building

You need CMake 3.16 or newer, a C++20 compiler (gcc 13 is known to work),
the libcurl and OpenSSL 3 development files, and Python 3 for the
end-to-end tests.

On Linux (Debian/Ubuntu names):

```sh
sudo apt install cmake g++ libcurl4-openssl-dev libssl-dev python3
cmake -S . -B ../natter-build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build ../natter-build -j
```

On Haiku:

```sh
pkgman install cmake curl_devel openssl3_devel
cmake -S . -B build
cmake --build build
```

On Haiku, CMake adds `libnetwork` for the socket calls and builds the app,
`Natter`, with its resources and icon. Keep build directories outside the
source tree if disk space is tight.

To make a package, build a release and run `tools/package-haiku.sh`:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNATTER_BUILD_TESTS=OFF
cmake --build build --target Natter
tools/package-haiku.sh build        # build/natter-<version>-1-<arch>.hpkg
```

The package installs `apps/Natter` with a Deskbar entry. It requires
`noto_emoji`, which Haiku's text rendering uses for emoji. The app server
picks up a newly installed font only after a restart.

Options:

| Option | Default | Meaning |
|---|---|---|
| `NATTER_BUILD_TESTS` | ON | unit, end-to-end and integration tests |
| `NATTER_BUILD_CLI` | ON | `natter-cli` |
| `NATTER_WARNINGS_AS_ERRORS` | OFF | `-Werror` for the core |
| `NATTER_WEBKIT` | (empty) | Summit's WebKit engine directory: adds sign-in on Slack's own web page |

With `NATTER_WEBKIT`, the sign-in window offers **Sign in with Slack's web
page**. It opens `slack.com` in Summit's engine, with a private profile.
After you sign in, Natter reads the web client's session tokens and its
HttpOnly `d` cookie through the engine's embedding API. The package then
requires `summit_webkit`.

## Testing

```sh
cd ../natter-build && ctest --output-on-failure
```

- `unit.*` are C++ tests with a fake transport and the JSON fixtures in
  `tests/fixtures`. They cover every API method's parsing and pagination,
  WebSocket framing and a loopback connection, mrkdwn and rich_text, events,
  the store's merge rules and cache, the poller and Session.
- `e2e.mock_slack` starts `tests/mock/mock_slack.py`, a Slack stand-in
  written with the Python standard library only. It serves the HTTP API and
  RTM and Socket Mode WebSockets. The test then drives `natter-cli` against
  it: sign-in, channels, history, send/react/edit/delete, upload and
  download, and `watch` over RTM, Socket Mode and the polling fallback. It
  runs once more over TLS (https and wss) with a self-signed certificate
  made at test time.
- `integration.real_slack` runs only when `NATTER_TEST_TOKEN`,
  `NATTER_TEST_COOKIE` and `NATTER_TEST_WORKSPACE` are set, and is reported
  as skipped otherwise. It is read-only unless you also set
  `NATTER_TEST_CHANNEL` and `NATTER_TEST_ALLOW_WRITE=1`.

To try the app without a Slack account, start the mock server and point the
sign-in window at it:

```sh
python3 tests/mock/mock_slack.py --fixtures tests/fixtures --port 8787 &
NATTER_API_BASE=http://127.0.0.1:8787/api/ build/Natter
```

Sign in with any workspace name and the cookie `xoxd-e2e%2Fcookie%3D`.
Accounts are saved in `~/config/settings/Natter/accounts/<team>.json`,
mode 0600.

## Signing in

Natter supports three kinds of credentials.

**Session (default).** Use the `xoxc-` token and `d` cookie of a browser
session, as Slack's own web client does:

1. Sign in to your workspace at `https://app.slack.com` in a browser.
2. Open the developer tools and go to Storage (or Application) → Cookies →
   `https://app.slack.com`.
3. Copy the value of the `d` cookie. It starts with `xoxd-`.
4. Derive and check the token, and save everything to a file with mode 0600:

   ```sh
   natter-cli --workspace acme --cookie 'xoxd-...' signin --save ~/natter.json
   natter-cli --credentials ~/natter.json channels
   ```

API calls go to `https://<workspace>.slack.com/api/<method>`, with the token
as a form field and `Cookie: d=...`. This also works on Enterprise Grid. The
RTM WebSocket sends the same cookie on its handshake.

**User token.** An `xoxp-` token from your own Slack app, with the user
scopes it needs. It is sent as `Authorization: Bearer` to
`https://slack.com/api/`. RTM is not available to new apps, so real time
falls back to polling unless you also give an app-level token.

**App-level token.** An `xapp-` token (`connections:write`) enables Socket
Mode, which then delivers events for whatever the app is subscribed to.

## Risks

The session method is **unofficial**. Slack's web client uses `xoxc-` tokens
with the `d` cookie, and so do internal methods such as `client.counts`, but
none of this is documented or supported. Slack can change or block it at any
time, and may later bind session tokens to the device (the desktop app
already has DPoP code). Automating a user session may also conflict with
your workspace's or Slack's terms of service. Check them before you use it.

The `d` cookie is your whole login session. Treat it, and the credentials
file, like a password. `Credentials::save` writes the file with mode 0600.

## Emoji table

`src/core/emoji_table.inc` is generated from `emoji.json` in
[iamcal/emoji-data](https://github.com/iamcal/emoji-data) (MIT), the data set
Slack uses. The JSON is not kept in this repository. To regenerate the table:

```sh
curl -L -o /tmp/emoji.json \
    https://raw.githubusercontent.com/iamcal/emoji-data/master/emoji.json
tools/gen_emoji_table.py /tmp/emoji.json src/core/emoji_table.inc
```

## Layout

```
include/natter/   public headers (natter.h includes them all)
src/core/         the library
src/ui/           the Haiku app
src/cli/          natter-cli
resources/        the app's resource definition and HVIF icon
tests/            unit tests, fixtures, the mock server, e2e and integration
tools/            the emoji table and icon generators, the cross-compile
                  check and the package script
vendor/nlohmann/  nlohmann/json 3.12.0 (MIT)
docs/             CORE-API.md
```

## Acknowledgements and licence

Natter is MIT licensed (see `LICENSE`).

Slack's protocol behaviour (endpoints, parameters, the session cookie
scheme, the shape of `client.counts`) was learned by studying
[make-slack-great-again](https://github.com/punarinta/make-slack-great-again),
a Qt Slack client licensed under GPL-3.0. No code, structure or comments
were taken from it; Natter is an independent implementation.

The vendored nlohmann/json is MIT licensed (`vendor/nlohmann/LICENSE.MIT`).
The emoji data comes from iamcal/emoji-data (MIT).
