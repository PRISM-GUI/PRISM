"""Flash and RAM attributed per origin, read from a GNU ld map file."""

from __future__ import annotations

import re
from dataclasses import dataclass

HEADER = "Linker script and memory map"

_NAMED_LINE = re.compile(r"^\s+(\S+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(\S.*)$")
_SECTION_NAME = re.compile(r"^\s+(\.[^\s]+)\s*$")
_SIZE_AND_ORIGIN = re.compile(r"^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(\S.*)$")
_OUTPUT_SECTION = re.compile(
    r"^(?P<name>\.\S+)\s+(?P<address>0x[0-9a-fA-F]+)\s+(?P<size>0x[0-9a-fA-F]+)"
    r"(?P<rest>.*)$"
)
_LOAD_ADDRESS = re.compile(r"load address (0x[0-9a-fA-F]+)")
_MEMORY_ROW = re.compile(r"^(\S+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s*(\S*)\s*$")

_FLATTENED_OBJECT = re.compile(r"/libprism[^/]*\.(?:so|a)\.p/([a-z0-9]+)_[^/]*$")
_DIRECTORY_OBJECT = re.compile(r"/src/([a-z0-9_]+)/")
_USER_OBJECT = re.compile(r"(?:^|/)src/[^/]+\.(?:cpp|cc|c|S)\.o$")
_ELF_BUILD_OBJECT = re.compile(r"\.elf\.p/")
_PRISM_ARCHIVE = re.compile(r"libprism[^/]*\.a\(")
_NEWLIB = re.compile(r"libc\.a|libm\.a|libnosys|newlib")
_HAL = re.compile(r"stm32-hal|/hal/|_hal_")
# simplify: a map does not print a section's type, so NOBITS sections are recognised by name.
# A custom NOLOAD section with a load address would be counted as flash; read the ELF to fix that.
_NOBITS = re.compile(r"\.(?:t|s)?bss\b|noinit|heap|stack")


class SizeError(Exception):
    """A map file the tool cannot read."""


@dataclass(frozen=True)
class Section:
    name: str
    size: int
    origin: str
    flash: bool = True


@dataclass(frozen=True)
class Region:
    origin: int
    length: int
    writable: bool

    def holds(self, address: int) -> bool:
        return self.origin <= address < self.origin + self.length


@dataclass(frozen=True)
class _Placed:
    name: str
    address: int
    load_address: int | None


def parse_map(text: str) -> list[Section]:
    lines = text.splitlines()
    start = _map_start(lines)
    regions = _regions(lines[:start])
    sections: list[Section] = []
    placed = _Placed("", 0, None)
    pending = ""
    for line in lines[start:]:
        output = _OUTPUT_SECTION.match(line)
        if output:
            placed = _placed(output)
            pending = ""
            continue
        named = _NAMED_LINE.match(line)
        if named:
            sections.append(
                _section(
                    named.group(1), named.group(3), named.group(4), placed, regions
                )
            )
            pending = ""
            continue
        sized = _SIZE_AND_ORIGIN.match(line)
        if sized:
            sections.append(
                _section(pending, sized.group(2), sized.group(3), placed, regions)
            )
            pending = ""
            continue
        name = _SECTION_NAME.match(line)
        pending = name.group(1) if name else pending
    return [section for section in sections if section.size > 0]


def by_family(sections: list[Section]) -> dict[str, int]:
    families: dict[str, int] = {}
    for section in _flash(sections):
        name = family(section.origin)
        families[name] = families.get(name, 0) + section.size
    return families


def family(origin: str) -> str:
    if "cmsis-dsp" in origin:
        return "cmsis-dsp"
    if "libstdc++" in origin:
        return "libstdc++"
    if "libgcc" in origin:
        return "libgcc"
    if _NEWLIB.search(origin):
        return "newlib"
    if "fonts" in origin:
        return "fonts"
    if "freertos" in origin:
        return "freertos"
    if _HAL.search(origin):
        return "hal"
    module = prism_module(origin)
    if module:
        return f"prism:{module}"
    if _PRISM_ARCHIVE.search(origin):
        return "prism"
    if _USER_OBJECT.search(origin) or _ELF_BUILD_OBJECT.search(origin):
        return "app"
    return "other"


def prism_module(origin: str) -> str:
    flattened = _FLATTENED_OBJECT.search(origin)
    if flattened:
        return flattened.group(1)
    directory = _DIRECTORY_OBJECT.search(origin)
    return directory.group(1) if directory else ""


def report(sections: list[Section], top: int = 10) -> str:
    families = by_family(sections)
    total = sum(families.values())
    lines = [f"flash {total} bytes"]
    for name, size in sorted(families.items(), key=lambda item: (-item[1], item[0])):
        lines.append(f"{size:9d} {_percent(size, total):5.1f}%  {name}")
    lines.append(f"ram {_ram_total(sections)} bytes")
    lines.append(f"-- top {top}")
    for section in sorted(_flash(sections), key=lambda item: -item.size)[:top]:
        lines.append(f"{section.size:9d}  {section.name}  {section.origin}")
    return "\n".join(lines)


def _flash(sections: list[Section]) -> list[Section]:
    return [section for section in sections if section.flash]


def _ram_total(sections: list[Section]) -> int:
    return sum(section.size for section in sections if not section.flash)


def _map_start(lines: list[str]) -> int:
    starts = [index for index, line in enumerate(lines) if line.startswith(HEADER)]
    if not starts:
        raise SizeError('not a linker map: no "Linker script and memory map" line')
    return starts[-1] + 1


def _regions(lines: list[str]) -> list[Region]:
    table = [line for line in _memory_table(lines) if not line.strip().startswith("*")]
    return [region for region in map(_region, table) if region is not None]


def _memory_table(lines: list[str]) -> list[str]:
    starts = [
        index
        for index, line in enumerate(lines)
        if line.startswith("Memory Configuration")
    ]
    if not starts:
        return []
    rows: list[str] = []
    for line in lines[starts[-1] + 1 :]:
        if line.startswith(HEADER):
            break
        if _MEMORY_ROW.match(line):
            rows.append(line)
    return rows


def _region(line: str) -> Region | None:
    row = _MEMORY_ROW.match(line)
    if row is None:
        return None
    return Region(
        origin=int(row.group(2), 16),
        length=int(row.group(3), 16),
        writable="w" in row.group(4),
    )


def _placed(output: re.Match[str]) -> _Placed:
    load = _LOAD_ADDRESS.search(output.group("rest"))
    return _Placed(
        name=output.group("name"),
        address=int(output.group("address"), 16),
        load_address=int(load.group(1), 16) if load else None,
    )


def _section(
    name: str, size: str, origin: str, placed: _Placed, regions: list[Region]
) -> Section:
    return Section(name, int(size, 16), origin.strip(), _in_flash(placed, regions))


def _in_flash(placed: _Placed, regions: list[Region]) -> bool:
    if not _writable(placed.address, regions):
        return True
    return (
        placed.load_address is not None
        and not _NOBITS.search(placed.name)
        and not _writable(placed.load_address, regions)
    )


def _writable(address: int, regions: list[Region]) -> bool:
    return any(region.holds(address) and region.writable for region in regions)


def _percent(size: int, total: int) -> float:
    return 100.0 * size / total if total else 0.0
