from __future__ import annotations

from types import SimpleNamespace

import pytest

from prism_cli import doctor
from prism_cli.__main__ import main
from prism_cli.doctor import Finding

TOOLS = {
    "g++": "/usr/bin/g++",
    "meson": "/home/x/.local/bin/meson",
    "ninja": "/usr/bin/ninja",
    "copier": "/home/x/.local/bin/copier",
}


def which_all(name: str) -> str | None:
    return TOOLS.get(name)


def run_ok(cmd: list[str], **kwargs: object) -> SimpleNamespace:
    if "-freflection" in cmd:
        return SimpleNamespace(returncode=0, stdout="", stderr="")
    if cmd[0].endswith("g++"):
        return SimpleNamespace(
            returncode=0,
            stdout="g++ (GCC) 16.1.0\nCopyright (C) 2026 Free Software Foundation, Inc.\n",
            stderr="",
        )
    if cmd[0].endswith("meson"):
        return SimpleNamespace(returncode=0, stdout="1.11.2\n", stderr="")
    return SimpleNamespace(returncode=0, stdout="", stderr="")


def test_all_present_is_ok() -> None:
    findings = doctor.check(run=run_ok, which=which_all)

    assert findings
    assert [finding for finding in findings if not finding.ok] == []
    assert {"g++", "meson", "ninja", "copier"} <= {finding.tool for finding in findings}


def test_old_gcc() -> None:
    def run(cmd: list[str], **kwargs: object) -> SimpleNamespace:
        if cmd[0].endswith("g++") and "--version" in cmd:
            return SimpleNamespace(returncode=0, stdout="g++ (GCC) 14.2.0\n", stderr="")
        return SimpleNamespace(returncode=0, stdout="", stderr="")

    findings = doctor.check(run=run, which=which_all)
    compiler = next(finding for finding in findings if finding.tool == "g++")

    assert not compiler.ok
    assert "14" in compiler.detail
    assert "GCC 16" in compiler.fix
    assert "reflection: manual" in compiler.fix


def test_missing_meson() -> None:
    def which(name: str) -> str | None:
        return None if name == "meson" else TOOLS.get(name)

    findings = doctor.check(run=run_ok, which=which)
    meson = next(finding for finding in findings if finding.tool == "meson")

    assert not meson.ok
    assert "not found" in meson.detail
    assert "uv tool install meson" in meson.fix


def test_tool_that_crashes() -> None:
    def run(cmd: list[str], **kwargs: object) -> SimpleNamespace:
        raise FileNotFoundError(2, "No such file or directory", cmd[0])

    findings = doctor.check(run=run, which=which_all)

    assert findings
    assert all(isinstance(finding, Finding) for finding in findings)
    assert any(not finding.ok for finding in findings)
    assert not next(finding for finding in findings if finding.tool == "g++").ok


def test_target_adds_its_toolchain() -> None:
    desktop = {finding.tool for finding in doctor.check(run=run_ok, which=which_all)}
    stm32 = doctor.check(target="stm32", run=run_ok, which=which_all)
    board = next(finding for finding in stm32 if finding.tool == "arm-none-eabi-gcc")

    assert "arm-none-eabi-gcc" not in desktop
    assert not board.ok
    assert "arm-none-eabi-gcc" in board.fix


def test_exit_code(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.setattr(
        doctor, "check", lambda target="desktop": [Finding("g++", True, "16.1.0", "")]
    )
    assert main(["doctor"]) == 0

    missing = Finding("meson", False, "not found", "uv tool install meson")
    monkeypatch.setattr(doctor, "check", lambda target="desktop": [missing])
    assert main(["doctor"]) == 1

    printed = capsys.readouterr()
    assert "meson" in printed.out + printed.err
    assert "uv tool install meson" in printed.out + printed.err
