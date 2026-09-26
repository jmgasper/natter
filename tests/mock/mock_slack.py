#!/usr/bin/env python3
# Natter - a local mock of the Slack Web API, RTM and Socket Mode.
# SPDX-License-Identifier: MIT
"""Serve enough of Slack for natter-cli's end-to-end tests.

Standard library only. The HTTP API answers from the JSON fixtures in
tests/fixtures plus an in-memory message store; chat.postMessage and friends
push the matching events to every connected RTM and Socket Mode client.

    mock_slack.py --fixtures tests/fixtures [--port 0] [--tls-cert c --tls-key k]

The first line on stdout is "PORT <n>". Control endpoints for tests:
    GET  /_mock/state    connections, acks, handshake cookies, uploads
    POST /_mock/config   {"rtm_disabled": true} and similar switches
    POST /_mock/push     a raw event to send to every RTM/Socket Mode client
"""

import argparse
import base64
import copy
import hashlib
import http.server
import json
import mimetypes
import os
import queue
import select
import socketserver
import ssl
import struct
import sys
import threading
import time
import urllib.parse

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
TOKEN = "xoxc-e2e-1111-2222-3333"
COOKIE = "xoxd-e2e%2Fcookie%3D"
USER_TOKEN = "xoxp-e2e-4444"
APP_TOKEN = "xapp-e2e-5555"
FILE_BYTES = b"\x89PNG\r\n\x1a\nmock-image-bytes"


class State:
    def __init__(self, fixtures):
        self.fixtures = fixtures
        self.lock = threading.RLock()
        self.config = {"rtm_disabled": False, "script_rtm": True, "script_socket": True}
        self.messages = {}   # channel -> {ts: message}
        self.replies = {}    # (channel, thread_ts) -> {ts: message}
        self.counter = 0
        self.clients = []    # connected WebSocket clients
        self.acks = []
        self.handshake_cookies = []
        self.rtm_connections = 0
        self.socket_connections = 0
        self.uploads = {}
        self.marks = []
        self.calls = []
        history = self.load("conversations.history.json")["messages"]
        self.messages["C01GENERAL"] = {m["ts"]: m for m in history}
        replies = self.load("conversations.replies.json")["messages"]
        root = replies[0]["ts"]
        self.replies[("C01GENERAL", root)] = {m["ts"]: m for m in replies[1:]}

    def load(self, name):
        with open(os.path.join(self.fixtures, name), encoding="utf-8") as handle:
            return json.load(handle)

    def next_ts(self):
        with self.lock:
            self.counter += 1
            return "%d.%06d" % (int(time.time()), self.counter)

    def broadcast(self, event):
        with self.lock:
            clients = list(self.clients)
        for client in clients:
            client.deliver(event)


class WebSocketClient:
    """One accepted WebSocket connection (server side: frames unmasked).

    Only this connection's own thread touches the socket (an SSL socket
    must not be read and written from two threads); other threads queue
    frames with send_frame().
    """

    def __init__(self, handler, kind, state):
        self.handler = handler
        self.kind = kind
        self.state = state
        self.outgoing = queue.Queue()
        self.closed = False
        self.envelopes = 0

    def send_frame(self, opcode, payload):
        if isinstance(payload, str):
            payload = payload.encode("utf-8")
        header = bytes([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header += bytes([length])
        elif length < 65536:
            header += bytes([126]) + struct.pack("!H", length)
        else:
            header += bytes([127]) + struct.pack("!Q", length)
        if not self.closed:
            self.outgoing.put(header + payload)

    def send_json(self, obj):
        self.send_frame(0x1, json.dumps(obj))

    def deliver(self, event):
        if self.kind == "rtm":
            self.send_json(event)
            return
        self.envelopes += 1
        self.send_json({"envelope_id": "live-%d" % self.envelopes, "type": "events_api",
                        "accepts_response_payload": False, "retry_attempt": 0,
                        "payload": {"team_id": "T0NATTER", "type": "event_callback",
                                    "event_id": "EvLive%d" % self.envelopes, "event": event}})

    @staticmethod
    def parse_frame(buffer):
        """(fin, opcode, payload, consumed) or None when incomplete."""
        if len(buffer) < 2:
            return None
        b0, b1 = buffer[0], buffer[1]
        if not b1 & 0x80:
            raise ValueError("client frame not masked")
        length = b1 & 0x7F
        offset = 2
        if length == 126:
            if len(buffer) < 4:
                return None
            length = struct.unpack("!H", buffer[2:4])[0]
            offset = 4
        elif length == 127:
            if len(buffer) < 10:
                return None
            length = struct.unpack("!Q", buffer[2:10])[0]
            offset = 10
        if len(buffer) < offset + 4 + length:
            return None
        key = buffer[offset:offset + 4]
        payload = bytearray(buffer[offset + 4:offset + 4 + length])
        for i in range(length):
            payload[i] ^= key[i % 4]
        return bool(b0 & 0x80), b0 & 0x0F, bytes(payload), offset + 4 + length

    def run(self):
        scripted = []
        if self.kind == "rtm":
            self.send_json({"type": "hello", "start": True})
            if self.state.config["script_rtm"]:
                scripted = [e for e in self.state.load("rtm_events.json")
                            if e.get("type") not in ("hello", "pong") and "reply_to" not in e]
        else:
            envelopes = self.state.load("socket_mode_envelopes.json")
            self.send_json(envelopes[0])
            if self.state.config["script_socket"]:
                scripted = envelopes[1:]
        with self.state.lock:
            self.state.clients.append(self)

        def push():
            for item in scripted:
                time.sleep(0.02)
                self.send_json(item)
        threading.Thread(target=push, daemon=True).start()

        sock = self.handler.connection
        buffer = b""
        fragments = b""
        try:
            while True:
                while True:
                    try:
                        sock.sendall(self.outgoing.get_nowait())
                    except queue.Empty:
                        break
                pending = isinstance(sock, ssl.SSLSocket) and sock.pending() > 0
                if pending or select.select([sock], [], [], 0.02)[0]:
                    data = sock.recv(65536)
                    if not data:
                        break
                    buffer += data
                closing = False
                while True:
                    frame = self.parse_frame(buffer)
                    if frame is None:
                        break
                    fin, opcode, payload, consumed = frame
                    buffer = buffer[consumed:]
                    if opcode == 0x8:
                        self.send_frame(0x8, payload[:2])
                        closing = True
                        break
                    if opcode == 0x9:
                        self.send_frame(0xA, payload)
                        continue
                    if opcode == 0xA:
                        continue
                    fragments += payload
                    if fin:
                        text, fragments = fragments.decode("utf-8"), b""
                        self.on_text(json.loads(text))
                if closing:
                    while True:
                        try:
                            sock.sendall(self.outgoing.get_nowait())
                        except queue.Empty:
                            break
                    break
        except (ConnectionError, OSError, ValueError, struct.error):
            pass
        finally:
            self.closed = True
            with self.state.lock:
                if self in self.state.clients:
                    self.state.clients.remove(self)

    def on_text(self, message):
        if self.kind == "socket":
            if "envelope_id" in message:
                with self.state.lock:
                    self.state.acks.append(message["envelope_id"])
            return
        if message.get("type") == "ping":
            self.send_json({"type": "pong", "reply_to": message.get("id"), "time": int(time.time())})


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MockSlack/1.0"

    def log_message(self, fmt, *args):
        if os.environ.get("MOCK_SLACK_VERBOSE"):
            sys.stderr.write("mock: " + fmt % args + "\n")

    @property
    def state(self):
        return self.server.state

    def base_url(self, scheme="http"):
        secure = self.server.tls
        if scheme == "ws":
            scheme = "wss" if secure else "ws"
        else:
            scheme = "https" if secure else "http"
        return "%s://127.0.0.1:%d" % (scheme, self.server.server_address[1])

    def reply(self, status, body, content_type="application/json; charset=utf-8", headers=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body)
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def cookies(self):
        jar = {}
        for part in self.headers.get("Cookie", "").split(";"):
            if "=" in part:
                key, value = part.strip().split("=", 1)
                jar[key] = value
        return jar

    def params(self):
        parsed = urllib.parse.urlparse(self.path)
        values = urllib.parse.parse_qs(parsed.query)
        if self.command == "POST":
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length) if length else b""
            kind = self.headers.get("Content-Type", "")
            if "json" in kind and body:
                return {k: v for k, v in json.loads(body).items()}, body
            if "x-www-form-urlencoded" in kind:
                values.update(urllib.parse.parse_qs(body.decode("utf-8"), keep_blank_values=True))
            else:
                return {k: v[0] for k, v in values.items()}, body
        return {k: v[0] for k, v in values.items()}, b""

    def auth(self, params):
        """The token this request carries, or an error code."""
        bearer = self.headers.get("Authorization", "")
        token = params.get("token") or (bearer[7:] if bearer.startswith("Bearer ") else "")
        if not token:
            return None, "not_authed"
        if token == TOKEN:
            if self.cookies().get("d") != COOKIE:
                return None, "invalid_auth"
            return token, None
        if token in (USER_TOKEN, APP_TOKEN):
            return token, None
        return None, "invalid_auth"

    # ---- routing ------------------------------------------------------------------

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path.startswith("/rtm/") or path.startswith("/socket/"):
            return self.websocket(path)
        if path == "/_mock/state":
            with self.state.lock:
                return self.reply(200, {
                    "acks": self.state.acks, "rtm_connections": self.state.rtm_connections,
                    "socket_connections": self.state.socket_connections,
                    "handshake_cookies": self.state.handshake_cookies,
                    "uploads": {k: len(v) for k, v in self.state.uploads.items()},
                    "marks": self.state.marks, "calls": self.state.calls,
                    "messages": {c: sorted(m.keys()) for c, m in self.state.messages.items()}})
        if path in ("/", "/messages", "/ssb/redirect"):
            return self.workspace_page(path)
        if path == "/signin" or path.startswith("/client"):
            return self.web_client(path)
        if path.startswith("/files-pri/"):
            return self.download()
        if path.startswith("/api/"):
            return self.api(path[5:])
        self.reply(404, "not found", "text/plain")

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/_mock/config":
            params, body = self.params()
            self.state.config.update(json.loads(body or b"{}") if not params else params)
            return self.reply(200, {"ok": True, "config": self.state.config})
        if path == "/_mock/push":
            _, body = self.params()
            self.state.broadcast(json.loads(body))
            return self.reply(200, {"ok": True})
        if path.startswith("/upload/"):
            length = int(self.headers.get("Content-Length", "0"))
            data = self.rfile.read(length)
            with self.state.lock:
                self.state.uploads[path[8:]] = data
            return self.reply(200, "OK - %d" % len(data), "text/plain")
        if path.startswith("/api/"):
            return self.api(path[5:])
        self.reply(404, "not found", "text/plain")

    # ---- sign-in page and files ---------------------------------------------------------

    def workspace_page(self, path):
        # Slack's boot page redirects twice; the cookie must survive both hops.
        if path == "/":
            return self.reply(302, "", "text/plain", {"Location": "/messages"})
        if path == "/messages":
            return self.reply(302, "", "text/plain", {"Location": "/ssb/redirect"})
        if self.cookies().get("d") == COOKIE:
            body = ('<html><script>var boot_data = {"team_id":"T0NATTER",'
                    '"api_token":"%s","version":"1"};</script></html>' % TOKEN)
            # The real thing answers the logged-in page with a 403.
            return self.reply(403, body, "text/html; charset=utf-8")
        return self.reply(200, '<html><script>var boot_data = {"api_token":null};</script></html>',
                          "text/html; charset=utf-8")

    def web_client(self, path):
        """Slack's web sign-in, as a browser sees it: signing in leaves the d
        cookie (HttpOnly) and the tokens in localStorage "localConfig_v2", and
        opens the web client at /client/<team>."""
        if path == "/signin":
            config = {"teams": {"T0NATTER": {"id": "T0NATTER", "name": "Natter Test", "domain": "natter-test",
                                             "url": self.base_url() + "/", "token": TOKEN}}}
            body = ("<html><head><title>Sign in</title></head><body><p>Signing in...</p><script>"
                    "localStorage.setItem('localConfig_v2', %s);"
                    "setTimeout(function () { location.href = '/client/T0NATTER'; }, 300);"
                    "</script></body></html>" % json.dumps(json.dumps(config)))
            return self.reply(200, body, "text/html; charset=utf-8",
                              {"Set-Cookie": "d=%s; Path=/; HttpOnly" % COOKIE})
        return self.reply(200, "<html><head><title>Slack</title></head><body>The web client</body></html>",
                          "text/html; charset=utf-8")

    def download(self):
        token, error = self.auth({})
        if error:
            return self.reply(302, "", "text/plain", {"Location": "/ssb/redirect?login=1"})
        # /files-pri/T0NATTER-<file id>/<name>: an uploaded file comes back as
        # it was sent; fixture files are FILE_BYTES.
        parts = urllib.parse.urlparse(self.path).path.split("/")
        file_id = parts[2].split("-", 1)[-1] if len(parts) > 3 else ""
        with self.state.lock:
            data = self.state.uploads.get(file_id)
        if data:
            kind = mimetypes.guess_type(parts[-1])[0] or "application/octet-stream"
            return self.reply(200, data, kind)
        self.reply(200, FILE_BYTES, "image/png")

    # ---- WebSockets -------------------------------------------------------------------

    def websocket(self, path):
        kind = "rtm" if path.startswith("/rtm/") else "socket"
        if self.headers.get("Upgrade", "").lower() != "websocket":
            return self.reply(400, "expected a WebSocket upgrade", "text/plain")
        with self.state.lock:
            self.state.handshake_cookies.append(self.headers.get("Cookie", ""))
        # Session RTM sockets are authenticated by the d cookie.
        if kind == "rtm" and path.endswith("/session") and self.cookies().get("d") != COOKIE:
            return self.reply(401, "missing cookie", "text/plain")
        key = self.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.wfile.flush()
        with self.state.lock:
            if kind == "rtm":
                self.state.rtm_connections += 1
            else:
                self.state.socket_connections += 1
        WebSocketClient(self, kind, self.state).run()
        self.close_connection = True

    # ---- the Web API ------------------------------------------------------------------

    def api(self, method):
        params, raw = self.params()
        token, error = self.auth(params)
        with self.state.lock:
            self.state.calls.append(method)
        if error:
            return self.reply(200, {"ok": False, "error": error})
        if method == "apps.connections.open":
            if token != APP_TOKEN:
                return self.reply(200, {"ok": False, "error": "not_allowed_token_type"})
        elif token == APP_TOKEN:
            return self.reply(200, {"ok": False, "error": "not_allowed_token_type"})
        handler = getattr(self, "m_" + method.replace(".", "_"), None)
        if handler is None:
            return self.reply(200, {"ok": False, "error": "unknown_method"})
        try:
            result = handler(params, token)
        except KeyError as missing:
            result = {"ok": False, "error": "invalid_arguments", "detail": str(missing)}
        self.reply(200, result)

    def fixture(self, name):
        return self.state.load(name)

    def paged(self, params, first, second):
        if params.get("cursor"):
            return self.fixture(second)
        return self.fixture(first)

    def m_auth_test(self, params, token):
        return self.fixture("auth.test.json")

    def m_team_info(self, params, token):
        return self.fixture("team.info.json")

    def m_users_list(self, params, token):
        return self.paged(params, "users.list.page1.json", "users.list.page2.json")

    def m_users_info(self, params, token):
        return self.fixture("users.info.json")

    def m_users_getPresence(self, params, token):
        return self.fixture("users.getPresence.json")

    def m_conversations_list(self, params, token):
        body = self.paged(params, "conversations.list.page1.json", "conversations.list.page2.json")
        if params.get("exclude_archived") == "true":
            body["channels"] = [c for c in body["channels"] if not c.get("is_archived")]
        return body

    def m_conversations_info(self, params, token):
        body = self.fixture("conversations.info.json")
        body["channel"]["id"] = params["channel"]
        return body

    def m_conversations_members(self, params, token):
        return self.paged(params, "conversations.members.page1.json",
                          "conversations.members.page2.json")

    def m_conversations_open(self, params, token):
        return self.fixture("conversations.open.json")

    def m_conversations_mark(self, params, token):
        with self.state.lock:
            self.state.marks.append([params["channel"], params["ts"]])
        return {"ok": True}

    def m_conversations_history(self, params, token):
        channel = params["channel"]
        limit = int(params.get("limit", "100"))
        oldest = float(params.get("oldest", "0") or 0)
        latest = float(params.get("latest", "9e12") or 9e12)
        with self.state.lock:
            messages = [copy.deepcopy(m) for m in self.state.messages.get(channel, {}).values()
                        if oldest < float(m["ts"]) < latest]
        messages.sort(key=lambda m: float(m["ts"]), reverse=True)
        return {"ok": True, "messages": messages[:limit], "has_more": len(messages) > limit,
                "response_metadata": {"next_cursor": ""}}

    def m_conversations_replies(self, params, token):
        channel, ts = params["channel"], params["ts"]
        with self.state.lock:
            root = self.state.messages.get(channel, {}).get(ts)
            replies = list(self.state.replies.get((channel, ts), {}).values())
        if root is None:
            return {"ok": False, "error": "thread_not_found"}
        replies.sort(key=lambda m: float(m["ts"]))
        return {"ok": True, "messages": [root] + replies, "has_more": False}

    def post(self, channel, message):
        """Store a new message and push it to real-time clients."""
        with self.state.lock:
            thread_ts = message.get("thread_ts")
            if thread_ts and thread_ts != message["ts"]:
                self.state.replies.setdefault((channel, thread_ts), {})[message["ts"]] = message
                root = self.state.messages.get(channel, {}).get(thread_ts)
                if root is not None:
                    root["reply_count"] = root.get("reply_count", 0) + 1
                    root["latest_reply"] = message["ts"]
                    root["thread_ts"] = thread_ts
            else:
                self.state.messages.setdefault(channel, {})[message["ts"]] = message
        event = dict(message)
        event["channel"] = channel
        self.state.broadcast(event)

    def m_chat_postMessage(self, params, token):
        channel = params["channel"]
        if channel.startswith("#"):
            channel = "C01GENERAL" if channel == "#general" else channel
        user = "U03SELF"
        message = {"type": "message", "user": user, "text": params.get("text", ""),
                   "ts": self.state.next_ts(), "team": "T0NATTER"}
        if params.get("thread_ts"):
            message["thread_ts"] = params["thread_ts"]
        if params.get("blocks"):
            message["blocks"] = json.loads(params["blocks"])
        self.post(channel, message)
        return {"ok": True, "channel": channel, "ts": message["ts"], "message": message}

    def find(self, channel, ts):
        found = self.state.messages.get(channel, {}).get(ts)
        if found is None:
            for (c, _), replies in self.state.replies.items():
                if c == channel and ts in replies:
                    return replies[ts]
        return found

    def m_chat_update(self, params, token):
        channel, ts = params["channel"], params["ts"]
        with self.state.lock:
            message = self.find(channel, ts)
            if message is None:
                return {"ok": False, "error": "message_not_found"}
            previous = copy.deepcopy(message)
            message["text"] = params.get("text", "")
            message["edited"] = {"user": "U03SELF", "ts": self.state.next_ts()}
            current = copy.deepcopy(message)
        self.state.broadcast({"type": "message", "subtype": "message_changed", "hidden": True,
                              "channel": channel, "ts": self.state.next_ts(), "message": current,
                              "previous_message": previous})
        return {"ok": True, "channel": channel, "ts": ts, "text": current["text"], "message": current}

    def m_chat_delete(self, params, token):
        channel, ts = params["channel"], params["ts"]
        with self.state.lock:
            if self.state.messages.get(channel, {}).pop(ts, None) is None:
                return {"ok": False, "error": "message_not_found"}
        self.state.broadcast({"type": "message", "subtype": "message_deleted", "hidden": True,
                              "channel": channel, "deleted_ts": ts, "ts": self.state.next_ts()})
        return {"ok": True, "channel": channel, "ts": ts}

    def reaction(self, params, added):
        channel, ts, name = params["channel"], params["timestamp"], params["name"]
        with self.state.lock:
            message = self.find(channel, ts)
            if message is None:
                return {"ok": False, "error": "message_not_found"}
            reactions = message.setdefault("reactions", [])
            entry = next((r for r in reactions if r["name"] == name), None)
            if added:
                if entry and "U03SELF" in entry["users"]:
                    return {"ok": False, "error": "already_reacted"}
                if entry is None:
                    entry = {"name": name, "users": [], "count": 0}
                    reactions.append(entry)
                entry["users"].append("U03SELF")
                entry["count"] += 1
            else:
                if entry is None or "U03SELF" not in entry["users"]:
                    return {"ok": False, "error": "no_reaction"}
                entry["users"].remove("U03SELF")
                entry["count"] -= 1
                if entry["count"] <= 0:
                    reactions.remove(entry)
        self.state.broadcast({"type": "reaction_added" if added else "reaction_removed",
                              "user": "U03SELF", "reaction": name,
                              "item": {"type": "message", "channel": channel, "ts": ts},
                              "event_ts": self.state.next_ts()})
        return {"ok": True}

    def m_reactions_add(self, params, token):
        return self.reaction(params, True)

    def m_reactions_remove(self, params, token):
        return self.reaction(params, False)

    def m_emoji_list(self, params, token):
        return self.fixture("emoji.list.json")

    def m_search_messages(self, params, token):
        query = params["query"].lower()
        matches = []
        with self.state.lock:
            for channel, messages in self.state.messages.items():
                for message in messages.values():
                    if query in message.get("text", "").lower():
                        match = copy.deepcopy(message)
                        match["channel"] = {"id": channel, "name": "general"
                                            if channel == "C01GENERAL" else channel}
                        match["permalink"] = "%s/archives/%s/p%s" % (
                            self.base_url(), channel, message["ts"].replace(".", ""))
                        matches.append(match)
        matches.sort(key=lambda m: float(m["ts"]), reverse=True)
        count = int(params.get("count", "20"))
        return {"ok": True, "query": params["query"], "messages": {
            "total": len(matches), "matches": matches[:count],
            "paging": {"count": count, "total": len(matches), "page": 1, "pages": 1}}}

    def m_client_counts(self, params, token):
        if token != TOKEN:
            return {"ok": False, "error": "not_allowed_token_type"}
        return self.fixture("client.counts.json")

    def m_users_counts(self, params, token):
        return self.fixture("users.counts.json")

    def m_rtm_connect(self, params, token):
        if self.state.config["rtm_disabled"]:
            return {"ok": False, "error": "not_allowed_token_type"}
        body = self.fixture("rtm.connect.json")
        body["url"] = "%s/rtm/%s" % (self.base_url("ws"),
                                     "session" if token == TOKEN else "oauth")
        return body

    def m_apps_connections_open(self, params, token):
        return {"ok": True, "url": "%s/socket/ticket-%d" % (self.base_url("ws"), time.time())}

    def m_files_getUploadURLExternal(self, params, token):
        file_id = "F%06d" % self.state.counter
        self.state.next_ts()
        with self.state.lock:
            self.state.uploads.setdefault(file_id, b"")
        return {"ok": True, "upload_url": "%s/upload/%s" % (self.base_url(), file_id),
                "file_id": file_id, "expected_length": params.get("length")}

    def m_files_completeUploadExternal(self, params, token):
        files = json.loads(params["files"])
        entries = []
        with self.state.lock:
            for item in files:
                data = self.state.uploads.get(item["id"])
                if data is None or not data:
                    return {"ok": False, "error": "file_not_found"}
                title = item.get("title", "")
                entry = {"id": item["id"], "title": title, "name": title, "size": len(data),
                         "mimetype": mimetypes.guess_type(title)[0] or "text/plain",
                         "url_private": "%s/files-pri/T0NATTER-%s/%s" % (
                             self.base_url(), item["id"], title)}
                # Images get a thumbnail (the file itself) and, for PNG, their size.
                if entry["mimetype"].startswith("image/"):
                    entry["thumb_360"] = entry["url_private"]
                    if data[:8] == b"\x89PNG\r\n\x1a\n" and len(data) >= 24:
                        entry["original_w"], entry["original_h"] = struct.unpack(">II", data[16:24])
                entries.append(entry)
        channel = params.get("channel_id")
        if channel:
            message = {"type": "message", "subtype": "file_share", "user": "U03SELF",
                       "text": params.get("initial_comment", ""), "ts": self.state.next_ts(),
                       "files": entries}
            if params.get("thread_ts"):
                message["thread_ts"] = params["thread_ts"]
            self.post(channel, message)
        return {"ok": True, "files": [{"id": e["id"], "title": e["title"]} for e in entries]}


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fixtures", required=True)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--tls-cert")
    parser.add_argument("--tls-key")
    args = parser.parse_args()

    server = Server(("127.0.0.1", args.port), Handler)
    server.state = State(args.fixtures)
    server.tls = bool(args.tls_cert)
    if server.tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.tls_cert, args.tls_key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    print("PORT %d" % server.server_address[1], flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
