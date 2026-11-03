#!/usr/bin/env python3
"""Smoke test of the editor's MCP stdio transport (#452): no display, no sockets.

Starts `<editor> --headless --mcp-stdio <editor args>`, speaks newline-delimited JSON-RPC on its stdin/stdout
(initialize, notifications/initialized, tools/list, tools/call problems_list), then closes stdin and requires the
editor to exit with 0. Every line on stdout must be a JSON-RPC 2.0 message: anything else (a stray print, a log line)
breaks an MCP client. Standard library only; every wait has a timeout, so the script cannot hang.

    tools/headless_mcp_smoke.py --editor build/dev-debug/Editor/editor [--stderr editor.log] -- \
        --config Editor/config/editor.cfg --project projects/sandbox
"""
import argparse
import json
import queue
import subprocess
import sys
import threading
import time

PROTOCOL_VERSION = "2025-06-18"


class SmokeFailure(Exception):
    pass


class EditorSession:
    def __init__(self, command, stderr_path, request_timeout):
        self.request_timeout = request_timeout
        self.stderr_file = open(stderr_path, "wb") if stderr_path else None
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr_file,
        )
        self.lines = queue.Queue()
        self.received = []
        self.reader = threading.Thread(target=self._read_stdout, daemon=True)
        self.reader.start()

    def _read_stdout(self):
        for raw in iter(self.process.stdout.readline, b""):
            self.lines.put(raw)
        self.lines.put(None)

    def _check_line(self, raw):
        if not raw.endswith(b"\n"):
            raise SmokeFailure(f"stdout line without its newline: {raw[:200]!r}")
        try:
            message = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as error:
            raise SmokeFailure(f"stdout line is not JSON ({error}): {raw[:200]!r}")
        if not isinstance(message, dict) or message.get("jsonrpc") != "2.0":
            raise SmokeFailure(f"stdout line is not a JSON-RPC 2.0 message: {raw[:200]!r}")
        if "id" not in message and "method" not in message:
            raise SmokeFailure(f"JSON-RPC message with neither id nor method: {raw[:200]!r}")
        self.received.append(message)
        return message

    def send(self, message):
        try:
            self.process.stdin.write((json.dumps(message) + "\n").encode("utf-8"))
            self.process.stdin.flush()
        except (BrokenPipeError, OSError) as error:
            raise SmokeFailure(f"the editor does not take input any more: {error}")

    def request(self, request_id, method, params):
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        deadline = time.monotonic() + self.request_timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise SmokeFailure(f"no answer to {method} (id {request_id}) within {self.request_timeout} s")
            try:
                raw = self.lines.get(timeout=remaining)
            except queue.Empty:
                continue
            if raw is None:
                raise SmokeFailure(f"stdout closed before the answer to {method} (exit code {self.process.poll()})")
            message = self._check_line(raw)
            if message.get("id") == request_id:
                if "error" in message:
                    raise SmokeFailure(f"{method} failed: {message['error']}")
                if "result" not in message:
                    raise SmokeFailure(f"{method}: an answer without a result: {message}")
                return message["result"]
            print(f"  (notification) {message.get('method')}")

    def close_and_wait(self, exit_timeout):
        self.process.stdin.close()
        try:
            code = self.process.wait(timeout=exit_timeout)
        except subprocess.TimeoutExpired:
            raise SmokeFailure(f"the editor did not exit within {exit_timeout} s after stdin was closed")
        self.reader.join(timeout=10)
        while True:
            try:
                raw = self.lines.get_nowait()
            except queue.Empty:
                break
            if raw is not None:
                self._check_line(raw)
        return code

    def kill(self):
        if self.process.poll() is None:
            self.process.kill()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass

    def close_files(self):
        if self.stderr_file:
            self.stderr_file.close()


def run(arguments):
    editor_args = arguments.editor_args
    if editor_args and editor_args[0] == "--":
        editor_args = editor_args[1:]
    command = [arguments.editor, "--headless", "--mcp-stdio", *editor_args]
    print("$ " + " ".join(command))
    session = EditorSession(command, arguments.stderr, arguments.request_timeout)
    try:
        init = session.request(1, "initialize", {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "headless_mcp_smoke", "version": "1"},
        })
        if init.get("protocolVersion") != PROTOCOL_VERSION:
            raise SmokeFailure(f"initialize negotiated {init.get('protocolVersion')!r}")
        print(f"initialize: {init.get('serverInfo')}")
        session.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

        tools = session.request(2, "tools/list", {})
        names = sorted(tool.get("name") for tool in tools.get("tools", []))
        print(f"tools/list: {len(names)} tools")
        if "problems_list" not in names:
            raise SmokeFailure(f"problems_list is not among the tools: {names}")

        called = session.request(3, "tools/call", {"name": "problems_list", "arguments": {}})
        if called.get("isError"):
            raise SmokeFailure(f"problems_list answered with an error: {called}")
        print("tools/call problems_list: ok")

        code = session.close_and_wait(arguments.exit_timeout)
        if code != 0:
            raise SmokeFailure(f"the editor exited with {code} after stdin was closed, expected 0")
        print(f"exit code 0 after stdin was closed; {len(session.received)} JSON-RPC lines on stdout")
        return 0
    finally:
        session.kill()
        session.close_files()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--editor", required=True, help="the editor executable")
    parser.add_argument("--stderr", help="file for the editor's stderr (the log); default: inherited")
    parser.add_argument("--request-timeout", type=float, default=90.0, help="seconds to wait for each answer")
    parser.add_argument("--exit-timeout", type=float, default=30.0, help="seconds to wait for the exit after stdin closes")
    parser.add_argument("editor_args", nargs=argparse.REMAINDER, help="more arguments for the editor, after --")
    arguments = parser.parse_args()
    try:
        return run(arguments)
    except SmokeFailure as failure:
        print(f"::error::MCP stdio smoke: {failure}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
