#!/usr/bin/env python3
"""Local Range/redirect server for MintVID HTTP source regression tests."""

import argparse
import http.server
import pathlib
import ssl
import threading
import urllib.parse


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    server_version = "MintVIDTestHTTP/1.0"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        if self.server.verbose:
            super().log_message(fmt, *args)

    def _record_headers(self):
        if not self.server.header_log:
            return
        line = (f"{self.command} {self.path}|"
                f"{self.headers.get('User-Agent', '')}|"
                f"{self.headers.get('Referer', '')}|"
                f"{self.headers.get('Range', '')}\n")
        with self.server.header_lock:
            with self.server.header_log.open("a", encoding="utf-8") as log:
                log.write(line)

    def _route(self):
        path = urllib.parse.urlsplit(self.path).path
        chunked = False
        head_length = False
        streaming = False
        drop = False
        if path.startswith("/chunked-head/"):
            chunked = True
            head_length = True
            path = path[len("/chunked-head"):]
        elif path.startswith("/chunked/"):
            chunked = True
            path = path[len("/chunked"):]
        elif path.startswith("/stream/"):
            # Length-less forward-only stream: chunked, no Content-Length, and
            # Range is ignored (always 200). Models a CDN serving live-style TS.
            chunked = True
            streaming = True
            path = path[len("/stream"):]
        elif path.startswith("/drop/"):
            # Seekable (Content-Length + Accept-Ranges + honours Range) but the
            # body is truncated after a small cap and the connection closed, so
            # the client must reconnect with a Range at a non-zero offset to
            # finish. Models a flaky CDN dropping a transfer mid-stream and
            # forces the byte-range resume path that moov-at-end files rely on.
            drop = True
            path = path[len("/drop"):]
        return path, chunked, head_length, streaming, drop

    def _candidate(self, path):
        if not path.startswith("/media/"):
            return None
        relative = urllib.parse.unquote(path[len("/media/"):])
        candidate = (self.server.root / relative).resolve()
        try:
            candidate.relative_to(self.server.root)
        except ValueError:
            self.send_error(403)
            return None
        if not candidate.is_file():
            self.send_error(404)
            return None
        return candidate

    def do_HEAD(self):
        self._record_headers()
        path, _chunked, _head_length, streaming, _drop = self._route()
        if path.startswith("/redirect/"):
            self.send_response(302)
            self.send_header("Location", "/media/" + path[len("/redirect/"):])
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        candidate = self._candidate(path)
        if candidate is None:
            if not self.wfile.closed and not path.startswith("/media/"):
                self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        if streaming:
            # No Content-Length and no Accept-Ranges: HEAD reveals no length,
            # so the client must fall back to forward-only streaming.
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(candidate.stat().st_size))
        self.send_header("Connection", "close")
        self.end_headers()

    def _serve_live_playlist(self):
        # A live media playlist: a sliding window that grows by one segment each
        # time it is polled (advancing EXT-X-MEDIA-SEQUENCE), until every segment
        # is published and EXT-X-ENDLIST is appended. Only GET advances the poll
        # counter, so a client following the stream sees each new segment once.
        total = self.server.live_total
        window = self.server.live_window
        with self.server.live_lock:
            poll = self.server.live_poll
            self.server.live_poll += 1
        published = min(window + poll, total)
        first = max(0, published - window)
        media_seq = first
        lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2",
                 f"#EXT-X-MEDIA-SEQUENCE:{media_seq}"]
        for j in range(first, published):
            lines += ["#EXTINF:2.0,", f"/media/hls/lseg{j}.ts"]
        if published >= total:
            lines.append("#EXT-X-ENDLIST")
        body = ("\n".join(lines) + "\n").encode("ascii")
        self.send_response(200)
        self.send_header("Content-Type", "application/vnd.apple.mpegurl")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        self._record_headers()
        path, chunked, head_length, streaming, drop = self._route()
        if (path.endswith("/no-zero-range.ts") and
                self.headers.get("Range") == "bytes=0-"):
            self.send_error(403)
            return
        if path == "/live/playlist.m3u8":
            self._serve_live_playlist()
            return

        if path.startswith("/redirect/"):
            location = "/media/" + path[len("/redirect/"):]
            if head_length:
                location = "/chunked-head" + location
            elif streaming:
                location = "/stream" + location
            elif chunked:
                location = "/chunked" + location
            elif drop:
                location = "/drop" + location
            self.send_response(302)
            self.send_header("Location", location)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        candidate = self._candidate(path)
        if candidate is None:
            if not path.startswith("/media/"):
                self.send_error(404)
            return

        size = candidate.stat().st_size
        start = 0
        status = 200
        range_header = self.headers.get("Range")
        # A streaming endpoint ignores Range entirely and always sends the whole
        # body with 200 + chunked, so the client can never learn a length.
        ignore_zero_open_range = streaming or (head_length and
                                               range_header == "bytes=0-")
        if range_header and not ignore_zero_open_range:
            if not range_header.startswith("bytes=") or "," in range_header:
                self.send_error(416)
                return
            value, _, last_text = range_header[6:].partition("-")
            try:
                start = int(value)
                last = int(last_text) if last_text else None
            except ValueError:
                self.send_error(416)
                return
            if start < 0 or start >= size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            status = 206
            if start > 0 and self.server.range_marker:
                self.server.range_marker.touch()

        end = size - 1
        if status == 206 and last is not None and last < end:
            end = last
        length = end - start + 1
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        if not streaming:
            self.send_header("Accept-Ranges", "bytes")
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(length))
        if status == 206:
            self.send_header("Content-Range",
                             f"bytes {start}-{end}/{size}")
        self.send_header("Connection", "close")
        self.end_headers()

        # A /drop/ response advertises the full length but only ever writes up
        # to drop_cap bytes before the Connection: close closes the socket,
        # forcing the client to resume with a fresh Range from where it stopped.
        sendable = min(length, self.server.drop_cap) if drop else length
        with candidate.open("rb") as source:
            source.seek(start)
            remaining = sendable
            while remaining:
                block = source.read(min(8191 if chunked else 64 * 1024,
                                        remaining))
                if not block:
                    break
                try:
                    if chunked:
                        self.wfile.write(
                            f"{len(block):X};fixture=yes\r\n".encode("ascii"))
                    self.wfile.write(block)
                    if chunked:
                        self.wfile.write(b"\r\n")
                except (BrokenPipeError, ConnectionResetError):
                    break
                remaining -= len(block)
            if chunked and not remaining:
                try:
                    self.wfile.write(b"0\r\nX-Fixture: done\r\n\r\n")
                except (BrokenPipeError, ConnectionResetError):
                    pass

    def do_POST(self):
        self._record_headers()
        path = urllib.parse.urlsplit(self.path).path
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(400)
            return
        body_in = self.rfile.read(length)
        if path != "/youtubei/v1/player" or not body_in:
            self.send_error(404)
            return
        body = (
            '{"streamingData":{"hlsManifestUrl":'
            '"https://manifest.googlevideo.com/api/manifest/'
            'hls_variant/file/index.m3u8"}}'
        ).encode("ascii")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--port-file", required=True)
    parser.add_argument("--cert")
    parser.add_argument("--key")
    parser.add_argument("--range-marker")
    parser.add_argument("--header-log")
    parser.add_argument("--drop-cap", type=int, default=16384,
                        help="max body bytes a /drop/ response sends before "
                             "closing, forcing a byte-range resume")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    root = pathlib.Path(args.root).resolve()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FixtureHandler)
    server.root = root
    server.verbose = args.verbose
    server.range_marker = (pathlib.Path(args.range_marker)
                           if args.range_marker else None)
    server.header_log = (pathlib.Path(args.header_log)
                         if args.header_log else None)
    server.header_lock = threading.Lock()
    server.drop_cap = args.drop_cap
    # Live HLS playlist state: total segments to publish, sliding-window size,
    # and the poll counter that advances the window on each GET.
    server.live_total = 6
    server.live_window = 3
    server.live_poll = 0
    server.live_lock = threading.Lock()
    if args.cert or args.key:
        if not args.cert or not args.key:
            parser.error("--cert and --key must be supplied together")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.cert, args.key)
        server.socket = context.wrap_socket(server.socket, server_side=True)

    pathlib.Path(args.port_file).write_text(str(server.server_port),
                                            encoding="ascii")
    server.serve_forever()


if __name__ == "__main__":
    main()
