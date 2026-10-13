"""Tiny MJPEG server for testing: streams solid red, green and blue frames in turn.

    python tests/mjpeg_server.py --port 8099 --size 320x240 --fps 10 [--chunked] [--no-length]

--chunked answers with HTTP/1.1 chunked encoding (as Go servers such as go2rtc do),
--no-length leaves Content-Length out of each part so the client has to find the end itself.
Needs Pillow (it is in the ESPHome dev environment).
"""

from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import time

from PIL import Image

COLORS = [(255, 0, 0), (0, 255, 0), (0, 0, 255)]


def make_frames(width: int, height: int) -> list[bytes]:
    frames = []
    for color in COLORS:
        out = io.BytesIO()
        Image.new("RGB", (width, height), color).save(out, format="JPEG", quality=80)
        frames.append(out.getvalue())
    return frames


def make_handler(frames: list[bytes], fps: float, chunked: bool, with_length: bool):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1" if chunked else "HTTP/1.0"

        def log_message(self, fmt: str, *args) -> None:
            print(f"[server] {self.address_string()} {fmt % args}", flush=True)

        def write_body(self, data: bytes) -> None:
            if chunked:
                self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
            else:
                self.wfile.write(data)

        def do_GET(self) -> None:  # noqa: N802 (http.server naming)
            if not self.path.startswith("/stream.mjpeg"):
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-cache")
            if chunked:
                self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            index = 0
            try:
                while True:
                    jpeg = frames[index % len(frames)]
                    part = b"--frame\r\nContent-Type: image/jpeg\r\n"
                    if with_length:
                        part += f"Content-Length: {len(jpeg)}\r\n".encode()
                    part += b"\r\n" + jpeg + b"\r\n"
                    self.write_body(part)
                    self.wfile.flush()
                    index += 1
                    time.sleep(1 / fps)
            except (BrokenPipeError, ConnectionResetError):
                print(f"[server] client went away after {index} frames", flush=True)

    return Handler


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8099)
    parser.add_argument("--size", default="320x240")
    parser.add_argument("--fps", type=float, default=10)
    parser.add_argument("--chunked", action="store_true")
    parser.add_argument("--no-length", action="store_true")
    args = parser.parse_args()
    width, height = (int(v) for v in args.size.lower().split("x"))
    frames = make_frames(width, height)
    handler = make_handler(frames, args.fps, args.chunked, not args.no_length)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    print(f"[server] http://127.0.0.1:{args.port}/stream.mjpeg ({width}x{height}, {args.fps} fps)", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
