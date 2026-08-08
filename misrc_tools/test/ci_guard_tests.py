#!/usr/bin/env python3
import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple


def fail(message: str) -> int:
    print(f"ERROR: {message}", file=sys.stderr)
    return 1


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def extract_function_body(source: str, signature: str) -> str:
    pattern = re.compile(rf"{re.escape(signature)}\s*\{{(?P<body>.*?)\n\}}", re.S)
    match = pattern.search(source)
    if not match:
        raise RuntimeError(f"Could not find function body for {signature}")
    return match.group("body")


def extract_apprun_script(workflow_text: str) -> str:
    marker = "cat > AppDir/AppRun <<'EOF'"
    start = workflow_text.find(marker)
    if start < 0:
        raise RuntimeError("Could not find AppRun heredoc start marker in workflow")
    start = workflow_text.find("\n", start)
    if start < 0:
        raise RuntimeError("Malformed AppRun heredoc in workflow")
    start += 1
    end = workflow_text.find("\n          EOF", start)
    if end < 0:
        raise RuntimeError("Could not find AppRun heredoc end marker in workflow")
    return textwrap.dedent(workflow_text[start:end]).lstrip("\n")


def run_checked(command: List[str], *, env: Optional[Dict[str, str]] = None) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=True, capture_output=True, text=True, env=env)


def check_cross_platform_workflow_coverage(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "linux-appimage:",
        "arch: x86_64",
        "arch: arm64",
        "windows-exe:",
        "runs-on: windows-2022",
        "macos-app-build:",
        "runner: macos-14",
        "runner: macos-15-intel",
        "macos-app-universal:",
        "release:",
        "- linux-appimage",
        "- windows-exe",
        "- macos-app-universal",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow cross-platform coverage is missing required snippet: {snippet}")
    return 0


def check_no_legacy_release_sanity_workflow(legacy_workflow_path: Path) -> int:
    if legacy_workflow_path.exists():
        return fail(f"Legacy workflow should be removed after replacement: {legacy_workflow_path}")
    return 0


def check_cross_platform_smoke_tests(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    required_smokes = [
        "\"$BUILD_DIR/misrc_gui\" --smoke-test",
        "APPIMAGE_EXTRACT_AND_RUN=1 \"./$APPIMAGE_NAME\" --smoke-test",
        "./dist/MISRC.exe --smoke-test",
        "dist/MISRC.app/Contents/MacOS/MISRC --smoke-test",
    ]
    for smoke in required_smokes:
        if smoke not in workflow_text:
            return fail(f"Missing expected smoke test command in workflow: {smoke}")
    return 0
def check_actions_runtime_policy(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    forbidden_action_pins = [
        "actions/checkout@v4",
        "actions/setup-python@v5",
        "actions/upload-artifact@v4",
        "actions/download-artifact@v4",
    ]
    for pin in forbidden_action_pins:
        if pin in workflow_text:
            return fail(f"Workflow contains deprecated action pin that triggers warning annotations: {pin}")

    required_action_pins = [
        "actions/checkout@v6",
        "actions/setup-python@v6",
        "actions/upload-artifact@v7",
        "actions/download-artifact@v8",
    ]
    for pin in required_action_pins:
        if pin not in workflow_text:
            return fail(f"Workflow is missing expected modern action pin: {pin}")
    return 0
def check_macos_brew_install_policy(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    forbidden_snippets = [
        "brew install cmake fftw flac libusb libuvc meson nasm ninja pkg-config libsoxr",
    ]
    for snippet in forbidden_snippets:
        if snippet in workflow_text:
            return fail(f"Workflow contains non-conditional brew install that emits warning annotations: {snippet}")
    required_snippets = [
        "for formula in cmake fftw flac libusb libuvc meson nasm ninja pkgconf libsoxr; do",
        "if ! brew list --versions \"$formula\" >/dev/null 2>&1; then",
        "brew install \"$formula\"",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing macOS conditional brew install snippet: {snippet}")
    return 0
def check_workflow_fft_dependency_policy(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "libfftw3-dev",
        "mingw-w64-x86_64-fftw",
        "mingw-w64-clang-aarch64-fftw",
        "for formula in cmake fftw flac libusb libuvc meson nasm ninja pkgconf libsoxr; do",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing required FFT dependency snippet: {snippet}")

    fft_probe = "pkg-config --modversion fftw3f"
    fft_probe_count = workflow_text.count(fft_probe)
    if fft_probe_count != 4:
        return fail(f"Workflow must probe fftw3f exactly 4 times (linux/windows x86/windows arm64/macos), found {fft_probe_count}")
    return 0


def check_meson_fft_policy(meson_path: Path) -> int:
    meson_text = read_text(meson_path)
    required_snippets = [
        "error('FFTW3 (fftw3f) is required for misrc_gui.",
        "gui_deps = deps + [ raylib_dep, fftw3f_dep ]",
    ]
    for snippet in required_snippets:
        if snippet not in meson_text:
            return fail(f"meson.build is missing required FFT policy snippet: {snippet}")

    forbidden_snippets = [
        "message('FFTW3 not found, building without FFT support')",
    ]
    for snippet in forbidden_snippets:
        if snippet in meson_text:
            return fail(f"meson.build still contains forbidden optional-FFT fallback snippet: {snippet}")
    return 0


def check_meson_vendored_hsdaoh_policy(meson_path: Path) -> int:
    """Ensure meson.build prefers the vendored .deps/install hsdaoh (mirrors CI)
    so a bare local build cannot silently link a stale system libhsdaoh that
    lacks the v1.0.9 connect fixes."""
    meson_text = read_text(meson_path)
    required_snippets = [
        "hsdaoh_vendored_pc",
        "fs.exists(hsdaoh_vendored_pc)",
        "Using vendored hsdaoh from .deps/install (mirrors CI",
        "declare_dependency",
        "deps = [ hsdaoh_dep ]",
    ]
    for snippet in required_snippets:
        if snippet not in meson_text:
            return fail(f"meson.build is missing vendored-hsdaoh policy snippet: {snippet}")
    forbidden_snippets = [
        "deps = [ dependency('hsdaoh', static: windows_static_deps) ]",
    ]
    for snippet in forbidden_snippets:
        if snippet in meson_text:
            return fail(f"meson.build still contains bare system-hsdaoh dependency (no vendored guard): {snippet}")
    return 0


def check_built_gui_links_vendored_hsdaoh(repo_root: Path) -> int:
    """Runtime check: if a local misrc_gui build exists, assert it links the
    vendored hsdaoh from .deps/install, not a stale system libhsdaoh."""
    gui = repo_root / "misrc_tools" / "build" / "misrc_gui"
    if not gui.exists():
        return 0  # no local build present; CI build uses AppImage bundling
    res = run_checked(["ldd", str(gui)])
    if res.returncode != 0:
        return fail(f"ldd misrc_gui failed: {res.stderr.strip()}")
    found_hsdaoh = False
    for line in res.stdout.splitlines():
        if "libhsdaoh" in line:
            found_hsdaoh = True
            stripped = line.strip()
            if "/usr/local/lib/" in line or " /usr/lib/" in line or " /lib/" in line:
                return fail(f"misrc_gui links a SYSTEM hsdaoh (stale, lacks v1.0.9 connect fixes); expected vendored .deps/install: {stripped}")
            if ".deps/install" not in line:
                return fail(f"misrc_gui links hsdaoh from unexpected path (expected .deps/install): {stripped}")
    if not found_hsdaoh:
        return fail("misrc_gui does not link libhsdaoh at all")
    return 0


def check_linux_desktop_metadata(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    required_desktop_fields = [
        "cat > AppDir/misrc.desktop <<EOF",
        "Exec=misrc_gui",
        "Icon=misrc",
        "StartupWMClass=MISRC Capture ${BUILD_VERSION}",
        "X-GNOME-WMClass=MISRC Capture ${BUILD_VERSION}",
        "Terminal=false",
        "StartupNotify=true",
        "ln -sf misrc.png AppDir/.DirIcon",
    ]
    for field in required_desktop_fields:
        if field not in workflow_text:
            return fail(f"Missing Linux desktop integration field in workflow/AppRun: {field}")
    return 0


def check_macos_layout_policy(gui_c_path: Path) -> int:
    source = read_text(gui_c_path)
    width_body = extract_function_body(source, "static int gui_layout_width(void)")
    height_body = extract_function_body(source, "static int gui_layout_height(void)")

    if "#if defined(__APPLE__)" not in width_body:
        return fail("gui_layout_width() is missing __APPLE__ guard")
    if "#if defined(__APPLE__)" not in height_body:
        return fail("gui_layout_height() is missing __APPLE__ guard")
    if "GetScreenWidth();" not in width_body:
        return fail("gui_layout_width() must use GetScreenWidth() on macOS")
    if "GetScreenHeight();" not in height_body:
        return fail("gui_layout_height() must use GetScreenHeight() on macOS")
    if "GetRenderWidth();" not in width_body:
        return fail("gui_layout_width() must use GetRenderWidth() for non-macOS")
    if "GetRenderHeight();" not in height_body:
        return fail("gui_layout_height() must use GetRenderHeight() for non-macOS")
    return 0

def check_macos_admin_elevation_contract(gui_c_path: Path) -> int:
    source = read_text(gui_c_path)
    required_snippets = [
        "static int gui_macos_relaunch_as_admin_if_needed(int argc, char **argv)",
        "MISRC_GUI_ELEVATED",
        "do shell script (item 1 of argv) with administrator privileges",
        "int elevate_rc = gui_macos_relaunch_as_admin_if_needed(argc, argv);",
        "Administrator permissions are required for MS2130 hsdaoh/libusb capture.",
    ]
    for snippet in required_snippets:
        if snippet not in source:
            return fail(f"Missing required macOS startup elevation contract snippet in misrc_gui.c: {snippet}")
    return 0


def check_windows_meson_subsystem_contract(meson_path: Path) -> int:
    meson_text = read_text(meson_path)
    required_snippets = [
        "gui_win_subsystem = 'console'",
        "gui_win_subsystem = 'windows'",
        "win_subsystem: gui_win_subsystem",
    ]
    for snippet in required_snippets:
        if snippet not in meson_text:
            return fail(f"Missing Windows GUI subsystem contract snippet in meson.build: {snippet}")
    return 0


def check_debug_view_contract(gui_c_path: Path) -> int:
    source = read_text(gui_c_path)
    required_snippets = [
        "--debug-view",
        "bool debug_view = false;",
        "if (strcmp(argv[i], \"--debug-view\") == 0)",
        "gui_enable_debug_console();",
    ]
    for snippet in required_snippets:
        if snippet not in source:
            return fail(f"Missing debug-view runtime contract snippet in misrc_gui.c: {snippet}")
    return 0


def check_settings_persistence_contract(gui_settings_c_path: Path) -> int:
    source = read_text(gui_settings_c_path)
    required_snippets = [
        "gui_settings_ensure_parent_dirs(path)",
        "getenv(\"APPDATA\")",
        "getenv(\"LOCALAPPDATA\")",
        "getenv(\"XDG_CONFIG_HOME\")",
        "_mkdir(path)",
        "mkdir(path, 0700)",
    ]
    for snippet in required_snippets:
        if snippet not in source:
            return fail(f"Missing settings persistence contract snippet in gui_settings.c: {snippet}")

    save_pos = source.find("void gui_settings_save(")
    if save_pos < 0:
        return fail("Could not locate gui_settings_save() in gui_settings.c")
    ensure_pos = source.find("gui_settings_ensure_parent_dirs(path)", save_pos)
    fopen_pos = source.find("FILE *f = fopen(path, \"w\");", save_pos)
    if ensure_pos < 0 or fopen_pos < 0:
        return fail("Missing parent-dir ensure or fopen call in gui_settings_save()")
    if ensure_pos > fopen_pos:
        return fail("gui_settings_save() must ensure parent directories before fopen")
    return 0

def check_flac_large_file_offsets_contract(flac_writer_c_path: Path) -> int:
    source = read_text(flac_writer_c_path)
    required_snippets = [
        "typedef __int64 flac_file_off_t;",
        "#define FLAC_STREAM_FSEEK _fseeki64",
        "#define FLAC_STREAM_FTELL _ftelli64",
        "#define FLAC_STREAM_FSEEK fseeko",
        "#define FLAC_STREAM_FTELL ftello",
        "static FLAC__uint64 flac_stream_max_offset(void)",
    ]
    for snippet in required_snippets:
        if snippet not in source:
            return fail(f"Missing FLAC large-file contract snippet in flac_writer.c: {snippet}")

    seek_start = source.find("static FLAC__StreamEncoderSeekStatus stream_seek_callback(")
    tell_start = source.find("static FLAC__StreamEncoderTellStatus stream_tell_callback(")
    report_start = source.find("static void report_error(")
    if seek_start < 0 or tell_start < 0 or report_start < 0:
        return fail("Missing FLAC stream callback functions in flac_writer.c")

    seek_body = source[seek_start:tell_start]
    tell_body = source[tell_start:report_start]

    if "flac_stream_max_offset()" not in seek_body:
        return fail("stream_seek_callback() must guard against out-of-range large offsets")
    if "FLAC_STREAM_FSEEK(" not in seek_body:
        return fail("stream_seek_callback() must use FLAC_STREAM_FSEEK macro")
    if "fseek(" in seek_body:
        return fail("stream_seek_callback() must not use plain fseek()")

    if "FLAC_STREAM_FTELL(" not in tell_body:
        return fail("stream_tell_callback() must use FLAC_STREAM_FTELL macro")
    if "ftell(" in tell_body:
        return fail("stream_tell_callback() must not use plain ftell()")
    return 0


def check_apprun_static_contract(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    apprun = extract_apprun_script(workflow_text)
    required_snippets = [
        "install_shortcuts()",
        "--create-shortcut",
        "local stable_appimage=\"$local_bin_dir/misrc_gui.AppImage\"",
        "ln -sfn \"$appimage_path\" \"$stable_appimage\"",
        "local startup_wm_class=\"misrc_gui\"",
        "\"$HERE/usr/bin/misrc_gui\" --version",
        "startup_wm_class=\"MISRC Capture $gui_version\"",
        "Icon=misrc",
        "StartupWMClass=${escaped_startup_wm_class}",
        "X-GNOME-WMClass=${escaped_startup_wm_class}",
        "StartupNotify=true",
    ]
    for snippet in required_snippets:
        if snippet not in apprun:
            return fail(f"AppRun shortcut contract is missing snippet: {snippet}")
    if "Exec=\\\"${escaped_launcher_exec_path}\\\" %U" not in apprun and "Exec=\\\\\\\"${escaped_launcher_exec_path}\\\\\\\" %U" not in apprun:
        return fail("AppRun shortcut contract is missing expected Exec launcher format")
    return 0


def check_apprun_runtime_behavior(workflow_path: Path, icon_path: Path) -> int:
    if not sys.platform.startswith("linux"):
        print("SKIP: AppRun runtime behavior (linux-only)")
        return 0
    if shutil.which("bash") is None:
        print("SKIP: AppRun runtime behavior (bash not available)")
        return 0

    workflow_text = read_text(workflow_path)
    script = extract_apprun_script(workflow_text)

    with tempfile.TemporaryDirectory(prefix="misrc_ci_guard_") as temp_root:
        root = Path(temp_root)
        appdir = root / "AppDir"
        (appdir / "usr/bin").mkdir(parents=True, exist_ok=True)

        apprun_path = appdir / "AppRun"
        apprun_path.write_text(script, encoding="utf-8")
        apprun_path.chmod(apprun_path.stat().st_mode | stat.S_IXUSR)

        run_checked(["bash", "-n", str(apprun_path)])

        for exe in ("misrc_gui", "misrc_capture", "misrc_extract"):
            exe_path = appdir / "usr/bin" / exe
            if exe == "misrc_gui":
                exe_path.write_text(
                    "#!/usr/bin/env bash\n"
                    "if [[ \"${1:-}\" == \"--version\" ]]; then\n"
                    "  echo \"test-version\"\n"
                    "  exit 0\n"
                    "fi\n"
                    "exit 0\n",
                    encoding="utf-8",
                )
            else:
                exe_path.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            exe_path.chmod(exe_path.stat().st_mode | stat.S_IXUSR)

        if icon_path.exists():
            shutil.copy2(icon_path, appdir / "misrc.png")
        else:
            (appdir / "misrc.png").write_bytes(b"\x89PNG\r\n\x1a\n")

        appimage_path = root / "MISRC Test Build.AppImage"
        appimage_path.write_text("fake", encoding="utf-8")
        appimage_path.chmod(appimage_path.stat().st_mode | stat.S_IXUSR)

        home = root / "home"
        desktop_dir = home / "Desktop"
        desktop_dir.mkdir(parents=True, exist_ok=True)

        env = os.environ.copy()
        env["HOME"] = str(home)
        env["APPIMAGE"] = str(appimage_path)
        env.pop("XDG_DATA_HOME", None)

        run_checked(["bash", str(apprun_path), "--create-shortcut"], env=env)

        launcher_path = home / ".local/share/applications/misrc_gui.desktop"
        desktop_shortcut_path = home / "Desktop/MISRC GUI.desktop"
        icon_install_path = home / ".local/share/icons/hicolor/512x512/apps/misrc.png"
        stable_exec_path = home / ".local/bin/misrc_gui.AppImage"

        if not launcher_path.exists():
            return fail("AppRun --create-shortcut did not create launcher file")
        if not desktop_shortcut_path.exists():
            return fail("AppRun --create-shortcut did not create Desktop shortcut")
        if not icon_install_path.exists():
            return fail("AppRun --create-shortcut did not install icon")
        if not stable_exec_path.exists():
            return fail("AppRun --create-shortcut did not create stable AppImage launcher path")
        if not os.path.samefile(stable_exec_path, appimage_path):
            return fail("Stable AppImage launcher path does not resolve to current AppImage")

        launcher = read_text(launcher_path)
        expected_exec = f'Exec=\"{stable_exec_path}\" %U'
        if expected_exec not in launcher:
            return fail(f"Launcher Exec entry mismatch. Expected: {expected_exec}")
        expected_wm_class = "MISRC Capture test-version"
        for required in ("Icon=misrc", "Terminal=false", f"StartupWMClass={expected_wm_class}", f"X-GNOME-WMClass={expected_wm_class}", "StartupNotify=true"):
            if required not in launcher:
                return fail(f"Launcher is missing required key: {required}")

        run_checked(["bash", str(apprun_path), "--smoke-test"], env=env)

    return 0


def check_windows_packaging_assertions(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "test \"$(find dist -maxdepth 1 -type f | wc -l)\" -eq 1",
        "test \"$(find dist -maxdepth 1 -name '*.exe' | wc -l)\" -eq 1",
        "objdump -p dist/MISRC.exe",
        "test \"$(objdump -p dist/MISRC.exe | awk '/^Subsystem[[:space:]]/ {print $2; exit}')\" = \"00000002\"",
        "assert_no_nonsystem_dlls()",
        "assert_no_nonsystem_dlls \"dist/MISRC.exe\"",
        "$ZipPath = \"windows_MISRC_${{ steps.version.outputs.version }}_x86.zip\"",
        "Compress-Archive -Path @(\"dist/MISRC.exe\")",
        "if ($zip.Entries.Count -ne 1)",
        "$entry.FullName.Contains('/') -or $entry.FullName.Contains('\\')",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing required Windows packaging assertion: {snippet}")
    return 0

def check_release_artifact_naming_contract(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "workflow_dispatch:",
        "artifact_suffix: x86",
        "artifact_suffix: arm64",
        "APPIMAGE_NAME=\"linux_MISRC_${BUILD_VERSION}_${{ matrix.artifact_suffix }}.AppImage\"",
        "ZIP_NAME=\"linux_MISRC_${BUILD_VERSION}_${{ matrix.artifact_suffix }}.zip\"",
        "path: linux_MISRC_*_${{ matrix.artifact_suffix }}.zip",
        "$ZipPath = \"windows_MISRC_${{ steps.version.outputs.version }}_x86.zip\"",
        "path: windows_MISRC_*_x86.zip",
        "DMG_NAME=\"macos_MISRC_${BUILD_VERSION}_universal.dmg\"",
        "path: macos_MISRC_*_universal.dmg",
        "release-assets/**/linux_MISRC_*_x86.zip",
        "release-assets/**/linux_MISRC_*_arm64.zip",
        "release-assets/**/windows_MISRC_*_x86.zip",
        "release-assets/**/macos_MISRC_*_universal.dmg",
    ]
    forbidden_snippets = [
        "misrc_gui-*-windows-x86_64.zip",
        "misrc_gui-*-macos-universal-app.tar.gz",
        "MISRC_*_windows_x86.zip",
        "MISRC_*_macos_universal.dmg",
        "MISRC_*_linux_x86.zip",
        "MISRC_*_linux_arm64.zip",
        "release-assets/**/misrc_gui-*-linux-x86_64.AppImage",
        "release-assets/**/misrc_gui-*-linux-arm64.AppImage",
        "release-assets/**/misrc_gui-*-windows-x86_64.zip",
        "release-assets/**/misrc_gui-*-macos-universal-app.tar.gz",
        "release-assets/**/MISRC_*_linux_x86.zip",
        "release-assets/**/MISRC_*_linux_arm64.zip",
        "release-assets/**/MISRC_*_windows_x86.zip",
        "release-assets/**/MISRC_*_macos_universal.dmg",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing required release artifact naming snippet: {snippet}")
    for snippet in forbidden_snippets:
        if snippet in workflow_text:
            return fail(f"Workflow still contains legacy release artifact naming snippet: {snippet}")
    return 0

def check_build_workflow_entrypoint_contract(build_workflow_path: Path) -> int:
    if not build_workflow_path.exists():
        return fail(f"Build workflow entrypoint is missing: {build_workflow_path}")
    workflow_text = read_text(build_workflow_path)
    required_snippets = [
        "name: Build and release binary",
        "workflow_dispatch:",
        "create_release:",
        "release_tag:",
        "push:",
        "- 'v*'",
        "preflight-guard-tests:",
        "linux-appimage:",
        "windows-exe:",
        "macos-app-universal:",
        "release:",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Build workflow entrypoint is missing required snippet: {snippet}")
    forbidden_snippets = [
        "uses: ./.github/workflows/release-sanity-build.yml",
    ]
    for snippet in forbidden_snippets:
        if snippet in workflow_text:
            return fail(f"Build workflow entrypoint still contains legacy reusable wrapper snippet: {snippet}")
    return 0


def check_no_capture_stability_clutter(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    forbidden_workflow_snippets = [
        "bash misrc_tools/test/capture_stability_ci.sh",
        "capture-stability-${{ matrix.arch }}",
        "capture-stability-linux-${{ matrix.arch }}",
        "capture-stability-linux-x86_64",
        "capture-stability-linux-arm64",
        "capture-stability-windows",
        "capture-stability-macos-universal",
        "Upload capture stability logs",
        "Upload macOS capture stability logs",
        "Run capture stability loops",
    ]
    for snippet in forbidden_workflow_snippets:
        if snippet in workflow_text:
            return fail(f"Workflow still contains capture-stability Actions clutter snippet: {snippet}")
    return 0


def check_record_ringbuffer_fallback_runtime(repo_root: Path) -> int:
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        print("SKIP: record ringbuffer fallback runtime guard (Linux/macOS only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for record ringbuffer fallback runtime guard")
        print("SKIP: record ringbuffer fallback runtime guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/bufmgr_record_fallback_harness.c"
    buffer_manager_path = repo_root / "misrc_tools/common/buffer_manager.c"
    include_dir = repo_root / "misrc_tools/common"

    if not harness_path.exists():
        return fail(f"Record fallback harness source is missing: {harness_path}")
    if not buffer_manager_path.exists():
        return fail(f"buffer_manager.c is missing: {buffer_manager_path}")

    with tempfile.TemporaryDirectory(prefix="misrc_bufmgr_guard_") as temp_root:
        exe_name = "bufmgr_record_fallback_guard.exe" if os.name == "nt" else "bufmgr_record_fallback_guard"
        exe_path = Path(temp_root) / exe_name
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-D_POSIX_C_SOURCE=200809L",
            "-D_DEFAULT_SOURCE",
            f"-I{include_dir}",
            str(harness_path),
            str(buffer_manager_path),
            "-o",
            str(exe_path),
        ]
        if sys.platform == "darwin":
            compile_cmd.insert(3, "-D_DARWIN_C_SOURCE")
        try:
            run_checked(compile_cmd)
        except subprocess.CalledProcessError as exc:
            return fail(
                "Failed to compile record ringbuffer fallback runtime harness\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            )

        try:
            run_checked([str(exe_path)])
        except subprocess.CalledProcessError as exc:
            return fail(
                "Record ringbuffer fallback runtime harness failed\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="MISRC CI guard tests")
    parser.add_argument(
        "--static-only",
        action="store_true",
        help="Run static/text invariants only (skip runtime AppRun simulation)",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    workflow_path = repo_root / ".github/workflows/build.yml"
    legacy_workflow_path = repo_root / ".github/workflows/release-sanity-build.yml"
    gui_c_path = repo_root / "misrc_tools/misrc_gui/core/misrc_gui.c"
    gui_settings_c_path = repo_root / "misrc_tools/misrc_gui/core/gui_settings.c"
    flac_writer_c_path = repo_root / "misrc_tools/common/flac_writer.c"
    meson_path = repo_root / "misrc_tools/meson.build"
    icon_path = repo_root / "assets/Icons/MISRC_Icon.png"

    checks: List[Tuple[str, Callable[[], int]]] = [
        ("cross-platform workflow coverage", lambda: check_cross_platform_workflow_coverage(workflow_path)),
        ("actions runtime policy", lambda: check_actions_runtime_policy(workflow_path)),
        ("macOS brew install policy", lambda: check_macos_brew_install_policy(workflow_path)),
        ("workflow FFT dependency policy", lambda: check_workflow_fft_dependency_policy(workflow_path)),
        ("meson FFT policy", lambda: check_meson_fft_policy(meson_path)),
        ("meson vendored hsdaoh policy", lambda: check_meson_vendored_hsdaoh_policy(meson_path)),
        ("cross-platform smoke tests", lambda: check_cross_platform_smoke_tests(workflow_path)),
        ("linux desktop metadata", lambda: check_linux_desktop_metadata(workflow_path)),
        ("macOS layout policy", lambda: check_macos_layout_policy(gui_c_path)),
        ("macOS startup admin elevation contract", lambda: check_macos_admin_elevation_contract(gui_c_path)),
        ("Windows meson subsystem contract", lambda: check_windows_meson_subsystem_contract(meson_path)),
        ("debug-view runtime contract", lambda: check_debug_view_contract(gui_c_path)),
        ("settings persistence contract", lambda: check_settings_persistence_contract(gui_settings_c_path)),
        ("FLAC large-file offsets contract", lambda: check_flac_large_file_offsets_contract(flac_writer_c_path)),
        ("AppRun static contract", lambda: check_apprun_static_contract(workflow_path)),
        ("Windows packaging assertions", lambda: check_windows_packaging_assertions(workflow_path)),
        ("release artifact naming contract", lambda: check_release_artifact_naming_contract(workflow_path)),
        ("build workflow entrypoint contract", lambda: check_build_workflow_entrypoint_contract(workflow_path)),
        ("legacy release-sanity workflow removed", lambda: check_no_legacy_release_sanity_workflow(legacy_workflow_path)),
        ("no capture-stability Actions clutter", lambda: check_no_capture_stability_clutter(workflow_path)),
    ]
    if not args.static_only:
        checks.insert(7, ("AppRun runtime behavior", lambda: check_apprun_runtime_behavior(workflow_path, icon_path)))
        checks.insert(8, ("record ringbuffer fallback runtime", lambda: check_record_ringbuffer_fallback_runtime(repo_root)))
        checks.insert(9, ("built GUI links vendored hsdaoh", lambda: check_built_gui_links_vendored_hsdaoh(repo_root)))

    for name, check in checks:
        rc = check()
        if rc != 0:
            print(f"FAILED: {name}", file=sys.stderr)
            return rc
        print(f"PASS: {name}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
