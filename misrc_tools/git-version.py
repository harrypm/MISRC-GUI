#!/usr/bin/env python3
"""Portable twin of git-version.sh (single source of truth stays the .sh).

Native-Windows meson/Python cannot execute a POSIX shell script, which left
local builds on the vcs_tag 'unknown_version' fallback (the v1.2.7 updater
false-positive class). This port resolves the SAME version string under any
Python that can run meson itself.

Resolution order (mirrors git-version.sh exactly):
  1. MISRC_TOOLS_VERSION_OVERRIDE / MISRC_TOOLS_VERSION env (CI release runs
     set this to the tag).
  2. An exact tag at HEAD (a checked-out release tag, e.g. v1.1.6).
  3. A date-stamped dev version: dev-YYYY-MM-DD-<sha> (UTC date + short
     commit SHA), with -dirty appended if the working tree has tracked
     changes.

The input path meson appends as an argument is ignored, same as the .sh.
"""
import datetime
import os
import subprocess
import sys


def _git(args):
    try:
        return subprocess.run(
            ["git"] + args, capture_output=True, text=True, timeout=30
        )
    except Exception:
        return None


def _tree_dirty():
    # --ignore-cr-at-eol: a fresh Windows checkout (CRLF) must not
    # phantom-tag -dirty under MSYS2 git (autocrlf=false); no-op on
    # Linux/macOS. Matches git-version.sh.
    r = _git(["diff", "--quiet", "--ignore-submodules", "--ignore-cr-at-eol", "--"])
    return r is None or r.returncode != 0


def main():
    v = os.environ.get("MISRC_TOOLS_VERSION_OVERRIDE") or os.environ.get("MISRC_TOOLS_VERSION") or ""
    if not v:
        r = _git(["describe", "--tags", "--exact-match",
                  "--match", "v*", "--match", "misrc_tools-*"])
        if r is not None and r.returncode == 0:
            v = r.stdout.strip()

    r = _git(["rev-parse", "--short", "HEAD"])
    sha = ""
    if r is not None and r.returncode == 0:
        sha = r.stdout.strip()
    if not sha:
        sha = "nogit"

    if not v:
        date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
        v = "dev-%s-%s" % (date, sha)

    if v.startswith("dev-"):
        if _tree_dirty():
            v += "-dirty"
    elif v.startswith("v") or v.startswith("misrc_tools-"):
        pass  # exact tag: keep as-is
    else:
        # Any other override string: append SHA + dirty so it stays traceable.
        v += "-" + sha + ("-dirty" if _tree_dirty() else "")

    if v.startswith("misrc_tools-"):
        v = v[len("misrc_tools-"):]

    sys.stdout.write(v + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
