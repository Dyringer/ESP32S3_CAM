#!/usr/bin/env python3
"""
Run the Ceedling test target from the build directory containing project.yml.

Usage:
    ceedling_run.py test   [--selector <pattern>] [--verbose]
"""

import argparse
import os
import subprocess
import sys


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["test"])
    parser.add_argument("--selector", default=None, help="Run only tests matching pattern")
    parser.add_argument("--verbose",  action="store_true")
    args = parser.parse_args()

    verbosity = []
    if args.verbose:
        verbosity = ["verbosity[4]"]
    elif os.environ.get("VERBOSE", "").strip() == "1":
        verbosity = ["verbosity[4]"]

    selector  = f"pattern[{args.selector}]" if args.selector else \
                f"pattern[{os.environ['TEST_SELECTOR']}]" if "TEST_SELECTOR" in os.environ \
                else "all"

    cmd = ["ceedling", *verbosity, f"{args.command}:{selector}"]
    rc = subprocess.run(cmd).returncode
    if rc != 0:
        print(f"ceedling exited with code {rc}", file=sys.stderr)
        sys.exit(rc)


if __name__ == "__main__":
    main()
