# Starting and maintaining a PRISM project: a Copier template and a `prism` tool

Date: 2026-11-24. Status: draft, for approval.

## Why

A newcomer cannot start a PRISM project today without reading the manual closely. The
newcomer audit of 2026-11-24 ranked it first: nothing showed how to build your own program, and
`install.md` mentioned a `prism.wrap` that did not exist. The docs now show the recipe by hand.

Two more costs showed up this week:

1. **Boards are copied, not started.** prism-mp3-player was made by copying prism-esp32's
   layout. Every board lesson (single-precision toolchain on the F7, SDRAM before constructors,
   the size options) lives in a repository a new project must copy again.
2. **Debugging a board is manual.** A crash report is a list of hex addresses. On the F746 it
   took several rounds of `addr2line` by hand to name a function. The flash-size report scripts
   live in prism-stm32 only.

## Goals

- `prism new myapp` gives a project that builds and runs, with a test and CI, in one command.
- The project can pull later template improvements (`prism update`), so this week's board
  lessons reach projects made last week.
- One `prism` command for the helpers a user needs: check the toolchain, report flash size,
  decode a board's crash report.
- The template cannot rot: PRISM's CI generates a project from it and builds it.

## Non-goals

- A package manager, or installing PRISM system-wide. PRISM stays a Meson subproject.
- Cookiecutter support. One template format, Copier (it updates projects; Cookiecutter cannot).
- IDE project files.
- Board bring-up for boards PRISM does not support yet.

## Approaches considered

1. **A Copier template, and a small `prism` CLI around it (chosen).** Copier asks the questions,
   renders the files, and `copier update` merges template changes into an existing project.
   The CLI adds defaults and the helpers.
2. **A hand-written generator in the CLI.** Full control, but re-implements what Copier does,
   and an update path would have to be written from scratch.
3. **GitHub template repositories.** No tooling, but one repository per target mix, names
   edited by hand, and no update path.

## Design

### The template (`templates/app/` in the PRISM repository)

`copier.yml` at the PRISM repository root, with `_subdirectory: templates/app`, so the command is:

```sh
copier copy gh:PRISM-GUI/PRISM myapp          # or: prism new myapp
```

Inside PRISM rather than a separate repository: PRISM's CI renders it against the same commit,
and a change to PRISM that breaks a fresh project fails PRISM's own CI. Copier records the
template commit in `.copier-answers.yml`, so `copier update` works without release tags.

Questions (each with a default, so `--defaults` works):

| Question | Values | Default |
|---|---|---|
| `project_name` | free text | (asked) |
| `project_slug` | identifier | derived from the name |
| `description` | free text | "A PRISM application" |
| `author` | free text | git's `user.name` |
| `targets` | multiple: `desktop`, `esp32_korvo1` | `desktop` |
| `reflection` | `auto` (GCC 16+), `manual` (a hand-written `view()`) | `auto` |
| `license` | `MIT`, `none` | `MIT` |
| `github_ci` | yes / no | yes |

Generated for `desktop`:

```text
myapp/
  meson.build              # project(), subproject('prism') with examples/tests/benchmarks off
  subprojects/prism.wrap   # wrap-git, PRISM main (pinned to the template's commit at render time)
  src/main.cpp             # a model struct, app::run
  src/model.hpp            # the struct: a few Fields, a derived value, a button
  tests/test_model.cpp     # doctest: the model's logic without a window
  .github/workflows/ci.yml # GCC 16, meson setup/test (if github_ci)
  README.md, LICENSE, .gitignore, .copier-answers.yml
```

With `reflection: manual` the model gets a `view()` and builds on compilers without reflection.

Generated for `esp32_korvo1`, in `boards/korvo1/`, the shape prism-mp3-player uses:
`prism-esp32` as a git submodule, an ESP-IDF `main/` component that runs the same model with
`frame_app`, the board's `sdkconfig.defaults`, and `scripts/build.sh` / `scripts/flash.sh`. The
model sources are shared with the desktop target, so one model runs on both.

STM32 targets are not offered in v1: prism-stm32 is a firmware repository, not a library a
project can depend on. → skipped: STM32 targets, add when prism-stm32 exposes its boards as a
Meson subproject (a board library plus its cross files).

### The `prism` tool (`tools/prism-cli/` in the PRISM repository)

A Python package, `prism-cli`, installing a `prism` command (`uv tool install prism-cli`, or from
a checkout). The Python SDK's module is also called `prism`; the tool's module is `prism_cli`,
so the two never clash. Dependencies: Copier, and the standard library.

| Command | Does |
|---|---|
| `prism new NAME [--target ...]` | `copier copy` with PRISM's template; asks only what it cannot default |
| `prism update` | `copier update` in the current project |
| `prism doctor` | checks: GCC version and `-freflection`, Meson >= 1.5, Ninja, Python, Copier; with a board target, ESP-IDF or the Arm toolchain. Prints what is missing and how to get it |
| `prism size ELF_OR_MAP` | flash and RAM per origin (PRISM module, libstdc++, newlib, fonts, ...), the biggest symbols: prism-stm32's `size-map.py`, moved here and generalised |
| `prism fault ELF [REPORT]` | reads a crash report (the F7 boot log's `pc lr ...` lines, an ESP-IDF backtrace), runs `addr2line` on every address, prints function and line |

`prism fault` reads the report from a file or standard input, so it works on a pasted serial
log.

### Tests

- The CLI: pytest, with the commands' pure parts tested directly (report parsing, size
  attribution from a sample map file, version checks with faked tool output).
- The template: a PRISM CI job renders it with `--defaults` and builds and tests the desktop
  target against the PRISM commit under test. The `manual` reflection variant is rendered and
  built too, by the job that has no reflection (clang/libc++).
- The ESP32 target: rendered and built in prism-esp32's firmware CI image.

### Docs

`doc/start/install.md`'s "your own project" section leads with `prism new`, keeping the
by-hand recipe below it. A new page, `doc/start/tooling.md`, documents the five commands.
prism-stm32's README points at `prism size` and `prism fault`.

## Left to the builder

- The exact model in the template (keep it small: three fields, one derived value, one button).
- Wording of the questions and of `doctor`'s advice.
- How `prism size` groups origins beyond the ones listed.
