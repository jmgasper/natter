#!/usr/bin/env python3
# Natter - end-to-end tests: natter-cli against the local mock Slack.
# SPDX-License-Identifier: MIT
"""Run natter-cli against tests/mock/mock_slack.py.

    test_e2e.py --cli build/natter-cli --fixtures tests/fixtures --work /tmp/x

Covers session sign-in from the d cookie, the Web API commands, file upload
and download, and `watch` over RTM, Socket Mode and the polling fallback -
plus the same over TLS (https + wss) with a certificate generated here.
"""

import argparse
import json
import os
import shutil
import ssl
import stat
import subprocess
import sys
import threading
import time
import traceback
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
MOCK = os.path.join(HERE, "..", "mock", "mock_slack.py")

TOKEN = "xoxc-e2e-1111-2222-3333"
COOKIE = "xoxd-e2e%2Fcookie%3D"
USER_TOKEN = "xoxp-e2e-4444"
APP_TOKEN = "xapp-e2e-5555"
FILE_BYTES = b"\x89PNG\r\n\x1a\nmock-image-bytes"


class Failure(Exception):
    pass


def expect(condition, message):
    if not condition:
        raise Failure(message)


class Mock:
    def __init__(self, fixtures, work, cert=None, key=None):
        command = [sys.executable, MOCK, "--fixtures", fixtures]
        if cert:
            command += ["--tls-cert", cert, "--tls-key", key]
        self.log = open(os.path.join(work, "mock-%s.log" % ("tls" if cert else "plain")), "w")
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=self.log,
                                        text=True)
        line = self.process.stdout.readline()
        if not line.startswith("PORT "):
            raise Failure("mock did not start: %r" % line)
        self.port = int(line.split()[1])
        self.scheme = "https" if cert else "http"
        self.context = None
        if cert:
            self.context = ssl.create_default_context(cafile=cert)

    def url(self, path):
        return "%s://127.0.0.1:%d%s" % (self.scheme, self.port, path)

    def state(self):
        with urllib.request.urlopen(self.url("/_mock/state"), context=self.context) as reply:
            return json.load(reply)

    def configure(self, **settings):
        request = urllib.request.Request(self.url("/_mock/config"),
                                         data=json.dumps(settings).encode(),
                                         headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, context=self.context) as reply:
            return json.load(reply)

    def stop(self):
        self.process.terminate()
        try:
            self.process.wait(5)
        except subprocess.TimeoutExpired:
            self.process.kill()
        self.log.close()


class Cli:
    def __init__(self, binary, mock, work):
        self.binary = binary
        self.mock = mock
        self.work = work

    def base(self, token=TOKEN, cookie=COOKIE, app_token=None, ca_file=None):
        args = [self.binary, "--api-base", self.mock.url("/api/"),
                "--workspace", "127.0.0.1:%d" % self.mock.port]
        if token:
            args += ["--token", token]
        if cookie:
            args += ["--cookie", cookie]
        if app_token:
            args += ["--app-token", app_token]
        if ca_file:
            args += ["--ca-file", ca_file]
        return args

    def run(self, *args, expect_ok=True, timeout=30, **auth):
        result = subprocess.run(self.base(**auth) + list(args), capture_output=True,
                                text=True, timeout=timeout)
        if expect_ok and result.returncode != 0:
            raise Failure("natter-cli %s failed (%d):\n%s%s" % (
                " ".join(args), result.returncode, result.stdout, result.stderr))
        return result

    def watch(self, *args, **auth):
        return Watcher(self.base(**auth) + ["watch"] + list(args))


class Watcher:
    """A running `natter-cli watch`, its output collected line by line."""

    def __init__(self, command):
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True)
        self.lines = []
        self.lock = threading.Condition()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stdout:
            with self.lock:
                self.lines.append(line.rstrip("\n"))
                self.lock.notify_all()

    def wait_for(self, text, timeout=15):
        deadline = time.time() + timeout
        with self.lock:
            while not any(text in line for line in self.lines):
                left = deadline - time.time()
                if left <= 0:
                    raise Failure("watch never printed %r; got:\n%s" % (text, self.output()))
                self.lock.wait(left)

    def finish(self, timeout=20):
        try:
            code = self.process.wait(timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            raise Failure("watch did not finish; got:\n%s" % self.output())
        self.reader.join(5)
        return code

    def output(self):
        return "\n".join(self.lines)


# ---- tests -------------------------------------------------------------------------------

def test_signin_from_cookie(cli, mock, work):
    path = os.path.join(work, "credentials.json")
    result = cli.run("signin", "--save", path, token=None)
    expect("ok team=Natter Test" in result.stdout, result.stdout)
    expect(stat.S_IMODE(os.stat(path).st_mode) == 0o600, "credentials file is not 0600")
    saved = json.load(open(path))
    expect(saved["token"] == TOKEN, saved)
    expect(saved["cookie"] == COOKIE, saved)
    expect(saved["team_id"] == "T0NATTER", saved)
    # The saved file alone is enough to talk to the workspace.
    result = subprocess.run([cli.binary, "--credentials", path, "auth"], capture_output=True,
                            text=True, timeout=30)
    expect(result.returncode == 0 and "mode=session" in result.stdout,
           result.stdout + result.stderr)


def test_bad_credentials(cli, mock, work):
    result = cli.run("auth", cookie="xoxd-wrong", expect_ok=False)
    expect(result.returncode == 1 and "invalid_auth" in result.stderr, result.stderr)
    result = cli.run("signin", token=None, cookie="xoxd-stale", expect_ok=False)
    expect(result.returncode == 1 and "token_not_found" in result.stderr, result.stderr)
    result = cli.run("history", "general", token="xoxc-nope", expect_ok=False)
    expect(result.returncode == 1, result.stdout)


def test_channels_and_counts(cli, mock, work):
    out = cli.run("channels").stdout
    expect("C01GENERAL\t#general\tmember unread=1 mentions=1" in out, out)
    expect("D04ALICE\t@ali\tmember unread=2 mentions=2" in out, out)
    expect("G05MPIM\talice, bob, natter" in out, out)
    expect("G03SECRET\t#secret\tmember private" in out, out)
    expect("C06OLD" not in out, "archived channel listed:\n" + out)
    counts = cli.run("counts").stdout
    expect(counts.startswith("source client.counts"), counts)


def test_history_and_thread(cli, mock, work):
    out = cli.run("history", "#general", "--limit", "50").stdout
    expect("Welcome everyone to #random \U0001F389 :tada:x2 :+1::skin-tone-3:x1" in out, out)
    expect("hello @ali & friends [file shot.png] (edited)" in out, out)
    expect("deploybot\tDeployed v1.2" in out, out)
    expect("Thread root [2 replies]" in out, out)
    expect("Plan for today with @Bob Brown \U0001F680" in out, out)
    thread = cli.run("thread", "general", "1727000200.000400").stdout.splitlines()
    expect(len(thread) == 3 and "first reply" in thread[1], thread)


def test_send_react_edit_delete_search(cli, mock, work):
    out = cli.run("send", "general", "an e2e searchable *note*").stdout
    expect(out.startswith("sent C01GENERAL "), out)
    ts = out.split()[2]
    cli.run("react", "general", ts, ":thumbsup:")
    history = cli.run("history", "general").stdout
    expect("an e2e searchable note :thumbsup:x1" in history, history)
    result = cli.run("react", "general", ts, "thumbsup", "--remove")
    expect("removed" in result.stdout, result.stdout)
    cli.run("edit", "general", ts, "an e2e searchable note, edited")
    history = cli.run("history", "general").stdout
    expect("an e2e searchable note, edited (edited)" in history, history)
    found = cli.run("search", "e2e searchable").stdout
    expect(found.startswith("1 matches"), found)
    expect("#general %s" % ts in found, found)
    reply = cli.run("send", "general", "threaded reply", "--thread", ts).stdout
    expect(reply.startswith("sent "), reply)
    thread = cli.run("thread", "general", ts).stdout
    expect("threaded reply" in thread, thread)
    cli.run("delete", "general", ts)
    history = cli.run("history", "general").stdout
    expect("e2e searchable" not in history, history)


def test_upload_and_download(cli, mock, work):
    path = os.path.join(work, "notes.txt")
    with open(path, "w") as handle:
        handle.write("some notes for the mock\n")
    out = cli.run("upload", "general", path, "--comment", "notes attached").stdout
    expect(out.startswith("uploaded F"), out)
    file_id = out.split()[1]
    expect(mock.state()["uploads"].get(file_id) == 24, mock.state()["uploads"])
    history = cli.run("history", "general").stdout
    expect("notes attached [file notes.txt]" in history, history)

    target = os.path.join(work, "shot.png")
    url = mock.url("/files-pri/T0NATTER-F01SHOT/shot.png")
    cli.run("download", url, target)
    expect(open(target, "rb").read() == FILE_BYTES, "downloaded bytes differ")
    denied = cli.run("download", url, target + ".2", cookie="xoxd-wrong", expect_ok=False)
    expect(denied.returncode == 1 and not os.path.exists(target + ".2"),
           denied.stdout + denied.stderr)


def test_watch_rtm(cli, mock, work):
    watch = cli.watch("--mode", "rtm", "--channel", "general", "--seconds", "30",
                      "--until", "rtm live message")
    watch.wait_for("ready")
    watch.wait_for("event dnd_updated_user")   # the end of the scripted stream
    cli.run("send", "general", "an rtm live message")
    code = watch.finish()
    out = watch.output()
    expect(code == 0, "watch exit %d:\n%s" % (code, out))
    for text in ("connection connected via rtm", "rtm says hi \U0001F44B\U0001F3FC",
                 "thread=1727000200.000400", "message_changed C01GENERAL 1727000600.000700",
                 "reaction_added C01GENERAL 1727000600.000700 :thumbsup: by U02BOB",
                 "reaction_removed", "user_typing C01GENERAL U01ALICE",
                 "presence_change away U01ALICE U02BOB", "message_deleted C01GENERAL",
                 "channel_marked C01GENERAL", "im_marked D04ALICE",
                 "channel_joined C07NOTMINE", "channel_left C02RANDOM",
                 "member_joined_channel G03SECRET U02BOB", "user_change U02BOB bobby",
                 "emoji_changed add natter", "emoji_changed remove", "| an rtm live message"):
        expect(text in out, "missing %r in:\n%s" % (text, out))
    cookies = mock.state()["handshake_cookies"]
    expect(("d=" + COOKIE) in cookies, cookies)


def test_watch_socket_mode(cli, mock, work):
    watch = cli.watch("--mode", "socket", "--seconds", "30", "--until", "socket live message",
                      app_token=APP_TOKEN)
    watch.wait_for("ready")
    watch.wait_for("event slash_commands")
    cli.run("send", "general", "a socket live message")
    code = watch.finish()
    out = watch.output()
    expect(code == 0, "watch exit %d:\n%s" % (code, out))
    expect(out.count("| socket mode hello") == 1, "retried envelope not de-duplicated:\n" + out)
    expect("reaction_added C01GENERAL 1727000800.000900 :eyes: by U01ALICE" in out, out)
    acks = mock.state()["acks"]
    for envelope in ("env-1", "env-2", "env-3", "env-4"):
        expect(envelope in acks, "no ack for %s: %s" % (envelope, acks))
    expect(any(a.startswith("live-") for a in acks), acks)


def test_watch_polling_fallback(cli, mock, work):
    mock.configure(rtm_disabled=True)
    try:
        watch = cli.watch("--channel", "general", "--seconds", "30", "--poll-ms", "300",
                          "--until", "a polled message")
        watch.wait_for("connection polling via polling")
        time.sleep(0.8)   # let the first poll record its baseline
        cli.run("send", "general", "a polled message")
        code = watch.finish()
        out = watch.output()
        expect(code == 0, "watch exit %d:\n%s" % (code, out))
        expect("not_allowed_token_type" in out, out)
        expect("counts " in out, out)
    finally:
        mock.configure(rtm_disabled=False)


def test_user_token_mode(cli, mock, work):
    out = cli.run("channels", token=USER_TOKEN, cookie=None).stdout
    expect("C01GENERAL\t#general\tmember unread=3 mentions=1" in out, out)
    counts = cli.run("counts", token=USER_TOKEN, cookie=None).stdout
    expect(counts.startswith("source users.counts"), counts)
    watch = cli.watch("--mode", "rtm", "--seconds", "20", "--until", "connected via rtm",
                      token=USER_TOKEN, cookie=None)
    expect(watch.finish() == 0, watch.output())
    expect(mock.state()["handshake_cookies"][-1] == "", "a cookie was sent for xoxp")


def make_certificate(work):
    openssl = shutil.which("openssl")
    if openssl is None:
        return None, None
    cert = os.path.join(work, "cert.pem")
    key = os.path.join(work, "key.pem")
    result = subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                             "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=127.0.0.1",
                             "-addext", "subjectAltName=IP:127.0.0.1"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        print("  (openssl failed, TLS test skipped: %s)" % result.stderr.strip())
        return None, None
    return cert, key


def test_tls(cli, mock, work):
    cert, key = make_certificate(work)
    if cert is None:
        print("  (no openssl; TLS test skipped)")
        return
    secure = Mock(cli.fixtures, work, cert, key)
    try:
        tls = Cli(cli.binary, secure, work)
        out = tls.run("auth", ca_file=cert).stdout
        expect("ok team=Natter Test" in out, out)
        # Without the CA the certificate must be refused.
        refused = tls.run("auth", expect_ok=False)
        expect(refused.returncode == 1 and "Transport" in refused.stderr, refused.stderr)
        watch = tls.watch("--mode", "rtm", "--seconds", "30", "--until", "tls live message",
                          ca_file=cert)
        watch.wait_for("connection connected via rtm")
        tls.run("send", "general", "a tls live message", ca_file=cert)
        code = watch.finish()
        expect(code == 0, "watch exit %d:\n%s" % (code, watch.output()))
    finally:
        secure.stop()


TESTS = [test_signin_from_cookie, test_bad_credentials, test_channels_and_counts,
         test_history_and_thread, test_send_react_edit_delete_search, test_upload_and_download,
         test_watch_rtm, test_watch_socket_mode, test_watch_polling_fallback,
         test_user_token_mode, test_tls]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--fixtures", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("only", nargs="*", help="run only tests whose name contains this")
    args = parser.parse_args()

    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    mock = Mock(args.fixtures, args.work)
    cli = Cli(os.path.abspath(args.cli), mock, args.work)
    cli.fixtures = args.fixtures
    failures = 0
    try:
        for test in TESTS:
            if args.only and not any(name in test.__name__ for name in args.only):
                continue
            started = time.time()
            try:
                test(cli, mock, args.work)
                print("[ OK ] %s (%.1f s)" % (test.__name__, time.time() - started), flush=True)
            except Exception as error:
                failures += 1
                print("[FAIL] %s: %s" % (test.__name__, error), flush=True)
                if not isinstance(error, Failure):
                    traceback.print_exc()
    finally:
        mock.stop()
    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
