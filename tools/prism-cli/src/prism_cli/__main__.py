"""`prism`: the command line of the PRISM project tools."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Sequence

from prism_cli import __version__, fault


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="prism", description="Start and maintain a PRISM project."
    )
    parser.add_argument("--version", action="version", version=f"prism {__version__}")

    commands = parser.add_subparsers(dest="command", metavar="COMMAND")

    fault = commands.add_parser(
        "fault", help="name the functions in a board's crash report"
    )
    fault.add_argument("elf", help="the firmware ELF the report came from")
    fault.add_argument(
        "report", nargs="?", help="the report file; standard input when omitted"
    )
    fault.set_defaults(handler=_fault)

    return parser


def _fault(args: argparse.Namespace) -> int:
    try:
        report = _read_report(args.report)
        frames = fault.symbolize(Path(args.elf), fault.addresses(report))
    except (fault.FaultError, OSError) as error:
        print(f"prism fault: {error}", file=sys.stderr)
        return 2

    if not frames:
        print(
            "prism fault: no pc, lr, MEPC, RA or Backtrace address in the report",
            file=sys.stderr,
        )
        return 2

    for frame in frames:
        print(f"0x{frame.address:08x}  {frame.function}  {frame.location}")
    return 0


def _read_report(path: str | None) -> str:
    if path is None:
        return sys.stdin.read()
    return Path(path).read_text(errors="replace")


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if getattr(args, "handler", None) is None:
        return 0
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
