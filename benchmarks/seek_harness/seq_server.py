#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Local harness for measuring Chrome seek behaviour.

Serves a test page and a video (with Range support) on 127.0.0.1, and collects
frame reports the page POSTs to /seq.  The video path comes from $SEEK_VIDEO.
A threading server is required: a seeking <video> issues several concurrent
Range requests, and a single-threaded server would stall the decoder and
invalidate the measurement (see docs/seek-debug.md).
"""

import http.server
import os
import re
import socketserver

PORT = int(os.environ.get("SEEK_PORT", "8756"))
BASE = os.path.dirname(os.path.abspath(__file__))
LOG = os.environ.get("SEEK_LOG", "/tmp/opencode/seq.log")
HTML_PATH = os.path.join(BASE, "seek_test.html")
VIDEO_PATH = os.environ.get("SEEK_VIDEO", "")
CHUNK = 1 << 20


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            with open(HTML_PATH, "rb") as fh:
                body = fh.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/video.mp4":
            self._serve_video()
            return
        self.send_response(404)
        self.end_headers()

    def _serve_video(self):
        if not VIDEO_PATH or not os.path.exists(VIDEO_PATH):
            self.send_response(404)
            self.end_headers()
            return
        size = os.path.getsize(VIDEO_PATH)
        rng = self.headers.get("Range")
        start, end, partial = 0, size - 1, False
        if rng:
            m = re.match(r"bytes=(\d*)-(\d*)", rng)
            if m:
                if m.group(1):
                    start = int(m.group(1))
                if m.group(2):
                    end = int(m.group(2))
                partial = True
        if start >= size:
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % size)
            self.end_headers()
            return
        length = end - start + 1
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if partial:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        with open(VIDEO_PATH, "rb") as fh:
            fh.seek(start)
            remaining = length
            while remaining > 0:
                data = fh.read(min(CHUNK, remaining))
                if not data:
                    break
                try:
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    return
                remaining -= len(data)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        data = self.rfile.read(n)
        os.makedirs(os.path.dirname(LOG), exist_ok=True)
        with open(LOG, "ab") as fh:
            fh.write(data + b"\n")
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

    def log_message(self, *args):
        pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    with Server(("127.0.0.1", PORT), Handler) as httpd:
        print("serving on http://127.0.0.1:%d/ video=%s log=%s"
              % (PORT, VIDEO_PATH, LOG), flush=True)
        httpd.serve_forever()
