#!/usr/bin/env python3
"""Smoke test of the editor's MCP stdio transport (#452): no display, no sockets.

Starts `<editor> --headless --mcp-stdio <editor args>`, speaks newline-delimited JSON-RPC on its stdin/stdout
(initialize, notifications/initialized, tools/list, tools/call problems_list), with --play also drives Play
(#491: play_start, play_pause, play_step{n: 2}, play_status until the two frames ran, play_stop), then closes stdin and
requires the editor to exit with 0. Every line on stdout must be a JSON-RPC 2.0 message: anything else (a stray print, a log line)
breaks an MCP client. Standard library only; every wait has a timeout, so the script cannot hang.

    tools/headless_mcp_smoke.py --editor build/dev-debug/Editor/editor [--stderr editor.log] [--play] -- \
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
    def __init__(self, command, stderr_path, request_timeout, overall_timeout):
        self.request_timeout = request_timeout
        self.overall_timeout = overall_timeout
        self.overall_deadline = time.monotonic() + overall_timeout
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

    def deadline(self, seconds):
        """now + seconds, but never past the overall deadline."""
        return min(time.monotonic() + seconds, self.overall_deadline)

    def request(self, request_id, method, params):
        what = f"{method} {params.get('name')}" if method == "tools/call" else method
        if time.monotonic() >= self.overall_deadline:
            raise SmokeFailure(f"the overall deadline of {self.overall_timeout} s passed before {what} (id {request_id})")
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        deadline = self.deadline(self.request_timeout)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise SmokeFailure(f"no answer to {what} (id {request_id}) within {self.request_timeout} s "
                                   f"or the overall deadline of {self.overall_timeout} s")
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


def call_tool(session, request_id, name, args=None):
    """tools/call; returns (is_error, structuredContent, text of the first content block)."""
    result = session.request(request_id, "tools/call", {"name": name, "arguments": args or {}})
    content = result.get("content") or []
    text = content[0].get("text", "") if content and isinstance(content[0], dict) else ""
    return bool(result.get("isError")), result.get("structuredContent"), text


def expect_status(name, structured, state):
    if not isinstance(structured, dict) or structured.get("state") != state:
        raise SmokeFailure(f"{name}: expected the state {state!r}, got {structured!r}")
    if not isinstance(structured.get("frame"), int) or not isinstance(structured.get("worlds"), list):
        raise SmokeFailure(f"{name}: not a play status: {structured!r}")
    return structured


def drive_play(session, first_id, timeout):
    """Start, pause, step 2 frames, wait for them, stop. Returns the next free request id."""
    request_id = first_id

    def next_id():
        nonlocal request_id
        request_id += 1
        return request_id

    is_error, structured, text = call_tool(session, next_id(), "play_status")
    if is_error:
        raise SmokeFailure(f"play_status answered with an error: {text}")
    expect_status("play_status", structured, "stopped")

    # The startup level may still be loading: Play refuses with `failed` until it is there.
    deadline = session.deadline(timeout)
    while True:
        is_error, structured, text = call_tool(session, next_id(), "play_start")
        if not is_error:
            break
        if not text.startswith("failed") or time.monotonic() > deadline:
            raise SmokeFailure(f"play_start answered with an error: {text}")
        time.sleep(0.5)
    expect_status("play_start", structured, "playing")
    if not structured["worlds"]:
        raise SmokeFailure(f"play_start: Play has no world: {structured!r}")
    print(f"tools/call play_start: playing, worlds {structured['worlds']}")

    is_error, structured, text = call_tool(session, next_id(), "play_step")
    if not is_error or "pause first" not in text:
        raise SmokeFailure(f"play_step while playing: expected 'pause first', got {text or structured!r}")

    is_error, structured, text = call_tool(session, next_id(), "play_pause")
    if is_error:
        raise SmokeFailure(f"play_pause answered with an error: {text}")
    paused_at = expect_status("play_pause", structured, "paused")["frame"]

    is_error, structured, text = call_tool(session, next_id(), "play_step", {"n": 2})
    if is_error:
        raise SmokeFailure(f"play_step answered with an error: {text}")
    expect_status("play_step", structured, "paused")
    deadline = session.deadline(timeout)
    while True:
        is_error, structured, text = call_tool(session, next_id(), "play_status")
        if is_error:
            raise SmokeFailure(f"play_status answered with an error: {text}")
        frame = expect_status("play_status", structured, "paused")["frame"]
        if frame == paused_at + 2:
            break
        if frame > paused_at + 2:
            raise SmokeFailure(f"play_step 2 from frame {paused_at} ran to frame {frame}")
        if time.monotonic() > deadline:
            raise SmokeFailure(f"play_step 2 from frame {paused_at} is still at frame {frame} after {timeout} s")
        time.sleep(0.1)
    print(f"tools/call play_step 2: frame {paused_at} -> {paused_at + 2}")

    is_error, structured, text = call_tool(session, next_id(), "play_stop")
    if is_error:
        raise SmokeFailure(f"play_stop answered with an error: {text}")
    expect_status("play_stop", structured, "stopped")
    print("tools/call play_stop: stopped")
    return next_id()


def run(arguments):
    editor_args = arguments.editor_args
    if editor_args and editor_args[0] == "--":
        editor_args = editor_args[1:]
    command = [arguments.editor, "--headless", "--mcp-stdio", *editor_args]
    print("$ " + " ".join(command))
    session = EditorSession(command, arguments.stderr, arguments.request_timeout, arguments.overall_timeout)
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

        if arguments.play:
            drive_play(session, 3, arguments.request_timeout)

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
    parser.add_argument("--overall-timeout", type=float, default=200.0,
                        help="seconds for everything before stdin is closed; a failure names the step that was waiting")
    parser.add_argument("--play", action="store_true", help="also start, pause, step and stop Play over MCP (#491)")
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
