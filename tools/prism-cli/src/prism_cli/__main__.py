"""`prism`: the command line of the PRISM project tools."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Sequence

from prism_cli import __version__, fault, size
from prism_cli import doctor as doctor_module
from prism_cli import project as project_module


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

    size = commands.add_parser("size", help="size per origin, from a linker map")
    size.add_argument("map", help="the GNU ld map file")
    size.add_argument("--top", type=int, default=10, help="how many sections to list")
    size.set_defaults(handler=_size)

    doctor = commands.add_parser("doctor", help="check the tools a project needs")
    doctor.add_argument(
        "--target",
        choices=doctor_module.TARGETS,
        default="desktop",
        help="the target whose toolchain to check",
    )
    doctor.set_defaults(handler=_doctor)

    new = commands.add_parser("new", help="start a project from PRISM's template")
    new.add_argument("name", help="the directory to create")
    new.add_argument(
        "--target",
        action="append",
        choices=project_module.TARGETS,
        help="a target to generate; may be repeated (default: desktop)",
    )
    new.add_argument(
        "--from",
        dest="source",
        default=project_module.DEFAULT_SOURCE,
        help="the template to copy",
    )
    new.add_argument(
        "--defaults", action="store_true", help="answer every question with its default"
    )
    new.set_defaults(handler=_new)

    update = commands.add_parser(
        "update", help="pull template changes into the current project"
    )
    update.set_defaults(handler=_update)

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


def _size(args: argparse.Namespace) -> int:
    try:
        sections = size.parse_map(Path(args.map).read_text(errors="replace"))
    except (size.SizeError, OSError) as error:
        print(f"prism size: {error}", file=sys.stderr)
        return 2

    print(size.report(sections, args.top))
    return 0


def _doctor(args: argparse.Namespace) -> int:
    findings = doctor_module.check(args.target)
    for finding in findings:
        print(f"{'ok  ' if finding.ok else 'FAIL'} {finding.tool}: {finding.detail}")
        if not finding.ok:
            print(f"     {finding.fix}")
    return 0 if all(finding.ok for finding in findings) else 1


def _new(args: argparse.Namespace) -> int:
    dest = Path(args.name)
    try:
        project_module.new(
            dest,
            args.target or ["desktop"],
            {"project_name": dest.name},
            src=args.source,
            defaults=args.defaults,
        )
    except (project_module.ProjectError, OSError) as error:
        print(f"prism new: {error}", file=sys.stderr)
        return 2

    print(f"created {dest}")
    return 0


def _update(args: argparse.Namespace) -> int:
    try:
        project_module.update(Path("."))
    except (project_module.ProjectError, OSError) as error:
        print(f"prism update: {error}", file=sys.stderr)
        return 2

    print("updated")
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
