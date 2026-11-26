"""Builds yffi's C ABI into a static library, through cargo.

Usage:
  build.py <Cargo.toml> <target-dir> <output.a>
  build.py <Cargo.toml> <target-dir> <output.a> --target T --rustflags F

Called by a Meson custom_target that is always stale, because cargo, not Meson, knows what changed
in the Rust tree. `CARGO` names another cargo executable (a cross build's, or a wrapper).

A target makes this a cross build, and std has to be rebuilt for it (-Zbuild-std), which only a
nightly cargo with the rust-src component can do. For the browser (wasm32-unknown-emscripten) it
changes two more things. Its std is rebuilt with threads (atomics), hence -Zbuild-std and
RUSTUP_TOOLCHAIN=nightly from the Meson side. And yrs 0.28.2 does not compile for emscripten as
published: it gates `impl Default for undo::Options` on `target_family = "wasm"`, while the clock
that impl needs is gated on `not(all(target_family = "wasm", target_os = "unknown"))`. On
emscripten target_family is wasm and target_os is emscripten, so the clock exists and Default does
not; upstream should use the narrower condition in both places
(https://github.com/y-crdt/y-crdt/pull/791; drop this patch once a release carries it). A
`[patch.crates-io]` entry cannot be added to a crates.io dependency from Meson's packagefiles, and
a patched yrs tree in the repository would be a vendored copy to keep in step by hand, so this
script copies the registry's yrs-0.28.2 into the target dir, applies that one fix, and points cargo
at the copy. The copy lives beside cargo's own output, so once fetched it is patched offline; for
an emscripten build --locked has to go, because a patch makes cargo re-resolve the lock.

A board (riscv32imafc-esp-espidf) needs neither the patch nor RUSTFLAGS: it is a plain std build.
Its cargo is the esp-rs toolchain `espup install` puts in rustup, selected with
RUSTUP_TOOLCHAIN=esp from the Meson cross file, not upstream nightly. Nightly's std pins libc
0.2.191, whose newlib-4 `struct stat` its own espidf filesystem code still reads as the old field
layout, so std does not compile; the esp-rs toolchain pins an older libc whose layout matches. ESP-IDF's newlib has no realpath(), and Rust's
canonicalize references it, so the app supplies a stub.
"""

import argparse
import glob
import os
import shutil
import subprocess

ARCHIVE = os.path.join("release", "libyrs.a")

# Pinned to the crate and version the wrap fetches; both spellings of the gate are exactly the ones
# in its undo module.
YRS = ("yrs", "0.28.2")
UNDO_GATE = (
    '#[cfg(not(target_family = "wasm"))]',
    '#[cfg(not(all(target_family = "wasm", target_os = "unknown")))]',
)


def run(*arguments):
    subprocess.run([os.environ.get("CARGO", "cargo"), *arguments], check=True)


def registry_crate(name, version):
    """Where cargo unpacked a fetched crate."""
    home = os.environ.get("CARGO_HOME", os.path.expanduser("~/.cargo"))
    matches = glob.glob(os.path.join(home, "registry", "src", "*", f"{name}-{version}"))
    if not matches:
        raise SystemExit(f"{name}-{version} is not in cargo's registry: fetch it first")
    return matches[0]


def patched_yrs(target_dir):
    """The registry's yrs with the undo fix, where cargo's [patch] can find it."""
    patched = os.path.join(target_dir, *YRS)
    shutil.rmtree(patched, ignore_errors=True)
    shutil.copytree(registry_crate(*YRS), patched)
    undo = os.path.join(patched, "src", "undo.rs")
    os.chmod(undo, 0o644)  # the registry copy is read-only
    with open(undo, encoding="utf-8") as source:
        text = source.read()
    with open(undo, "w", encoding="utf-8") as target:
        target.write(text.replace(*UNDO_GATE))
    return patched


def cross_arguments(manifest, target_dir, target, rustflags):
    """What one cross target needs from cargo, after fetching its crates."""
    os.environ["RUSTFLAGS"] = rustflags or ""
    run(
        "fetch",
        "--manifest-path",
        manifest,
        "--target",
        target,
    )
    arguments = [
        "--target",
        target,
        # std rebuilt for the target's libc: the browser adds atomics, a board needs nothing else.
        "-Zbuild-std=std,panic_abort",
    ]
    if "emscripten" in target:
        arguments += [
            "--config",
            f'patch.crates-io.yrs.path="{patched_yrs(target_dir)}"',
        ]
    return arguments


def private_crate(manifest, target_dir):
    """A copy of the crate to build from. The yrs patch makes cargo rewrite Cargo.lock, and the
    crate's own lock is shared with a native build of the same checkout, which reads it --locked."""
    source = os.path.dirname(manifest)
    copy = os.path.join(target_dir, "yffi")
    shutil.rmtree(copy, ignore_errors=True)
    shutil.copytree(source, copy, ignore=shutil.ignore_patterns("target"))
    return os.path.join(copy, "Cargo.toml")


def build(manifest, target_dir, output, target=None, rustflags=None):
    if target is not None and "emscripten" in target:
        manifest = private_crate(manifest, target_dir)
    arguments = [
        "build",
        "--release",
        "--quiet",
        # A Rust panic must not unwind into C++: abort instead.
        "--config",
        'profile.release.panic="abort"',
        "--manifest-path",
        manifest,
        "--target-dir",
        target_dir,
    ]
    if target is None:
        arguments.append("--locked")
    else:
        arguments += cross_arguments(manifest, target_dir, target, rustflags)
    run(*arguments)
    shutil.copyfile(os.path.join(target_dir, target or "", ARCHIVE), output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("manifest")
    parser.add_argument("target_dir")
    parser.add_argument("output")
    parser.add_argument("--target")
    parser.add_argument("--rustflags")
    options = parser.parse_args()
    build(
        options.manifest,
        options.target_dir,
        options.output,
        options.target,
        options.rustflags,
    )
