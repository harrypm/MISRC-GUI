"""Update CLI end-to-end test: drives the REAL built misrc_gui binary through
the headless update flags against a local test server, in an isolated HOME.

Covers (the same gui_updater pipeline the GUI's Releases Page / Install
buttons use):
- --check-update: manifest ok, latest tag resolved via the manifest's
  tag_lookup_url (302 redirect), current version and update availability
  checked for development builds and for equal/newer release tags;
- --update-dry-run: the platform asset URL + the running exe path + updates
  dir + archive path resolved, nothing downloaded or replaced;
- --install-update WITHOUT --yes: refused (non-zero exit, binary untouched);
- --install-update --yes: the real install pipeline - the asset zip is
  downloaded into the isolated updates dir and kept, the old binary is
  ARCHIVED by rename (content preserved, never deleted), the zip payload
  replaces the binary at the original path, and the CLI reports
  install: ok + the archive path.

Network-free: everything is served from 127.0.0.1 (gui_updater's URL scheme
rule allows http on loopback hosts for exactly this test). The install step
shells out to unzip (POSIX) / tar (Windows) like the real updater. macOS runs
check/dry-run/refusal only: a real install needs a DMG, which is not
synthesized here."""
import http.server
import io
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import zipfile
from pathlib import Path

GUI = str(Path(sys.argv[1]).resolve())
PAYLOAD_NAME = "misrc_update_cli_test_payload"
PAYLOAD_BYTES = b"MISRC-UPDATE-CLI-TEST-PAYLOAD\x00" + bytes(range(256))
DOWNLOAD_RE = re.compile(r"^/releases/download/([^/]+)/(.+)$")


def build_zip() -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr(PAYLOAD_NAME, PAYLOAD_BYTES)
    return buf.getvalue()


ZIP_BYTES = build_zip()


class Handler(http.server.BaseHTTPRequestHandler):
    base = None   # set once the ephemeral port is known
    manifest = None
    tag = "v9.9.9"  # release tag for non-comparable development builds

    def log_message(self, fmt, *args):
        pass

    def _serve(self, head_only=False):
        path = self.path.split("?")[0]
        if path == "/manifest.json":
            body = self.manifest.encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if not head_only:
                self.wfile.write(body)
            return
        if path == "/releases/latest":
            self.send_response(302)
            self.send_header("Location", f"{self.base}/releases/tag/{self.tag}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        match = DOWNLOAD_RE.match(path)
        if match and match.group(1) == self.tag:
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Length", str(len(ZIP_BYTES)))
            self.end_headers()
            if not head_only:
                self.wfile.write(ZIP_BYTES)
            return
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        self._serve()

    def do_HEAD(self):
        self._serve(head_only=True)


def parse_output(text):
    """Parse the CLI's 'key: value' stdout lines into a dict."""
    out = {}
    for line in text.splitlines():
        if ": " in line:
            key, value = line.split(": ", 1)
            out[key] = value
    return out


def expected_asset(tag: str) -> str:
    system = platform.system()
    machine = platform.machine().lower()
    arm = machine in ("arm64", "aarch64")
    if system == "Linux":
        return f"Linux_MISRC_GUI_{tag}_{'arm64' if arm else 'x86'}.zip"
    if system == "Darwin":
        return f"macOS_MISRC_GUI_{tag}_universal.dmg"
    return f"Windows_MISRC_GUI_{tag}_{'arm64' if arm else 'x86'}.zip"


def check_update(exe, manifest_url, env, version, tag, comparable, available):
    Handler.tag = tag
    r = subprocess.run(
        [str(exe), "--check-update", "--update-manifest", manifest_url],
        capture_output=True, text=True, env=env)
    out = parse_output(r.stdout)
    assert r.returncode == 0, f"check-update failed:\n{r.stdout}\n{r.stderr}"
    assert out["update_mode"] == "check"
    assert out["manifest"] == "ok"
    assert out["latest"] == tag
    assert out["current"] == version
    assert out["current_comparable"] == str(comparable).lower(), out
    assert out["update_available"] == str(available).lower(), out


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    Handler.base = f"http://127.0.0.1:{port}"
    Handler.manifest = json.dumps({
        "schema": 1,
        "releases_page": "https://github.com/harrypm/MISRC-GUI/releases/",
        "tag_lookup_url": f"{Handler.base}/releases/latest",
        "download_base_url": f"{Handler.base}/releases/download",
        "asset_android": "Android_MISRC_GUI_{tag}_arm64.apk",
        "asset_macos": "macOS_MISRC_GUI_{tag}_universal.dmg",
        "asset_windows_x86": "Windows_MISRC_GUI_{tag}_x86.zip",
        "asset_windows_arm64": "Windows_MISRC_GUI_{tag}_arm64.zip",
        "asset_linux_x86": "Linux_MISRC_GUI_{tag}_x86.zip",
        "asset_linux_arm64": "Linux_MISRC_GUI_{tag}_arm64.zip",
    })
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="misrc-update-cli-") as tmp:
            root = Path(tmp)
            home = root / "home"
            home.mkdir()
            # A COPY of the binary is the "old version" so the install step
            # archives/replaces the copy, never the build output.
            exe = (root / "misrc_gui_copy").resolve()
            shutil.copy2(GUI, exe)
            exe.chmod(0o755)
            original_bytes = exe.read_bytes()

            env = os.environ.copy()
            if os.name == "nt":
                env["LOCALAPPDATA"] = str(home)
                # A tar beside the app/on PATH must not replace Windows' system tar.
                # where.exe rejects tar's flags, so using this shadow must fail.
                shutil.copy2(Path(os.environ["SystemRoot"]) / "System32" / "where.exe", root / "tar.exe")
                env["PATH"] = str(root) + os.pathsep + env.get("PATH", "")
            else:
                env["HOME"] = str(home)
            manifest_url = f"{Handler.base}/manifest.json"

            version = subprocess.run([str(exe), "--version"], capture_output=True,
                                      text=True, env=env, check=True).stdout.strip()
            print(f"current version: {version}")

            # 1. --check-update. Local and non-release CI builds use a dev
            # version. Release builds, including CI tag overrides, compare
            # their numeric version triplet with the fixture tag.
            release = re.match(r"^[vV]?([0-9]+)\.([0-9]+)\.([0-9]+)", version)
            tag = "v9.9.9"
            if release:
                major, minor, patch = map(int, release.groups())
                check_update(exe, manifest_url, env, version,
                             f"v{major}.{minor}.{patch}", True, False)
                tag = f"v{major}.{minor}.{patch + 1}"
            check_update(exe, manifest_url, env, version, tag, bool(release), bool(release))
            asset = expected_asset(tag)

            # 2. --update-dry-run: resolve paths without downloading/replacing files.
            if os.name == "nt":
                updates = home / "misrc" / "updates"
                assert not updates.exists()
            r = subprocess.run(
                [str(exe), "--update-dry-run", "--update-manifest", manifest_url],
                capture_output=True, text=True, env=env)
            out = parse_output(r.stdout)
            assert r.returncode == 0, f"update-dry-run failed:\n{r.stdout}\n{r.stderr}"
            assert out["update_mode"] == "dry-run"
            assert out["asset_url"] == f"{Handler.base}/releases/download/{tag}/{asset}"
            assert out["exe_path"] == str(exe), out["exe_path"]
            assert out["archive_path"] == f"{exe}.archived-{version}"
            assert not Path(out["archive_path"]).exists()
            assert exe.read_bytes() == original_bytes  # nothing replaced
            if os.name == "nt":
                assert Path(out["updates_dir"]) == updates
                assert updates.is_dir()
                assert not (updates / asset).exists()

                # Also create a fresh tree from a forward-slash drive path,
                # then resolve it again with the directories already present.
                slash_home = home / "forward-slash"
                slash_env = env.copy()
                slash_env["LOCALAPPDATA"] = slash_home.as_posix()
                for _ in range(2):
                    r = subprocess.run(
                        [str(exe), "--update-dry-run", "--update-manifest", manifest_url],
                        capture_output=True, text=True, env=slash_env)
                    assert r.returncode == 0, f"forward-slash dry-run failed:\n{r.stdout}\n{r.stderr}"
                    assert Path(parse_output(r.stdout)["updates_dir"]) == slash_home / "misrc" / "updates"
                assert (slash_home / "misrc" / "updates").is_dir()
                assert exe.read_bytes() == original_bytes

            # 3. --install-update without --yes: refused, binary untouched.
            r = subprocess.run(
                [str(exe), "--install-update", "--update-manifest", manifest_url],
                capture_output=True, text=True, env=env)
            assert r.returncode != 0, "--install-update must refuse without --yes"
            assert "--install-update requires --yes" in r.stderr, f"{r.stdout}\n{r.stderr}"
            assert exe.read_bytes() == original_bytes

            if platform.system() == "Darwin":
                print("macOS: install step skipped (a real install needs a DMG; "
                      "check/dry-run/refusal verified)")
            else:
                # 4. --install-update --yes: the full real pipeline.
                r = subprocess.run(
                    [str(exe), "--install-update", "--yes",
                     "--update-manifest", manifest_url],
                    capture_output=True, text=True, env=env)
                out = parse_output(r.stdout)
                assert r.returncode == 0, f"install failed:\n{r.stdout}\n{r.stderr}"
                assert out["install"] == "ok"
                assert out["archive"] == f"{exe}.archived-{version}"
                archive = Path(out["archive"])
                assert archive.is_file()
                assert archive.read_bytes() == original_bytes, \
                    "the archived old binary must be preserved byte-for-byte (never deleted)"
                assert exe.read_bytes() == PAYLOAD_BYTES, \
                    "the zip payload must replace the binary at the original path"
                downloads = list(home.rglob(asset))
                assert downloads, "the downloaded asset zip must be kept in the updates dir"
                assert downloads[0].read_bytes() == ZIP_BYTES
                print(f"install verified: archive={archive.name}, "
                      f"replacement installed, download kept at {downloads[0]}")
    finally:
        server.shutdown()
        server.server_close()
    print("update CLI: check, dry-run, install-refusal and confirmed install PASS")


if __name__ == "__main__":
    main()
