"""Reading a board's crash report and naming the functions in it."""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable

ELF_MACHINE_ARM = 40
ELF_MACHINE_RISCV = 243

_ADDR2LINE = {
    ELF_MACHINE_ARM: "arm-none-eabi-addr2line",
    ELF_MACHINE_RISCV: "riscv32-esp-elf-addr2line",
}

_ANSI = re.compile(r"\x1b\[[0-9;]*[ -/]*[@-~]")
_REGISTER_ADDRESS = re.compile(r"\b(?:pc|lr|MEPC|RA)\b\s*:?\s*(0x[0-9a-fA-F]{8})\b")
_BACKTRACE = re.compile(r"Backtrace:\s*(.*)")
_BACKTRACE_ADDRESS = re.compile(r"(0x[0-9a-fA-F]{8}):0x[0-9a-fA-F]+")


class FaultError(Exception):
    """A report or an ELF file the tool cannot read."""


@dataclass(frozen=True)
class Frame:
    address: int
    function: str
    location: str


def addresses(report: str) -> list[int]:
    found: dict[int, None] = {}
    for line in _ANSI.sub("", report).splitlines():
        for token in _addresses_of_line(line):
            found.setdefault(int(token, 16), None)
    return list(found)


def symbolize(
    elf: Path,
    addrs: list[int],
    run: Callable[..., Any] = subprocess.run,
) -> list[Frame]:
    if not addrs:
        return []
    tool = addr2line_for(elf)
    command = [
        tool,
        "-f",
        "-C",
        "-e",
        str(elf),
        *(f"0x{address:08x}" for address in addrs),
    ]
    result = run(command, capture_output=True, text=True)
    if result.returncode != 0:
        reason = _first_line(result.stderr) or f"{tool} exited {result.returncode}"
        raise FaultError(reason)
    return _frames(addrs, result.stdout.splitlines())


def addr2line_for(elf: Path) -> str:
    machine = _elf_machine(elf)
    if machine not in _ADDR2LINE:
        raise FaultError(f"{elf}: no addr2line for ELF machine {machine}")
    return _ADDR2LINE[machine]


def _addresses_of_line(line: str) -> Iterable[str]:
    backtrace = _BACKTRACE.search(line)
    if backtrace:
        return [
            match.group(1) for match in _BACKTRACE_ADDRESS.finditer(backtrace.group(1))
        ]
    return [match.group(1) for match in _REGISTER_ADDRESS.finditer(line)]


def _frames(addrs: list[int], lines: list[str]) -> list[Frame]:
    expected = 2 * len(addrs)
    if len(lines) != expected:
        raise FaultError(
            f"addr2line printed {len(lines)} lines for {len(addrs)} addresses"
        )
    return [
        Frame(address, lines[2 * index].strip(), lines[2 * index + 1].strip())
        for index, address in enumerate(addrs)
    ]


def _elf_machine(elf: Path) -> int:
    try:
        with Path(elf).open("rb") as stream:
            header = stream.read(20)
    except OSError as error:
        raise FaultError(f"cannot read {elf}: {error.strerror}") from None
    if len(header) < 20 or header[:4] != b"\x7fELF":
        raise FaultError(f"{elf} is not an ELF file")
    order = "little" if header[5] == 1 else "big"
    return int.from_bytes(header[18:20], order)


def _first_line(text: str) -> str:
    return text.strip().splitlines()[0] if text.strip() else ""
