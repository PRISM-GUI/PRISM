from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import pytest

from prism_cli import fault
from prism_cli.__main__ import main
from prism_cli.fault import Frame

DATA = Path(__file__).parent / "data"


def read_fixture(name: str) -> str:
    return (DATA / name).read_text()


def minimal_elf(path: Path, machine: int) -> Path:
    header = bytearray(52)
    header[0:4] = b"\x7fELF"
    header[4] = 1  # ELFCLASS32
    header[5] = 1  # little endian
    header[6] = 1  # EV_CURRENT
    header[16:18] = (2).to_bytes(2, "little")  # ET_EXEC
    header[18:20] = machine.to_bytes(2, "little")
    header[20:24] = (1).to_bytes(4, "little")
    path.write_bytes(bytes(header))
    return path


def test_f7_report_addresses() -> None:
    assert fault.addresses(read_fixture("f746_fault.txt")) == [0x0800ABCD, 0x08001234]


def test_esp32_panic_addresses() -> None:
    assert fault.addresses(read_fixture("esp32_panic.txt")) == [
        0x4201A2B4,
        0x4200C7E8,
        0x4201A2B1,
        0x4200C7E5,
        0x4205F123,
        0x4037D1A9,
    ]


def test_noise_is_ignored() -> None:
    report = read_fixture("f746_fault.txt")
    colour = "\x1b[2J\x1b[1;33m"
    reset = "\x1b[0m"
    truncated = "\r\npc 0x0800ab\r\nlr 0x\r\n"
    pasted = colour + report.replace("\n", "\r\n") + reset

    assert fault.addresses(pasted + truncated + pasted) == [0x0800ABCD, 0x08001234]


def test_symbolize_uses_injected_runner(tmp_path: Path) -> None:
    elf = minimal_elf(tmp_path / "app.elf", fault.ELF_MACHINE_ARM)
    calls: list[list[str]] = []

    def run(cmd: list[str], **kwargs: object) -> SimpleNamespace:
        calls.append(cmd)
        stdout = (
            "prism::ui::Level::record(DrawList&) const\n"
            "/home/x/src/ui/level.hpp:42\n"
            "prism::app::draw_frame\n"
            "/home/x/src/app/frame.cpp:107\n"
        )
        return SimpleNamespace(returncode=0, stdout=stdout, stderr="")

    frames = fault.symbolize(elf, [0x0800ABCC, 0x08001234], run=run)

    assert frames == [
        Frame(
            0x0800ABCC,
            "prism::ui::Level::record(DrawList&) const",
            "/home/x/src/ui/level.hpp:42",
        ),
        Frame(0x08001234, "prism::app::draw_frame", "/home/x/src/app/frame.cpp:107"),
    ]
    assert calls[0][0].endswith("arm-none-eabi-addr2line")
    assert "0x0800abcc" in calls[0]


@pytest.mark.parametrize(
    ("machine", "tool"),
    [
        (fault.ELF_MACHINE_ARM, "arm-none-eabi-addr2line"),
        (fault.ELF_MACHINE_RISCV, "riscv32-esp-elf-addr2line"),
    ],
)
def test_addr2line_for_machine(tmp_path: Path, machine: int, tool: str) -> None:
    elf = minimal_elf(tmp_path / f"elf{machine}", machine)
    assert fault.addr2line_for(elf) == tool


def test_cli_version(capsys: pytest.CaptureFixture[str]) -> None:
    with pytest.raises(SystemExit) as exit_info:
        main(["--version"])

    assert exit_info.value.code == 0
    assert "prism" in capsys.readouterr().out.lower()
