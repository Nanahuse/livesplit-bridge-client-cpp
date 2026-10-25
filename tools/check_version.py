#!/usr/bin/env python3
"""Validate project versions for pull requests and release tagging.

Rules:
- CMakeLists.txt and vcpkg.json must use the same MAJOR.MINOR.PATCH version.
- A PR may keep the current version (no release) or increase it.
- A version increase must not reuse an existing vX.Y.Z tag.
- If the base revision has no project version yet, the current version is
  treated as the initial release.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

SEMVER_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
ZERO_RE = re.compile(r"^0+$")


def fail(message: str) -> None:
    print(f"version-check: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_semver(value: str, source: str) -> tuple[int, int, int]:
    if not SEMVER_RE.fullmatch(value):
        fail(f"{source} must be MAJOR.MINOR.PATCH without leading zeros: {value!r}")
    major, minor, patch = (int(part) for part in value.split("."))
    return major, minor, patch


def read_cmake_version(root: Path) -> str:
    try:
        text = (root / "CMakeLists.txt").read_text(encoding="utf-8-sig")
    except OSError as error:
        fail(f"cannot read CMakeLists.txt: {error}")
    project = re.search(r"\bproject\s*\((.*?)\)", text, re.IGNORECASE | re.DOTALL)
    if not project:
        fail("CMakeLists.txt has no project()")
    version = re.search(r"\bVERSION\s+([^\s)]+)", project.group(1), re.IGNORECASE)
    if not version:
        fail("CMakeLists.txt project() has no VERSION")
    return version.group(1)


def read_vcpkg_version(root: Path) -> str:
    try:
        data = json.loads((root / "vcpkg.json").read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read vcpkg.json: {error}")
    version = data.get("version-semver")
    if not isinstance(version, str):
        fail("vcpkg.json has no string version-semver")
    return version


def git_output(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), *args],
        check=False,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        fail(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def ref_exists(root: Path, ref: str) -> bool:
    result = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}"],
        check=False,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0


def read_ref_file(root: Path, ref: str, path: str) -> str | None:
    exists = subprocess.run(
        ["git", "-C", str(root), "cat-file", "-e", f"{ref}:{path}"],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if exists.returncode != 0:
        return None
    return git_output(root, "show", f"{ref}:{path}")


def parse_ref_version(root: Path, ref: str) -> tuple[str, str] | None:
    cmake_text = read_ref_file(root, ref, "CMakeLists.txt")
    vcpkg_text = read_ref_file(root, ref, "vcpkg.json")
    if cmake_text is None and vcpkg_text is None:
        return None
    if cmake_text is None or vcpkg_text is None:
        fail(f"base revision {ref} contains only one of CMakeLists.txt and vcpkg.json")
    cmake_text = cmake_text.lstrip("\ufeff")
    vcpkg_text = vcpkg_text.lstrip("\ufeff")

    project = re.search(r"\bproject\s*\((.*?)\)", cmake_text, re.IGNORECASE | re.DOTALL)
    if not project:
        fail(f"base revision {ref}: CMakeLists.txt has no project()")
    cmake_version = re.search(r"\bVERSION\s+([^\s)]+)", project.group(1), re.IGNORECASE)
    if not cmake_version:
        fail(f"base revision {ref}: project() has no VERSION")

    try:
        vcpkg_data = json.loads(vcpkg_text)
    except json.JSONDecodeError as error:
        fail(f"base revision {ref}: cannot parse vcpkg.json: {error}")
    vcpkg_version = vcpkg_data.get("version-semver")
    if not isinstance(vcpkg_version, str):
        fail(f"base revision {ref}: vcpkg.json has no string version-semver")

    return cmake_version.group(1), vcpkg_version


def tag_commit(root: Path, tag: str) -> str | None:
    result = subprocess.run(
        ["git", "-C", str(root), "rev-list", "-n", "1", f"refs/tags/{tag}"],
        check=False,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    return result.stdout.strip() if result.returncode == 0 else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--base-ref",
        default="",
        help="base revision to compare against (for example origin/main or a commit SHA)",
    )
    parser.add_argument(
        "--github-output",
        default="",
        help="optional GITHUB_OUTPUT file",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    cmake_version = read_cmake_version(root)
    vcpkg_version = read_vcpkg_version(root)
    if cmake_version != vcpkg_version:
        fail(
            "CMakeLists.txt and vcpkg.json versions differ: "
            f"{cmake_version} != {vcpkg_version}"
        )
    current = parse_semver(cmake_version, "project version")

    base_ref = args.base_ref.strip()
    if base_ref and ZERO_RE.fullmatch(base_ref):
        base_ref = ""
    if base_ref and not ref_exists(root, base_ref):
        fail(f"base revision does not exist: {base_ref}")

    base_version: str | None = None
    base_tuple: tuple[int, int, int] | None = None
    if base_ref:
        base = parse_ref_version(root, base_ref)
        if base is not None:
            base_cmake, base_vcpkg = base
            if base_cmake != base_vcpkg:
                fail(
                    f"base revision versions are inconsistent: {base_cmake} != {base_vcpkg}"
                )
            base_version = base_cmake
            base_tuple = parse_semver(base_version, "base version")

    if base_tuple is None:
        changed = True
        print("Base version: (none; initial release)")
    elif current < base_tuple:
        fail(
            "version must not decrease: "
            f"{cmake_version} is older than base {base_version}"
        )
    else:
        changed = current > base_tuple
        print(f"Base version: {base_version}")

    print(f"CMake version: {cmake_version}")
    print(f"vcpkg version: {vcpkg_version}")
    print(f"Version changed: {'yes' if changed else 'no'}")

    if changed:
        tag = f"v{cmake_version}"
        existing = tag_commit(root, tag)
        if existing is not None:
            head = git_output(root, "rev-parse", "HEAD")
            if existing != head:
                fail(f"tag {tag} already exists on another commit")
            print(f"Tag {tag} already points to HEAD (rerun)")

    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8", newline="\n") as output:
            output.write(f"version={cmake_version}\n")
            output.write(f"changed={'true' if changed else 'false'}\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
