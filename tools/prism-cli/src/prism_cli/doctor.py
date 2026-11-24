"""Checking that a machine has what a PRISM project needs."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from typing import Any, Callable

TARGETS = ("desktop", "esp32_korvo1", "stm32")

GCC_MINIMUM = 16
MESON_MINIMUM = (1, 5)
PYTHON_MINIMUM = (3, 11)

_VERSION = re.compile(r"(\d+)\.(\d+)")

_GCC_COMPILER = "install GCC 16 or newer"
_ARM_TOOLCHAIN = "install the Arm toolchain (arm-none-eabi-gcc)"


@dataclass(frozen=True)
class Finding:
    tool: str
    ok: bool
    detail: str
    fix: str


def check(
    target: str = "desktop",
    run: Callable[..., Any] = subprocess.run,
    which: Callable[[str], str | None] = shutil.which,
) -> list[Finding]:
    if target not in TARGETS:
        raise ValueError(f"unknown target: {target}")
    return [
        _compiler(run, which),
        _versioned("meson", MESON_MINIMUM, run, which, "uv tool install meson"),
        _installed("ninja", which, "uv tool install ninja"),
        _installed("copier", which, "uv tool install copier"),
        _python(),
        *_target_tools(target, which),
    ]


def _compiler(run: Callable[..., Any], which: Callable[[str], str | None]) -> Finding:
    path = which("g++")
    if path is None:
        return Finding("g++", False, "not found", _GCC_COMPILER)

    version = _tool_version(path, run)
    if version is None:
        return Finding("g++", False, "could not read its version", _GCC_COMPILER)
    if version[0] < GCC_MINIMUM:
        return Finding(
            "g++", False, f"GCC {_text(version)} is too old", _reflection_fix()
        )
    return _reflection(path, version, run)


def _reflection(
    path: str, version: tuple[int, int], run: Callable[..., Any]
) -> Finding:
    result = _run(
        run,
        [path, "-std=c++26", "-freflection", "-fsyntax-only", "-x", "c++", "-"],
        input="int main(){}\n",
    )
    if result is None:
        return Finding(
            "g++", False, f"GCC {_text(version)} could not run", _GCC_COMPILER
        )
    if result.returncode != 0:
        return Finding(
            "g++",
            False,
            f"GCC {_text(version)} rejects -freflection",
            _reflection_fix(),
        )
    return Finding("g++", True, f"GCC {_text(version)}, -freflection accepted", "")


def _versioned(
    tool: str,
    minimum: tuple[int, int],
    run: Callable[..., Any],
    which: Callable[[str], str | None],
    fix: str,
) -> Finding:
    path = which(tool)
    if path is None:
        return Finding(tool, False, "not found", fix)

    version = _tool_version(path, run)
    if version is None:
        return Finding(tool, False, "could not read its version", fix)
    if version < minimum:
        return Finding(tool, False, f"version {_text(version)} is too old", fix)
    return Finding(tool, True, f"version {_text(version)}", "")


def _installed(tool: str, which: Callable[[str], str | None], fix: str) -> Finding:
    path = which(tool)
    if path is None:
        return Finding(tool, False, "not found", fix)
    return Finding(tool, True, path, "")


def _python() -> Finding:
    actual = (sys.version_info.major, sys.version_info.minor)
    detail = f"python {_text(actual)}"
    if actual < PYTHON_MINIMUM:
        return Finding("python", False, detail, "install Python 3.11 or newer")
    return Finding("python", True, detail, "")


def _target_tools(target: str, which: Callable[[str], str | None]) -> list[Finding]:
    if target == "esp32_korvo1":
        return [_esp_idf(which)]
    if target == "stm32":
        return [_installed("arm-none-eabi-gcc", which, _ARM_TOOLCHAIN)]
    return []


def _esp_idf(which: Callable[[str], str | None]) -> Finding:
    path = which("idf.py")
    if path is not None:
        return Finding("idf.py", True, path, "")
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        return Finding("esp-idf", True, idf_path, "")
    return Finding(
        "idf.py",
        False,
        "not found and IDF_PATH is unset",
        "install ESP-IDF, then source its export.sh",
    )


def _tool_version(path: str, run: Callable[..., Any]) -> tuple[int, int] | None:
    result = _run(run, [path, "--version"])
    if result is None or result.returncode != 0:
        return None
    match = _VERSION.search(result.stdout)
    return (int(match.group(1)), int(match.group(2))) if match else None


def _run(run: Callable[..., Any], command: list[str], **kwargs: Any) -> Any | None:
    try:
        return run(command, capture_output=True, text=True, **kwargs)
    except OSError:
        return None


def _text(version: tuple[int, ...]) -> str:
    return ".".join(str(part) for part in version)


def _reflection_fix() -> str:
    return f"{_GCC_COMPILER}, or set reflection: manual in the template"
