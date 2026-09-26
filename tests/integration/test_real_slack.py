#!/usr/bin/env python3
# Natter - integration test against a real Slack workspace (opt-in).
# SPDX-License-Identifier: MIT
"""Exercise natter-cli against a real workspace.

Runs only when these are set (exits 77, "skipped", otherwise):
    NATTER_TEST_TOKEN      xoxc-... (or xoxp-...)
    NATTER_TEST_COOKIE     the d cookie, xoxd-... (session tokens)
    NATTER_TEST_WORKSPACE  acme or acme.slack.com

Read-only by default. With NATTER_TEST_CHANNEL (a channel id or name) and
NATTER_TEST_ALLOW_WRITE=1 it also posts, reacts to, edits and deletes one
message there. NATTER_TEST_APP_TOKEN (xapp-) adds a Socket Mode check.
"""

import argparse
import os
import subprocess
import sys

SKIP = 77


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    args = parser.parse_args()

    token = os.environ.get("NATTER_TEST_TOKEN", "")
    cookie = os.environ.get("NATTER_TEST_COOKIE", "")
    workspace = os.environ.get("NATTER_TEST_WORKSPACE", "")
    if not (token and cookie and workspace):
        print("NATTER_TEST_TOKEN, NATTER_TEST_COOKIE and NATTER_TEST_WORKSPACE are not set; "
              "skipping the real-Slack integration test")
        return SKIP

    base = [args.cli, "--token", token, "--cookie", cookie, "--workspace", workspace]
    failures = []

    def run(name, extra, check=None, timeout=60, required=True):
        result = subprocess.run(base + extra, capture_output=True, text=True, timeout=timeout)
        ok = result.returncode == 0 and (check is None or check(result.stdout))
        print("[%s] %s" % ("OK" if ok else ("FAIL" if required else "WARN"), name))
        if not ok:
            print(result.stdout[-2000:] + result.stderr[-2000:])
            if required:
                failures.append(name)
        return result

    run("auth.test", ["auth"], lambda out: out.startswith("ok "))
    if token.startswith("xoxc-"):
        # The unofficial part most likely to break: scraping the boot page.
        derived = subprocess.run([args.cli, "--cookie", cookie, "--workspace", workspace,
                                  "signin"], capture_output=True, text=True, timeout=60)
        ok = derived.returncode == 0 and derived.stdout.startswith("ok ")
        print("[%s] token from the d cookie" % ("OK" if ok else "FAIL"))
        if not ok:
            print(derived.stdout + derived.stderr)
            failures.append("signin")
    channels = run("conversations.list", ["channels"], lambda out: "\t" in out)
    run("unread counts", ["counts"], lambda out: out.startswith("source "))
    run("emoji.list", ["emoji"], required=False)
    run("search.messages", ["search", "the", "--count", "3"], lambda out: "matches" in out,
        required=False)

    channel = os.environ.get("NATTER_TEST_CHANNEL", "")
    if not channel:
        for line in channels.stdout.splitlines():
            fields = line.split("\t")
            if len(fields) >= 3 and fields[2].startswith("member"):
                channel = fields[0]
                break
    if channel:
        run("conversations.history", ["history", channel, "--limit", "5"])
    run("real time comes up", ["watch", "--seconds", "20", "--until", "ready"], timeout=40)

    app_token = os.environ.get("NATTER_TEST_APP_TOKEN", "")
    if app_token:
        run("socket mode", ["--app-token", app_token, "watch", "--mode", "socket",
                            "--seconds", "20", "--until", "ready"], timeout=40)

    if channel and os.environ.get("NATTER_TEST_ALLOW_WRITE") == "1":
        sent = run("chat.postMessage", ["send", channel, "natter integration test, please ignore"],
                   lambda out: out.startswith("sent "))
        if sent.returncode == 0:
            ts = sent.stdout.split()[2]
            run("reactions.add", ["react", channel, ts, "white_check_mark"])
            run("chat.update", ["edit", channel, ts, "natter integration test (edited)"])
            run("chat.delete", ["delete", channel, ts])

    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
