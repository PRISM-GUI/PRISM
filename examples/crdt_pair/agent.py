#!/usr/bin/env python3
"""Join a PRISM crdt hub as an agent and show that an agent is just a peer.

The hub speaks the y-protocols sync messages, so pycrdt's Provider does the
whole job: it syncs the document, then sends our writes. Nothing below is
PRISM-specific.

    python3 agent.py ws://127.0.0.1:8000/room [gain]
"""

import asyncio
import json
import sys
from urllib.parse import urlsplit, urlunsplit

from httpx_ws import aconnect_ws
from pycrdt import Doc, Map, Provider
from pycrdt.websocket.websocket import HttpxWebsocket

SYNC_TIMEOUT = 10.0
ROOT_MAP = "model"


def http_url(ws_url: str) -> tuple[str, str]:
    """Return the URL httpx-ws upgrades and the room path inside it.

    httpx-ws performs the upgrade with an ordinary HTTP request, so it rejects
    ws:// URLs; only the scheme has to be translated.
    """
    parts = urlsplit(ws_url)
    scheme = {"ws": "http", "wss": "https"}.get(parts.scheme)
    if scheme is None:
        raise ValueError(f"expected a ws:// or wss:// URL, got {ws_url!r}")
    if parts.path in ("", "/"):
        raise ValueError(f"expected a room in {ws_url!r}")
    return urlunsplit((scheme, parts.netloc, parts.path, "", "")), parts.path


async def join(url: str, gain: float | None) -> None:
    upgraded, path = http_url(url)
    doc = Doc()
    async with aconnect_ws(upgraded) as websocket:
        provider = Provider(doc, HttpxWebsocket(websocket, path))
        async with provider:
            await asyncio.wait_for(provider.synced.wait(), SYNC_TIMEOUT)
            model = doc.get(ROOT_MAP, type=Map)
            print(json.dumps(dict(model), default=str), flush=True)
            if gain is not None:
                model["gain"] = gain
        # Leaving the provider's context is the wait for the write to go out:
        # on a normal exit pycrdt finishes sending queued updates before it stops.
        if gain is not None:
            print(f"SET gain={gain}", flush=True)


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3):
        print("usage: agent.py ws://host:port/room [gain]", file=sys.stderr)
        return 2
    gain = float(argv[2]) if len(argv) == 3 else None
    try:
        asyncio.run(join(argv[1], gain))
    except TimeoutError:
        print(f"no sync from the room within {SYNC_TIMEOUT:g} s", file=sys.stderr)
        return 1
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
