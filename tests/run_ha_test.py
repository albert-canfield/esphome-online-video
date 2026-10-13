"""End-to-end test of the Home Assistant camera mode, with a stand-in for Home Assistant.

    python tests/run_ha_test.py path/to/program

`program` is the binary built from tests/host-ha-test.yaml. The script:
1. starts the device; it must not request the stream before it has a token,
2. connects to it over the native API like Home Assistant, and answers the subscription to
   camera.test's access_token,
3. checks frames arrive, then rotates the token and restarts the server so only the new token
   works, and checks the device reconnects with it,
4. checks the token never appears in the device logs.
Needs aioesphomeapi (it is in the ESPHome dev environment).
"""

from __future__ import annotations

import argparse
import asyncio
import os
from pathlib import Path
import re
import subprocess
import sys

from aioesphomeapi import APIClient

HERE = Path(__file__).parent
TOKEN_ONE = "tok1-0123456789abcdef0123456789abcdef0123456789abcdef0123456789ab"
TOKEN_TWO = "tok2-fedcba9876543210fedcba9876543210fedcba9876543210fedcba987654"


def start_server(tokens: str, log: Path) -> subprocess.Popen:
    return subprocess.Popen(
        [sys.executable, str(HERE / "mjpeg_server.py"), "--port", "8099", "--fps", "2", "--ha-tokens", tokens],
        stdout=log.open("a"),
        stderr=subprocess.STDOUT,
    )


def frames(text: str) -> int:
    return len(re.findall(r"frame rgb=\d+,\d+,\d+", text))


async def main(program: Path) -> int:
    work = HERE / ".ha-test"
    work.mkdir(exist_ok=True)
    device_log, server_log = work / "device.log", work / "server.log"
    device_log.write_text("")
    server_log.write_text("")
    server = start_server(TOKEN_ONE, server_log)
    device = subprocess.Popen(
        [str(program)],
        stdout=device_log.open("w"),
        stderr=subprocess.STDOUT,
        env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
    )
    problems: list[str] = []
    try:
        await asyncio.sleep(4)
        if "HA request" in server_log.read_text():
            problems.append("the device asked for the stream before it had a token")

        # Act as Home Assistant.
        client = APIClient("127.0.0.1", 6053, None)
        await client.connect(login=True)
        current = {"token": TOKEN_ONE}
        subscribed = asyncio.Event()

        def on_state_sub(entity_id: str, attribute: str | None) -> None:
            if entity_id == "camera.test" and attribute == "access_token":
                client.send_home_assistant_state(entity_id, attribute, current["token"])
                subscribed.set()

        client.subscribe_home_assistant_states(on_state_sub)
        await asyncio.wait_for(subscribed.wait(), 10)
        await asyncio.sleep(8)
        first = frames(device_log.read_text())
        if first < 8:
            problems.append(f"only {first} frames with the first token")

        # Home Assistant rotates its camera tokens; only the new one works after a restart.
        current["token"] = TOKEN_TWO
        client.send_home_assistant_state("camera.test", "access_token", TOKEN_TWO)
        server.terminate()
        server.wait()
        server = start_server(TOKEN_TWO, server_log)
        await asyncio.sleep(12)
        second = frames(device_log.read_text()) - first
        if second < 8:
            problems.append(f"only {second} frames after the token changed")
        await client.disconnect()
    finally:
        device.terminate()
        device.wait()
        server.terminate()
        server.wait()

    log = device_log.read_text()
    if TOKEN_ONE in log or TOKEN_TWO in log or "tok1-" in log or "tok2-" in log:
        problems.append("the access token appears in the device log")
    accepted = server_log.read_text().count("token accepted")
    summary = f"{first} frames with token 1, {second} after rotating to token 2, {accepted} accepted connections"
    print(("PASS  " if not problems else "FAIL  ") + summary + ("" if not problems else " | " + "; ".join(problems)))
    return 0 if not problems else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("program", type=Path)
    sys.exit(asyncio.run(main(parser.parse_args().program)))
