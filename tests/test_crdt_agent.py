"""The Python agent is a peer: it syncs a hub's map, writes, and the hub sees it.

The C++ fixture runs a hub holding `gain` and reports the value it receives, so
this test covers both directions of one real WebSocket session.
"""

import json
import os
import subprocess
import sys

FIXTURE = os.environ["PRISM_CRDT_HUB_FIXTURE"]
AGENT = os.environ["PRISM_CRDT_AGENT"]


def start_fixture() -> subprocess.Popen[str]:
    return subprocess.Popen(
        [FIXTURE], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )


def read_port(fixture: subprocess.Popen[str]) -> int:
    line = fixture.stdout.readline()
    assert line.startswith("PORT "), (
        f"the fixture printed {line!r} (exit {fixture.poll()}) instead of its port"
    )
    return int(line.split()[1])


def run_agent(url: str, gain: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, AGENT, url, gain], capture_output=True, text=True, timeout=30
    )


def test_agent_sets_gain():
    fixture = start_fixture()
    try:
        url = f"ws://127.0.0.1:{read_port(fixture)}/room"
        agent = run_agent(url, "0.75")
        assert agent.returncode == 0, agent.stderr
        assert "SET gain=0.75" in agent.stdout
        # Binding never writes, so the hub's map starts empty: only its shape is known.
        assert isinstance(json.loads(agent.stdout.splitlines()[0]), dict)
        assert fixture.wait(timeout=30) == 0
        assert "GAIN 0.75" in fixture.stdout.read()
    finally:
        if fixture.poll() is None:
            fixture.kill()
            fixture.wait()
