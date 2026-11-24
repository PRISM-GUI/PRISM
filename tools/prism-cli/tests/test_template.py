from __future__ import annotations

import re
from pathlib import Path

from copier import run_copy

ROOT = Path(__file__).resolve().parents[3]

DESKTOP_FILES = (
    "meson.build",
    "subprojects/prism.wrap",
    "src/model.hpp",
    "src/main.cpp",
    "tests/test_model.cpp",
    "tests/meson.build",
    "README.md",
    "LICENSE",
    ".gitignore",
    ".github/workflows/ci.yml",
    ".copier-answers.yml",
)

IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def render(dst: Path, **data: object) -> Path:
    run_copy(str(ROOT), str(dst), data=data, defaults=True, vcs_ref="HEAD", quiet=True)
    return dst


def identifiers(out: Path) -> tuple[str, str]:
    meson = (out / "meson.build").read_text()
    model = (out / "src/model.hpp").read_text()
    return (
        re.search(r"project\('([^']+)'", meson).group(1),
        re.search(r"struct (\w+)Model", model).group(1),
    )


def test_defaults_render(tmp_path: Path) -> None:
    out = render(tmp_path / "demo")

    for name in DESKTOP_FILES:
        assert (out / name).is_file(), name

    wrap = (out / "subprojects/prism.wrap").read_text()
    assert "url = https://github.com/PRISM-GUI/PRISM.git" in wrap
    assert "depth = 1" in wrap

    meson = (out / "meson.build").read_text()
    for option in ("examples=disabled", "tests=disabled", "benchmarks=disabled"):
        assert option in meson


def test_slug_from_awkward_name(tmp_path: Path) -> None:
    slug, model_class = identifiers(
        render(tmp_path / "awkward", project_name="2D Plotter")
    )

    assert IDENTIFIER.fullmatch(slug), slug
    assert not slug[0].isdigit(), slug
    assert IDENTIFIER.fullmatch(model_class), model_class


def test_manual_reflection_has_view(tmp_path: Path) -> None:
    out = render(tmp_path / "manual", reflection="manual")

    assert "void view(app::ViewBuilder& vb)" in (out / "src/model.hpp").read_text()


def test_no_license_no_ci(tmp_path: Path) -> None:
    out = render(tmp_path / "bare", license="none", github_ci=False)

    assert not (out / "LICENSE").exists()
    assert not (out / ".github").exists()
