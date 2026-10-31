#!/usr/bin/env python3
import argparse
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
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
        "runner: macos-15",
        "runner: macos-15-intel",
        "macos-app-universal:",
        "android-apk:",
        "release:",
        "- linux-appimage",
        "- windows-exe",
        "- macos-app-universal",
        "- android-apk",
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
        "actions/cache@v4",
    ]
    for pin in forbidden_action_pins:
        if pin in workflow_text:
            return fail(f"Workflow contains deprecated action pin that triggers warning annotations: {pin}")

    required_action_pins = [
        "actions/checkout@v6",
        "actions/setup-python@v6",
        "actions/upload-artifact@v7",
        "actions/download-artifact@v8",
        "actions/cache@v6",
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
def check_msys2_toolchain_policy(workflow_path: Path) -> int:
    """MSYS2 toolchain policy for the Windows CI jobs.

    - MINGW64 is deprecated by MSYS2 (run 37035393223 emitted the
      '[msystem-mingw64] MINGW64 is deprecated. Migrate to UCRT64 or CLANG64'
      warning annotation); the x86_64 job must use UCRT64.
    - Both Windows jobs must install libsoxr: run 37035393223's arm64 job
      logged 'libsoxr not found, building without resample support' because
      mingw-w64-clang-aarch64-libsoxr was missing from its install list, so
      the ARM64 build silently shipped without resample support while
      x86_64 had it.
    - The arm64 job must install the LLVM OpenMP runtime: MSYS2's clang-built
      static libsoxr.a references libomp (run 37053411644: ld.lld 'undefined
      symbol: __kmpc_fork_call / omp_init_lock' referenced by
      libsoxr.a(soxr.c.obj)/(filter.c.obj)); meson links -lomp for it.
    - The Windows x86_64 deps cache key must name the ucrt64 toolchain so a
      MINGW64-built .deps/install (msvcrt-based static libs) can never be
      reused by the UCRT64 job.
    """
    workflow_text = read_text(workflow_path)
    forbidden_snippets = [
        "msystem: MINGW64",
    ]
    for snippet in forbidden_snippets:
        if snippet in workflow_text:
            return fail(f"Workflow uses the deprecated MSYS2 environment (migrate to UCRT64/CLANG64): {snippet}")
    required_snippets = [
        "msystem: UCRT64",
        "msystem: CLANGARM64",
        "mingw-w64-ucrt-x86_64-libsoxr",
        "mingw-w64-clang-aarch64-libsoxr",
        "mingw-w64-clang-aarch64-llvm-openmp",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing required MSYS2 toolchain/resample parity snippet: {snippet}")
    if "deps-windows-ucrt64-x86_64-" not in workflow_text:
        return fail(
            "Windows x86_64 deps cache key must name the ucrt64 toolchain "
            "(deps-windows-ucrt64-x86_64-) so a MINGW64-built cache entry "
            "(msvcrt-based static libs) can never be reused after the "
            "UCRT64 migration"
        )
    return 0


def check_workflow_fft_dependency_policy(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "libfftw3-dev",
        "mingw-w64-ucrt-x86_64-fftw",
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


# ---------------------------------------------------------------------------
# Libc direct-include contract (include-what-you-use, libc subset).
#
# Regression this prevents (Actions run 37035393223): gui_record_direct.c
# called malloc/free with no <stdlib.h> of its own. common/threading.h
# includes <stdlib.h> only in its POSIX branch, so glibc builds got malloc
# declared transitively while the MSYS2 (Windows) builds did not -> implicit
# declaration: a hard error on MSYS2 GCC/clang, only a warning on
# ubuntu-22.04 gcc 11 / Apple clang / NDK clang. The bug class is invisible
# on every platform except the one it breaks. This static guard makes the
# 10-second preflight (any platform, before any deps build) fail instead:
# a .c/.h whose TOP-LEVEL code (outside any platform-conditional #if, so
# compiled on every platform) calls a libc function must include the libc
# header itself, or get it from a project header that includes it OUTSIDE
# any platform-conditional #if. Platform-conditional provision (threading.h's
# POSIX-only <stdlib.h>) does NOT count - that is exactly the provision
# that compiles on glibc and breaks on MinGW. Calls inside platform-gated
# regions are exempt: by construction their includes live in the same branch
# (the threading.h/posix-branch pattern), and each platform's CI build
# verifies those branches compile with their own branch-local includes.
# ---------------------------------------------------------------------------

_PLATFORM_MACRO_RE = re.compile(
    r"\b(_WIN32|_WIN64|__CYGWIN__|__MINGW32__|__MINGW64__|MSVC|_MSC_VER|"
    r"__APPLE__|__MACH__|__ANDROID__|__linux__|__unix__|__FreeBSD__|"
    r"__NetBSD__|__OpenBSD__|__sun|__EMSCRIPTEN__)\b"
)

_LIBC_HEADER_FAMILIES = {
    "stdlib.h": (
        "malloc", "calloc", "realloc", "free", "exit", "abort", "atexit",
        "atoi", "atol", "strtol", "strtoul", "strtoll", "strtod", "strtof",
        "qsort", "bsearch", "abs", "labs", "rand", "srand", "getenv",
        "posix_memalign", "aligned_alloc",
    ),
    "stdio.h": (
        "printf", "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf",
        "vsnprintf", "fopen", "fclose", "fread", "fwrite", "fseek", "ftell",
        "feof", "ferror", "clearerr", "fflush", "rewind", "remove", "rename",
        "fputs", "fgets", "fgetc", "fputc", "putchar", "puts", "scanf",
        "sscanf", "getline", "perror", "fileno", "setvbuf", "ungetc",
    ),
    "string.h": (
        "memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp",
        "strncmp", "strcpy", "strncpy", "strcat", "strncat", "strchr",
        "strrchr", "strstr", "strdup", "strndup", "strtok", "strtok_r",
        "strcasecmp", "strncasecmp", "strerror", "strcspn", "strspn",
        "strnlen",
    ),
    "time.h": (
        "clock_gettime", "nanosleep", "localtime", "gmtime", "strftime",
        "mktime", "difftime",
    ),
    "math.h": (
        "sin", "cos", "tan", "sqrt", "pow", "floor", "ceil", "fabs",
        "fmod", "round", "lround", "atan2", "exp", "log10", "asin",
        "acos", "atan", "hypot", "fmin", "fmax", "trunc", "rint",
    ),
}


def _strip_c_lines(text: str) -> List[str]:
    strip = _make_c_stripper()
    return [strip(line) for line in text.splitlines()]


def _parse_source(path: Path) -> Tuple[set, List[Path], str]:
    """Parse one source file for the libc direct-include contract.

    Returns (unconditional_system_includes, unconditional_project_include_paths,
    top_level_text) where top_level_text is the comment/string-stripped text
    of the lines OUTSIDE any platform-conditional #if region (code compiled on
    every platform).

    - Directive recognition uses comment/string-stripped lines (a directive
      inside a comment strips to nothing and is skipped), but include names
      are extracted from the RAW line: the stripper blanks string literals,
      which would destroy `#include "header.h"` names.
    - An #include counts as unconditional only if no enclosing #if/#ifdef/#elif
      condition mentions a platform macro. Include guards (#ifndef X_H) do not
      mention platform macros, so guarded headers still count. threading.h's
      POSIX-only <stdlib.h> sits inside `#else` of a _WIN32 chain and does not
      count. Unresolvable project includes (e.g. generated version.h) are
      dropped.
    """
    text = read_text(path)
    raw_lines = text.splitlines()
    stripped_lines = _strip_c_lines(text)
    system: set = set()
    project: List[Path] = []
    top_level_lines: List[str] = []
    stack: List[bool] = []
    for raw_line, stripped_line in zip(raw_lines, stripped_lines):
        s = stripped_line.strip()
        m = re.match(r"#\s*(\w+)", s)
        m_raw = re.match(r"#\s*\w+\s*(.*)", raw_line.strip())
        rest = m_raw.group(1).strip() if m_raw else ""
        if not m:
            if not any(stack):
                top_level_lines.append(stripped_line)
            continue
        if not any(stack):
            top_level_lines.append(s)
        directive = m.group(1)
        if directive in ("if", "ifdef", "ifndef"):
            stack.append(bool(_PLATFORM_MACRO_RE.search(rest)))
        elif directive == "elif" and stack:
            stack[-1] = stack[-1] or bool(_PLATFORM_MACRO_RE.search(rest))
        elif directive == "endif":
            if stack:
                stack.pop()
        elif directive == "include" and not any(stack):
            inc_sys = re.match(r"<([^>]+)>", rest)
            inc_proj = re.match(r'"([^"]+)"', rest)
            if inc_sys:
                system.add(inc_sys.group(1))
            elif inc_proj:
                cand = path.parent / inc_proj.group(1)
                if cand.exists():
                    project.append(cand)
    return system, project, "\n".join(top_level_lines)


def check_libc_direct_include_contract(repo_root: Path) -> int:
    root = repo_root / "misrc_tools"
    if not root.exists():
        return 0
    parsed: Dict[Path, Tuple[set, List[Path], str]] = {}

    def parse(path: Path) -> Tuple[set, List[Path], str]:
        if path not in parsed:
            parsed[path] = _parse_source(path)
        return parsed[path]

    def available_headers(entry: Path) -> set:
        seen = {entry}
        stack = [entry]
        available = set()
        while stack:
            path = stack.pop()
            system, project_paths, _ = parse(path)
            available |= system
            for q in project_paths:
                if q not in seen:
                    seen.add(q)
                    stack.append(q)
        return available

    failures: List[str] = []
    files = sorted(
        p for p in root.rglob("*")
        if p.suffix in (".c", ".h") and p.is_file()
        and ".deps" not in p.parts and "build" not in p.parts
    )
    for path in files:
        _, _, top_level_text = parse(path)
        if not top_level_text:
            continue
        available = available_headers(path)
        for header, funcs in _LIBC_HEADER_FAMILIES.items():
            if header in available:
                continue
            called = [
                fn for fn in funcs
                if re.search(rf"(?<![\w.>]){re.escape(fn)}\s*\(", top_level_text)
            ]
            if called:
                rel = path.relative_to(repo_root)
                failures.append(
                    f"{rel}: top-level code calls {', '.join(called)} without {header} "
                    f"(direct or via a non-platform-conditional project header)"
                )
    if failures:
        for line in failures:
            print(f"ERROR: libc direct-include violation: {line}", file=sys.stderr)
        return fail(
            "libc direct-include contract violated in "
            f"{len(failures)} file(s); add the missing #include(s). A libc call "
            "satisfied only via a platform-conditional transitive include "
            "(e.g. malloc via threading.h's POSIX-only <stdlib.h>) compiles on "
            "glibc but is an implicit-declaration hard error on MSYS2 "
            "(Windows) - run 37035393223."
        )
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
        "Using vendored hsdaoh from ' + hsdaoh_vendored_prefix",
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


def check_built_gui_links_vendored_hsdaoh(repo_root: Path, gui_path: Optional[Path] = None) -> int:
    """Runtime check: assert the built misrc_gui links the vendored hsdaoh from
    .deps/install, not a stale system libhsdaoh. Platform-aware:
      - Linux: ldd (dynamic, must resolve to .deps/install)
      - macOS: otool -L (dynamic, must resolve to @rpath/.deps/install)
      - Windows (MSYS2/MinGW static build): no runtime hsdaoh dylib expected —
        assert objdump -p shows no libhsdaoh DLL NEEDED (it's statically linked).
    In pre-build (preflight) mode with no gui_path and no default build/misrc_gui,
    this skips (returns 0) so preflight still passes; CI runs it again post-build
    against the real $BUILD_DIR/misrc_gui via --gui-path.
    """
    gui = gui_path if gui_path is not None else (repo_root / "misrc_tools" / "build" / "misrc_gui")
    if not gui.exists():
        return 0  # no local build present; preflight or no-build context

    if sys.platform == "darwin":
        try:
            res = run_checked(["otool", "-L", str(gui)])
        except subprocess.CalledProcessError as exc:
            return fail(f"otool -L misrc_gui failed (rc={exc.returncode}): {(exc.stderr or '').strip()}")
        found_hsdaoh = False
        for line in res.stdout.splitlines():
            if "libhsdaoh" in line:
                found_hsdaoh = True
                stripped = line.strip()
                # Portable macOS bundles use @rpath/libhsdaoh... resolved from
                # the app Frameworks dir; a bare /usr/local/lib or /opt/homebrew
                # path means a stale system lib got linked.
                if "/usr/local/lib/" in line or "/opt/homebrew/" in line or "/usr/lib/" in line:
                    return fail(f"misrc_gui links a SYSTEM hsdaoh on macOS (expected @rpath/.deps/install): {stripped}")
        if not found_hsdaoh:
            return fail("misrc_gui does not link libhsdaoh at all (macOS otool -L)")
        return 0

    if os.name == "nt" or sys.platform.startswith("win"):
        # Windows MSYS2/MinGW build statically links hsdaoh (.deps/install/lib/libhsdaoh.a).
        # A correctly-built misrc_gui.exe must NOT have a libhsdaoh DLL NEEDED entry.
        objdump = shutil.which("objdump")
        if not objdump:
            return 0  # objdump unavailable (non-MSYS2 python); skip on Windows
        res = subprocess.run([objdump, "-p", str(gui)], capture_output=True, text=True)
        if res.returncode != 0:
            return fail(f"objdump -p misrc_gui failed: {res.stderr.strip()}")
        for line in res.stdout.splitlines():
            if "DLL Name:" in line and "hsdaoh" in line.lower():
                return fail(f"misrc_gui.exe has a runtime libhsdaoh DLL dependency (expected static link to vendored .deps/install/lib/libhsdaoh.a): {line.strip()}")
        return 0  # static: no hsdaoh DLL NEEDED -> correct

    # Linux (and other ELF platforms): ldd
    try:
        res = run_checked(["ldd", str(gui)])
    except subprocess.CalledProcessError as exc:
        return fail(f"ldd misrc_gui failed (rc={exc.returncode}): {(exc.stderr or '').strip()} — not a dynamic ELF binary?")
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


def check_meson_fx3_policy(meson_path: Path) -> int:
    """Ensure meson.build builds the vendored cyusb compatibility shim from
    source (third_party/cyusb/cyusb.c) on all platforms and sets ENABLE_FX3=1
    when libusb-1.0 is present. This makes FX3 native on every platform with no
    external libcyusb package and no system-lib shadowing possible."""
    meson_text = read_text(meson_path)
    required_snippets = [
        "third_party/cyusb/cyusb.c",
        "static_library('cyusb_compat'",
        "declare_dependency",
        "-DENABLE_FX3=1",
        "fx3_enabled = true",
    ]
    for snippet in required_snippets:
        if snippet not in meson_text:
            return fail(f"meson.build is missing FX3 native-build policy snippet: {snippet}")
    forbidden_snippets = [
        "dependency('libcyusb', required : false)",
    ]
    for snippet in forbidden_snippets:
        if snippet in meson_text:
            return fail(f"meson.build still contains bare external libcyusb pkg-config lookup (no vendored shim): {snippet}")
    return 0


def check_built_gui_has_fx3_symbols(repo_root: Path, gui_path: Optional[Path] = None) -> int:
    """Runtime check: the built misrc_gui must have FX3 compiled in. Catches a
    silent FX3-disable where libusb was missing and FX3 compiled out, which would
    ship a binary without FX3 support and nobody would know. Uses nm/strings."""
    gui = gui_path if gui_path is not None else (repo_root / "misrc_tools" / "build" / "misrc_gui")
    if not gui.exists():
        return 0  # no local build present; preflight skips
    nm = shutil.which("nm")
    if nm:
        res = subprocess.run([nm, str(gui)], capture_output=True, text=True)
        if res.returncode == 0 and "gui_fx3_start" in res.stdout:
            return 0
    # Fall back to strings (works on stripped binaries + Windows .exe)
    strings = shutil.which("strings")
    if not strings:
        return fail("neither nm nor strings available to verify FX3 symbols in misrc_gui")
    res = subprocess.run([strings, str(gui)], capture_output=True, text=True)
    if res.returncode != 0:
        return fail(f"strings misrc_gui failed: {res.stderr.strip()}")
    # gui_fx3_* log strings are present when gui_fx3.c is compiled in.
    if "[FX3]" not in res.stdout or "fx3usbadc start command sent" not in res.stdout:
        return fail("misrc_gui has no FX3 symbols/strings — FX3 did not compile in (libusb missing or ENABLE_FX3 not set)")
    return 0


APPIMAGE_ASSET_DIR = Path("assets/appimage")
APPIMAGE_DESKTOP_ID = "misrc_gui"
# X11 WM_CLASS class the GUI window is created with; the launcher's
# StartupWMClass must equal it and must NOT contain the version.
APPIMAGE_WM_CLASS = "MISRC Capture"


def read_desktop_key(text: str, key: str) -> Optional[str]:
    for line in text.splitlines():
        if line.startswith(f"{key}="):
            return line.split("=", 1)[1]
    return None


def check_linux_desktop_metadata(repo_root: Path, workflow_path: Path) -> int:
    """The AppDir desktop entry and AppRun are real repo files shared by the CI
    workflow and scripts/build-appimage-local.sh (inline heredocs in YAML broke
    before). The desktop entry carries a version-independent StartupWMClass."""
    workflow_text = read_text(workflow_path)
    required_workflow = [
        "install -m 0755 assets/appimage/AppRun AppDir/AppRun",
        "install -m 0644 assets/appimage/misrc_gui.desktop AppDir/misrc_gui.desktop",
        "-d AppDir/misrc_gui.desktop",
        "ln -sf misrc.png AppDir/.DirIcon",
    ]
    for snippet in required_workflow:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing Linux desktop integration snippet: {snippet}")
    forbidden_workflow = [
        "cat > AppDir/AppRun",
        "cat > AppDir/misrc.desktop",
        "MISRC Capture ${BUILD_VERSION}",
    ]
    for snippet in forbidden_workflow:
        if snippet in workflow_text:
            return fail(
                "Workflow still contains a forbidden inline/versioned AppImage desktop snippet "
                f"(use assets/appimage/* and a version-independent StartupWMClass): {snippet}"
            )

    local_script = read_text(repo_root / "scripts" / "build-appimage-local.sh")
    for snippet in ('assets/appimage/AppRun', 'assets/appimage/misrc_gui.desktop'):
        if snippet not in local_script:
            return fail(f"scripts/build-appimage-local.sh must use the shared file: {snippet}")
    for snippet in ('cat > "$appdir/AppRun"', 'cat > "$appdir/misrc.desktop"'):
        if snippet in local_script:
            return fail(f"scripts/build-appimage-local.sh still has an inline copy: {snippet}")

    desktop_path = repo_root / APPIMAGE_ASSET_DIR / "misrc_gui.desktop"
    if not desktop_path.exists():
        return fail(f"Missing AppDir desktop entry: {desktop_path}")
    desktop_text = read_text(desktop_path)
    expected = {
        "Type": "Application",
        "Exec": "misrc_gui",
        "Icon": "misrc",
        "Terminal": "false",
        "StartupNotify": "true",
        "StartupWMClass": APPIMAGE_WM_CLASS,
    }
    for key, value in expected.items():
        actual = read_desktop_key(desktop_text, key)
        if actual != value:
            return fail(f"{desktop_path.name}: expected {key}={value!r}, found {actual!r}")
    if not (repo_root / APPIMAGE_ASSET_DIR / "AppRun").exists():
        return fail("Missing assets/appimage/AppRun")
    return 0


def check_window_identity_contract(repo_root: Path) -> int:
    """GLFW (raylib) builds X11 WM_CLASS from the window title at creation, so
    the window must be created with a fixed title (the versioned title changed
    the class every release and left StartupWMClass stale). The fixed title,
    the launcher StartupWMClass and AppRun's launcher text must all agree."""
    gui_c = read_text(repo_root / "misrc_tools/misrc_gui/core/misrc_gui.c")
    define = f'#define MISRC_WINDOW_CLASS_TITLE "{APPIMAGE_WM_CLASS}"'
    required = [
        define,
        f'setenv("RESOURCE_NAME", "{APPIMAGE_DESKTOP_ID}", 0);',
        "InitWindow(default_window_width, default_window_height, MISRC_WINDOW_CLASS_TITLE);",
        "SetWindowTitle(window_title);",
    ]
    for snippet in required:
        if snippet not in gui_c:
            return fail(f"misrc_gui.c is missing window identity snippet: {snippet}")
    if "InitWindow(default_window_width, default_window_height, window_title)" in gui_c:
        return fail("misrc_gui.c must not create the window with the versioned title (unstable WM_CLASS)")
    init_pos = gui_c.find("InitWindow(default_window_width")
    title_pos = gui_c.find("SetWindowTitle(window_title);")
    if init_pos < 0 or title_pos < init_pos:
        return fail("SetWindowTitle(window_title) must come after InitWindow()")

    apprun = read_text(repo_root / APPIMAGE_ASSET_DIR / "AppRun")
    if f'export RESOURCE_NAME="{APPIMAGE_DESKTOP_ID}"' not in apprun:
        return fail(f'AppRun must export RESOURCE_NAME="{APPIMAGE_DESKTOP_ID}"')
    if f"StartupWMClass={APPIMAGE_WM_CLASS}" not in apprun:
        return fail(f"AppRun launcher text must use StartupWMClass={APPIMAGE_WM_CLASS}")
    if f'launcher="${{data_home}}/applications/{APPIMAGE_DESKTOP_ID}.desktop"' not in apprun:
        return fail(f"AppRun must install the launcher as {APPIMAGE_DESKTOP_ID}.desktop")
    return 0


def check_macos_layout_policy(gui_ui_c_path: Path) -> int:
    source = read_text(gui_ui_c_path)
    width_body = extract_function_body(source, "static int gui_ui_get_base_layout_width(void)")
    height_body = extract_function_body(source, "static int gui_ui_get_base_layout_height(void)")

    if "#if defined(__APPLE__)" not in width_body:
        return fail("gui_ui_get_base_layout_width() is missing __APPLE__ guard")
    if "#if defined(__APPLE__)" not in height_body:
        return fail("gui_ui_get_base_layout_height() is missing __APPLE__ guard")
    if "GetScreenWidth();" not in width_body:
        return fail("gui_ui_get_base_layout_width() must use GetScreenWidth() on macOS")
    if "GetScreenHeight();" not in height_body:
        return fail("gui_ui_get_base_layout_height() must use GetScreenHeight() on macOS")
    if "GetRenderWidth();" not in width_body:
        return fail("gui_ui_get_base_layout_width() must use GetRenderWidth() for non-macOS")
    if "GetRenderHeight();" not in height_body:
        return fail("gui_ui_get_base_layout_height() must use GetRenderHeight() for non-macOS")
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


def check_dev_version_naming(repo_root: Path, meson_path: Path, workflow_path: Path) -> int:
    """Dev/untagged builds MUST use a date-stamped version (dev-YYYY-MM-DD-<sha>)
    derived by misrc_tools/git-version.sh, not a hardcoded "vX.Y.Z-dev" literal.
    Regression: the repo carried a hardcoded vN.N.N-dev literal in git-version.sh
    + 4 CI fallbacks and another in the VERSION file while the current release
    had advanced past it, so dev builds reported a version behind the last
    release. This guard forbids stale vX.Y.Z-dev literals across the
    version-resolution path and requires git-version.sh to derive the dev string
    from the current UTC date.
    ci_guard_tests.py check_dev_version_naming."""
    stale_re = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+-dev")
    targets = [
        meson_path,
        workflow_path,
        repo_root / "misrc_tools" / "git-version.sh",
        repo_root / "misrc_tools" / "ci-resolve-version.sh",
        repo_root / "android" / "build-apk.sh",
        repo_root / "scripts" / "build-appimage-local.sh",
        repo_root / "VERSION",
    ]
    for path in targets:
        if not path.exists():
            continue
        m = stale_re.search(read_text(path))
        if m:
            rel = path.relative_to(repo_root)
            return fail(
                f"{rel} contains a stale hardcoded dev-version literal ({m.group(0)}). "
                "Dev versions must be date-stamped (dev-YYYY-MM-DD-<sha>) via "
                "misrc_tools/git-version.sh, not a vX.Y.Z-dev string that goes "
                "stale as releases advance."
            )
    gv = repo_root / "misrc_tools" / "git-version.sh"
    if gv.exists():
        gv_text = read_text(gv)
        if "date -u" not in gv_text or "dev-" not in gv_text:
            return fail(
                "misrc_tools/git-version.sh must derive the untagged dev version "
                "from `date -u` with a `dev-` prefix (date-stamped scheme)."
            )
        if "--ignore-cr-at-eol" not in gv_text:
            return fail(
                "misrc_tools/git-version.sh dirty check must use --ignore-cr-at-eol "
                "so a fresh Windows checkout (CRLF) does not phantom-tag -dirty "
                "under MSYS2 git (autocrlf=false). It is a no-op on Linux/macOS."
            )
    return 0


def check_no_tracked_generated_dirty_sources(repo_root: Path) -> int:
    """Generated, machine-specific files that `git-version.sh`'s dirty check
    would see MUST be gitignored (untracked), not committed. Regression:
    android/deps-versions.txt was tracked and overwritten by
    build-deps-android.sh with a build timestamp + host NDK path, so every
    Android CI build dirty-tagged the version. Same class as the gitignored
    android/aarch64-linux-android.ini cross-file. Asserts both stay untracked."""
    targets = [
        repo_root / "android" / "deps-versions.txt",
        repo_root / "android" / "aarch64-linux-android.ini",
    ]
    for path in targets:
        if not path.exists():
            continue
        rel = path.relative_to(repo_root)
        tracked = subprocess.run(
            ["git", "ls-files", "--error-unmatch", str(path)],
            cwd=repo_root, capture_output=True, text=True,
        )
        if tracked.returncode == 0:
            return fail(
                f"{rel} is tracked but is a generated, machine-specific file. "
                "It dirty-tags the version string on any host/CI that regenerates "
                "it. Add it to .gitignore and `git rm --cached` it."
            )
        ign = subprocess.run(
            ["git", "check-ignore", str(path)],
            cwd=repo_root, capture_output=True, text=True,
        )
        if ign.returncode != 0:
            return fail(
                f"{rel} is untracked but NOT gitignored; add it to .gitignore so "
                "regenerating it cannot dirty the tree."
            )
    return 0


# Optional-dependency feature macros defined by misrc_tools/meson.build. A
# struct member declared inside an enabled #if <MACRO> branch must not be
# referenced (->name / .name) outside that branch, or the build breaks when
# the dependency is absent. Regression 64b2171: gui_demod.c declared
# soxr_in_buf/soxr_out_buf inside #if LIBSOXR_ENABLED but referenced them
# unguarded; Windows arm64 (no mingw-w64-clang-aarch64-libsoxr =>
# LIBSOXR_ENABLED=0) failed to compile with "no member named 'soxr_in_buf'".
# Regression e34dce9: gui_playback.c's channel-preparation helpers sat
# outside #if LIBFLAC_ENABLED while touching guarded members (decoder_a/b,
# io_a/b) and calling functions defined inside the guard. Member
# declarations are resolved across the source AND the repo headers, so
# members declared unguarded in headers (playback_file_info_t.duration_seconds
# in gui_playback.h) are not false-flagged.
_OPTIONAL_DEP_MACROS = (
    "LIBSOXR_ENABLED",
    "LIBFLAC_ENABLED",
    "LIBFFTW_ENABLED",
    "LIBASOUND_ENABLED",
    "ENABLE_FX3",
    "ENABLE_DDD",
    "ENABLE_RTLSDR",
)

_C_TYPE_KEYWORDS = {
    "const", "static", "extern", "volatile", "register", "auto",
    "unsigned", "signed", "short", "long", "int", "char", "float",
    "double", "void", "bool", "struct", "union", "enum",
    "size_t", "ssize_t", "ptrdiff_t", "wchar_t",
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
}


def _make_c_stripper():
    """Return a stateful function that strips C comments and string/char
    literals from successive lines, replacing their contents with empty
    strings so braces/parens inside them do not affect structural parsing."""
    state = {"in_block_comment": False}

    def strip(line: str) -> str:
        out: List[str] = []
        i = 0
        n = len(line)
        while i < n:
            ch = line[i]
            nxt = line[i + 1] if i + 1 < n else ""
            if state["in_block_comment"]:
                if ch == "*" and nxt == "/":
                    state["in_block_comment"] = False
                    i += 2
                    continue
                i += 1
                continue
            if ch == "/" and nxt == "/":
                break  # line comment to end of line
            if ch == "/" and nxt == "*":
                state["in_block_comment"] = True
                i += 2
                continue
            if ch == '"':
                out.append('""')
                i += 1
                while i < n and line[i] != '"':
                    if line[i] == "\\" and i + 1 < n:
                        i += 2
                        continue
                    i += 1
                i += 1  # skip closing quote
                continue
            if ch == "'":
                out.append("''")
                i += 1
                while i < n and line[i] != "'":
                    if line[i] == "\\" and i + 1 < n:
                        i += 2
                        continue
                    i += 1
                i += 1
                continue
            out.append(ch)
            i += 1
        return "".join(out)

    return strip


def _scan_struct_member_source(
    source: str,
    macro: str,
    members: Optional[Dict[str, Dict[str, bool]]],
    refs: Optional[List[Tuple[int, str, str, bool]]],
) -> None:
    """One comment/string-stripped pass over a C source. When `members` is
    given, record per-region struct member DECLARATIONS (a name declared
    inside an enabled #if <macro> branch / inside a #else branch of that
    chain / outside it). When `refs` is given, record member-style references
    (->name / .name) with their region/enclosure state. Both may be given
    (declarations + references from the same source in one pass)."""
    strip = _make_c_stripper()
    lines = [strip(l) for l in source.splitlines()]

    # pp_stack entries: (macro_or_None, in_else, chain_mentions_macro)
    pp_stack: List[Tuple[str, bool, bool]] = []
    in_struct = False
    struct_depth = 0
    struct_open_re = re.compile(r"(?:typedef\s+)?struct\s+\w*\s*\{")
    member_re = re.compile(r"\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;")
    ref_re = re.compile(r"(?:->|\.)\s*([A-Za-z_]\w*)")
    macro_token_re = re.compile(rf"\b{re.escape(macro)}\b")

    for idx, line in enumerate(lines, start=1):
        stripped = line.strip()
        if stripped.startswith("#"):
            m = re.match(r"#\s*(\w+)(.*)", stripped)
            if m:
                directive, rest = m.group(1), m.group(2).strip()
                mentions = bool(macro_token_re.search(rest))
                if directive in ("if", "ifdef"):
                    negated = bool(re.search(rf"!\s*defined\s*\(\s*{re.escape(macro)}\s*\)", rest)) \
                        or bool(re.search(rf"!\s*{re.escape(macro)}\b", rest))
                    positive = mentions and not negated
                    pp_stack.append((macro if positive else None, False, mentions))
                elif directive == "ifndef":
                    # Block runs when <macro> is UNDEFINED; the chain mentions it.
                    pp_stack.append((None, False, mentions))
                elif directive == "elif":
                    if pp_stack:
                        top_macro, _, top_mentions = pp_stack[-1]
                        pp_stack[-1] = (top_macro, True, top_mentions or mentions)
                elif directive == "else":
                    if pp_stack:
                        top_macro, _, top_mentions = pp_stack[-1]
                        pp_stack[-1] = (top_macro, True, top_mentions)
                elif directive == "endif":
                    if pp_stack:
                        pp_stack.pop()
            continue

        # Region of this line relative to <macro>, and whether any enclosing
        # conditional chain mentions <macro> at all.
        region = "outside"
        enclosed = False
        for m_macro, m_else, m_mentions in pp_stack:
            if m_mentions:
                enclosed = True
            if m_macro == macro:
                region = "disabled" if m_else else "enabled"

        if in_struct:
            for ch in line:
                if ch == "{":
                    struct_depth += 1
                elif ch == "}":
                    struct_depth -= 1
            if struct_depth <= 0:
                in_struct = False
                continue
            if members is not None:
                for m in member_re.finditer(line):
                    name = m.group(1)
                    if name in _C_TYPE_KEYWORDS:
                        continue
                    rec = members.setdefault(name, {"enabled": False, "disabled": False, "outside": False})
                    rec[region] = True
            continue

        if struct_open_re.search(line):
            in_struct = True
            struct_depth = 0
            for ch in line:
                if ch == "{":
                    struct_depth += 1
                elif ch == "}":
                    struct_depth -= 1
            if struct_depth <= 0:
                in_struct = False
            continue

        # Outside any struct body: collect member-style references.
        if refs is not None:
            for m in ref_re.finditer(line):
                refs.append((idx, m.group(1), region, enclosed))


def _struct_member_decl_baseline(header_texts: List[str], macro: str) -> Dict[str, Dict[str, bool]]:
    """Member-name declaration records collected from the repo's headers.

    Struct types legitimately live in headers regardless of whether the
    header text mentions <macro>, so the baseline scans EVERY header: a
    member name declared by any header struct outside an enabled
    #if <macro> branch is NOT guard-only. False-positive class this fixes
    (2026-10-19): gui_playback.c declares duration_seconds inside
    #if LIBFLAC_ENABLED (playback_vorbis_rf_tags_t) while
    playback_file_info_t in gui_playback.h also declares it unguarded, so
    file-local-only analysis false-flagged a plain info->duration_seconds
    reference outside the guard."""
    members: Dict[str, Dict[str, bool]] = {}
    for text in header_texts:
        _scan_struct_member_source(text, macro, members, None)
    return members


def _find_unguarded_struct_member_refs(
    source: str,
    macro: str,
    preseed_members: Optional[Dict[str, Dict[str, bool]]] = None,
) -> List[Tuple[int, str]]:
    """Return [(lineno, member_name), ...] for struct member references
    (->name / .name) that occur in plain code NOT enclosed by any conditional
    chain that mentions <macro>, while the member is declared only inside an
    enabled #if <macro> branch (not outside it, not in a #else branch) across
    the source file AND the repo headers (preseed_members: struct types
    legitimately live in headers, so a member declared unguarded in any
    header is not guard-only). Those compile-fail when <macro> is undefined.

    Limitation: to avoid false positives on the legitimate #if/#elif !MACRO/
    #else idiom (where the final #else is only reached when MACRO is defined),
    a reference is NOT flagged if any enclosing #if/#elif/#else chain mentions
    <macro> at all. This means references inside a negated guard (#if !MACRO)
    are not caught — that rarer pattern would need a real preprocessor to
    evaluate precisely. The guard still catches the common regression where a
    guarded-only member is referenced in plain code with no macro conditional
    around it (regression 64b2171 in gui_demod.c; regression e34dce9 in
    gui_playback.c: decoder_a/b + io_a/b referenced by unguarded
    channel-preparation helpers)."""
    # member name -> per-region declared flags, pre-seeded from the headers
    members: Dict[str, Dict[str, bool]] = {}
    if preseed_members:
        for name, rec in preseed_members.items():
            members[name] = dict(rec)
    # (lineno, name, region, enclosed_by_macro_chain)
    refs: List[Tuple[int, str, str, bool]] = []
    _scan_struct_member_source(source, macro, members, refs)

    guarded_only = {
        name for name, rec in members.items()
        if rec["enabled"] and not rec["outside"] and not rec["disabled"]
    }
    return [(lineno, name) for lineno, name, region, enclosed in refs
            if name in guarded_only and region != "enabled" and not enclosed]


def check_windows_gui_link_no_dll_import_libs(meson_path: Path) -> int:
    """Forbid -l:lib<name>.dll.a DLL import-library references in meson.build.
    Windows builds are static (-static -static-libgcc); such import libs are
    only shipped by the matching MSYS2 mingw package, which the CI install
    list does not necessarily include. Regression 4cb9ced added
    -l:libglfw3.dll.a, breaking the Windows x86_64 link because raylib 5.5
    bundles GLFW into libraylib.a (no glfw DLL import lib is installed)."""
    meson_text = read_text(meson_path)
    # Strip meson line comments so the explanatory comment that names the
    # forbidden flag does not trip the check.
    code = "\n".join(line.split("#", 1)[0] for line in meson_text.splitlines())
    m = re.search(r"-l:lib\w+\.dll\.a", code)
    if m:
        return fail(
            "meson.build Windows GUI link flags reference a DLL import library ("
            + m.group(0) + "). Windows builds are static and the CI MSYS2 install "
            "list does not ship every mingw DLL import lib (regression 4cb9ced: "
            "-l:libglfw3.dll.a broke the link; raylib 5.5 bundles GLFW into "
            "libraylib.a). Link the static archive / system lib name instead."
        )
    return 0


def check_optional_dep_guard_consistency(repo_root: Path) -> int:
    """For each optional-dependency feature macro, scan the C sources for
    struct members declared inside an enabled #if <MACRO> branch that are
    referenced (->name / .name) outside the branch. Those fail to compile
    when the dependency is absent. Regression 64b2171: gui_demod.c soxr
    scratch buffers were #if LIBSOXR_ENABLED-guarded but referenced unguarded,
    breaking the Windows arm64 build (no libsoxr => LIBSOXR_ENABLED=0).
    Regression e34dce9: gui_playback.c channel-preparation helpers sat
    outside #if LIBFLAC_ENABLED while touching guarded members (decoder_a/b,
    io_a/b) and calling FLAC/reader functions defined inside the guard.
    Member declarations are resolved across the source file AND the repo
    headers, so members declared unguarded in headers
    (playback_file_info_t.duration_seconds in gui_playback.h) are not
    false-flagged."""
    tools_dir = repo_root / "misrc_tools"
    sources = [(p, read_text(p)) for p in sorted(tools_dir.rglob("*.c"))]
    header_texts = [read_text(p) for p in sorted(tools_dir.rglob("*.h"))]
    any_problem = False
    for macro in _OPTIONAL_DEP_MACROS:
        # Per-macro header declaration baseline (lazy: only built when some
        # source mentions the macro).
        header_baseline: Optional[Dict[str, Dict[str, bool]]] = None
        for src_path, text in sources:
            if macro not in text:
                continue
            if header_baseline is None:
                header_baseline = _struct_member_decl_baseline(header_texts, macro)
            for lineno, name in _find_unguarded_struct_member_refs(
                text, macro, preseed_members=header_baseline
            ):
                any_problem = True
                rel = src_path.relative_to(repo_root)
                print(
                    f"ERROR: {rel}:{lineno}: struct member '{name}' is declared "
                    f"inside #if {macro} but referenced outside it; the build "
                    f"breaks when {macro} is undefined (a platform without the "
                    f"dependency). Move the member outside the guard or guard the "
                    f"reference.",
                    file=sys.stderr,
                )
    return 1 if any_problem else 0


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
        "file_getenv_utf8(\"APPDATA\")",
        "file_getenv_utf8(\"LOCALAPPDATA\")",
        "file_getenv_utf8(\"XDG_CONFIG_HOME\")",
        "file_mkdir_utf8(path, 0700)",
    ]
    for snippet in required_snippets:
        if snippet not in source:
            return fail(f"Missing settings persistence contract snippet in gui_settings.c: {snippet}")

    # Directory creation and file I/O now live in the shared UTF-8 helpers.
    # Retain the platform checks here as well as the settings call-site checks.
    file_utils = read_text(gui_settings_c_path.parents[2] / "common/file_utils.c")
    for snippet in (
        "GetEnvironmentVariableW(wide_name,",
        "getenv(name)",
        "_wmkdir(wide)",
        "mkdir(path, (mode_t)mode)",
        "_wfopen(wide, wide_mode)",
        "fopen(filename, mode)",
    ):
        if snippet not in file_utils:
            return fail(f"Missing settings filesystem contract snippet in file_utils.c: {snippet}")

    save_pos = source.find("void gui_settings_save(")
    if save_pos < 0:
        return fail("Could not locate gui_settings_save() in gui_settings.c")
    ensure_pos = source.find("gui_settings_ensure_parent_dirs(path)", save_pos)
    fopen_pos = source.find("FILE *f = file_open_utf8(path, \"w\");", save_pos)
    if ensure_pos < 0 or fopen_pos < 0:
        return fail("Missing parent-dir ensure or file_open_utf8 call in gui_settings_save()")
    if ensure_pos > fopen_pos:
        return fail("gui_settings_save() must ensure parent directories before file_open_utf8")
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


def check_apprun_static_contract(repo_root: Path) -> int:
    apprun_path = repo_root / APPIMAGE_ASSET_DIR / "AppRun"
    if not apprun_path.exists():
        return fail(f"Missing AppRun: {apprun_path}")
    apprun = read_text(apprun_path)
    # Explanatory comments legitimately mention things the code must not do
    # (e.g. "no set -e"), so scan code lines only.
    code = "\n".join(line for line in apprun.splitlines()
                     if not line.lstrip().startswith("#"))
    required_snippets = [
        "misrc_gui_integrate",
        "--create-shortcut",
        "MISRC_GUI_NO_INTEGRATION",
        "TryExec=${APPIMAGE}",
        "Icon=misrc",
        "StartupNotify=true",
        "update-desktop-database",
        "gtk-update-icon-cache",
    ]
    for snippet in required_snippets:
        if snippet not in code:
            return fail(f"AppRun shortcut contract is missing snippet: {snippet}")
    forbidden_snippets = {
        "set -e": "desktop integration must never be able to block launching the app",
        ".local/bin": "no ~/.local/bin AppImage symlink side effect",
        "%U": "the GUI takes no file arguments; a dropped file would route into CLI capture mode",
        "X-GNOME-WMClass": "legacy key; StartupWMClass is the contract",
        "--version": "launcher identity is version-independent; do not run the binary to build it",
    }
    for snippet, why in forbidden_snippets.items():
        if snippet in code:
            return fail(f"AppRun contains forbidden snippet {snippet!r}: {why}")

    # The Desktop icon is user-requested only. Earlier builds copied one to
    # ~/Desktop on every launch, so it reappeared after being deleted.
    func_start = apprun.find("misrc_gui_create_desktop_icon() {")
    if func_start < 0:
        return fail("AppRun is missing misrc_gui_create_desktop_icon()")
    func_end = apprun.find("\n}\n", func_start)
    func_body = apprun[func_start:func_end]
    outside = apprun[:func_start] + apprun[func_end:]
    stray = [line for line in outside.splitlines()
             if "MISRC GUI.desktop" in line and not line.lstrip().startswith("#")]
    if stray:
        return fail("AppRun writes 'MISRC GUI.desktop' outside misrc_gui_create_desktop_icon()")
    if "MISRC GUI.desktop" not in func_body:
        return fail("misrc_gui_create_desktop_icon() must be the only writer of the Desktop icon")
    calls = [i for i in range(len(apprun)) if apprun.startswith("misrc_gui_create_desktop_icon", i)]
    call_positions = [i for i in calls if i != func_start]
    if len(call_positions) != 1:
        return fail("misrc_gui_create_desktop_icon must be called exactly once (inside --create-shortcut)")
    branch_start = apprun.find('if [ "${1:-}" = "--create-shortcut" ]; then')
    branch_end = apprun.find("\nfi\n", branch_start)
    if branch_start < 0 or not (branch_start < call_positions[0] < branch_end):
        return fail("The Desktop icon may only be created inside the explicit --create-shortcut branch")
    return 0


def _apprun_sandbox(root: Path, apprun_src: Path, icon_path: Path) -> Tuple[Path, Path]:
    """AppDir with stub binaries that log their invocation. Returns (AppRun, calls log)."""
    appdir = root / "AppDir"
    (appdir / "usr/bin").mkdir(parents=True, exist_ok=True)
    calls = root / "calls.log"
    apprun = appdir / "AppRun"
    apprun.write_text(read_text(apprun_src), encoding="utf-8")
    apprun.chmod(apprun.stat().st_mode | stat.S_IXUSR)
    for exe in ("misrc_gui", "misrc_capture", "misrc_extract"):
        exe_path = appdir / "usr/bin" / exe
        exe_path.write_text(
            "#!/usr/bin/env bash\n"
            "printf '%s RESOURCE_NAME=%s args=[%s]\\n' \"$(basename \"$0\")\" "
            "\"${RESOURCE_NAME:-}\" \"$*\" >> \"${MISRC_TEST_CALLS}\"\n"
            "exit 0\n",
            encoding="utf-8",
        )
        exe_path.chmod(exe_path.stat().st_mode | stat.S_IXUSR)
    icon_dir = appdir / "usr/share/icons/hicolor/512x512/apps"
    icon_dir.mkdir(parents=True, exist_ok=True)
    if icon_path.exists():
        shutil.copy2(icon_path, icon_dir / "misrc.png")
    else:
        (icon_dir / "misrc.png").write_bytes(b"\x89PNG\r\n\x1a\n")
    return apprun, calls


def _apprun_home(root: Path, name: str) -> Path:
    """Fresh HOME with a Desktop folder and a deterministic user-dirs config."""
    home = root / name
    (home / "Desktop").mkdir(parents=True, exist_ok=True)
    (home / ".config").mkdir(parents=True, exist_ok=True)
    (home / ".config/user-dirs.dirs").write_text(
        'XDG_DESKTOP_DIR="$HOME/Desktop"\n', encoding="utf-8")
    return home


def _apprun_run(apprun: Path, calls: Path, home: Path, args: List[str],
                appimage: Optional[Path] = None,
                extra_env: Optional[Dict[str, str]] = None) -> subprocess.CompletedProcess:
    # Environment is built from scratch so nothing can leak in from the real
    # session (XDG_*, APPIMAGE) and xdg-user-dir can never resolve the real Desktop.
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(home),
           "LANG": "C", "MISRC_TEST_CALLS": str(calls)}
    if appimage is not None:
        env["APPIMAGE"] = str(appimage)
    env.update(extra_env or {})
    return subprocess.run(["bash", str(apprun), *args], env=env,
                          capture_output=True, text=True)


def _home_files(home: Path) -> List[str]:
    baseline = {".config/user-dirs.dirs"}
    # By-products of update-desktop-database / gtk-update-icon-cache when those
    # tools exist on the host; benign and not something AppRun writes itself.
    tool_cache_names = {"mimeinfo.cache", "icon-theme.cache"}
    found = []
    for path in sorted(home.rglob("*")):
        if path.is_file() or path.is_symlink():
            rel = path.relative_to(home).as_posix()
            if rel not in baseline and path.name not in tool_cache_names:
                found.append(rel)
    return found


def check_apprun_runtime_behavior(repo_root: Path, icon_path: Path) -> int:
    if not sys.platform.startswith("linux"):
        print("SKIP: AppRun runtime behavior (linux-only)")
        return 0
    if shutil.which("bash") is None:
        print("SKIP: AppRun runtime behavior (bash not available)")
        return 0

    apprun_src = repo_root / APPIMAGE_ASSET_DIR / "AppRun"
    with tempfile.TemporaryDirectory(prefix="misrc_ci_guard_") as temp_root:
        root = Path(temp_root)
        apprun, calls = _apprun_sandbox(root, apprun_src, icon_path)
        try:
            run_checked(["bash", "-n", str(apprun)])
        except subprocess.CalledProcessError as exc:
            return fail(f"AppRun has a shell syntax error: {exc.stderr}")

        # A path with a space and a literal '%' exercises .desktop Exec escaping.
        appimage = root / "MISRC 100% Test Build.AppImage"
        appimage.write_text("fake", encoding="utf-8")
        appimage.chmod(appimage.stat().st_mode | stat.S_IXUSR)

        # 1. Plain GUI launch installs the launcher + icon and nothing else.
        home = _apprun_home(root, "home_plain")
        res = _apprun_run(apprun, calls, home, [], appimage)
        if res.returncode != 0:
            return fail(f"AppRun plain launch failed (rc={res.returncode}): {res.stderr}")
        launcher = home / ".local/share/applications/misrc_gui.desktop"
        icon = home / ".local/share/icons/hicolor/512x512/apps/misrc.png"
        if not launcher.exists() or not icon.exists():
            return fail("AppRun plain launch did not install the launcher and icon")
        text = read_text(launcher)
        for required in (f'Exec="{str(appimage).replace("%", "%%")}"',
                         f"TryExec={appimage}", "Icon=misrc", "Terminal=false",
                         "StartupNotify=true", f"StartupWMClass={APPIMAGE_WM_CLASS}"):
            if required not in text.splitlines():
                return fail(f"Launcher is missing line: {required}\n{text}")
        if "%U" in text or "X-GNOME-WMClass" in text:
            return fail("Launcher must not contain %U or X-GNOME-WMClass")
        if shutil.which("desktop-file-validate"):
            check = subprocess.run(["desktop-file-validate", str(launcher)],
                                   capture_output=True, text=True)
            if check.returncode != 0:
                return fail(f"Generated launcher failed desktop-file-validate: {check.stdout}{check.stderr}")
        unexpected = [f for f in _home_files(home)
                      if f not in (".local/share/applications/misrc_gui.desktop",
                                   ".local/share/icons/hicolor/512x512/apps/misrc.png")]
        if unexpected:
            return fail(f"AppRun plain launch created unexpected files: {unexpected}")
        if any((home / "Desktop").iterdir()):
            return fail("AppRun plain launch must NEVER create a Desktop icon")
        if "misrc_gui RESOURCE_NAME=misrc_gui args=[]" not in read_text(calls):
            return fail("AppRun must exec misrc_gui with RESOURCE_NAME=misrc_gui exported")

        # 2. Idempotent: a second launch must not rewrite anything.
        before = (launcher.stat().st_mtime_ns, icon.stat().st_mtime_ns)
        res = _apprun_run(apprun, calls, home, [], appimage)
        after = (launcher.stat().st_mtime_ns, icon.stat().st_mtime_ns)
        if res.returncode != 0 or before != after:
            return fail("AppRun rewrote an unchanged launcher/icon on a repeat launch")

        # 3. Re-pointing to a different AppImage rewrites Exec (follows the last-run copy).
        other = root / "MISRC Other.AppImage"
        other.write_text("fake", encoding="utf-8")
        _apprun_run(apprun, calls, home, [], other)
        if f'Exec="{other}"' not in read_text(launcher).splitlines():
            return fail("AppRun did not re-point the launcher to the AppImage that ran last")

        # 4. GUI-only flags integrate too (mirrors misrc_gui.c main()).
        for args in (["--debug-view"], ["--config", "/tmp/x.json"], ["--auto-connect", "--config", "/tmp/x.json"]):
            fresh = _apprun_home(root, "home_flags_" + "_".join(a.strip("-/.") for a in args))
            _apprun_run(apprun, calls, fresh, args, appimage)
            if not (fresh / ".local/share/applications/misrc_gui.desktop").exists():
                return fail(f"AppRun did not integrate for GUI launch args: {args}")

        # 5. Non-GUI launches must not touch the desktop at all.
        zero_touch = [
            (["capture", "-h"], appimage, None, "misrc_capture"),
            (["extract", "-h"], appimage, None, "misrc_extract"),
            (["--smoke-test"], appimage, None, "misrc_gui"),
            (["--version"], appimage, None, "misrc_gui"),
            (["--help"], appimage, None, "misrc_gui"),
            (["-a", "-r", "x"], appimage, None, "misrc_gui"),
            ([], None, None, "misrc_gui"),
            ([], appimage, {"MISRC_GUI_NO_INTEGRATION": "1"}, "misrc_gui"),
            ([], root / "bad$name.AppImage", None, "misrc_gui"),
            ([], root / 'bad"name.AppImage', None, "misrc_gui"),
        ]
        for idx, (args, image, extra, expected_exe) in enumerate(zero_touch):
            if image is not None and not image.exists():
                image.write_text("fake", encoding="utf-8")
            fresh = _apprun_home(root, f"home_zero_{idx}")
            calls.write_text("", encoding="utf-8")
            res = _apprun_run(apprun, calls, fresh, args, image, extra)
            if res.returncode != 0:
                return fail(f"AppRun {args} failed (rc={res.returncode}): {res.stderr}")
            touched = _home_files(fresh)
            if touched or any((fresh / "Desktop").iterdir()):
                return fail(f"AppRun {args} (extra={extra}) must not touch the desktop, but created: {touched}")
            if not read_text(calls).startswith(expected_exe + " "):
                return fail(f"AppRun {args} dispatched to the wrong binary; calls: {read_text(calls)!r}")

        # 6. Explicit --create-shortcut is the only way to get a Desktop icon,
        #    and it does not start the GUI.
        home = _apprun_home(root, "home_shortcut")
        calls.write_text("", encoding="utf-8")
        res = _apprun_run(apprun, calls, home, ["--create-shortcut"], appimage)
        desktop_icon = home / "Desktop/MISRC GUI.desktop"
        if res.returncode != 0 or not desktop_icon.exists():
            return fail(f"--create-shortcut did not create the Desktop icon (rc={res.returncode}): {res.stderr}")
        if read_text(desktop_icon) != read_text(home / ".local/share/applications/misrc_gui.desktop"):
            return fail("Desktop icon must be a copy of the installed launcher")
        if not os.access(desktop_icon, os.X_OK):
            return fail("Desktop icon must be executable so the file manager will launch it")
        if read_text(calls).strip():
            return fail("--create-shortcut must not start the GUI")
        res = _apprun_run(apprun, calls, _apprun_home(root, "home_shortcut_noimage"),
                          ["--create-shortcut"], None)
        if res.returncode == 0:
            return fail("--create-shortcut without an AppImage file must fail instead of pretending")

        # 7. Regression: a deleted Desktop icon must STAY deleted (it used to be
        #    copied back on every launch).
        desktop_icon.unlink()
        for _ in range(2):
            res = _apprun_run(apprun, calls, home, [], appimage)
            if res.returncode != 0:
                return fail(f"AppRun launch after deleting the Desktop icon failed: {res.stderr}")
        if desktop_icon.exists() or any((home / "Desktop").iterdir()):
            return fail("REGRESSION: AppRun re-created the deleted Desktop icon on launch")
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
        "$ZipPath = \"Windows_MISRC_GUI_${{ steps.version.outputs.version }}_x86.zip\"",
        "Compress-Archive -Path @(\"dist/MISRC.exe\")",
        "if ($zip.Entries.Count -ne 1)",
        "$entry.FullName.Contains('/') -or $entry.FullName.Contains('\\')",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing required Windows packaging assertion: {snippet}")
    return 0

def check_android_packaging_assertions(workflow_path: Path) -> int:
    """Assert the android-apk CI job verifies the APK the same way local build-apk.sh does:
    - apksigner verify (signature integrity)
    - aapt2 dump badging sdkVersion:'30' + targetSdkVersion:'34' + package dev.misrc.gui
    - lib/arm64-v8a/libmisrc_gui.so present in the APK
    - libmisrc_gui.so is ARM aarch64 (not x86_64 host leak)
    - no host /usr/lib/x86_64 or /usr/local/lib leaked into the cross .so
    These mirror the local verification in android/build-apk.sh + the manual
    checks performed during the android-support branch development."""
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "bash android/build-deps-android.sh",
        "bash android/gen-cross-file.sh",
        "meson setup --cross-file android/aarch64-linux-android.ini",
        "meson compile -C build-android misrc_gui",
        "test -f build-android/libmisrc_gui.so",
        "bash android/build-apk.sh",
        # The CI job invokes tools via $ANDROID_HOME variable-prefixed paths,
        # so assert on the tool-name substrings rather than bare "apksigner verify".
        "34.0.0/apksigner",
        "verify \"$APK\"",
        "34.0.0/aapt2",
        "dump badging",
        "sdkVersion:'30'",
        "targetSdkVersion:'34'",
        "package: name='dev.misrc.gui'",
        "lib/arm64-v8a/libmisrc_gui.so",
        "ARM aarch64",
        "/usr/lib/x86_64|/usr/local/lib",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing required Android packaging assertion: {snippet}")
    return 0

def check_release_artifact_naming_contract(repo_root: Path, workflow_path: Path) -> int:
    """Assert every release artifact filename follows
    <Platform>_MISRC_GUI_<version>_<arch>.<ext> with the platform name capitalized
    (Linux, Windows, macOS, Android), and that the lowercase v1.1.7 malform
    cannot regress.

    Regression (v1.1.7): Linux + Android release assets shipped as
    linux_MISRC_dev-..._arm64.zip / android_MISRC_dev-..._arm64.apk under tag
    v1.1.7 (lowercase prefix AND dev-named). This guard forbids the lowercase
    platform prefixes on release artifacts so the naming half of the malform is
    caught; check_release_version_resolution_contract covers the dev-name half.
    """
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "workflow_dispatch:",
        "artifact_suffix: x86",
        "artifact_suffix: arm64",
        "APPIMAGE_NAME=\"Linux_MISRC_GUI_${BUILD_VERSION}_${{ matrix.artifact_suffix }}.AppImage\"",
        "ZIP_NAME=\"Linux_MISRC_GUI_${BUILD_VERSION}_${{ matrix.artifact_suffix }}.zip\"",
        "path: Linux_MISRC_GUI_*_${{ matrix.artifact_suffix }}.zip",
        "$ZipPath = \"Windows_MISRC_GUI_${{ steps.version.outputs.version }}_x86.zip\"",
        "path: Windows_MISRC_GUI_*_x86.zip",
        "$ZipPath = \"Windows_MISRC_GUI_${{ steps.version.outputs.version }}_arm64.zip\"",
        "path: Windows_MISRC_GUI_*_arm64.zip",
        "DMG_NAME=\"macOS_MISRC_GUI_${BUILD_VERSION}_universal.dmg\"",
        "path: macOS_MISRC_GUI_*_universal.dmg",
        "release-assets/**/Linux_MISRC_GUI_*_x86.zip",
        "release-assets/**/Linux_MISRC_GUI_*_arm64.zip",
        "release-assets/**/Windows_MISRC_GUI_*_x86.zip",
        "release-assets/**/Windows_MISRC_GUI_*_arm64.zip",
        "release-assets/**/macOS_MISRC_GUI_*_universal.dmg",
        "APK_ARM64=\"Android_MISRC_GUI_${TAG}_arm64.apk\"",
        "release-assets/**/Android_MISRC_GUI_*_arm64.apk",
    ]
    forbidden_snippets = [
        # Legacy pre-convention APK/shapes.
        "misrc_gui-${TAG}-android-arm64.apk",
        "release-assets/**/misrc_gui-*-android-arm64.apk",
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
        "release-assets/**/Linux_MISRC_*_x86.zip",
        "release-assets/**/Linux_MISRC_*_arm64.zip",
        "release-assets/**/Windows_MISRC_*_x86.zip",
        "release-assets/**/Windows_MISRC_*_arm64.zip",
        "release-assets/**/macOS_MISRC_*_universal.dmg",
        "release-assets/**/Android_MISRC_*_arm64.apk",
        # Lowercase platform prefixes on release artifacts (v1.1.7 malform).
        "linux_MISRC_${BUILD_VERSION}",
        "windows_MISRC_${{ steps.version.outputs.version }}",
        "macos_MISRC_${BUILD_VERSION}",
        "android_MISRC_${TAG}",
        "release-assets/**/linux_MISRC_*_x86.zip",
        "release-assets/**/linux_MISRC_*_arm64.zip",
        "release-assets/**/windows_MISRC_*_x86.zip",
        "release-assets/**/windows_MISRC_*_arm64.zip",
        "release-assets/**/macos_MISRC_*_universal.dmg",
        "release-assets/**/android_MISRC_*_arm64.apk",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing required release artifact naming snippet: {snippet}")
    for snippet in forbidden_snippets:
        if snippet in workflow_text:
            return fail(f"Workflow still contains forbidden release artifact naming snippet: {snippet}")
    # The Android APK filename is produced by android/build-apk.sh (not the
    # workflow), so assert the capitalized convention there too.
    apk_script = repo_root / "android" / "build-apk.sh"
    if not apk_script.exists():
        return fail(f"android/build-apk.sh is missing: {apk_script}")
    apk_text = read_text(apk_script)
    if "Android_MISRC_GUI_${VERSION}_arm64.apk" not in apk_text:
        return fail(
            "android/build-apk.sh must name the APK Android_MISRC_GUI_${VERSION}_arm64.apk "
            "(capitalized platform prefix, matching the release convention)."
        )
    if "android_MISRC_${VERSION}_arm64.apk" in apk_text:
        return fail(
            "android/build-apk.sh still uses the lowercase android_MISRC_ prefix "
            "(v1.1.7 malform); use Android_MISRC_."
        )
    return 0


def check_release_download_link_contract(repo_root: Path, workflow_path: Path) -> int:
    """Assert the manifest-driven update pipeline wiring + contract.

    History: the updater built per-platform asset URLs from filename patterns
    compiled INTO the binary. v1.2.4 shipped patterns missing the "_GUI"
    infix while the workflow published <Platform>_MISRC_GUI_<tag>_<arch>.<ext>,
    so every platform's Download button 404'd; the class recurred whenever a
    compiled-in mapping drifted, and installed binaries could never be fixed
    retroactively (every fix needed a new release).

    The 2026-10-22 rework makes the failure class impossible by construction:
    - the manual button opens the releases PAGE (a page URL cannot 404 per
      platform),
    - a remote auto-patch manifest (update_manifest.json at the repo root,
      served from a fixed raw URL on main) defines the releases page, tag
      lookup, download base and per-platform asset patterns for EVERY
      installed app: a fix is a commit, not a release,
    - confirmed self-update (two-click) downloads + installs, archiving the
      old binary by RENAME (never deleted), gated off while capturing or
      recording.

    This guard pins: the manifest file + its structural contract, the
    app-side wiring (include, page button, confirm gate, capture gate,
    archive-not-delete rename), the absence of ANY compiled-in asset
    filename pattern, the updater unit test + meson wiring, and the live
    release_link_check.py CI test + its two workflow wirings.
    """
    gui_ui_c = repo_root / "misrc_tools" / "misrc_gui" / "ui" / "gui_ui.c"
    updater_h = repo_root / "misrc_tools" / "misrc_gui" / "core" / "gui_updater.h"
    updater_c = repo_root / "misrc_tools" / "misrc_gui" / "core" / "gui_updater.c"
    manifest_path = repo_root / "update_manifest.json"
    link_check = repo_root / "misrc_tools" / "test" / "release_link_check.py"
    updater_test = repo_root / "misrc_tools" / "test" / "gui_updater_test.c"
    meson_path = repo_root / "misrc_tools" / "meson.build"

    for path, label in [
        (gui_ui_c, "gui_ui.c"),
        (updater_h, "gui_updater.h"),
        (updater_c, "gui_updater.c"),
        (manifest_path, "update_manifest.json"),
        (link_check, "release_link_check.py"),
        (updater_test, "gui_updater_test.c"),
    ]:
        if not path.exists():
            return fail(f"Missing {label}: {path}")

    # 1. Repo manifest contract (full JSON parse; same acceptance rules as
    #    the app's gui_updater_parse_manifest_text + the live link check).
    try:
        manifest = json.loads(read_text(manifest_path))
    except json.JSONDecodeError as exc:
        return fail(f"update_manifest.json is not valid JSON: {exc}")
    if not isinstance(manifest, dict) or manifest.get("schema") != 1:
        return fail("update_manifest.json schema must be 1")
    for key in [
        "releases_page", "tag_lookup_url", "download_base_url",
        "asset_android", "asset_macos", "asset_windows_x86",
        "asset_windows_arm64", "asset_linux_x86", "asset_linux_arm64",
    ]:
        value = manifest.get(key)
        if not isinstance(value, str) or not value:
            return fail(f"update_manifest.json missing or empty key: {key}")
        if key in ("releases_page", "tag_lookup_url", "download_base_url"):
            if not value.startswith("https://"):
                return fail(f"update_manifest.json {key} must be https: {value}")
    for key in ["asset_android", "asset_macos", "asset_windows_x86",
                "asset_windows_arm64", "asset_linux_x86", "asset_linux_arm64"]:
        pattern = manifest[key]
        if "{tag}" not in pattern:
            return fail(f"update_manifest.json asset pattern must contain {{tag}}: {key}={pattern}")

    # 2. App-side constants + pipeline wiring.
    gui_text = read_text(gui_ui_c)
    updater_h_text = read_text(updater_h)
    updater_c_text = read_text(updater_c)

    for snippet, label in [
        ('#include "../core/gui_updater.h"', "the updater module include"),
        ("Releases Page", "the releases page button label"),
        ("VersionInfoInstallButton", "the install button"),
        ("s_update_install_armed", "the two-click confirm state"),
        ("Confirm install", "the armed confirm label"),
        ("gui_updater_fetch_manifest", "the manifest fetch in the check thread"),
        ("gui_updater_build_asset_url", "manifest-driven asset URL building"),
        ("gui_updater_install_asset", "the confirmed self-update install call"),
        ("Stop capture/recording before installing an update", "the capture/record install gate"),
    ]:
        if snippet not in gui_text:
            return fail(f"gui_ui.c update pipeline missing {label}: {snippet}")

    for snippet, label in [
        ("GUI_UPDATER_MANIFEST_URL", "the manifest URL macro"),
        ("https://raw.githubusercontent.com/harrypm/MISRC-GUI/main/update_manifest.json",
         "the fixed raw manifest URL"),
        ("GUI_UPDATER_RELEASES_PAGE_URL", "the releases page fallback constant"),
        ("https://github.com/harrypm/MISRC-GUI/releases/", "the releases page URL"),
        ("GUI_UPDATER_ARCHIVE_SUFFIX", "the archive suffix constant"),
        ("GUI_UPDATER_TAG_LOOKUP_URL", "the compiled tag-lookup fallback"),
    ]:
        if snippet not in updater_h_text:
            return fail(f"gui_updater.h missing {label}: {snippet}")

    for snippet, label in [
        ("GUI_UPDATER_MANIFEST_URL", "the manifest URL use in the fetch"),
        ("rename(target, archive_path)", "archive the old binary by rename"),
        ("rename(archive_path, target)", "restore the archived old binary on failure"),
        ("gui_updater_build_archive_path", "the archive path builder"),
    ]:
        if snippet not in updater_c_text:
            return fail(f"gui_updater.c missing {label}: {snippet}")

    # 3. Compiled-in asset filename patterns are FORBIDDEN anywhere in the
    #    app: asset naming lives ONLY in the remotely-patchable manifest, so
    #    the v1.2.4-class 404 bug is impossible by construction.
    for text, label in [(gui_text, "gui_ui.c"), (updater_c_text, "gui_updater.c"),
                        (updater_h_text, "gui_updater.h")]:
        for pattern in ["_MISRC_GUI_%s_", "_MISRC_%s_", "%s_arm64.apk",
                        "%s_universal.dmg", "%s_arm64.zip", "%s_x86.zip"]:
            if pattern in text:
                return fail(
                    f"{label} still contains a compiled-in release asset filename "
                    f"pattern ({pattern}) - asset naming must live ONLY in "
                    "update_manifest.json (the remotely-patchable manifest); "
                    "compiled-in patterns are the v1.2.4-class 404 bug"
                )

    # 4. Updater unit test + meson wiring.
    updater_test_text = read_text(updater_test)
    for snippet, label in [
        ("gui_updater_parse_manifest_text", "the manifest parse coverage"),
        ("gui_updater_compare_versions", "the version compare coverage"),
        ("gui_updater_build_archive_path", "the archive path coverage"),
        ("gui_updater_install_asset", "the install pipeline failure coverage"),
    ]:
        if snippet not in updater_test_text:
            return fail(f"gui_updater_test.c missing {label}: {snippet}")
    meson_text = read_text(meson_path)
    for snippet, label in [
        ("'misrc_gui/core/gui_updater.c'", "the updater module in the GUI build"),
        ("test('gui_updater'", "the updater unit test registered as a meson test"),
    ]:
        if snippet not in meson_text:
            return fail(f"misrc_tools/meson.build missing {label}: {snippet}")

    # 5. Live link check CI test + both workflow wirings.
    link_text = read_text(link_check)
    for snippet, label in [
        ("update_manifest.json", "the manifest file name"),
        ("raw.githubusercontent.com", "the live raw manifest fetch"),
        ("tag_lookup_url", "the manifest tag lookup"),
        ("releases_page", "the releases page check"),
        ("{tag}", "the {tag} placeholder"),
        ("--tag", "the release-job tag override"),
    ]:
        if snippet not in link_text:
            return fail(f"release_link_check.py missing {label}: {snippet}")
    workflow_text = read_text(workflow_path)
    for snippet, label in [
        ("release-link-check:", "the every-trigger link check job"),
        ("python3 misrc_tools/test/release_link_check.py", "the job's command"),
        ('release_link_check.py --tag "${{ steps.tag.outputs.tag }}"',
         "the release job's post-publish verification step"),
    ]:
        if snippet not in workflow_text:
            return fail(f"Workflow missing release download link check wiring ({label}): {snippet}")
    return 0


def check_update_cli_contract(repo_root: Path) -> int:
    """Assert the headless update CLI + its end-to-end automated test wiring.

    The updater pipeline (gui_updater.c) must be reachable WITHOUT the GUI
    (--check-update / --update-dry-run / --install-update --yes): the update
    flow has to be automatically testable, so the update_cli meson test drives
    the REAL built binary through every mode against a local test server
    (manifest + releases/latest redirect + asset zip, isolated HOME) -
    including the full confirmed install, the install refusal without --yes,
    and the old binary archived by rename (never deleted).

    Pins: the misrc_gui.c flags + runner + confirmation gate, the gui_updater
    manifest-source load (URL/file) + the loopback-http-only test carve-out,
    the update_cli test's assertions, the meson wiring (the captured
    misrc_gui_exe + test('update_cli')), and the README documentation.
    """
    gui_c_path = repo_root / "misrc_tools" / "misrc_gui" / "core" / "misrc_gui.c"
    updater_h = repo_root / "misrc_tools" / "misrc_gui" / "core" / "gui_updater.h"
    updater_c = repo_root / "misrc_tools" / "misrc_gui" / "core" / "gui_updater.c"
    update_cli_test = repo_root / "misrc_tools" / "test" / "update_cli_test.py"
    meson_path = repo_root / "misrc_tools" / "meson.build"
    readme_path = repo_root / "README.md"

    for path, label in [
        (gui_c_path, "misrc_gui.c"),
        (updater_h, "gui_updater.h"),
        (updater_c, "gui_updater.c"),
        (update_cli_test, "update_cli_test.py"),
        (meson_path, "meson.build"),
        (readme_path, "README.md"),
    ]:
        if not path.exists():
            return fail(f"Missing {label}: {path}")

    gui_text = read_text(gui_c_path)
    for snippet, label in [
        ("--check-update", "the check flag"),
        ("--update-dry-run", "the dry-run flag"),
        ("--install-update", "the install flag"),
        ("--update-manifest", "the manifest source override flag"),
        ("--yes", "the install confirmation flag"),
        ("gui_run_update_cli", "the headless update runner"),
        ("gui_updater_load_manifest_from", "the manifest source load"),
        ("update_available: ", "the update_available output key"),
        ("--install-update requires --yes", "the install confirmation gate"),
    ]:
        if snippet not in gui_text:
            return fail(f"misrc_gui.c headless update CLI missing {label}: {snippet}")

    for snippet, label in [
        ("gui_updater_load_manifest_from", "the manifest-source load declaration"),
    ]:
        if snippet not in read_text(updater_h):
            return fail(f"gui_updater.h missing {label}: {snippet}")

    updater_text = read_text(updater_c)
    for snippet, label in [
        ("gui_updater_load_manifest_from", "the manifest-source load implementation"),
        ("127.0.0.1", "the loopback-http-only test carve-out"),
    ]:
        if snippet not in updater_text:
            return fail(f"gui_updater.c missing {label}: {snippet}")

    test_text = read_text(update_cli_test)
    for snippet, label in [
        ("--check-update", "the check mode"),
        ("--update-dry-run", "the dry-run mode"),
        ("--install-update", "the install mode"),
        ("--yes", "the confirmation flag"),
        ("--update-manifest", "the manifest override"),
        ("releases/latest", "the tag redirect endpoint"),
        ('out["install"] == "ok"', "the install success assertion"),
        ("archived-", "the archive rename assertion"),
        ("never deleted", "the archive-not-delete assertion"),
    ]:
        if snippet not in test_text:
            return fail(f"update_cli_test.py missing {label}: {snippet}")

    meson_text = read_text(meson_path)
    for snippet, label in [
        ("test('update_cli'", "the update_cli meson test registration"),
        ("misrc_gui_exe = executable('misrc_gui'", "the captured misrc_gui exe the test drives"),
    ]:
        if snippet not in meson_text:
            return fail(f"misrc_tools/meson.build missing {label}: {snippet}")

    readme_text = read_text(readme_path)
    for snippet, label in [
        ("Headless update mode", "the README section"),
        ("--check-update", "the README check flag doc"),
        ("--install-update", "the README install flag doc"),
    ]:
        if snippet not in readme_text:
            return fail(f"README.md missing {label}: {snippet}")
    return 0


def check_release_version_resolution_contract(repo_root: Path, workflow_path: Path) -> int:
    """Assert release-context CI runs resolve the tag (never a dev string) and
    that a release can never ship a dev-named artifact under the tag.

    Regression (v1.1.7): the Linux job received an empty release_tag input and
    silently fell through to git-version.sh on a --no-tags --depth=1 shallow
    clone (-> dev-2026-08-25-c991f39), and the android-apk job had no
    version-resolution step and never passed MISRC_TOOLS_VERSION_OVERRIDE to
    build-apk.sh (which calls git-version.sh directly -> always dev-...,
    versionCode 1). Both shipped dev-named assets under tag v1.1.7.

    Requires:
      - misrc_tools/ci-resolve-version.sh exists, is executable, and contains
        the empty-release_tag hard-fail + the release+dev hard-fail.
      - every build job (linux, windows-x86, windows-arm64, macos, android)
        calls the shared resolver.
      - every build job exports MISRC_TOOLS_VERSION_OVERRIDE from the version
        step (Android especially: build-apk.sh reads it via git-version.sh).
      - the release job has the pre-upload 'no dev leak / tag-named' assertion.
    """
    resolver = repo_root / "misrc_tools" / "ci-resolve-version.sh"
    if not resolver.exists():
        return fail(f"Missing shared release/version resolver: {resolver}")
    if not os.access(resolver, os.X_OK):
        return fail("misrc_tools/ci-resolve-version.sh must be executable (chmod +x)")
    rv = read_text(resolver)
    for snippet in [
        "refs/tags/",
        "workflow_dispatch",
        "CI_CREATE_RELEASE",
        "CI_RELEASE_TAG",
        "release_tag input is empty",
        "release run resolved a dev version",
    ]:
        if snippet not in rv:
            return fail(f"misrc_tools/ci-resolve-version.sh is missing required snippet: {snippet}")

    wf = read_text(workflow_path)
    for snippet in [
        "CI_EVENT_NAME: ${{ github.event_name }}",
        "CI_CREATE_RELEASE: ${{ github.event.inputs.create_release }}",
        "CI_RELEASE_TAG: ${{ github.event.inputs.release_tag }}",
    ]:
        if snippet not in wf:
            return fail(f"Workflow is missing release-version-resolution env snippet: {snippet}")

    # The resolver must be invoked by all 5 build jobs (linux, windows-x86,
    # windows-arm64, macos, android).
    call_count = wf.count("misrc_tools/ci-resolve-version.sh")
    if call_count < 5:
        return fail(
            f"ci-resolve-version.sh must be called by all 5 build jobs (linux, "
            f"windows-x86, windows-arm64, macos, android); found {call_count} call(s)."
        )

    # All 5 build jobs must export MISRC_TOOLS_VERSION_OVERRIDE from the version
    # step. Android is the critical one: build-apk.sh calls git-version.sh
    # directly, so without the override it always yields dev-... on the shallow
    # clone (the v1.1.7 APK regression).
    override_count = wf.count("MISRC_TOOLS_VERSION_OVERRIDE: ${{ steps.version.outputs.version }}")
    if override_count < 5:
        return fail(
            f"All 5 build jobs must export MISRC_TOOLS_VERSION_OVERRIDE from the "
            f"version step (Android was missing this in the v1.1.7 regression); "
            f"found {override_count}."
        )

    # The release job must have the pre-upload assertion.
    if "Assert release assets are tag-named" not in wf:
        return fail(
            "Release job is missing the pre-upload 'Assert release assets are "
            "tag-named (no dev leak)' step."
        )
    if "*_MISRC_GUI_dev-*" not in wf:
        return fail("Release pre-upload assertion must match '*_MISRC_GUI_dev-*' dev-named artifacts.")
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
        # CI must also run on direct pushes to main (regression e34dce9: a
        # guard break was pushed straight to main and ran no CI for a day;
        # the tags-only push trigger never fired).
        "push:\n    branches:\n      - main\n    tags:\n      - 'v*'",
        "preflight-guard-tests:",
        "linux-appimage:",
        "windows-exe:",
        "macos-app-universal:",
        "android-apk:",
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
def check_local_build_bootstrap_contract(repo_root: Path,
                                         dev_notes_path: Path,
                                         installation_md_path: Path) -> int:
    script_path = repo_root / "scripts/build-local.ps1"
    if not script_path.exists():
        return fail(f"Missing Windows local-build bootstrap script: {script_path}")
    script_text = read_text(script_path)
    required_script_snippets = [
        "param(",
        "BootstrapOnly",
        "NoAutoInstall",
        "python -m mesonbuild.mesonmain --version",
        "python -m pip install --user --upgrade meson ninja",
        "Add-PythonUserScriptsToPath",
    ]
    for snippet in required_script_snippets:
        if snippet not in script_text:
            return fail(f"build-local.ps1 is missing required bootstrap snippet: {snippet}")

    if not dev_notes_path.exists():
        return fail(f"Missing dev notes file for bootstrap contract: {dev_notes_path}")
    dev_notes_text = read_text(dev_notes_path)
    required_dev_notes_snippets = [
        "Windows local build bootstrap note",
        "pwsh -File scripts/build-local.ps1 -BootstrapOnly",
        "pwsh -File scripts/build-local.ps1",
        "python misrc_tools/test/ci_guard_tests.py --static-only",
    ]
    for snippet in required_dev_notes_snippets:
        if snippet not in dev_notes_text:
            return fail(f"Dev notes are missing local build bootstrap guidance: {snippet}")

    # The build/installation snippets live in INSTALLATION.md at the repo root
    # (the main README.md is app-use-only). misrc_tools/README.md points here.
    if not installation_md_path.exists():
        return fail(f"Missing INSTALLATION.md for bootstrap contract: {installation_md_path}")
    installation_text = read_text(installation_md_path)
    required_installation_snippets = [
        "scripts/build-local.ps1 -BootstrapOnly",
        "scripts/build-local.ps1",
    ]
    for snippet in required_installation_snippets:
        if snippet not in installation_text:
            return fail(f"INSTALLATION.md is missing local-build bootstrap snippet: {snippet}")
    return 0


def check_local_deps_cache_contract(repo_root: Path,
                                     workflow_path: Path,
                                     dev_notes_path: Path,
                                     installation_md_path: Path) -> int:
    """Assert the local==CI deps caching path is present and wired.

    Prevents silent removal of the deps scripts, stamp gate, build-local
    auto-invocation, CI actions/cache, or docs that describe the model.
    """
    deps_win = repo_root / "scripts/build-deps-windows.sh"
    deps_unix = repo_root / "scripts/build-deps-unix.sh"
    publish = repo_root / "scripts/publish-deps-cache.sh"
    build_local_ps1 = repo_root / "scripts/build-local.ps1"
    build_local_sh = repo_root / "scripts/build-local.sh"

    for path, label in [(deps_win, "build-deps-windows.sh"),
                        (deps_unix, "build-deps-unix.sh"),
                        (publish, "publish-deps-cache.sh"),
                        (build_local_sh, "build-local.sh")]:
        if not path.exists():
            return fail(f"Missing deps-cache contract script: {label} ({path})")

    deps_win_text = read_text(deps_win)
    deps_unix_text = read_text(deps_unix)
    for label, text in [("build-deps-windows.sh", deps_win_text),
                        ("build-deps-unix.sh", deps_unix_text)]:
        if "compute_stamp" not in text:
            return fail(f"{label} is missing the stamp-gate function 'compute_stamp'")
        if ".build-stamp" not in text:
            return fail(f"{label} is missing the stamp file path '.build-stamp'")
    if "v0.0.7" not in deps_win_text:
        return fail("build-deps-windows.sh is missing the pinned LIBUVC_REF v0.0.7")

    ps1_text = read_text(build_local_ps1)
    if "Invoke-DepsBuild" not in ps1_text:
        return fail("build-local.ps1 must auto-invoke the deps script via Invoke-DepsBuild (not bail at the deps gate)")
    if "build-deps-windows.sh" not in ps1_text:
        return fail("build-local.ps1 must reference build-deps-windows.sh")

    sh_text = read_text(build_local_sh)
    if "build-deps-unix.sh" not in sh_text:
        return fail("build-local.sh must reference build-deps-unix.sh")

    workflow_text = read_text(workflow_path)
    if "actions/cache@v6" not in workflow_text:
        return fail("build.yml is missing actions/cache@v6 for deps caching")
    if "deps cache hit" not in workflow_text:
        return fail("build.yml is missing the deps cache-hit guard ('deps cache hit')")
    if "v0.0.7" not in workflow_text:
        return fail("build.yml must pin libuvc to v0.0.7 (parity with build-deps-windows.sh)")

    dev_notes_text = read_text(dev_notes_path)
    required_dev_snippets = [
        "Vendored deps caching",
        "build-deps-windows.sh",
        "actions/cache@v4",
        "publish-deps-cache.sh",
    ]
    for snippet in required_dev_snippets:
        if snippet not in dev_notes_text:
            return fail(f"Dev notes are missing deps-cache contract snippet: {snippet}")

    # The Vendored deps caching model section lives in the repo-root
    # INSTALLATION.md (misrc_tools/README.md only points here); the
    # parameter is named for the file it actually receives.
    installation_text = read_text(installation_md_path)
    required_installation_snippets = [
        "Vendored deps caching model",
        "build-deps-windows.sh",
        "build-deps-unix.sh",
        "publish-deps-cache.sh",
    ]
    for snippet in required_installation_snippets:
        if snippet not in installation_text:
            return fail(f"INSTALLATION.md is missing deps-cache contract snippet: {snippet}")
    return 0


def check_local_guard_gate_contract(repo_root: Path) -> int:
    """The local build scripts must run the static CI guard tests before any
    build work, and direct pushes to main must run CI. Regression e34dce9:
    guarded libFLAC struct members (decoder_a/b, io_a/b) were referenced
    outside the guard in gui_playback.c; local builds passed because the
    guard tests were never run locally, and the direct push to main ran no
    CI (the push trigger was tags-only), so the break surfaced only when a
    manually dispatched workflow run failed a day later."""
    ps1_path = repo_root / "scripts/build-local.ps1"
    sh_path = repo_root / "scripts/build-local.sh"
    workflow_path = repo_root / ".github/workflows/build.yml"
    for path in (ps1_path, sh_path, workflow_path):
        if not path.exists():
            return fail(f"Missing local guard-gate contract file: {path}")
    for label, script_path in [("build-local.ps1", ps1_path), ("build-local.sh", sh_path)]:
        text = read_text(script_path)
        if "misrc_tools/test/ci_guard_tests.py" not in text:
            return fail(f"{label} must run the static CI guard tests before building")
        if "--static-only" not in text:
            return fail(f"{label} must run the guard in --static-only mode (the documented local mode)")
        if "CI guard tests failed" not in text:
            return fail(f"{label} must FAIL the build when the CI guard tests fail ('CI guard tests failed')")
    workflow_text = read_text(workflow_path)
    push_main = "push:\n    branches:\n      - main\n    tags:\n      - 'v*'"
    if push_main not in workflow_text:
        return fail(
            "build.yml must run CI on direct pushes to main (push: branches: [main] "
            "alongside the v* tags trigger); a tags-only push trigger let a guard "
            "break reach main with no CI (regression e34dce9)"
        )
    return 0


def check_playback_drag_drop_contract(repo_root: Path) -> int:
    """The playback drag-and-drop wiring (2026-10-20, the full-capture-set
    loading feature: RF per channel zone + clockgen audio in one drop).

    - The main loop polls the raylib drop event exactly once and routes it
      through the logical pointer wrapper (never a second raw
      GetMousePosition call - the UI scale integration contract allows
      exactly one call site, in gui_ui.c).
    - gui_playback owns the drop handler + the position-synced audio feed
      (the monitor queue push + the seek/loop flush + the prepare-time open
      + the release).
    - gui_audio implements the playback push/flush queue API.
    - The audio file setting persists (save + load) and the settings panel
      shows + clears it.
    """
    gui_c_path = repo_root / "misrc_tools/misrc_gui/core/misrc_gui.c"
    gui_c = read_text(gui_c_path)
    for snippet, label in [
        ("IsFileDropped()", "the per-frame drop poll"),
        ("LoadDroppedFiles()", "the dropped-files load"),
        ("UnloadDroppedFiles(", "the dropped-files unload"),
        ("gui_playback_handle_dropped_files(", "the drop handler call"),
        ("gui_ui_get_mouse_position()", "the logical drop point (wrapper)"),
    ]:
        if snippet not in gui_c:
            return fail(f"misrc_gui.c drag-and-drop wiring is missing {label}: {snippet}")
    if "GetMousePosition()" in gui_c:
        return fail(
            "misrc_gui.c must not call GetMousePosition() directly (the UI scale "
            "integration contract allows exactly one call site, in gui_ui.c)"
        )
    pb_h = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_playback.h")
    if "gui_playback_handle_dropped_files" not in pb_h:
        return fail("gui_playback.h must declare gui_playback_handle_dropped_files")
    pb_c = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_playback.c")
    for snippet, label in [
        ("gui_playback_audio_feed(", "the position-synced audio feed"),
        ("gui_audio_push_playback(", "the monitor queue push"),
        ("gui_audio_reset_playback_queue()", "the seek/loop queue flush"),
        ("gui_playback_audio_open(", "the prepare-time audio source open"),
        ("gui_playback_audio_destroy(", "the audio source release"),
        ("PLAYBACK_AUDIO_FEED_LOOKAHEAD_MS", "the feed lookahead contract"),
        ("gui_playback_drop_zone(", "the panel-zone mapping"),
        ("gui_playback_classify_drop(", "the content classifier"),
    ]:
        if snippet not in pb_c:
            return fail(f"gui_playback.c drag-and-drop is missing {label}: {snippet}")
    audio_c = read_text(repo_root / "misrc_tools/misrc_gui/output/gui_audio.c")
    audio_h = read_text(repo_root / "misrc_tools/misrc_gui/output/gui_audio.h")
    if "gui_audio_push_playback" not in audio_h:
        return fail("gui_audio.h must declare gui_audio_push_playback")
    if "gui_audio_reset_playback_queue" not in audio_h:
        return fail("gui_audio.h must declare gui_audio_reset_playback_queue")
    if "void gui_audio_push_playback(" not in audio_c:
        return fail("gui_audio.c must implement gui_audio_push_playback")
    if "void gui_audio_reset_playback_queue(void)" not in audio_c:
        return fail("gui_audio.c must implement gui_audio_reset_playback_queue")
    app_h = read_text(repo_root / "misrc_tools/misrc_gui/core/gui_app.h")
    if "playback_audio_file[" not in app_h:
        return fail("gui_app.h settings must carry playback_audio_file")
    settings_c = read_text(repo_root / "misrc_tools/misrc_gui/core/gui_settings.c")
    if settings_c.count('"playback_audio_file"') != 2:
        return fail(
            "gui_settings.c must persist playback_audio_file in BOTH the save "
            "and the load paths (found "
            + str(settings_c.count('"playback_audio_file"'))
            + " of 2 key references)"
        )
    ui_c = read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c")
    if "PlaybackFileClearAudio" not in ui_c:
        return fail("gui_ui.c must show + clear the playback audio file row")
    return 0


def check_windows_modal_resize_contract(repo_root: Path) -> int:
    """The Windows modal move/size render driver (2026-10-20, backlog item
    2: the window froze during move/resize because DefWindowProc's modal
    loop blocked raylib's event poll for the whole drag).

    - The Windows-only module subclasses the GLFW window proc, drives a
      WM_TIMER render frame between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE,
      and passes everything to the original proc (CallWindowProcW). The
      module itself never polls events.
    - misrc_gui.c registers the render-only modal frame + installs the
      driver before the main loop and shuts it down before cleanup; the
      modal frame body is the frame tail core WITHOUT event polling or the
      normal frame tail (a nested PeekMessage inside DefWindowProc's modal
      loop is reentrancy).
    - meson compiles the module only on windows/cygwin; the header provides
      no-op stubs for the other platforms (no modal loop there).
    """
    modal_c_path = repo_root / "misrc_tools/misrc_gui/core/gui_win32_modal.c"
    modal_h_path = repo_root / "misrc_tools/misrc_gui/core/gui_win32_modal.h"
    for path in (modal_c_path, modal_h_path):
        if not path.exists():
            return fail(f"Missing the Windows modal render driver file: {path}")
    modal_c = read_text(modal_c_path)
    for snippet, label in [
        ("WM_ENTERSIZEMOVE", "the modal-loop entry"),
        ("WM_EXITSIZEMOVE", "the modal-loop exit"),
        ("WM_TIMER", "the timer-driven frame"),
        ("SetTimer(", "the timer start"),
        ("KillTimer(", "the timer stop"),
        ("CallWindowProcW(", "the original-proc passthrough"),
        ("SetWindowLongPtrW(", "the window-proc subclass"),
        ("GetWindowHandle()", "the raylib window handle"),
    ]:
        if snippet not in modal_c:
            return fail(f"gui_win32_modal.c is missing {label}: {snippet}")
    if "PollInputEvents" in modal_c or "glfwPollEvents" in modal_c:
        return fail(
            "gui_win32_modal.c must never poll events itself (the modal frame "
            "callback renders only - a nested PeekMessage inside DefWindowProc's "
            "modal loop is reentrancy)"
        )
    gui_c = read_text(repo_root / "misrc_tools/misrc_gui/core/misrc_gui.c")
    for snippet, label in [
        ("gui_win32_modal_set_frame(app_modal_frame", "the modal frame registration"),
        ("gui_win32_modal_install()", "the driver install"),
        ("gui_win32_modal_shutdown()", "the driver shutdown"),
        ("app_build_render_commands(", "the shared Clay layout build"),
    ]:
        if snippet not in gui_c:
            return fail(f"misrc_gui.c modal-loop wiring is missing {label}: {snippet}")
    frame_body = func_body_from(gui_c, "static void app_modal_frame(void *ctx)")
    for snippet in [
        "rlDrawRenderBatchActive()",
        "SwapScreenBuffer()",
        "Clay_SetLayoutDimensions",
        "gui_audio_update_playback",
        "app_build_render_commands",
    ]:
        if snippet not in frame_body:
            return fail(
                f"the modal frame body is missing {snippet} "
                "(render core without polling)"
            )
    for forbidden in ["EndDrawing()", "PollInputEvents"]:
        if forbidden in frame_body:
            return fail(
                f"the modal frame must NOT contain {forbidden} "
                "(no event polling / FPS wait inside the modal loop)"
            )
    modal_h = read_text(modal_h_path)
    if "static inline bool gui_win32_modal_install" not in modal_h:
        return fail(
            "gui_win32_modal.h must provide no-op stubs for non-Windows platforms"
        )
    meson = read_text(repo_root / "misrc_tools/meson.build")
    if "misrc_gui/core/gui_win32_modal.c" not in meson:
        return fail(
            "meson.build must compile gui_win32_modal.c on the windows/cygwin "
            "source list"
        )
    return 0


def check_secam_mode_contract(repo_root: Path) -> int:
    """The SECAM FM-chroma mode (2026-10-20, backlog item 4): the tbc-tools
    secamdecoder pipeline (libchromadec v0.1.0 parameters) ported to the
    streaming real-time per-line decode.

    - Parameters VERBATIM from the reference (pinned below): the nominal
      carriers (272/282 fH), deviations, scales, bell centre, LF break,
      combined band, calibration gates, the exact 10*fH pair split, the
      deviation rails, the killer ratio, the colour-diff LPF.
    - The pipeline stages + the field-commit + the ident ladder + the
      concealment are wired in gui_cvbs.c; the mode is user-selectable
      through the CVBS System dropdown's SECAM entry (2026-10-20, the
      same 0/1/2 value space as gui_cvbs_set_format) and the dormant
      tape-format selection, gated on colour AUTO + PLL lock.
    - The harness carries the numeric port contract (the time-domain
      filters vs the reference closed forms) + synthetic SECAM decode
      contracts (studio block, converter offset, LF pre-correction).
    """
    cvbs_c = read_text(repo_root / "misrc_tools/misrc_gui/signal/gui_cvbs.c")
    verbatim = [
        ("4250000.0", "the D'b rest carrier (272 fH)"),
        ("4406250.0", "the D'r rest carrier (282 fH)"),
        ("230000.0", "the D'B deviation"),
        ("280000.0", "the D'R deviation"),
        ("1.505", "the D'B scale"),
        ("-1.902", "the D'R scale"),
        ("4286000.0", "the bell centre"),
        ("85000.0", "the LF pre-correction break"),
        ("450000.0", "the combined band flat"),
        ("580000.0", "the combined band stop"),
        ("78125.0", "the exact 10*fH pair split"),
        ("90000.0", "the calibration min separation"),
        ("230000.0", "the calibration max separation"),
        ("350000.0", "the calibration max carrier offset"),
        ("10000.0", "the recentre threshold"),
        ("0.25", "the colour killer ratio"),
        ("1600000.0", "the colour-diff LPF pass"),
        ("1950000.0", "the colour-diff LPF stop"),
        ("4.0/5.0", "the discriminator tap 4/5"),
        ("-1.0/280.0", "the discriminator tap -1/280"),
        ("2.032062", "the D'B->U matrix constant"),
        ("1.139883", "the D'R->V matrix constant"),
    ]
    for value, label in verbatim:
        if value not in cvbs_c:
            return fail(f"gui_cvbs.c SECAM parameters: missing {label} ({value})")
    wiring = [
        ("cvbs_secam_decode_line(", "the per-line FM decode"),
        ("cvbs_secam_field_commit(decoder)", "the field-boundary commit"),
        ("cvbs_secam_bell_apply", "the cloche application"),
        ("cvbs_secam_build_filters", "the filter builder"),
        ("g_secam_diff_taps", "the discriminator taps"),
        ("CVBS_FORMAT_SECAM", "the SECAM format gate"),
        ("dr_on_even", "the identification state"),
        ("bottle", "the bottles ident cross-check"),
        ("noise_mad", "the concealment noise term"),
        ("one-line hold", "the causal hold"),
        ("\"PAL\", \"NTSC\", \"SECAM\"", "the System selector options"),
        ("gui_cvbs_set_format(decoder, sys_values[i])", "the System dropdown click wiring"),
        ("gui_cvbs_get_secam_diag(decoder", "the OSD Stats SECAM diag readout"),
    ]
    for snippet, label in wiring:
        if snippet.lower() not in cvbs_c.lower():
            return fail(f"gui_cvbs.c SECAM wiring: missing {label} ({snippet})")
    cvbs_h = read_text(repo_root / "misrc_tools/misrc_gui/signal/gui_cvbs.h")
    if "cvbs_secam_state_t" not in cvbs_h:
        return fail("gui_cvbs.h must declare the SECAM state struct")
    if "gui_cvbs_get_secam_diag" not in cvbs_h:
        return fail("gui_cvbs.h must declare gui_cvbs_get_secam_diag")
    if "system_options_rect[3]" not in cvbs_h:
        return fail("gui_cvbs.h must size the System selector dropdown for the three PAL/NTSC/SECAM options")
    test_c = read_text(repo_root / "misrc_tools/test/gui_cvbs_test.c")
    for snippet, label in [
        ("ref_bell_mag", "the reference bell closed form"),
        ("ref_inverse_lf_mag", "the reference de-emphasis closed form"),
        ("run_secam_numeric_match", "the numeric port contract"),
        ("run_secam_bars", "the synthetic decode contracts"),
        ("gen_secam(", "the synthetic SECAM generator"),
        ("105000.0", "the converter-offset variant"),
        ("pre_correct", "the LF pre-correction variant"),
    ]:
        if snippet not in test_c:
            return fail(f"gui_cvbs_test.c SECAM harness: missing {label} ({snippet})")
    return 0


def func_body_from(source: str, signature: str) -> str:
    """Return the text of the function whose definition starts at
    `signature`, from the signature up to the next column-0 closing brace.
    All target functions keep nested braces indented, so this is exact."""
    start = source.find(signature)
    if start < 0:
        raise RuntimeError(f"Function signature not found: {signature!r}")
    end = source.find("\n}", start)
    if end < 0:
        raise RuntimeError(f"Function end not found for: {signature!r}")
    return source[start:end]


def check_raw_direct_passthrough_contract(repo_root: Path) -> int:
    """Static contract for the direct native RAW passthrough path (CXADC,
    FLAC off, no resampling): tap placement before the A/B pairing
    truncation, shared eligibility predicate, naming by content
    (.u8/.u16 direct, .s8/.s16 converted), single producer per record
    ring, and the start/stop tap handshakes."""
    record_c = read_text(repo_root / "misrc_tools/misrc_gui/output/gui_record.c")
    record_h = read_text(repo_root / "misrc_tools/misrc_gui/output/gui_record.h")
    direct_c = read_text(repo_root / "misrc_tools/misrc_gui/output/gui_record_direct.c")
    cxadc_c = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_cxadc.c")
    cxadc_h = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_cxadc.h")
    extract_c = read_text(repo_root / "misrc_tools/misrc_gui/processing/gui_extract.c")
    extract_h = read_text(repo_root / "misrc_tools/misrc_gui/processing/gui_extract.h")
    settings_c = read_text(repo_root / "misrc_tools/misrc_gui/core/gui_settings.c")
    meson = read_text(repo_root / "misrc_tools/meson.build")
    harness_c = read_text(repo_root / "misrc_tools/test/gui_record_direct_harness.c")

    required_snippets = [
        (direct_c, "atomic_fetch_add(&ctx->tap_inflight, 1);", "producer marks in-flight"),
        (direct_c, "atomic_load(&ctx->tap_enabled)", "producer checks the enabled flag"),
        (direct_c, "bufmgr_fill_level(ctx->bufmgr, (buffer_id_t)ctx->buf_id)", "writer reads variable lengths"),
        (direct_c, "GUI_RECORD_DIRECT_MAX_BLOCK_BYTES", "writer read granularity cap"),
        (cxadc_c, "gui_record_direct_tap_push_latched(0, true, card_buf_a", "card A latched native tap"),
        (cxadc_c, "gui_record_direct_tap_push_latched(1, true, card_buf_b", "card B latched native tap"),
        (cxadc_c, "gui_record_tap_iteration_enter(0)", "iteration gate enter (A)"),
        (cxadc_c, "gui_record_tap_iteration_enter(1)", "iteration gate enter (B)"),
        (cxadc_c, "gui_record_tap_iteration_leave(0)", "iteration gate leave (A)"),
        (cxadc_c, "gui_record_tap_iteration_leave(1)", "iteration gate leave (B)"),
        (cxadc_c, "bool tap_this_iter = a_on && b_on;", "per-iteration latched record decision (A/B chunk symmetry)"),
        (record_h, "gui_record_direct_tap_push_latched", "latched tap wrapper declaration"),
        (direct_c, "gui_record_direct_push_latched(", "latched push in the record-direct module"),
        (cxadc_h, "gui_cxadc_direct_record_available", "CXADC direct availability query"),
        (record_h, "gui_record_direct_tap_push", "tap entry point declaration"),
        (record_h, "gui_record_spill_read_block", "public spill read"),
        (record_h, "gui_record_spill_backlog_bytes", "public spill backlog query"),
        (record_c, '"Recording (RAW direct)..."', "direct status text"),
        (record_c, "RAW direct native passthrough", "capture_format log line"),
        (record_c, "gui_record_direct_channel_eligible", "shared eligibility predicate"),
        (record_c, "Direct RAW channel A: bytes_read=", "end summary read vs written"),
        (extract_h, "gui_extract_set_recording(bool enabled, bool use_flac, uint8_t rf_bits_a, uint8_t rf_bits_b,", "set_recording carries the direct mask"),
        (meson, "'misrc_gui/output/gui_record_direct.c'", "module in the GUI build"),
        (meson, "test('gui_record_direct'", "harness registered as a meson test"),
        (harness_c, "gui_record_direct_push(", "harness drives the real push"),
        (harness_c, "gui_record_direct_writer_thread", "harness drives the real writer"),
    ]
    for text, snippet, label in required_snippets:
        if snippet not in text:
            return fail(f"Direct RAW passthrough contract missing {label}: {snippet}")

    # Tap placement: the lockstep tap gate must sit AFTER the A/B reads and
    # pairing truncation (the pushed bytes are the exact card reads, never
    # the paired/truncated samples) and BEFORE the display-ring write, with
    # the per-iteration ordering: enter both channels' taps, latch the
    # decision once, push both chunks with the same decision, then leave.
    # A record start/stop handshake landing between the per-channel pushes
    # then cannot split the channels by one chunk.
    gate_enter_a = cxadc_c.find("gui_record_tap_iteration_enter(0)")
    gate_latch = cxadc_c.find("bool tap_this_iter = a_on && b_on;")
    tap_a = cxadc_c.find("gui_record_direct_tap_push_latched(0, true, card_buf_a")
    tap_b = cxadc_c.find("gui_record_direct_tap_push_latched(1, true, card_buf_b")
    gate_leave_a = cxadc_c.find("gui_record_tap_iteration_leave(0)")
    truncation = cxadc_c.find("output_samples_b < output_samples")
    display_write = cxadc_c.find("bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF")
    if min(gate_enter_a, gate_latch, tap_a, tap_b, gate_leave_a, truncation, display_write) < 0:
        return fail("Direct RAW tap gate placement anchors not found in gui_cxadc.c")
    if not (truncation < gate_enter_a < gate_latch < tap_a < tap_b < gate_leave_a):
        return fail("Direct RAW tap gate must sit after the A/B pairing, latching the decision once before both latched pushes")
    if not (gate_leave_a < display_write):
        return fail("Direct RAW tap gate must complete before the display-ring write")

    # Producer handshake: mark in-flight, THEN read enabled.
    push_body = func_body_from(direct_c, "gui_record_direct_push_result_t gui_record_direct_push")
    if push_body.find("atomic_fetch_add(&ctx->tap_inflight, 1);") > push_body.find("atomic_load(&ctx->tap_enabled)"):
        return fail("Direct RAW push must mark in-flight before reading tap_enabled")

    # Producer ordering rule: while the spill backlog is non-zero, pushes go
    # to the spill (never into the ring ahead of older spill data).
    if "backlog > 0" not in push_body:
        return fail("Direct RAW push must key its spill-vs-ring decision on the spill backlog")

    # Writer drain order: ringbuffer before spill (oldest data first).
    writer_body = func_body_from(direct_c, "int gui_record_direct_writer_thread(void *ctx_ptr)")
    if writer_body.find("bufmgr_read_begin") > writer_body.find("ctx->cb.spill_read"):
        return fail("Direct RAW writer must drain the ringbuffer before the spill backlog")

    # Naming by content: direct -> .u8/.u16, converted -> .s8/.s16.
    ext_body = func_body_from(record_c, "static const char *raw_ext_for_bits")
    for snippet in ['"u8"', '"u16"', '"s8"', '"s16"']:
        if snippet not in ext_body:
            return fail(f"gui_record.c raw_ext_for_bits must map direct/converted to u8/u16/s8/s16: missing {snippet}")
    settings_ext_body = func_body_from(settings_c, "static const char *raw_ext_for_bits")
    for snippet in ['"s8"', '"s16"']:
        if snippet not in settings_ext_body:
            return fail(f"gui_settings.c raw_ext_for_bits (converted preview) missing {snippet}")
    for snippet in ['"u8"', '"u16"']:
        if snippet in settings_ext_body:
            return fail(f"gui_settings.c load-time preview must not claim native naming: {snippet}")

    # Predicate gates on: CXADC device, FLAC off, per-channel resample off,
    # running capture, live card sample-width match.
    pred_body = func_body_from(record_c, "static bool gui_record_direct_channel_eligible")
    for snippet in ["DEVICE_TYPE_CXADC", "app->settings.use_flac", "enable_resample_a", "enable_resample_b",
                    "gui_cxadc_is_running", "gui_cxadc_direct_record_available"]:
        if snippet not in pred_body:
            return fail(f"Direct RAW eligibility predicate missing required condition: {snippet}")

    # Single producer per record ring: extraction skips direct channels.
    extract_body = func_body_from(extract_c, "static int extraction_thread(void *ctx)")
    for snippet in ["!atomic_load(&s_direct_channel_a)", "!atomic_load(&s_direct_channel_b)"]:
        if snippet not in extract_body:
            return fail(f"Extraction must skip record writes for direct channels: {snippet}")
    set_rec_body = func_body_from(extract_c, "void gui_extract_set_recording")
    for snippet in ["s_direct_channel_a", "s_direct_channel_b"]:
        if snippet not in set_rec_body:
            return fail(f"gui_extract_set_recording must carry the direct mask: {snippet}")

    # Stop handshake: taps disabled and idle BEFORE is_recording clears.
    stop_body = func_body_from(record_c, "void gui_record_stop(gui_app_t *app)")
    tap_disable_pos = stop_body.find("gui_record_direct_tap_disable")
    tap_idle_pos = stop_body.find("gui_record_direct_tap_wait_idle")
    is_recording_pos = stop_body.find("app->is_recording = false;")
    if min(tap_disable_pos, tap_idle_pos, is_recording_pos) < 0:
        return fail("Direct RAW stop handshake anchors not found in gui_record_stop")
    if not (tap_disable_pos < tap_idle_pos < is_recording_pos):
        return fail("gui_record_stop must disable+idle the taps before clearing is_recording")

    # Start order: writers, then taps, then extraction recording (skip mask).
    start_writer = record_c.find("gui_record_direct_writer_thread")
    start_tap = record_c.find("gui_record_direct_tap_enable(&s_direct_ctx_a)")
    start_extract = record_c.find("gui_extract_set_recording(true, false, bits_a, bits_b, direct_a, direct_b)")
    if min(start_writer, start_tap, start_extract) < 0:
        return fail("Direct RAW start-order anchors not found in gui_record.c")
    if not (start_writer < start_tap < start_extract):
        return fail("Record start must start writers, then enable taps, then extraction recording")
    return 0


def check_gui_auto_test_flags(repo_root: Path) -> int:
    """Static contract for the unattended local test flags
    (--select-device / --auto-capture / --auto-record) and the
    overwrite_files popup bypass that unattended runs depend on."""
    gui_c = read_text(repo_root / "misrc_tools/misrc_gui/core/misrc_gui.c")
    record_c = read_text(repo_root / "misrc_tools/misrc_gui/output/gui_record.c")

    required_snippets = [
        (gui_c, 'strcmp(a, "--select-device")', "--select-device parsed as a GUI flag"),
        (gui_c, 'strcmp(a, "--auto-capture")', "--auto-capture parsed as a GUI flag"),
        (gui_c, 'strcmp(a, "--auto-record")', "--auto-record parsed as a GUI flag"),
        (gui_c, "[AUTO] --select-device: no device matches", "--select-device fails fast on no match"),
        (gui_c, "gui_strcasestr_local", "portable case-insensitive device name match"),
    ]
    for text, snippet, label in required_snippets:
        if snippet not in text:
            return fail(f"GUI auto-test flags contract missing {label}: {snippet}")

    # The unattended record cycle must report each phase and exit nonzero on failure.
    for marker in ["[AUTO] auto-capture armed", "[AUTO] recording started", "[AUTO] recording stopped",
                   "[AUTO] record cycle complete", "return auto_rec_failed ? 1 : 0;"]:
        if marker not in gui_c:
            return fail(f"GUI auto-record cycle missing required marker: {marker}")

    # usage text documents all three flags
    for flag in ["--select-device", "--auto-capture", "--auto-record <seconds>"]:
        if flag not in gui_c:
            return fail(f"GUI usage text missing auto-test flag: {flag}")

    # overwrite_files must bypass the overwrite confirmation popup so
    # unattended runs cannot stall on it.
    start_body = func_body_from(record_c, "int gui_record_start(gui_app_t *app)")
    if "!app->settings.overwrite_files" not in start_body:
        return fail("gui_record_start must skip the overwrite popup when overwrite_files is on")
    return 0


def check_cxadc_rate_probe_contract(repo_root: Path) -> int:
    """Static contract for the CXADC measured feed-rate check: probe at
    startup enumeration and capture start, tier snapping via the shared
    dependency-free header, effective rate preferred by the UI/settings
    rate getters, and the env opt-out."""
    cxadc_c = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_cxadc.c")
    cxadc_h = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_cxadc.h")
    capture_c = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_capture.c")
    ui_c = read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c")
    tiers_h = read_text(repo_root / "misrc_tools/common/cxadc_rate_tiers.h")
    tiers_test_c = read_text(repo_root / "misrc_tools/test/cxadc_rate_tiers_test.c")
    meson = read_text(repo_root / "misrc_tools/meson.build")

    required_snippets = [
        (cxadc_c, "cxadc_measure_card_rate_hz", "timed blocking-read measurement"),
        (cxadc_c, "cxadc_probe_and_cache_card_rate", "probe + cache + sysfs comparison"),
        (cxadc_c, "MISRC_GUI_NO_CXADC_RATE_PROBE", "probe env opt-out"),
        (cxadc_c, "CXADC_RATE_PROBE_START_BYTES", "capture-start probe window"),
        (cxadc_c, "gui_cxadc_get_effective_rate_hz", "effective-rate API"),
        (cxadc_c, "cxadc_snap_rate_tier_hz", "tier snapping from the shared header"),
        (cxadc_c, "s_cxadc_measured_rate_state", "measured-rate cache"),
        (cxadc_c, "CXADC_RATE_PROBE_DRAIN_CHUNK_MIN_US", "backlog drain phase before the timed window"),
        (cxadc_h, "gui_cxadc_get_effective_rate_hz", "effective-rate declaration"),
        (cxadc_h, "gui_cxadc_probe_card_rates", "probe entry point declaration"),
        (capture_c, "gui_cxadc_probe_card_rates(cxadc_card_count", "startup enumeration probe"),
        (capture_c, "gui_cxadc_get_effective_rate_hz(0, tenbit_mode_a", "CXADC profile sync uses the effective rate"),
        (ui_c, "gui_cxadc_get_effective_rate_hz(card_idx, false", "UI base rate uses the effective rate"),
        (ui_c, "gui_cxadc_get_effective_rate_hz(card_idx, tenbit", "UI hw rate uses the effective rate"),
        (tiers_h, "cxadc_snap_rate_tier_hz", "tier snap helper"),
        (tiers_h, "35795454", "upsampled 35.8 exclusion documented/absent as tier"),
        (tiers_test_c, "cxadc_snap_rate_tier_hz", "tier snap unit test drives the real helper"),
        (meson, "test('cxadc_rate_tiers'", "tier snap test registered"),
    ]
    for text, snippet, label in required_snippets:
        if snippet not in text:
            return fail(f"CXADC rate probe contract missing {label}: {snippet}")

    # The tier table must NOT contain the upsampled 35.8 feed (35795454 /
    # 35800000 must appear only in comments, never as a tier value).
    tiers_body = tiers_h[tiers_h.find("CXADC_RATE_TIER_HZ[] = {"):tiers_h.find("};")]
    for forbidden in ("35795454U,", "35800000U,"):
        if forbidden in tiers_body:
            return fail(f"35.8 MHz upsampled feed must not be a snap tier: {forbidden}")

    # The measured rate must override the sysfs belief when they disagree
    # (the reported stale-crystal case), at every capture start.
    start_body = func_body_from(cxadc_c, "int gui_cxadc_start(gui_app_t *app, int card_count, bool misrc_clockgen_mode)")
    probe_pos = start_body.find("cxadc_probe_and_cache_card_rate")
    override_pos = start_body.find("s_cxadc.card_sample_rate_hz[i] = s_cxadc_measured_rate_hz[i][t]")
    # rfind: the probe block's re-assignment is the LAST occurrence (the
    # original sysfs-derived assignment earlier in the function stays).
    rf_pos = start_body.rfind("s_cxadc.rf_sample_rate_hz = s_cxadc.card_sample_rate_hz[0]")
    if min(probe_pos, override_pos, rf_pos) < 0:
        return fail("Capture-start rate-probe anchors not found in gui_cxadc_start")
    if not (probe_pos < override_pos < rf_pos):
        return fail("gui_cxadc_start must probe, apply the measured rate, then set rf_sample_rate_hz")
    return 0


def check_raw_direct_writer_runtime(repo_root: Path) -> int:
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        print("SKIP: direct RAW writer runtime guard (Linux/macOS only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for direct RAW writer runtime guard")
        print("SKIP: direct RAW writer runtime guard (cc not available)")
        return 0

    sources = [
        repo_root / "misrc_tools/test/gui_record_direct_harness.c",
        repo_root / "misrc_tools/misrc_gui/output/gui_record_direct.c",
        repo_root / "misrc_tools/common/buffer_manager.c",
        repo_root / "misrc_tools/common/ringbuffer.c",
        repo_root / "misrc_tools/common/rb_event.c",
    ]
    for path in sources:
        if not path.exists():
            return fail(f"Direct RAW writer runtime source is missing: {path}")

    with tempfile.TemporaryDirectory(prefix="misrc_direct_raw_guard_") as temp_root:
        exe_name = "gui_record_direct_guard.exe" if os.name == "nt" else "gui_record_direct_guard"
        exe_path = Path(temp_root) / exe_name
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-D_POSIX_C_SOURCE=200809L",
            "-D_DEFAULT_SOURCE",
        ]
        if sys.platform == "darwin":
            compile_cmd.insert(3, "-D_DARWIN_C_SOURCE")
        compile_cmd += [str(p) for p in sources]
        if sys.platform.startswith("linux"):
            compile_cmd += ["-lpthread"]
        compile_cmd += ["-o", str(exe_path)]
        try:
            run_checked(compile_cmd)
        except subprocess.CalledProcessError as exc:
            return fail(
                "Failed to compile direct RAW writer runtime harness\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            )
        try:
            run_checked([str(exe_path)])
        except subprocess.CalledProcessError as exc:
            return fail(
                "Direct RAW writer runtime harness failed\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            )
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


def check_ui_scale_policy_runtime(repo_root: Path) -> int:
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for UI scale policy runtime guard")
        print("SKIP: UI scale policy runtime guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/gui_ui_scale_harness.c"
    policy_path = repo_root / "misrc_tools/misrc_gui/ui/gui_ui_scale.c"
    include_dir = repo_root / "misrc_tools/misrc_gui/ui"

    for path, label in [(harness_path, "UI scale harness"),
                        (policy_path, "UI scale policy")]:
        if not path.exists():
            return fail(f"{label} source is missing: {path}")

    with tempfile.TemporaryDirectory(prefix="misrc_ui_scale_guard_") as temp_root:
        exe_name = "gui_ui_scale_guard.exe" if os.name == "nt" else "gui_ui_scale_guard"
        exe_path = Path(temp_root) / exe_name
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            f"-I{include_dir}",
            str(harness_path),
            str(policy_path),
            "-lm",
            "-o",
            str(exe_path),
        ]
        try:
            run_checked(compile_cmd)
            run_checked([str(exe_path)])
        except subprocess.CalledProcessError as exc:
            return fail(
                "UI scale policy runtime guard failed\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            )
    return 0


def check_ui_scale_integration_contract(repo_root: Path, gui_c_path: Path,
                                        gui_settings_c_path: Path,
                                        meson_path: Path) -> int:
    gui_c = read_text(gui_c_path)
    settings_c = read_text(gui_settings_c_path)
    gui_app_h = read_text(repo_root / "misrc_tools/misrc_gui/core/gui_app.h")
    gui_ui_h = read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.h")
    gui_ui_c = read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c")
    popup_c = read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_popup.c")
    renderer_c = read_text(repo_root / "misrc_tools/misrc_gui/ui/clay_renderer_raylib.c")
    oscilloscope_c = read_text(repo_root / "misrc_tools/misrc_gui/visualization/gui_oscilloscope.c")
    fft_c = read_text(repo_root / "misrc_tools/misrc_gui/visualization/gui_fft.c")
    phosphor_h = read_text(repo_root / "misrc_tools/misrc_gui/visualization/gui_phosphor_rt.h")
    phosphor_c = read_text(repo_root / "misrc_tools/misrc_gui/visualization/gui_phosphor_rt.c")
    meson = read_text(meson_path)

    required_snippets = [
        (gui_app_h, "int ui_scale_percent;", "persisted settings field"),
        (settings_c, "settings->ui_scale_percent = GUI_UI_SCALE_DEFAULT_PERCENT;", "100% default"),
        (settings_c, '\\"ui_scale_percent\\": %d', "settings save key"),
        (settings_c, "gui_ui_scale_parse_percent(value)", "validated settings load"),
        (meson, "'misrc_gui/ui/gui_ui_scale.c'", "UI scale policy product source"),
        (gui_c, "gui_ui_zoom_process(&ui_zoom_state", "single wheel routing policy"),
        (gui_c, "IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_KP_0)", "100% reset shortcut"),
        (gui_c, "IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)", "keyboard zoom-in shortcut"),
        (gui_c, "IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)", "keyboard zoom-out shortcut"),
        (gui_c, "gui_ui_scale_step_percent(app.settings.ui_scale_percent", "shared keyboard zoom-step policy"),
        (gui_c, "ui_zoom_result.step_attempted || keyboard_zoom_pressed", "zoom HUD attempt feedback"),
        (gui_c, "gui_ui_show_scale_hud(ui_zoom_result.percent);", "zoom HUD trigger"),
        (gui_c, "ui_zoom_result.passthrough_x * 20.0f", "Clay horizontal wheel routing"),
        (gui_c, "ui_zoom_result.passthrough_y * 20.0f", "Clay vertical wheel routing"),
        (gui_ui_h, "Vector2 gui_ui_get_mouse_position(void);", "logical pointer API"),
        (gui_ui_c, "position.x /= scale;", "pointer inverse transform"),
        (gui_ui_c, "CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH", "pointer-transparent zoom HUD"),
        (gui_ui_c, "gui_ui_scale_hud_opacity(remaining_s)", "zoom HUD fade policy"),
        (gui_ui_c, "render_ui_scale_hud();", "zoom HUD render integration"),
        (gui_ui_c, "Cmd+0 to reset to 100%", "macOS zoom reset hint"),
        (gui_ui_c, "Ctrl+0 to reset to 100%", "desktop zoom reset hint"),
        (gui_ui_c, "gui_ui_toolbar_uses_two_rows(toolbar_width,", "content-aware toolbar policy"),
        (gui_ui_c, "gui_ui_get_status_layout_mode(status_width, status_height,", "responsive status policy"),
        (gui_ui_c, 'strstr(raw_status_gate, "Capture stopped:")', "critical stop reason priority"),
        (gui_ui_c, "gui_ui_modal_max_extent(gui_ui_get_layout_width()", "viewport-clamped modals"),
        (popup_c, "gui_ui_modal_max_extent(gui_ui_get_layout_width()", "viewport-clamped generic popup"),
        (popup_c, 'CLAY_ID("PopupContentScroll")', "scrollable popup content"),
        (popup_c, "CLAY_SIZING_FIT(.max = popup_message_max_width)", "viewport-bounded popup text"),
        (renderer_c, "Matrix outer_modelview = rlGetMatrixModelview();", "outer render transform capture"),
        (renderer_c, "rlScalef(ui_scale, ui_scale, 1.0f);", "global render transform"),
        (renderer_c, "box.x * ui_scale", "scaled scissor transform"),
        (renderer_c, "rlSetMatrixModelview(outer_modelview);", "balanced render transform"),
        (phosphor_h, "Matrix outer_modelview;", "saved phosphor model-view"),
        (phosphor_c, "rlGetMatrixModelview()", "phosphor transform capture"),
        (phosphor_c, "rlSetMatrixModelview", "phosphor transform restore"),
        (phosphor_c, "DrawTexturePro", "logical-size phosphor composite"),
        (gui_ui_h, "Vector2 gui_ui_get_render_scale(void);", "framebuffer-density API"),
        (oscilloscope_c, "gui_ui_get_render_scale();", "scale-aware waveform texture"),
        (fft_c, "gui_ui_get_render_scale();", "scale-aware FFT texture"),
    ]
    for source, snippet, label in required_snippets:
        if snippet not in source:
            return fail(f"Missing UI scale integration contract ({label}): {snippet}")

    if gui_c.count("GetMouseWheelMoveV(") != 1:
        return fail("misrc_gui.c must snapshot GetMouseWheelMoveV() exactly once per frame")
    if re.search(r"\bGetMouseWheelMove\(", gui_c):
        return fail("misrc_gui.c must not re-read scalar GetMouseWheelMove()")

    ordered = [
        gui_c.find("GetMouseWheelMoveV("),
        gui_c.find("gui_ui_zoom_process(&ui_zoom_state"),
        gui_c.find("Clay_UpdateScrollContainers"),
        gui_c.find("panel_handle_all_scrolls"),
    ]
    if any(pos < 0 for pos in ordered) or ordered != sorted(ordered):
        return fail("UI scale wheel routing must occur before Clay and panel consumers")

    modifier_snippets = [
        "KEY_LEFT_CONTROL", "KEY_RIGHT_CONTROL",
        "KEY_LEFT_SUPER", "KEY_RIGHT_SUPER",
    ]
    for snippet in modifier_snippets:
        if snippet not in gui_c:
            return fail(f"UI scale primary modifier mapping is missing {snippet}")

    direct_mouse_calls = []
    gui_root = repo_root / "misrc_tools/misrc_gui"
    for source_path in gui_root.rglob("*.c"):
        source = read_text(source_path)
        count = source.count("GetMousePosition(")
        if count:
            direct_mouse_calls.append((source_path, count))
    expected_pointer_source = repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c"
    if direct_mouse_calls != [(expected_pointer_source, 1)]:
        details = ", ".join(f"{path.relative_to(repo_root)}:{count}"
                            for path, count in direct_mouse_calls)
        return fail(f"Raw GetMousePosition() escaped the logical pointer wrapper: {details}")

    return 0


def check_audio_record_alignment_contract(repo_root: Path) -> int:
    """Static contract for the F1 record-alignment transitions in the
    audio thread: transitions detected at the top of the loop (bounded by
    the 2 ms read timeout, so within ~2-4 ms of the record button), a
    discard-to-now of buffered pre-record audio at record START at the
    576 B producer granularity, and a flush-to-now of buffered tail audio
    at record STOP (the old semantics wrote up to one full ~350 ms block
    of pre-record audio at record START)."""
    audio_c = read_text(repo_root / "misrc_tools/misrc_gui/output/gui_audio.c")
    buffer_manager_c = read_text(repo_root / "misrc_tools/common/buffer_manager.c")

    required_snippets = [
        (audio_c, "#define AUDIO_SYNC_CHUNK_BYTES 576", "producer-granularity sync chunk (48 frames x 12 B)"),
        (audio_c, "static uint64_t audio_discard_ring_to_now(", "discard-to-now helper"),
        (audio_c, "static uint64_t audio_flush_ring_to_files(", "flush-to-now helper"),
        (audio_c, "static void audio_write_recorded_block(", "shared block-write helper"),
        (audio_c, "BUF_CAPTURE_AUDIO,\n                                    AUDIO_SYNC_CHUNK_BYTES, 0)", "sync helpers use no-wait full chunks"),
        (audio_c, "[AUDIO] Record start aligned", "start-alignment log line"),
        (audio_c, "[AUDIO] Record stop aligned", "stop-alignment log line"),
        (audio_c, "bool was_recording = (a->f_4ch != NULL)", "pre-opened-files recording init"),
        (audio_c, "bufmgr_read_begin(a->bufmgr, BUF_CAPTURE_AUDIO, len, 2);", "2 ms read timeout keeps the transition observation at ~2-4 ms"),
        (buffer_manager_c, "if (timeout_ms == 0) return NULL;", "timeout-0 read returns only full available chunks"),
    ]
    for text, snippet, label in required_snippets:
        if snippet not in text:
            return fail(f"Audio record alignment contract missing {label}: {snippet}")

    # The transition handler must sit at the TOP of the audio loop, before
    # the main full-block read, so transitions are observed within the
    # 10 ms read-timeout spin instead of only at the next full ~350 ms block.
    thread_body = func_body_from(audio_c, "static int audio_thread_main(void *ctx)")
    loop_pos = thread_body.find("while (1) {")
    transition_pos = thread_body.find("bool rec_now = a->app && a->app->is_recording;")
    read_pos = thread_body.find("bufmgr_read_begin(a->bufmgr, BUF_CAPTURE_AUDIO, len, 2);")
    if min(loop_pos, transition_pos, read_pos) < 0:
        return fail("Audio record alignment anchors not found in audio_thread_main")
    if not (loop_pos < transition_pos < read_pos):
        return fail("Audio record transition handler must sit at the top of the audio loop, before the main full-block read")

    # START discards, STOP flushes: both through the producer-granularity
    # chunk (12,288 B = one clockgen capture-thread write) and both before
    # the main full-block read path runs.
    start_discard_pos = thread_body.find("audio_discard_ring_to_now(a);", transition_pos)
    stop_flush_pos = thread_body.find("audio_flush_ring_to_files(a);", transition_pos)
    if start_discard_pos < 0 or stop_flush_pos < 0:
        return fail("Audio record transitions must call the discard/flush helpers")
    if not (start_discard_pos < read_pos and stop_flush_pos < read_pos):
        return fail("Audio record transition handlers must run before the main full-block read")

    # The flush must write through the same conversion path (peaks + block
    # writer) rather than dropping the tail.
    flush_body = func_body_from(audio_c, "static uint64_t audio_flush_ring_to_files")
    for snippet in ["audio_update_peaks", "audio_write_recorded_block", "bufmgr_read_end"]:
        if snippet not in flush_body:
            return fail(f"audio_flush_ring_to_files must use the shared write path: missing {snippet}")
    return 0


def check_cxadc_sync_start_contract(repo_root: Path) -> int:
    """Static contract for the F1b synchronized card starts: after the
    sequential capture-start rate probes (which leave card 0's stream ~one
    probe duration behind card 1's in wall-clock content time), both cards
    are drained to their current 2 MiB release boundary and skipped to a
    common content instant T_ref BEFORE the RF lockstep thread starts, so
    the recorded A/B content offset is bounded by read granularity and
    measurement slop instead of the probe skew (~114 ms measured)."""
    cxadc_c = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_cxadc.c")

    required_snippets = [
        (cxadc_c, "static void cxadc_sync_card_starts(", "synchronized-start routine"),
        (cxadc_c, "#define CXADC_SYNC_CHUNK_BYTES", "sync chunk granularity"),
        (cxadc_c, "#define CXADC_SYNC_LIVE_CHUNK_MIN_US", "live-detection threshold"),
        (cxadc_c, "#define CXADC_SYNC_GRID_LIVE_MIN_US", "grid-read live-detection threshold"),
        (cxadc_c, "#define CXADC_SYNC_TREF_MARGIN_US", "T_ref margin"),
        (cxadc_c, "#define CXADC_SYNC_MAX_DRAIN_BYTES", "drain byte cap"),
        (cxadc_c, "#define CXADC_SYNC_MAX_TOTAL_US", "total sync budget"),
        (cxadc_c, "static size_t s_cxadc_probe_consumed_bytes[CXADC_MAX_CARDS];", "per-session consumed grid reference"),
        (cxadc_c, "[CXADC] sync-start:", "sync-start log line"),
    ]
    for text, snippet, label in required_snippets:
        if snippet not in text:
            return fail(f"CXADC synchronized start contract missing {label}: {snippet}")

    sync_body = func_body_from(cxadc_c, "static void cxadc_sync_card_starts")
    for snippet in [
        "CXADC_SYNC_LIVE_CHUNK_MIN_US",          # phase-1 drain live heuristic
        "% CXADC_SYNC_RELEASE_BYTES",           # grid-align to the 2 MiB boundary grid
        "rem - (size_t)got",                    # exact-remainder consumption (no overshoot)
        "CXADC_SYNC_GRID_LIVE_MIN_US",          # grid-read full-period wait detection
        "cxadc_sync_read_card(ctx, i, buf, 1)",  # 1-byte boundary-crossing probe
        "probe_us >= 1000",                     # probe must truly block (crossing measured)
        "t_ref += CXADC_SYNC_TREF_MARGIN_US",   # common target with margin
        "delta_us * byte_rate",                 # skip bytes from content-time delta
        "card_sample_rate_hz[i]",               # per-card measured rate
        "tenbit_mode[i]",                       # per-card sample width
        "s_cxadc_probe_consumed_bytes[i]",       # grid reference from the probe's consumed count
    ]:
        if snippet not in sync_body:
            return fail(f"cxadc_sync_card_starts missing required logic: {snippet}")

    # The sync must run AFTER the capture-start probe loop and BEFORE the RF
    # capture thread is created, gated on two or more cards.
    start_body = func_body_from(
        cxadc_c, "int gui_cxadc_start(gui_app_t *app, int card_count, bool misrc_clockgen_mode)")
    probe_pos = start_body.find("cxadc_probe_and_cache_card_rate(app, i, s_cxadc.tenbit_mode[i],")
    sync_call_pos = start_body.find("cxadc_sync_card_starts(&s_cxadc, card_count);")
    gate_pos = start_body.find("if (card_count > 1) {")
    rf_thread_pos = start_body.find("thrd_create_with_priority(&s_cxadc.rf_thread,")
    if min(probe_pos, sync_call_pos, gate_pos, rf_thread_pos) < 0:
        return fail("CXADC sync-start ordering anchors not found in gui_cxadc_start")
    if not (probe_pos < sync_call_pos < rf_thread_pos):
        return fail("cxadc_sync_card_starts must run after the capture-start probes and before the RF thread starts")
    if not (gate_pos < sync_call_pos):
        return fail("cxadc_sync_card_starts must be gated on card_count > 1")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="MISRC CI guard tests")
    parser.add_argument(
        "--static-only",
        action="store_true",
        help="Run static/text invariants only (skip runtime AppRun simulation)",
    )
    parser.add_argument(
        "--post-build",
        action="store_true",
        help="Post-build mode: also run binary-introspection guards (hsdaoh linkage, "
             "FX3 symbols) against --gui-path. Used by CI build jobs after misrc_gui is built.",
    )
    parser.add_argument(
        "--gui-path",
        type=Path,
        default=None,
        help="Path to the built misrc_gui binary for --post-build binary-introspection checks.",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    workflow_path = repo_root / ".github/workflows/build.yml"
    legacy_workflow_path = repo_root / ".github/workflows/release-sanity-build.yml"
    gui_c_path = repo_root / "misrc_tools/misrc_gui/core/misrc_gui.c"
    gui_settings_c_path = repo_root / "misrc_tools/misrc_gui/core/gui_settings.c"
    gui_ui_c_path = repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c"
    flac_writer_c_path = repo_root / "misrc_tools/common/flac_writer.c"
    meson_path = repo_root / "misrc_tools/meson.build"
    installation_md_path = repo_root / "INSTALLATION.md"
    dev_notes_path = repo_root / "misrc_tools/misrc_gui/dev/dev_notes_README.md"
    icon_path = repo_root / "assets/Icons/MISRC_Icon.png"

    checks: List[Tuple[str, Callable[[], int]]] = [
        ("cross-platform workflow coverage", lambda: check_cross_platform_workflow_coverage(workflow_path)),
        ("actions runtime policy", lambda: check_actions_runtime_policy(workflow_path)),
        ("macOS brew install policy", lambda: check_macos_brew_install_policy(workflow_path)),
        ("workflow FFT dependency policy", lambda: check_workflow_fft_dependency_policy(workflow_path)),
        ("MSYS2 toolchain policy", lambda: check_msys2_toolchain_policy(workflow_path)),
        ("libc direct-include contract", lambda: check_libc_direct_include_contract(repo_root)),
        ("meson FFT policy", lambda: check_meson_fft_policy(meson_path)),
        ("meson vendored hsdaoh policy", lambda: check_meson_vendored_hsdaoh_policy(meson_path)),
        ("meson FX3 native-build policy", lambda: check_meson_fx3_policy(meson_path)),
        ("cross-platform smoke tests", lambda: check_cross_platform_smoke_tests(workflow_path)),
        ("linux desktop metadata", lambda: check_linux_desktop_metadata(repo_root, workflow_path)),
        ("window identity contract", lambda: check_window_identity_contract(repo_root)),
        ("macOS layout policy", lambda: check_macos_layout_policy(gui_ui_c_path)),
        ("macOS startup admin elevation contract", lambda: check_macos_admin_elevation_contract(gui_c_path)),
        ("Windows meson subsystem contract", lambda: check_windows_meson_subsystem_contract(meson_path)),
        ("dev version naming", lambda: check_dev_version_naming(repo_root, meson_path, workflow_path)),
        ("no tracked generated dirty sources", lambda: check_no_tracked_generated_dirty_sources(repo_root)),
        ("Windows GUI link no DLL import libs", lambda: check_windows_gui_link_no_dll_import_libs(meson_path)),
        ("optional-dep guard consistency", lambda: check_optional_dep_guard_consistency(repo_root)),
        ("debug-view runtime contract", lambda: check_debug_view_contract(gui_c_path)),
        ("settings persistence contract", lambda: check_settings_persistence_contract(gui_settings_c_path)),
        ("UI scale integration contract", lambda: check_ui_scale_integration_contract(
            repo_root, gui_c_path, gui_settings_c_path, meson_path)),
        ("FLAC large-file offsets contract", lambda: check_flac_large_file_offsets_contract(flac_writer_c_path)),
        ("AppRun static contract", lambda: check_apprun_static_contract(repo_root)),
        ("Windows packaging assertions", lambda: check_windows_packaging_assertions(workflow_path)),
        ("Android packaging assertions", lambda: check_android_packaging_assertions(workflow_path)),
        ("release artifact naming contract", lambda: check_release_artifact_naming_contract(repo_root, workflow_path)),
        ("release download link contract", lambda: check_release_download_link_contract(repo_root, workflow_path)),
        ("update CLI contract", lambda: check_update_cli_contract(repo_root)),
        ("release version resolution contract", lambda: check_release_version_resolution_contract(repo_root, workflow_path)),
        ("build workflow entrypoint contract", lambda: check_build_workflow_entrypoint_contract(workflow_path)),
        ("legacy release-sanity workflow removed", lambda: check_no_legacy_release_sanity_workflow(legacy_workflow_path)),
        ("no capture-stability Actions clutter", lambda: check_no_capture_stability_clutter(workflow_path)),
        ("local build bootstrap contract", lambda: check_local_build_bootstrap_contract(repo_root, dev_notes_path, installation_md_path)),
        ("local deps cache contract", lambda: check_local_deps_cache_contract(repo_root, workflow_path, dev_notes_path, installation_md_path)),
        ("local CI guard gate contract", lambda: check_local_guard_gate_contract(repo_root)),
        ("playback drag-and-drop contract", lambda: check_playback_drag_drop_contract(repo_root)),
        ("Windows modal move/size render contract", lambda: check_windows_modal_resize_contract(repo_root)),
        ("SECAM FM-chroma mode contract", lambda: check_secam_mode_contract(repo_root)),
        ("raw direct passthrough contract", lambda: check_raw_direct_passthrough_contract(repo_root)),
        ("GUI auto-test flags contract", lambda: check_gui_auto_test_flags(repo_root)),
        ("CXADC rate probe contract", lambda: check_cxadc_rate_probe_contract(repo_root)),
        ("audio record alignment contract", lambda: check_audio_record_alignment_contract(repo_root)),
        ("CXADC synchronized start contract", lambda: check_cxadc_sync_start_contract(repo_root)),
    ]
    if not args.static_only:
        checks.insert(7, ("AppRun runtime behavior", lambda: check_apprun_runtime_behavior(repo_root, icon_path)))
        checks.insert(8, ("record ringbuffer fallback runtime", lambda: check_record_ringbuffer_fallback_runtime(repo_root)))
        checks.insert(9, ("UI scale policy runtime", lambda: check_ui_scale_policy_runtime(repo_root)))
        checks.insert(10, ("direct RAW writer runtime", lambda: check_raw_direct_writer_runtime(repo_root)))
        checks.insert(11, ("built GUI links vendored hsdaoh", lambda: check_built_gui_links_vendored_hsdaoh(repo_root, args.gui_path)))
    # --post-build: always run the binary-introspection guards against the real
    # built misrc_gui (passed via --gui-path by CI build jobs). This is the mode
    # that catches vendored-dep shadowing and silent FX3-disable on every build.
    if args.post_build:
        if args.gui_path is None:
            return fail("--post-build requires --gui-path pointing at the built misrc_gui")
        # In post-build mode the binary MUST exist: a missing misrc_gui means the
        # build step failed or the path is wrong, and silently skipping would let
        # CI pass without ever validating vendored-dep linkage or FX3 compilation.
        # (Preflight mode above still skips gracefully when no local build exists.)
        if not args.gui_path.exists():
            return fail(f"--post-build --gui-path does not exist (build did not produce misrc_gui?): {args.gui_path}")
        checks.append(("built GUI links vendored hsdaoh (post-build)", lambda: check_built_gui_links_vendored_hsdaoh(repo_root, args.gui_path)))
        checks.append(("built GUI has FX3 symbols (post-build)", lambda: check_built_gui_has_fx3_symbols(repo_root, args.gui_path)))

    for name, check in checks:
        rc = check()
        if rc != 0:
            print(f"FAILED: {name}", file=sys.stderr)
            return rc
        print(f"PASS: {name}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
