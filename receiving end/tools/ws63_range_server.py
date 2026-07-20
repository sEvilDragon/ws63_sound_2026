#!/usr/bin/env python3
"""Small static HTTP server with single-range support for WS63 testing.

This intentionally has no external dependencies.  It serves files below
``--directory`` and implements the ``bytes=N-`` form used by the WS63 client,
returning a standards-compliant ``206 Partial Content`` response.
"""

from __future__ import annotations

import argparse
import http.server
import os
import re
import socketserver
from urllib.parse import urlsplit


RANGE_RE = re.compile(r"^bytes=(\d*)-(\d*)$")


class RangeRequestHandler(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _open_file(self):
        path = self.translate_path(urlsplit(self.path).path)
        if os.path.isdir(path):
            self.send_error(404, "directory listing is disabled")
            return None
        try:
            return open(path, "rb"), path
        except OSError:
            self.send_error(404, "file not found")
            return None

    def _send_file(self, head_only: bool):
        opened = self._open_file()
        if opened is None:
            return
        file_obj, path = opened
        try:
            self.close_connection = True
            size = os.fstat(file_obj.fileno()).st_size
            range_header = self.headers.get("Range")
            start = 0
            end = size - 1
            status = 200

            if range_header:
                match = RANGE_RE.fullmatch(range_header.strip())
                if match is None or "," in range_header:
                    self._send_range_error(size)
                    return
                first, last = match.groups()
                if first == "" and last == "":
                    self._send_range_error(size)
                    return
                if first == "":
                    suffix_length = int(last)
                    if suffix_length <= 0:
                        self._send_range_error(size)
                        return
                    start = max(0, size - suffix_length)
                else:
                    start = int(first)
                    if start >= size:
                        self._send_range_error(size)
                        return
                    if last:
                        end = min(int(last), size - 1)
                if start > end:
                    self._send_range_error(size)
                    return
                status = 206

            length = end - start + 1 if size else 0
            self.send_response(status)
            self.send_header("Content-Type", self.guess_type(path))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(length))
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.send_header("Connection", "close")
            self.end_headers()
            if head_only or length == 0:
                return

            file_obj.seek(start)
            remaining = length
            while remaining:
                block = file_obj.read(min(1024 * 1024, remaining))
                if not block:
                    break
                self.wfile.write(block)
                remaining -= len(block)
        finally:
            file_obj.close()

    def _send_range_error(self, size: int):
        self.send_response(416, "Range Not Satisfiable")
        self.send_header("Content-Range", f"bytes */{size}")
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()

    def do_GET(self):  # noqa: N802 - required by BaseHTTPRequestHandler
        self._send_file(head_only=False)

    def do_HEAD(self):  # noqa: N802 - required by BaseHTTPRequestHandler
        self._send_file(head_only=True)


class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--directory", required=True)
    args = parser.parse_args()

    directory = os.path.abspath(os.path.expanduser(args.directory))
    if not os.path.isdir(directory):
        parser.error(f"directory does not exist: {directory}")

    handler = lambda *h_args, **h_kwargs: RangeRequestHandler(  # noqa: E731
        *h_args, directory=directory, **h_kwargs
    )
    with ThreadingHTTPServer((args.bind, args.port), handler) as server:
        print(f"serving {directory} on http://{args.bind}:{args.port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
