from __future__ import annotations

from pathlib import Path

import pytest

from prism_cli import size
from prism_cli.__main__ import main
from prism_cli.size import Section

DATA = Path(__file__).parent / "data"

PRISM_BUILD = "/home/x/app/build/subprojects/prism/src"
USER_BUILD = "/home/x/app/build"


def read_fixture(name: str) -> str:
    return (DATA / name).read_text()


@pytest.fixture()
def sections() -> list[Section]:
    return size.parse_map(read_fixture("small.map"))


def test_parse_input_sections() -> None:
    expected = [
        Section(
            ".text.prism_level", 0x140, f"{PRISM_BUILD}/libprism.so.p/ui_level.cpp.o"
        ),
        Section(
            ".text.prism_widget_tree",
            0x300,
            f"{PRISM_BUILD}/libprism.so.p/app_widget_tree.cpp.o",
        ),
        Section(".text.prism_field", 0x100, f"{PRISM_BUILD}/core/field.cpp.o"),
        Section(".text.level_now", 0x40, f"{USER_BUILD}/src/main.cpp.o"),
        Section(
            ".text.std_string",
            0x400,
            "/usr/local/arm-none-eabi/lib/libstdc++.a(string.o)",
        ),
        Section(
            ".text.gcc_bpabi",
            0x80,
            "/usr/lib/gcc/arm-none-eabi/13.2.1/libgcc.a(bpabi.o)",
        ),
        Section(
            ".text.newlib_memcpy",
            0x200,
            "/usr/local/arm-none-eabi/lib/libc.a(lib_a-memcpy.o)",
        ),
        Section(
            ".text.inter_regular", 0x600, f"{USER_BUILD}/fonts/inter_regular.cpp.o"
        ),
        Section(
            ".text.freertos_tasks",
            0x180,
            f"{USER_BUILD}/subprojects/freertos-kernel/tasks.cpp.o",
        ),
        Section(
            ".text.hal_gpio", 0x90, f"{USER_BUILD}/components/stm32-hal/hal_gpio.cpp.o"
        ),
        Section(
            ".text.cmsis_fft", 0x120, f"{USER_BUILD}/components/cmsis-dsp/arm_fft.cpp.o"
        ),
        Section(".text.third_party", 0x20, f"{USER_BUILD}/third_party/foo/bar.cpp.o"),
    ]

    assert size.parse_map(read_fixture("small.map")) == expected


def test_families(sections: list[Section]) -> None:
    assert size.by_family(sections) == {
        "prism:ui": 0x140,
        "prism:app": 0x300,
        "prism:core": 0x100,
        "app": 0x40,
        "libstdc++": 0x400,
        "libgcc": 0x80,
        "newlib": 0x200,
        "fonts": 0x600,
        "freertos": 0x180,
        "hal": 0x90,
        "cmsis-dsp": 0x120,
        "other": 0x20,
    }


def test_report_lists_top_symbols(sections: list[Section]) -> None:
    lines = size.report(sections, top=3).splitlines()
    start = next(index for index, line in enumerate(lines) if line.startswith("-- top"))
    top_lines = lines[start + 1 :]

    assert any("prism:ui" in line for line in lines[:start])
    assert len(top_lines) == 3
    assert ".text.inter_regular" in top_lines[0]
    assert ".text.std_string" in top_lines[1]
    assert ".text.prism_widget_tree" in top_lines[2]


def test_not_a_map_file(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    with pytest.raises(size.SizeError) as error:
        size.parse_map("hello\nworld\n")
    assert "\n" not in str(error.value).strip()

    bad = tmp_path / "bad.map"
    bad.write_text("hello\nworld\n")

    assert main(["size", str(bad)]) == 2
    assert "prism size:" in capsys.readouterr().err


def real_sections() -> list[Section]:
    return size.parse_map(read_fixture("f746_widgets.map"))


def test_real_map_flash_excludes_ram() -> None:
    sections = real_sections()

    in_ram = {section.name for section in sections if not section.flash}

    assert in_ram == {".bss.prism_registry", ".bss.freertos_ready", ".sdram_heap"}
    assert next(
        section for section in sections if section.name == ".data.prism_theme"
    ).flash

    text = size.report(sections, top=1)
    assert "flash 3584 bytes" in text
    assert "ram 1050880 bytes" in text


def test_real_map_families() -> None:
    assert size.by_family(real_sections()) == {
        "prism": 0x480,
        "prism:ui": 0x300,
        "app": 0x280,
        "libstdc++": 0x300,
        "freertos": 0x100,
    }
