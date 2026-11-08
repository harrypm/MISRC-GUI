#!/usr/bin/env python3
"""Verify a pinned archive before applying the MISRC dependency patch."""

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
PINS = {
    "liba52": (
        "https://distfiles.adelielinux.org/source/a52dec/a52dec-0.8.0.tar.gz",
        "a52dec-0.8.0.tar.gz",
        "03c181ce9c3fe0d2f5130de18dab9bd8bc63c354071515aa56983c74a9cffcc9",
        "a52dec-0.8.0",
        "misrc.patch",
    ),
}


def verify(path, expected):
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
    actual = sha.hexdigest()
    if actual != expected:
        raise ValueError(f"SHA-256 mismatch for {path}: expected {expected}, got {actual}")


def verified_archive(url, name, sha):
    cache = ROOT / (os.environ.get("MISRC_DEP_CACHE") or ".deps/downloads")
    archive = cache / name
    if archive.is_file():
        verify(archive, sha)
        return archive

    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"{name}-", dir=cache) as work:
        download = Path(work) / name
        print(f"Downloading {url}", flush=True)
        with urllib.request.urlopen(url, timeout=60) as response, download.open("wb") as out:
            shutil.copyfileobj(response, out)
        verify(download, sha)
        try:
            download.replace(archive)
        except PermissionError:
            # Another build may already have published and opened the same pin.
            verify(archive, sha)
    return archive


def extract(archive, directory, expected_root):
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar:
            name = PurePosixPath(member.name)
            if (not name.parts or name.parts[0] != expected_root or ".." in name.parts
                    or "\\" in member.name or ":" in member.name):
                raise ValueError(f"Unexpected archive path {member.name!r} in {archive}")
            target = directory.joinpath(*name.parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as data, target.open("wb") as out:
                    shutil.copyfileobj(data, out)
            else:
                raise ValueError(f"Unsupported archive member {member.name!r} in {archive}")
    return directory / expected_root


def apply_patch(source, patch):
    env = os.environ.copy()
    for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
        env.pop(key, None)
    # Use an isolated Git directory so Git cannot filter paths against MISRC's checkout.
    # Relative paths work with both native Git and MSYS2 Git on Windows.
    git_dir = "../patch-repository.git"
    subprocess.run(["git", "init", "--bare", "--quiet", "--template=", git_dir],
                   cwd=source, env=env, check=True)
    subprocess.run(["git", f"--git-dir={git_dir}", "--work-tree=.", "-c", "core.autocrlf=false",
                    "apply", "--whitespace=nowarn", "-"],
                   input=patch, cwd=source, env=env, check=True)


def publish_source(source, destination, previous, marker, pin):
    (source / marker.name).write_bytes(pin)
    if destination.exists():
        if not marker.is_file():
            raise ValueError(f"Refusing to replace an unmanaged source directory: {destination}")
        destination.rename(previous)
    try:
        source.rename(destination)
    except OSError:
        if previous.exists():
            previous.rename(destination)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dependency", choices=PINS)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--decoder-files", type=Path, nargs="+",
                        help="publish only the listed liba52 decoder files for Meson")
    args = parser.parse_args()
    if args.decoder_files and args.dependency != "liba52":
        parser.error("--decoder-files is only valid for liba52")

    url, name, sha, archive_root, patch_name = PINS[args.dependency]
    patch_file = ROOT / "third_party" / args.dependency / patch_name
    patch = patch_file.read_bytes().replace(b"\r\n", b"\n")
    archive = verified_archive(url, name, sha)
    destination = args.destination.resolve()
    marker = destination / ".misrc-source-pin"
    helper = Path(__file__).read_bytes().replace(b"\r\n", b"\n")
    pin = f"{sha}\n{hashlib.sha256(patch).hexdigest()}\n{hashlib.sha256(helper).hexdigest()}\n".encode("ascii")
    if not args.decoder_files and marker.is_file() and marker.read_bytes() == pin:
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="misrc-dependency-", dir=destination.parent) as work:
        work = Path(work)
        source = extract(archive, work, archive_root)
        apply_patch(source, patch)
        if args.decoder_files:
            for output in args.decoder_files:
                subdir = "include" if output.name in ("a52.h", "mm_accel.h") else "liba52"
                shutil.copyfile(source / subdir / output.name, output)
        else:
            publish_source(source, destination, work / "previous", marker, pin)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        print(f"Dependency preparation failed: {error}", file=sys.stderr)
        sys.exit(1)
