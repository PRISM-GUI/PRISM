from __future__ import annotations

import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest

from prism_cli import project
from prism_cli.__main__ import main

ROOT = Path(__file__).resolve().parents[3]
GIT = ("git", "-c", "user.name=Test", "-c", "user.email=test@example.com")


def fake_copy(calls: list[tuple[object, ...]]) -> object:
    def copy(*args: object, **kwargs: object) -> None:
        calls.append((*args, kwargs))

    return copy


def no_copier(*args: object, **kwargs: object) -> None:
    pytest.fail("copier must not run")


def clean_git(inside: str = "true\n", status: str = "") -> object:
    def run(command: list[str], **kwargs: object) -> SimpleNamespace:
        output = inside if "rev-parse" in command else status
        return SimpleNamespace(returncode=0, stdout=output, stderr="")

    return run


def test_new_passes_name_and_targets(tmp_path: Path) -> None:
    calls: list[tuple[object, ...]] = []
    dest = tmp_path / "myapp"

    project.new(
        dest,
        ["desktop"],
        {"project_name": "myapp"},
        copy=fake_copy(calls),
        defaults=True,
    )

    source, target, kwargs = calls[0][0], calls[0][1], calls[0][2]
    assert source == "gh:PRISM-GUI/PRISM"
    assert Path(target) == dest
    assert kwargs["data"]["project_name"] == "myapp"
    assert kwargs["data"]["targets"] == ["desktop"]
    assert kwargs["defaults"] is True


def test_new_refuses_nonempty_dir(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    dest = tmp_path / "busy"
    dest.mkdir()
    (dest / "notes.txt").write_text("keep me")

    with pytest.raises(project.ProjectError) as failure:
        project.new(dest, ["desktop"], {"project_name": "busy"}, copy=no_copier)

    assert str(dest) in str(failure.value)
    assert main(["new", str(dest)]) == 2
    assert str(dest) in capsys.readouterr().err


def test_new_makes_first_commit(tmp_path: Path) -> None:
    dest = tmp_path / "app"
    rendered: list[tuple[object, ...]] = []
    commands: list[tuple[str, ...]] = []
    working: list[Path] = []

    def copy(*args: object, **kwargs: object) -> None:
        rendered.append((*args, kwargs))
        dest.mkdir()

    def run(command: list[str], **kwargs: object) -> SimpleNamespace:
        commands.append(tuple(command))
        working.append(kwargs.get("cwd"))
        return SimpleNamespace(returncode=0, stdout="", stderr="")

    project.new(dest, ["desktop"], {"project_name": "app"}, copy=copy, run=run)

    assert commands[-3:] == [
        ("git", "init"),
        ("git", "add", "-A"),
        ("git", "commit", "-m", "Start from the PRISM template"),
    ]
    assert working[-3:] == [dest, dest, dest]


def test_new_survives_missing_git(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    dest = tmp_path / "app"

    def copy(*args: object, **kwargs: object) -> None:
        dest.mkdir()

    def run(command: list[str], **kwargs: object) -> None:
        raise FileNotFoundError(2, "No such file or directory", command[0])

    project.new(dest, ["desktop"], {"project_name": "app"}, copy=copy, run=run)

    assert dest.is_dir()
    assert "git" in capsys.readouterr().out


def test_copier_error_is_one_line(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    def broken_copy(*args: object, **kwargs: object) -> None:
        raise ValueError("Unknown revision\n  abc123")

    with pytest.raises(project.ProjectError) as failure:
        project.new(tmp_path / "app", ["desktop"], {}, copy=broken_copy)

    message = str(failure.value)
    assert "\n" not in message
    assert "Unknown revision abc123" in message
    assert "ValueError" in message
    assert isinstance(failure.value.__cause__, ValueError)

    project_dir = tmp_path / "generated"
    project_dir.mkdir()
    (project_dir / project.ANSWERS_FILE).write_text("project_name: generated\n")

    def broken_update(*args: object, **kwargs: object) -> None:
        raise ValueError("Unknown revision")

    with pytest.raises(project.ProjectError) as failure:
        project.update(project_dir, update=broken_update, run=clean_git())

    assert "\n" not in str(failure.value)
    assert "Unknown revision" in str(failure.value)

    def failing_new(*args: object, **kwargs: object) -> None:
        raise project.ProjectError("ValueError: Unknown revision")

    monkeypatch.setattr(project, "new", failing_new)
    assert main(["new", str(tmp_path / "elsewhere")]) == 2
    stderr = capsys.readouterr().err
    assert stderr.count("\n") == 1
    assert "Unknown revision" in stderr


def test_update_needs_answers_file(tmp_path: Path) -> None:
    with pytest.raises(project.ProjectError) as failure:
        project.update(tmp_path, update=no_copier)

    assert project.ANSWERS_FILE in str(failure.value)
    assert "prism new" in str(failure.value)


def test_update_refuses_not_a_repository(tmp_path: Path) -> None:
    (tmp_path / project.ANSWERS_FILE).write_text("project_name: myapp\n")

    with pytest.raises(project.ProjectError) as failure:
        project.update(tmp_path, update=no_copier, run=clean_git(inside="false\n"))

    assert "git" in str(failure.value)


def test_update_refuses_dirty_tree(tmp_path: Path) -> None:
    (tmp_path / project.ANSWERS_FILE).write_text("project_name: myapp\n")

    with pytest.raises(project.ProjectError) as failure:
        project.update(
            tmp_path, update=no_copier, run=clean_git(status=" M README.md\n")
        )

    assert "commit or stash your changes before prism update" in str(failure.value)


def test_update_runs_in_a_generated_project(tmp_path: Path) -> None:
    (tmp_path / project.ANSWERS_FILE).write_text("project_name: myapp\n")
    calls: list[tuple[object, ...]] = []

    def update(*args: object, **kwargs: object) -> None:
        calls.append((*args, kwargs))

    project.update(tmp_path, update=update, run=clean_git())

    assert Path(calls[0][0]) == tmp_path


@pytest.mark.slow
def test_new_then_update_brings_a_template_change(tmp_path: Path) -> None:
    template = tmp_path / "template"
    template.mkdir()
    shutil.copy(ROOT / "copier.yml", template / "copier.yml")
    shutil.copytree(ROOT / "templates", template / "templates")
    _git(template, "init")
    _git(template, "add", "-A")
    _git(template, "commit", "-m", "the template")

    project_dir = tmp_path / "app"
    project.new(
        project_dir,
        ["desktop"],
        {"project_name": "app"},
        src=str(template),
        defaults=True,
    )
    assert _git(project_dir, "status", "--porcelain").stdout.strip() == ""

    (template / "templates/app/README.md.jinja").write_text("# from the update\n")
    _git(template, "add", "-A")
    _git(template, "commit", "-m", "change the readme")

    project.update(project_dir)

    assert "from the update" in (project_dir / "README.md").read_text()


def _git(repo: Path, *args: str) -> SimpleNamespace:
    return subprocess.run(
        [*GIT, *args], cwd=repo, check=True, capture_output=True, text=True
    )
