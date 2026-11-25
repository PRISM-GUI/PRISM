"""Builds yffi's C ABI into a static library, through cargo.

Usage: build.py <Cargo.toml> <target-dir> <output.a>

Called by a Meson custom_target that is always stale, because cargo, not Meson, knows what changed
in the Rust tree. `CARGO` names another cargo executable (a cross build's, or a wrapper).
"""

import os
import shutil
import subprocess
import sys

ARCHIVE = os.path.join("release", "libyrs.a")


def build(manifest, target_dir, output):
    cargo = os.environ.get("CARGO", "cargo")
    subprocess.run(
        [
            cargo,
            "build",
            "--release",
            "--locked",
            "--quiet",
            # A Rust panic must not unwind into C++: abort instead.
            "--config",
            'profile.release.panic="abort"',
            "--manifest-path",
            manifest,
            "--target-dir",
            target_dir,
        ],
        check=True,
    )
    shutil.copyfile(os.path.join(target_dir, ARCHIVE), output)


if __name__ == "__main__":
    build(*sys.argv[1:])
