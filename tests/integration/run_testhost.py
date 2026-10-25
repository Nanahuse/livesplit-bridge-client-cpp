#!/usr/bin/env python3

import argparse
import os
import queue
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

SKIP = 77


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True)
    parser.add_argument("--testhost", default="")
    parser.add_argument("--with-icons", action="store_true")
    args = parser.parse_args()

    testhost_path = args.testhost or os.environ.get("LIVESPLIT_BRIDGE_TESTHOST", "")
    testhost = Path(testhost_path)
    if not testhost_path or not testhost.is_file():
        print("LIVESPLIT_BRIDGE_TESTHOST is not set to an executable; skipping", file=sys.stderr)
        return SKIP

    port = free_port()
    command = [str(testhost), "--port", str(port)]
    if args.with_icons:
        command.append("--with-icons")

    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        creationflags=creationflags,
    )
    assert process.stdout is not None
    lines: queue.Queue[str] = queue.Queue()

    def read_output() -> None:
        try:
            for line in process.stdout:
                lines.put(line.rstrip())
        finally:
            lines.put("EOF")

    threading.Thread(target=read_output, daemon=True).start()

    output: list[str] = []
    ready = False
    deadline = time.monotonic() + 30
    try:
        while time.monotonic() < deadline:
            try:
                line = lines.get(timeout=max(0.05, deadline - time.monotonic()))
            except queue.Empty:
                break
            output.append(line)
            if line == "READY":
                ready = True
                break
            if line == "EOF":
                break
        if not ready:
            print("LiveSplit.Bridge TestHost did not become ready:", file=sys.stderr)
            print("\n".join(output), file=sys.stderr)
            return 1

        client = [args.client, "--port", str(port)]
        if args.with_icons:
            client.append("--with-icons")
        result = subprocess.run(client, text=True)
        return result.returncode
    finally:
        if process.poll() is None:
            assert process.stdin is not None
            try:
                process.stdin.write("\n")
                process.stdin.flush()
                process.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                process.kill()
                process.wait(timeout=5)
        for stream in (process.stdin, process.stdout):
            if stream is not None:
                stream.close()


if __name__ == "__main__":
    sys.exit(main())
