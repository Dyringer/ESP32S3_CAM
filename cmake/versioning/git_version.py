#!/usr/bin/env python3
"""
Generate a version.h header from git state.

Usage:
    git_version.py --prefix PICO_CORE_ --output /path/to/version.h --template /path/to/version.h.in

Version format: MAJOR.MINOR.PATCH-HASH  (e.g. 1.2.3-abcdef0)
Derived from the nearest semver git tag reachable from HEAD.
Falls back to 0.0.0-<hash> when no tag is found, or 0.0.0-unknown
when not inside a git repo.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str]) -> tuple[int, str]:
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result.returncode, result.stdout.strip()


def git_version() -> tuple[str, int, int, int, str]:
    """Return (full, major, minor, patch, hash)."""
    # Commit hash (always available if in a repo)
    rc, commit = run(["git", "log", "--pretty=format:%h", "-n", "1"])
    if rc != 0:
        return "0.0.0-unknown", 0, 0, 0, "unknown"

    # Nearest semver tag — exact match first, then nearest ancestor
    rc, described = run(["git", "describe", "--tags", "--match", "[0-9]*.[0-9]*.[0-9]*", "--long"])
    if rc == 0:
        # Format: 1.2.3-4-gabcdef  (tag-distance-ghash)
        m = re.match(r"^v?(\d+)\.(\d+)\.(\d+)-\d+-g([0-9a-f]+)$", described)
        if m:
            major, minor, patch, hash_ = int(m[1]), int(m[2]), int(m[3]), m[4]
            return f"{major}.{minor}.{patch}-{hash_}", major, minor, patch, hash_

    # No tag found — use 0.0.0 + commit hash
    return f"0.0.0-{commit}", 0, 0, 0, commit


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix",   default="",   help="Macro prefix, e.g. PICO_CORE_")
    parser.add_argument("--output",   required=True, help="Output header path")
    parser.add_argument("--template", required=True, help="version.h.in template path")
    args = parser.parse_args()

    full, major, minor, patch, hash_ = git_version()
    print(f"Version: {full}")

    template = Path(args.template).read_text()
    header = (template
        .replace("@PROJECT_PREFIX@", args.prefix.upper())
        .replace("@GIT_PROJECT_VERSION@",       full)
        .replace("@GIT_PROJECT_VERSION_MAJOR@", str(major))
        .replace("@GIT_PROJECT_VERSION_MINOR@", str(minor))
        .replace("@GIT_PROJECT_VERSION_PATCH@", str(patch))
        .replace("@GIT_PROJECT_VERSION_TWEAK@", hash_))

    out = Path(args.output)
    # Only write if content changed — avoids triggering rebuilds on every make
    if not out.exists() or out.read_text() != header:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(header)


if __name__ == "__main__":
    main()
