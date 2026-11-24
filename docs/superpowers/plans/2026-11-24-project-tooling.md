# Project Tooling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `prism new myapp` gives a building, tested PRISM project (desktop, optionally the ESP32
Korvo board), `prism update` pulls template improvements, and `prism doctor|size|fault` help users.

**Architecture:** A Copier template in PRISM (`copier.yml` at the root, `_subdirectory:
templates/app`), rendered and built by PRISM's CI against the commit under test. A small Python
package, `prism-cli` (module `prism_cli`, command `prism`), wraps Copier and carries the helpers;
each helper is a pure function over text plus a thin runner, so the pure part is tested directly.

**Tech Stack:** Python 3.11+, Copier 9, pytest, argparse (stdlib); Meson >= 1.5, GCC 16, doctest
in the generated project; GitHub Actions.

**Spec:** /home/jeandet/Documents/prog/PRSIM/docs/superpowers/specs/2026-11-24-project-tooling-design.md

## Global Constraints

- One template format: Copier. No Cookiecutter.
- The template lives in PRISM: `copier.yml` at the repository root with `_subdirectory: templates/app`.
- Package name `prism-cli`, Python module `prism_cli`, console command `prism`. Never a module named `prism` (that is the Python SDK).
- Dependencies of `prism-cli`: Copier and the standard library only.
- Template questions and defaults exactly as the spec's table: `project_name` (asked),
  `project_slug` (derived), `description` ("A PRISM application"), `author` (git `user.name`),
  `targets` (multiple of `desktop`, `esp32_korvo1`; default `desktop`), `reflection` (`auto` |
  `manual`; default `auto`), `license` (`MIT` | `none`; default `MIT`), `github_ci` (default yes).
- The generated `meson.build` calls `subproject('prism', default_options: ['examples=disabled', 'tests=disabled', 'benchmarks=disabled'])` and uses `prism_dep`.
- `subprojects/prism.wrap`: `[wrap-git]`, `url = https://github.com/PRISM-GUI/PRISM.git`, `revision` = the template commit at render time (Copier's `_copier_conf.vcs_ref_hash`, falling back to `main`), `depth = 1`.
- STM32 targets are not offered in v1.
- Builds and tests of C++ run on the Coder workspace (`coder-builds` rules: own build dirs, one at a time, foreground). The Python package's own tests are pure Python and may run locally with `uv run`.
- Style: PRISM AGENTS.md; for Python, small functions, type hints, no comment-decorated blocks.

## Review Focus

1. A project name with spaces, capitals or a leading digit ("2D Plotter") must give a valid
   `project_slug` and a Meson project and C++ identifiers that compile. Pinned in Task 4.
2. `prism fault` on a pasted serial log with noise (ANSI colour codes, CR line ends, partial lines,
   repeated reports) must still find every address, once each, in order. Pinned in Task 1.
3. `prism doctor` on a machine missing a tool must say what is missing and how to get it, and
   exit non-zero, never crash with a traceback. Pinned in Task 3.
4. `prism new` into a directory that already exists and is not empty must refuse with a clear
   message instead of overwriting. Pinned in Task 5.
5. `prism size` on a map file from a different toolchain layout (no "Linker script and memory
   map" header, or Windows paths) must fail with a message, not a traceback. Pinned in Task 2.

---

### Task 1: The package and `prism fault`

**Files:**
- Create: `tools/prism-cli/pyproject.toml`, `tools/prism-cli/src/prism_cli/__init__.py`,
  `tools/prism-cli/src/prism_cli/__main__.py` (argparse, subcommands), `tools/prism-cli/src/prism_cli/fault.py`
- Test: `tools/prism-cli/tests/test_fault.py`, fixtures in `tools/prism-cli/tests/data/`
  (`f746_fault.txt`: the F7 boot log fault report from 2026-11-23; `esp32_panic.txt`: an ESP-IDF RISC-V panic with `MEPC`, `RA` and a `Backtrace:` line)

**Interfaces:**
- Produces: `fault.addresses(report: str) -> list[int]`; `fault.symbolize(elf: Path, addrs: list[int], run=subprocess.run) -> list[Frame]` with `Frame(address: int, function: str, location: str)`; `fault.addr2line_for(elf: Path) -> str` (arm-none-eabi-addr2line for an ARM ELF, riscv32-esp-elf-addr2line for RISC-V, from the ELF header's e_machine); CLI `prism fault ELF [REPORT]` (stdin when REPORT is omitted); `prism --version`.

- [ ] **Step 1: Write the failing tests:** `test_f7_report_addresses` (pc and lr of the fixture, in that order); `test_esp32_panic_addresses` (MEPC, RA, then each Backtrace pc, skipping the `:0x` stack-pointer halves); `test_noise_is_ignored` (ANSI codes, `\r\n`, a truncated line, a report pasted twice: each address once, first-seen order); `test_symbolize_uses_injected_runner` (fake `run` returning addr2line `-f -C` output: two Frames with function and file:line); `test_addr2line_for_machine` (minimal ELF headers for e_machine 40 and 243).
- [ ] **Step 2: Run** `cd tools/prism-cli && uv run --extra test pytest tests/test_fault.py -q`. Expected: FAIL (module missing).
- [ ] **Step 3: Implement** the package (pyproject: `[project.scripts] prism = "prism_cli.__main__:main"`, `optional-dependencies.test = ["pytest"]`, dependency `copier>=9`) and `fault.py`. An address is a `0x` + 8 hex token after one of `pc`, `lr`, `MEPC`, `RA`, `Backtrace:`; addresses of 0 or with the low bit-cleared value outside any loaded segment are kept (addr2line prints `??`).
- [ ] **Step 4: Run** the tests. Expected: PASS.
- [ ] **Step 5: Commit** `feat(cli): prism-cli package and prism fault`.

### Task 2: `prism size`

**Files:**
- Create: `tools/prism-cli/src/prism_cli/size.py`
- Test: `tools/prism-cli/tests/test_size.py`, fixture `tests/data/small.map` (a trimmed GNU ld map with input sections from libprism.a, libstdc++.a, libc.a, a fonts object and a board object)

**Interfaces:**
- Consumes: Task 1's argparse entry (`prism size MAP [--top N]`).
- Produces: `size.parse_map(text: str) -> list[Section]` with `Section(name: str, size: int, origin: str)`; `size.by_family(sections) -> dict[str, int]` (families: `prism:<module>` from `src/<module>/`, `libstdc++`, `newlib`, `libgcc`, `fonts`, `freertos`, `hal`, `cmsis-dsp`, `app`, `other`); `size.report(sections, top: int) -> str`.

- [ ] **Step 1: Write the failing tests:** `test_parse_input_sections` (sizes and origins of the fixture's lines, including a section name on its own line with size on the next); `test_families` (the fixture's totals per family, exact numbers); `test_report_lists_top_symbols` (`top=3` gives the 3 largest, largest first); `test_not_a_map_file` (text without the "Linker script and memory map" header raises `SizeError` with a one-line message, and the CLI prints it and exits 2).
- [ ] **Step 2: Run** them. Expected: FAIL.
- [ ] **Step 3: Implement** by porting prism-stm32's `scripts/size-map.py` (same parsing of input-section lines after the "Linker script and memory map" line) into these functions.
- [ ] **Step 4: Run** them. Expected: PASS.
- [ ] **Step 5: Commit** `feat(cli): prism size`.

### Task 3: `prism doctor`

**Files:**
- Create: `tools/prism-cli/src/prism_cli/doctor.py`
- Test: `tools/prism-cli/tests/test_doctor.py`

**Interfaces:**
- Produces: `doctor.check(target: str = "desktop", run=subprocess.run, which=shutil.which) -> list[Finding]` with `Finding(tool: str, ok: bool, detail: str, fix: str)`; CLI `prism doctor [--target desktop|esp32_korvo1|stm32]` exits 0 when every finding is ok, 1 otherwise.
- Checks: `g++` version >= 16 and `-freflection` accepted (compile `int main(){}` with it); `meson` >= 1.5; `ninja`; `copier`; for `esp32_korvo1`: `idf.py` or `IDF_PATH`; for `stm32`: `arm-none-eabi-gcc`.

- [ ] **Step 1: Write the failing tests** with fake `run`/`which`: `test_all_present_is_ok`; `test_old_gcc` (g++ 14: not ok, detail names 14, fix mentions GCC 16 and `reflection: manual`); `test_missing_meson` (which returns None: not ok, fix `uv tool install meson`); `test_tool_that_crashes` (run raises OSError: a not-ok Finding, no exception escapes); `test_exit_code`.
- [ ] **Step 2: Run** them. Expected: FAIL.
- [ ] **Step 3: Implement** `doctor.py`.
- [ ] **Step 4: Run** them. Expected: PASS.
- [ ] **Step 5: Commit** `feat(cli): prism doctor`.

### Task 4: The desktop template, and its CI build

**Files:**
- Create: `copier.yml` (root), `templates/app/` with: `meson.build.jinja`, `subprojects/prism.wrap.jinja`,
  `src/model.hpp.jinja`, `src/main.cpp.jinja`, `tests/test_model.cpp.jinja`, `tests/meson.build.jinja`,
  `README.md.jinja`, `{% if license == 'MIT' %}LICENSE{% endif %}.jinja`, `.gitignore`,
  `{% if github_ci %}.github{% endif %}/workflows/ci.yml.jinja`, `{{ _copier_conf.answers_file }}.jinja`
- Modify: `.github/workflows/ci.yml` (a `template` job)
- Test: `tools/prism-cli/tests/test_template.py` (render only, pure Python, uses `copier.run_copy` with `vcs_ref="HEAD"` on the local checkout)

**Interfaces:**
- Produces: the template's question names (Global Constraints); the generated model is
  `struct {{ project_class }}Model` in `src/model.hpp` with three Fields, one Derived and one Button
  and, for `reflection: manual`, a `view()`; `project_class` is a computed Copier variable
  (CamelCase of the slug).

- [ ] **Step 1: Write the failing tests** (`test_template.py`): `test_defaults_render` (files listed above exist; `prism.wrap` has the url and `depth = 1`; `meson.build` has the three `=disabled` options); `test_slug_from_awkward_name` ("2D Plotter" gives slug `plotter_2d` or another valid C identifier not starting with a digit, and the Meson project name and model class are valid identifiers); `test_manual_reflection_has_view`; `test_no_license_no_ci` (license none, github_ci false: neither file).
- [ ] **Step 2: Run** them. Expected: FAIL.
- [ ] **Step 3: Implement** `copier.yml` (questions, `_subdirectory`, `_answers_file: .copier-answers.yml`, the computed `project_class`, a validator rejecting an empty name) and the template files. The model test (`tests/test_model.cpp`) checks the Derived value after setting the Fields, no window.
- [ ] **Step 4: Run** them. Expected: PASS.
- [ ] **Step 5: Build a rendered project on Coder:** render with `--defaults --data project_name=Demo` into `~/tooling-demo`, replace its `subprojects/prism` by a symlink to `~/PRSIM`, then `meson setup build && ninja -C build && meson test -C build`. Expected: builds, 1 test passes. Repeat with `reflection=manual`.
- [ ] **Step 6: Add the CI job** `template` to `.github/workflows/ci.yml`, modelled on `gcc16-reflection` (container `gcc:16-trixie`): install Copier, render with `--defaults --data project_name=Demo --vcs-ref HEAD`, symlink the checkout into `subprojects/prism`, build, test. A second step renders `reflection=manual`.
- [ ] **Step 7: Commit** `feat(template): the desktop project template, rendered and built in CI`.

### Task 5: `prism new` and `prism update`

**Files:**
- Create: `tools/prism-cli/src/prism_cli/project.py`
- Test: `tools/prism-cli/tests/test_project.py`

**Interfaces:**
- Consumes: Task 4's question names.
- Produces: `project.new(dest: Path, targets: list[str], data: dict, src: str = "gh:PRISM-GUI/PRISM", copy=copier.run_copy)`; `project.update(path: Path, update=copier.run_update)`; CLI `prism new NAME [--target T ...] [--defaults] [--from SRC]`, `prism update`.

- [ ] **Step 1: Write the failing tests** with a fake `copy`/`update`: `test_new_passes_name_and_targets` (data has `project_name`, `targets`); `test_new_refuses_nonempty_dir` (an existing non-empty dest: `ProjectError` with the path, `copy` not called, CLI exit 2); `test_update_needs_answers_file` (no `.copier-answers.yml`: a message saying it was not made by `prism new`).
- [ ] **Step 2: Run** them. Expected: FAIL.
- [ ] **Step 3: Implement** `project.py`.
- [ ] **Step 4: Run** them, then the whole package suite: `uv run --extra test pytest -q`. Expected: all PASS.
- [ ] **Step 5: Commit** `feat(cli): prism new and prism update`.

### Task 6: The ESP32 Korvo target

**Files:**
- Create in `templates/app/`: `{% if 'esp32_korvo1' in targets %}boards{% endif %}/korvo1/` with
  `CMakeLists.txt.jinja`, `main/CMakeLists.txt.jinja`, `main/main.cpp.jinja` (runs the shared
  `src/model.hpp` with `frame_app`, as prism-mp3-player's board main does), `sdkconfig.defaults.jinja`,
  `scripts/build.sh.jinja`, `scripts/flash.sh.jinja`, `.gitmodules.jinja` entry for
  `subprojects/prism-esp32` (url `https://github.com/PRISM-GUI/prism-esp32.git`)
- Modify: `tools/prism-cli/tests/test_template.py`; prism-esp32's `.github/workflows/ci.yml`
  (render the template with `targets=[desktop, esp32_korvo1]` and build the board target in its
  firmware image)

- [ ] **Step 1: Write the failing test** `test_esp32_target_renders` (the board files exist only when the target is chosen; `.gitmodules` names prism-esp32).
- [ ] **Step 2: Run** it. Expected: FAIL.
- [ ] **Step 3: Implement** the board files, copying the structure prism-mp3-player's `boards/korvo1` uses (read it; keep only what a minimal app needs).
- [ ] **Step 4: Run** it. Expected: PASS. Then build the rendered board target on Coder with prism-esp32's build script. Expected: a `.bin` is produced.
- [ ] **Step 5: Add the prism-esp32 CI step** and commit both repositories (`feat(template): the ESP32 Korvo target`; in prism-esp32 `ci: build a project from PRISM's template`).

### Task 7: Docs

**Files:**
- Create: `doc/start/tooling.md` (the five commands, each with an example run)
- Modify: `doc/start/install.md` ("Use PRISM in your project" leads with `prism new`; the by-hand recipe stays below), `doc/index.md` (toctree), `tools/prism-cli/README.md`; prism-stm32 `docs/debugging.md` (point at `prism size` and `prism fault`)

- [ ] **Step 1: Write the pages**, every command shown verified by running it (Python commands locally, builds on Coder).
- [ ] **Step 2: Build the manual** on Coder: `.venv-doc/bin/sphinx-build -q -b html doc /tmp/docout`. Expected: no output, exit 0.
- [ ] **Step 3: Commit** `doc: starting a project with prism new; the prism tool`.
