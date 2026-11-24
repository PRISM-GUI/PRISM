# prism-cli

The `prism` command: start a PRISM project, keep it in step with the template, check the build
tools, read a firmware's linker map, and name the functions in a crash report.

The user-facing page is [`doc/start/tooling.md`](../../doc/start/tooling.md). This file is for
whoever works on the tool.

## Install

There is no PyPI release. Install it from the checkout:

```bash
uv tool install --from . prism-cli
prism --version
```

`uvx --from . prism doctor` runs it once without installing. Python 3.11 or newer.

## The five commands

| Command | Input | Output |
|---|---|---|
| `prism doctor [--target desktop\|stm32\|esp32_korvo1]` | nothing | one line per tool; exit 1 if any is unusable |
| `prism new NAME [--target T ...] [--defaults] [--from SRC]` | nothing | a project directory, committed |
| `prism update` | writes into the current directory | re-applies the template |
| `prism size MAP [--top N]` | a linker map | flash total, per-family lines, RAM total, the N largest sections |
| `prism fault ELF [REPORT]` | an ELF and a report (stdin when omitted) | one address, function and `file:line` per frame |

Every command prints its own errors as `prism <command>: <reason>` on stderr and exits 2. Success
exits 0, except `doctor`, which exits 1 when a finding is not ok.

## How it is built

| Module | What it holds |
|---|---|
| `src/prism_cli/__main__.py` | the argument parser, one handler per command |
| `src/prism_cli/doctor.py` | `check(target, run, which)` returning `Finding(tool, ok, detail, fix)` |
| `src/prism_cli/project.py` | `new()` and `update()`, over Copier, plus the git guards |
| `src/prism_cli/size.py` | `parse_map()`, `family()`, `by_family()`, `report()` |
| `src/prism_cli/fault.py` | `addresses()`, `addr2line_for()`, `symbolize()` |

Subprocesses and Copier are injected (`run=`, `which=`, `copy=`, `update=`), so the tests never
need a compiler, a linker map from a real build, or a network. Fixtures live in `tests/data/`.

The template the `new` and `update` commands use is not in this directory: it is
[`copier.yml`](../../copier.yml) and [`templates/app/`](../../templates/app/) at the repository
root, because Copier reads them from the root of the repository it copies.

## Size families

`size.py` reads the map's `Memory Configuration` table to tell flash from RAM. An input section
counts as flash when its output section sits in a region without `w`, or when its load address does
(the initial values of `.data`). `.bss`, heaps and stacks cost RAM and are reported on their own
line.

`family()` buckets an origin in this order: `cmsis-dsp`, `libstdc++`, `libgcc`, `newlib`, `fonts`,
`freertos`, `hal` (`stm32-hal`, `/hal/`, `_hal_`), `prism:<module>`, `prism` (objects inside
`libprism*.a`, where the module name is not visible), `app` (the firmware's own objects, including
`*.elf.p/`), else `other`.

Objects the patterns miss land in `other`. On the F746 widgets map that is a generated
`embedded_noto_sans_latin.cpp.o` and a few BSP objects; the total still matches the `.bin` within a
kilobyte, so the split is what is off, not the sum.

## Tests

```bash
uv run --extra test pytest -q
```

`pytest -m slow` covers the one end-to-end test: it renders the real template into a temporary git
repository, changes the template, and checks that `update` carries the change.

## Known limits

- `prism fault` needs the toolchain's `addr2line` on `PATH`: `arm-none-eabi-addr2line` for STM32,
  `riscv32-esp-elf-addr2line` for ESP32.
- `prism size` reads the map, not the ELF, so it cannot see what the linker garbage-collected.
- The template has no STM32 target yet; `templates/app/boards/` holds `korvo1/` only.
- Generating from a template checkout that has uncommitted changes records a synthetic `_commit`
  in the answers file (Copier prints `DirtyLocalWarning`). The next `update` then fails with
  `ValueError: Unknown revision`. Commit the template before rendering a project from it.
