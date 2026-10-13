"""Run the end-to-end host test against the three kinds of stream the server can send.

    python tests/run_host_test.py path/to/program [--seconds 12]

`program` is the binary built from tests/host-test.yaml (esphome compile tests/host-test.yaml).
For each server mode it checks that frames arrive, decode to the colour the server sent, and come
in the order the server sends them, with no stream errors.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import time

HERE = Path(__file__).parent
# name: (server arguments, whether the frame is smaller than the 320x240 image)
MODES = {
    "content-length": ([], False),
    "chunked (like go2rtc)": (["--chunked"], False),
    "no content-length": (["--no-length"], False),
    "640x480, decoded at 1/2": (["--size", "640x480"], False),
    "1280x960, decoded at 1/4": (["--size", "1280x960"], False),
    "160x120, centred with borders": (["--size", "160x120"], True),
    "400x240, edges cropped to fill (cover)": (["--size", "400x240"], False),
}
FRAME_RE = re.compile(r"frame rgb=(\d+),(\d+),(\d+) corner=(\d+),(\d+),(\d+)")
ORDER = "RGB"


def colour(r: int, g: int, b: int) -> str:
    values = {"R": r, "G": g, "B": b}
    top = max(values, key=values.get)
    others = [v for k, v in values.items() if k != top]
    return top if values[top] > 180 and max(others) < 80 else "?"


def run_mode(program: Path, extra: list[str], bordered: bool, seconds: float) -> tuple[bool, str]:
    server = subprocess.Popen(
        [sys.executable, str(HERE / "mjpeg_server.py"), "--port", "8099", "--fps", "10", *extra],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    time.sleep(1.0)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy")
    device = subprocess.Popen(
        [str(program)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env
    )
    try:
        out, _ = device.communicate(timeout=seconds)
    except subprocess.TimeoutExpired:
        device.terminate()
        out, _ = device.communicate()
    server.terminate()
    server.communicate()

    matches = [tuple(map(int, m.groups())) for m in FRAME_RE.finditer(out)]
    colours = "".join(colour(*m[:3]) for m in matches)
    problems = []
    for m in matches:
        corner_black = max(m[3:]) < 30
        if bordered != corner_black or (not bordered and colour(*m[3:]) != colour(*m[:3])):
            problems.append(f"corner pixel {m[3:]} is wrong for this frame size")
            break
    if len(colours) < seconds * 4:
        problems.append(f"only {len(colours)} frames in {seconds:.0f} s")
    if "?" in colours:
        problems.append(f"{colours.count('?')} frames had the wrong colour")
    for a, b in zip(colours, colours[1:]):
        if "?" not in (a, b) and b == a:
            problems.append("the same colour twice in a row (a frame was shown twice)")
            break
    if "stream error" in out:
        problems.append("the stream reported an error")
    summary = f"{len(colours)} frames: {colours[:30]}{'...' if len(colours) > 30 else ''}"
    return not problems, summary + ("" if not problems else " | " + "; ".join(problems))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("program", type=Path)
    parser.add_argument("--seconds", type=float, default=12)
    args = parser.parse_args()
    all_ok = True
    for name, (extra, bordered) in MODES.items():
        ok, summary = run_mode(args.program, extra, bordered, args.seconds)
        all_ok &= ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}: {summary}", flush=True)
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
