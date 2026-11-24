# The `prism` tool

`prism` is one command with five jobs:

| Command | What it does |
|---|---|
| `prism doctor` | tells you which build tool is missing, and how to get it |
| `prism new` | makes a project that builds against PRISM |
| `prism update` | brings a generated project up to date with the template |
| `prism size` | splits a firmware's flash use by origin, from a linker map |
| `prism fault` | names the functions in a board's crash report |

It lives in [`tools/prism-cli/`](../../tools/prism-cli/). It is Python 3.11 or newer.

## Install it

There is no PyPI release yet. Install it straight from GitHub:

```bash
uv tool install "git+https://github.com/PRISM-GUI/PRISM.git#subdirectory=tools/prism-cli"
```

Or from a PRISM checkout:

```bash
git clone https://github.com/PRISM-GUI/PRISM.git
uv tool install --from ./PRISM/tools/prism-cli prism-cli
prism --version
```

```text
prism 0.1.0
```

To run it once without installing, `uvx --from ./PRISM/tools/prism-cli prism doctor` does the
same. Replace `./PRISM` with wherever you cloned the repository.

## `prism doctor`

Run it first. It checks the compiler, Meson, Ninja, Copier and Python, and says what to do about
anything it cannot use:

```console
$ prism doctor
ok   g++: GCC 16.2, -freflection accepted
ok   meson: version 1.11
ok   ninja: /usr/bin/ninja
ok   copier: /home/you/.local/bin/copier
ok   python: python 3.15
```

A `FAIL` line is followed by the command that fixes it:

```console
$ prism doctor
ok   g++: GCC 16.2, -freflection accepted
ok   meson: version 1.11
ok   ninja: /usr/bin/ninja
FAIL copier: not found
     uv tool install copier
ok   python: python 3.15
```

The exit code is 0 when every line starts with `ok`, and 1 otherwise.

`--target` adds the tools that target needs:

| Target | What it adds |
|---|---|
| `desktop` (the default) | nothing more |
| `stm32` | `arm-none-eabi-gcc` |
| `esp32_korvo1` | `idf.py`, or `IDF_PATH` |

```console
$ prism doctor --target stm32
ok   g++: GCC 16.2, -freflection accepted
ok   meson: version 1.11
ok   ninja: /usr/bin/ninja
ok   copier: /home/you/.local/bin/copier
ok   python: python 3.15
ok   arm-none-eabi-gcc: /usr/bin/arm-none-eabi-gcc
```

The compiler check compiles a one-line program with `-freflection`. A GCC older than 16 fails it:
the fix line then says to write a `view()` per struct instead, which
[Install](install.md) explains.

## `prism new`

This makes a project directory, so you do not write a wrap file or a `meson.build` by hand:

```console
$ prism new hello_prism --defaults
```

```text
    create  .copier-answers.yml
    create  LICENSE
    create  .github/workflows/ci.yml
    create  tests/test_model.cpp
    create  tests/meson.build
    create  subprojects/prism.wrap
    create  src/model.hpp
    create  src/main.cpp
    create  meson.build
    create  README.md
    create  .gitignore
created hello_prism
```

Then build and run it:

```bash
cd hello_prism
meson setup builddir
meson compile -C builddir
./builddir/hello_prism
```

`meson setup` fetches PRISM through `subprojects/prism.wrap`, so the first build takes as long as
building PRISM. The project pins the template commit it was made from, not `main`.

Without `--defaults`, Copier asks the questions one by one: the project name, the identifier Meson
and C++ use, a description, the author, the targets, whether reflection is available, the licence,
and whether to add a GitHub CI job. `--defaults` answers all of them with their defaults: a desktop
project, reflection, the MIT licence, and CI.

`--target` adds a board. Repeat it for both:

```bash
prism new hello_board --target desktop --target esp32_korvo1
```

The board target adds `boards/korvo1/`, `scripts/build.sh`, `scripts/flash.sh` and a
`.gitmodules` entry for the ESP32 port. [Embedded boards](../project/embedded.md) covers that
side.

`--from` copies a different template; the default is `gh:PRISM-GUI/PRISM`. Use it to try a local
checkout:

```bash
prism new hello_prism --defaults --from ~/Documents/prog/PRISM
```

`prism new` also makes the first commit, so `prism update` has something to work from. If `git` is
not installed it prints a line saying so and leaves the files in place.

It refuses a destination that already holds something:

```console
$ prism new hello_prism --defaults
prism new: hello_prism already exists and is not empty
```

## `prism update`

The template moves. `prism update` re-applies it, keeps your answers, and keeps the files you
edited:

```console
$ prism update
updated
```

It needs three things, and says which one is missing:

```console
$ prism update
prism update: . has no .copier-answers.yml: it was not made by `prism new`
```

```console
$ prism update
prism update: commit or stash your changes before prism update
```

```console
$ prism update
prism update: /tmp/demo is not a git repository: `prism update` needs one
```

Commit or stash before updating, then review the diff: an update rewrites the files it owns.

## `prism size`

`prism size` reads the linker map of a firmware and says where the flash went:

```console
$ prism size build/app.map --top 5
flash 372745 bytes
   119361  32.0%  prism
    97491  26.2%  app
    40357  10.8%  libstdc++
    23581   6.3%  other
    22601   6.1%  prism:platform
    16365   4.4%  newlib
    15005   4.0%  fonts
    12814   3.4%  freertos
     9024   2.4%  hal
     8258   2.2%  prism:raster
     7888   2.1%  libgcc
ram 1073980 bytes
-- top 5
    20468  .rodata.…noto_sans_latin_dataE  …/libprism-raster.a.p/meson-generated_.._embedded_noto_sans_latin.cpp.o
    10416  .rodata.…fast_float15powers_template…  …/libstdc++.a(floating_from_chars.o)
     7044  .text._ZN5prism3app10WidgetTreeC2I…  stm32f746g_disco.elf.p/boards_stm32f746g_disco_main.cpp.o
     4292  .rodata._ZL16board_early_initv.str1.1  stm32f746g_disco.elf.p/boards_stm32f746g_disco_main.cpp.o
     4216  .text.…fast_floatL19from_chars_advancedIdEE…  …/libstdc++.a(floating_from_chars.o)
```

Long names are cut with `…` on this page.

The first number is the flash total. The lines under it are the same bytes grouped by where they
came from, largest first:

| Family | What it holds |
|---|---|
| `prism` | PRISM's code whose module the map does not name (objects inside `libprism.a`) |
| `prism:<module>` | one PRISM module: `ui`, `app`, `platform`, `raster`, … |
| `app` | your program's own objects |
| `libstdc++`, `libgcc`, `newlib` | the C++ and C libraries |
| `fonts`, `freertos`, `hal`, `cmsis-dsp` | the embedded support: glyphs, the RTOS, the board SDK, the DSP library |
| `other` | everything the patterns above miss |

`ram` is a separate line. A `.bss` or a heap costs RAM, not flash, and is never mixed into the
families. `--top N` (default 10) then lists the N largest flash sections, with the object that
holds each one.

The map is the file the linker writes when you pass `-Wl,-Map=<file>` in the link arguments.
Any map with a `Linker script and memory map` line works, whatever built it.
[prism-stm32's debugging page](https://github.com/PRISM-GUI/prism-stm32/blob/main/docs/debugging.md)
shows one way to get a map out of an existing Meson build.

A text file without that line is refused:

```console
$ prism size notes.txt
prism size: not a linker map: no "Linker script and memory map" line
```

On the F746 widgets build the flash total lands within a kilobyte of the `.bin`, so it is a good
cross-check against `arm-none-eabi-size`.

## `prism fault`

A HardFault on a board prints registers, not names. `prism fault` takes the ELF and the report,
and names the functions:

```console
$ prism fault build/app.elf crash.txt
0x0800abcd  std::expected<void, std::error_code> prism::profile::dump<UartStream>(prism::profile::Recorder const&, UartStream&)  …/include/prism/profile/dump.hpp:316
0x08001234  unsigned int std::__detail::__to_chars_len<unsigned int>(unsigned int, int)  …/include/c++/15.2.0/bits/charconv.h:73
```

The second argument is optional: with no file, the report comes from stdin, so a serial log can be
piped in.

The ELF must be the one the board runs: an address that is not in that image prints `??` and
`??:0`.

The tool reads `pc`, `lr`, `MEPC` and `RA` lines, and the `pc:sp` pairs of an ESP32 `Backtrace:`
line. It runs the `addr2line` that matches the ELF's architecture, so the toolchain must be on
`PATH`:

| Board | Tool |
|---|---|
| STM32 | `arm-none-eabi-addr2line` |
| ESP32 | `riscv32-esp-elf-addr2line`, from ESP-IDF |

`prism doctor` checks the compiler, not `addr2line`. When the tool is missing, `prism fault` names
the one it looked for:

```console
$ prism fault build/app.elf crash.txt
prism fault: [Errno 2] No such file or directory: 'arm-none-eabi-addr2line'
```

## Where to go next

- A project of your own: [Install](install.md) and [Your first app](first-app.md).
- A board: [Embedded boards](../project/embedded.md).
- The tool's own README: [`tools/prism-cli/README.md`](../../tools/prism-cli/README.md).
