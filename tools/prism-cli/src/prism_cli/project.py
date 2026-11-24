"""Starting a project from PRISM's template, and updating it later."""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Any, Callable

import copier

DEFAULT_SOURCE = "gh:PRISM-GUI/PRISM"
ANSWERS_FILE = ".copier-answers.yml"
TARGETS = ("desktop", "esp32_korvo1")

_FIRST_COMMIT = (
    ("git", "init"),
    ("git", "add", "-A"),
    ("git", "commit", "-m", "Start from the PRISM template"),
)


class ProjectError(Exception):
    """A destination or a project the tool will not touch."""


def new(
    dest: Path,
    targets: list[str],
    data: dict[str, Any],
    src: str = DEFAULT_SOURCE,
    copy: Callable[..., Any] = copier.run_copy,
    defaults: bool = False,
    run: Callable[..., Any] = subprocess.run,
) -> Path:
    _refuse_existing(dest)
    _copier(copy, src, str(dest), data=_answers(targets, data, run), defaults=defaults)
    _commit_first_version(dest, run)
    return dest


def update(
    path: Path,
    update: Callable[..., Any] = copier.run_update,
    run: Callable[..., Any] = subprocess.run,
) -> None:
    _require_generated_project(path)
    _require_clean_repository(path, run)
    _copier(update, path, overwrite=True, defaults=True)


def _refuse_existing(dest: Path) -> None:
    if dest.is_file() or (dest.is_dir() and any(dest.iterdir())):
        raise ProjectError(f"{dest} already exists and is not empty")


def _require_generated_project(path: Path) -> None:
    if not (path / ANSWERS_FILE).exists():
        raise ProjectError(
            f"{path} has no {ANSWERS_FILE}: it was not made by `prism new`"
        )


def _require_clean_repository(path: Path, run: Callable[..., Any]) -> None:
    if not _is_git_repository(path, run):
        raise ProjectError(f"{path} is not a git repository: `prism update` needs one")
    if _git(path, run, "status", "--porcelain").stdout.strip():
        raise ProjectError("commit or stash your changes before prism update")


def _is_git_repository(path: Path, run: Callable[..., Any]) -> bool:
    result = _git(path, run, "rev-parse", "--is-inside-work-tree")
    return result.returncode == 0 and result.stdout.strip() == "true"


def _git(path: Path, run: Callable[..., Any], *args: str) -> Any:
    try:
        return run(
            ["git", *args], cwd=path, capture_output=True, text=True, check=False
        )
    except OSError as error:
        raise ProjectError(f"git is not available: {error}") from error


def _answers(
    targets: list[str], data: dict[str, Any], run: Callable[..., Any]
) -> dict[str, Any]:
    answers = {**data, "targets": list(targets)}
    author = _git_author(run)
    if author:
        answers.setdefault("author", author)
    return answers


def _git_author(run: Callable[..., Any]) -> str:
    try:
        result = run(["git", "config", "user.name"], capture_output=True, text=True)
    except OSError:
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def _commit_first_version(dest: Path, run: Callable[..., Any]) -> None:
    if not dest.is_dir():
        return
    try:
        for command in _FIRST_COMMIT:
            run(list(command), cwd=dest, check=True, capture_output=True)
    except OSError:
        print(f"git not found: make the first commit in {dest} yourself")
    except subprocess.CalledProcessError:
        print(f"could not commit {dest}: make its first commit yourself")


def _copier(call: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
    try:
        return call(*args, **kwargs)
    except Exception as error:
        raise ProjectError(_one_line(error)) from error


def _one_line(error: Exception) -> str:
    message = " ".join(str(error).split())
    return f"{type(error).__name__}: {message}" if message else type(error).__name__
