#!/usr/bin/env python3
"""Prepare an exact ESP-IDF checkout using tools cached in the CI image."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import quote, urlsplit


def run(
    command: list[str],
    log_file: Path,
    description: str,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
) -> None:
    with log_file.open("a", encoding="utf-8") as output:
        output.write(f"\n==== {description} ====\n")
        output.flush()
        completed = subprocess.run(
            command,
            cwd=cwd,
            env=env,
            stdout=output,
            stderr=subprocess.STDOUT,
        )
    if completed.returncode == 0:
        return

    print(
        f"{description} failed with exit code {completed.returncode}; "
        "showing the full setup log:",
        file=sys.stderr,
    )
    print(log_file.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
    raise RuntimeError(f"{description} failed")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--install-qemu", action="store_true")
    return parser.parse_args()


def required_env(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"required environment variable {name} is not set")
    return value


def main() -> int:
    args = parse_args()
    log_file = Path(tempfile.gettempdir()) / f"setup_idf-{args.version}.log"
    log_file.write_text("", encoding="utf-8")

    idf_path = Path(required_env("IDF_PATH")).resolve()
    if idf_path.exists():
        raise RuntimeError(f"IDF_PATH already exists: {idf_path}")

    project_url = urlsplit(required_env("CI_PROJECT_URL"))
    if project_url.scheme != "https" or not project_url.netloc:
        raise RuntimeError("CI_PROJECT_URL must be an absolute HTTPS URL")

    job_token = required_env("CI_JOB_TOKEN")
    gitlab_root = f"{project_url.scheme}://{project_url.netloc}/"
    authenticated_root = f"{project_url.scheme}://gitlab-ci-token:{quote(job_token, safe='')}@{project_url.netloc}/"
    repository = f"{authenticated_root}espressif/esp-idf.git"

    run(
        [
            "git",
            "clone",
            "--depth=1",
            "--branch",
            args.version,
            repository,
            str(idf_path),
        ],
        log_file,
        "Clone ESP-IDF",
    )
    run(
        [
            "git",
            "config",
            "--global",
            f"url.{authenticated_root}.insteadOf",
            gitlab_root,
        ],
        log_file,
        "Configure GitLab authentication",
    )
    run(
        ["git", "submodule", "update", "--init", "--depth=1"],
        log_file,
        "Update ESP-IDF submodules",
        cwd=idf_path,
    )

    install_command = ["bash", "install.sh"]
    if not args.version.startswith("v6."):
        install_command.append("--enable-pytest")

    install_env = os.environ.copy()
    virtual_env = install_env.pop("VIRTUAL_ENV", None)
    install_env.pop("IDF_PYTHON_ENV_PATH", None)
    active_env = virtual_env or (sys.prefix if sys.prefix != sys.base_prefix else None)
    if active_env:
        active_env_bin = Path(active_env).resolve() / "bin"
        install_env["PATH"] = os.pathsep.join(
            entry
            for entry in install_env["PATH"].split(os.pathsep)
            if Path(entry).resolve() != active_env_bin
        )
    run(
        install_command,
        log_file,
        "Install ESP-IDF tools",
        cwd=idf_path,
        env=install_env,
    )

    if args.install_qemu:
        run(
            [
                sys.executable,
                "tools/idf_tools.py",
                "--non-interactive",
                "install",
                "qemu-riscv32",
            ],
            log_file,
            "Install QEMU",
            cwd=idf_path,
        )

    log_file.unlink()
    print(f"Prepared ESP-IDF {args.version} at {idf_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
