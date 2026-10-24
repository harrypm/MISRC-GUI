#!/usr/bin/env pwsh
[CmdletBinding()]
param(
    [string]$BuildDir = "build-local",
    [ValidateSet("debug", "debugoptimized", "release", "minsize")]
    [string]$BuildType = "release",
    [switch]$All,
    [switch]$Clean,
    [switch]$SkipSmoke,
    [switch]$BootstrapOnly,
    [switch]$NoAutoInstall
)

$ErrorActionPreference = "Stop"

function Write-Log {
    param([string]$Message)
    Write-Host "[build-local.ps1] $Message"
}

function Fail {
    param([string]$Message)
    throw "[build-local.ps1] ERROR: $Message"
}

function Require-Command {
    param(
        [string]$Name,
        [string]$Hint = ""
    )
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        if ($Hint) {
            Fail "Missing required tool '$Name'. $Hint"
        }
        Fail "Missing required tool '$Name'."
    }
}

function Add-PythonUserScriptsToPath {
    param([string]$PythonExe)
    $scriptsPath = ""
    try {
        $scriptsPath = (& $PythonExe -c "import sysconfig; print(sysconfig.get_path('scripts', scheme='nt_user'))" 2>$null | Select-Object -Last 1).Trim()
    } catch {
        $scriptsPath = ""
    }
    if (-not $scriptsPath) {
        $userBase = (& $PythonExe -m site --user-base 2>$null | Select-Object -Last 1).Trim()
        if (-not $userBase) {
            return
        }
        $scriptsPath = Join-Path $userBase "Scripts"
    }
    if (-not (Test-Path $scriptsPath)) {
        return
    }

    $pathParts = @()
    if ($env:PATH) {
        $pathParts = $env:PATH -split ';'
    }
    if ($pathParts -contains $scriptsPath) {
        return
    }

    $env:PATH = "$scriptsPath;$env:PATH"
    Write-Log "Added Python user Scripts path for this session: $scriptsPath"
}

function Ensure-MesonAndNinja {
    param([string]$PythonExe)

    Add-PythonUserScriptsToPath -PythonExe $PythonExe

    $hasMesonModule = $false
    try {
        & $PythonExe -c "import mesonbuild" | Out-Null
        $hasMesonModule = ($LASTEXITCODE -eq 0)
    } catch {
        $hasMesonModule = $false
    }

    $hasNinjaExe = [bool](Get-Command ninja -ErrorAction SilentlyContinue)
    $hasNinjaModule = $false
    try {
        & $PythonExe -c "import ninja" | Out-Null
        $hasNinjaModule = ($LASTEXITCODE -eq 0)
    } catch {
        $hasNinjaModule = $false
    }

    if (-not ($hasMesonModule -and ($hasNinjaExe -or $hasNinjaModule))) {
        if ($NoAutoInstall) {
            Fail "Meson/Ninja is not available. Re-run without -NoAutoInstall or install with: python -m pip install --user --upgrade meson ninja"
        }
        Write-Log "Meson/Ninja not fully available; installing/updating user packages..."
        & $PythonExe -m pip install --user --upgrade meson ninja
        if ($LASTEXITCODE -ne 0) {
            Fail "pip install for meson/ninja failed."
        }
        Add-PythonUserScriptsToPath -PythonExe $PythonExe
    }

    & $PythonExe -m mesonbuild.mesonmain --version | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Fail "Meson module bootstrap failed (python -m mesonbuild.mesonmain --version)."
    }

    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        Fail "Ninja executable is still unavailable on PATH. Install or expose it, then retry."
    }
}

function Invoke-Meson {
    param(
        [string]$PythonExe,
        [string[]]$MesonArgs
    )
    & $PythonExe -m mesonbuild.mesonmain @MesonArgs
    if ($LASTEXITCODE -ne 0) {
        Fail "Meson command failed: meson $($MesonArgs -join ' ')"
    }
}

function Resolve-DepsPrefix {
    param([string]$RepoRoot)

    $candidates = @()
    if ($env:MISRC_DEPS_PREFIX) {
        $candidates += $env:MISRC_DEPS_PREFIX
    }
    $candidates += (Join-Path $RepoRoot ".deps/install")
    $candidates += (Join-Path $RepoRoot ".deps/install-appimage-local")

    foreach ($candidate in $candidates) {
        if (-not $candidate -or -not (Test-Path $candidate)) {
            continue
        }
        $pcFiles = @(
            "lib/pkgconfig/hsdaoh.pc",
            "lib/pkgconfig/libhsdaoh.pc",
            "lib64/pkgconfig/hsdaoh.pc",
            "lib64/pkgconfig/libhsdaoh.pc"
        )
        foreach ($rel in $pcFiles) {
            if (Test-Path (Join-Path $candidate $rel)) {
                return (Resolve-Path $candidate).Path
            }
        }
    }

    return $null
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir
$sourceRoot = Join-Path $repoRoot "misrc_tools"
$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repoRoot $BuildDir }

function ConvertTo-MsysPath {
    param([string]$WinPath)
    $p = $WinPath -replace '\\','/'
    if ($p -match '^([A-Za-z]):') {
        $drive = $matches[1].ToLower()
        $p = '/' + $drive + $p.Substring(2)
    }
    return $p
}

function Invoke-DepsBuild {
    # Ensure vendored deps (.deps/install) exist and are fresh by delegating to
    # scripts/build-deps-windows.sh under MSYS2 UCRT64. That script mirrors the
    # windows-exe CI deps block (static libuvc + hsdaoh + raylib) and self-skips
    # via a content-addressed stamp when inputs are unchanged, so this is cheap
    # (a stamp hash, no rebuild) on subsequent runs. This keeps local == CI and
    # means a fresh terminal just runs `pwsh -File scripts/build-local.ps1`.
    $depsScript = Join-Path $scriptDir "build-deps-windows.sh"
    if (-not (Test-Path $depsScript)) {
        Fail "Deps build script not found: $depsScript"
    }
    $envExe = "C:\msys64\usr\bin\env.exe"
    $bashExe = "C:\msys64\usr\bin\bash.exe"
    if (-not (Test-Path $envExe) -or -not (Test-Path $bashExe)) {
        Fail "MSYS2 not found at C:\msys64. Install MSYS2 (ucrt64 environment) to build deps locally, or set MISRC_DEPS_PREFIX to a prebuilt .deps/install."
    }
    $depsScriptMsys = ConvertTo-MsysPath $depsScript
    Write-Log "Ensuring local deps via MSYS2 UCRT64 (skips instantly if already built): $depsScript"
    & $envExe MSYSTEM=UCRT64 $bashExe -lc "bash '$depsScriptMsys'"
    if ($LASTEXITCODE -ne 0) {
        Fail "Deps build script failed (see output above). Fix the reported error and rerun."
    }
}

Require-Command -Name "python" -Hint "Install Python 3 and make sure 'python' is available on PATH."
$pythonExe = (Get-Command python).Source


function Add-PathEntryIfExists {
    param([string]$PathEntry)

    if (-not $PathEntry -or -not (Test-Path $PathEntry)) {
        return
    }

    $pathParts = @()
    if ($env:PATH) {
        $pathParts = $env:PATH -split ';'
    }
    if ($pathParts -contains $PathEntry) {
        return
    }

    $env:PATH = "$PathEntry;$env:PATH"
    Write-Log "Added toolchain path for this session: $PathEntry"
}

function Connect-MsysToolchainPaths {
    # ucrt64 first: it is the CI toolchain (MINGW64 is deprecated by MSYS2);
    # the others stay as fallbacks for machines that only have them installed.
    $candidateDirs = @(
        "C:\msys64\ucrt64\bin",
        "C:\msys64\mingw64\bin",
        "C:\msys64\clang64\bin",
        "C:\msys64\usr\bin"
    )

    foreach ($dir in $candidateDirs) {
        if ((Test-Path (Join-Path $dir "pkg-config.exe")) -or
            (Test-Path (Join-Path $dir "nasm.exe")) -or
            (Test-Path (Join-Path $dir "gcc.exe")) -or
            (Test-Path (Join-Path $dir "clang.exe"))) {
            Add-PathEntryIfExists -PathEntry $dir
        }
    }
}
Connect-MsysToolchainPaths
Ensure-MesonAndNinja -PythonExe $pythonExe

# Force mingw-w64 GCC as the C/C++ compiler so Meson (like CMake) does not pick a
# stray Clang from the inherited Windows PATH. The MSYS2 UCRT64 toolchain is
# what CI's windows-exe job uses; matching it keeps local == CI.
$env:CC = "gcc"
$env:CXX = "g++"

if ($BootstrapOnly) {
    Write-Log "Meson/Ninja bootstrap OK."
    & $pythonExe -m mesonbuild.mesonmain --version
    & ninja --version
    exit 0
}

# CI parity gate: run the same static guard tests CI's preflight runs BEFORE
# any deps/meson work, so a guard break (optional-dep guard consistency,
# workflow contracts, ...) fails the local build first instead of only the
# manually dispatched CI run. Regression e34dce9: libFLAC-guarded struct
# members were referenced outside the guard in gui_playback.c; local builds
# passed and the dispatched CI run failed a day later.
$guardScript = Join-Path $repoRoot "misrc_tools/test/ci_guard_tests.py"
if (-not (Test-Path $guardScript)) {
    Fail "CI guard test script not found: $guardScript"
}
Write-Log "CI guard tests (static) before build"
& $pythonExe $guardScript --static-only
if ($LASTEXITCODE -ne 0) {
    Fail "CI guard tests failed (see output above). Fix the reported errors and rerun."
}

Require-Command -Name "pkg-config" -Hint "Install pkg-config (or provide it in your build shell)."
Require-Command -Name "nasm" -Hint "Install nasm (required by this project build)."

# Ensure vendored deps (.deps/install) exist and are fresh. The deps script is
# the single source of truth: it builds libuvc + hsdaoh + raylib (static) on
# first run and self-skips via a content-addressed stamp on subsequent runs, so
# new terminals reuse until an input changes (local == CI). Skip only when the
# caller points at a prebuilt prefix via MISRC_DEPS_PREFIX.
if (-not $env:MISRC_DEPS_PREFIX) {
    Invoke-DepsBuild -RepoRoot $repoRoot
}

$depsPrefix = Resolve-DepsPrefix -RepoRoot $repoRoot
if (-not $depsPrefix) {
    Fail "No vendored hsdaoh pc file found under .deps/install or .deps/install-appimage-local after deps build. Set MISRC_DEPS_PREFIX to a prebuilt prefix if not using the vendored build."
}
Write-Log "Using deps prefix: $depsPrefix"

$pkgPaths = @(
    (Join-Path $depsPrefix "lib/pkgconfig"),
    (Join-Path $depsPrefix "lib64/pkgconfig")
)
# Append the MSYS2 UCRT64/MINGW64 pkgconfig dirs so pacman-provided deps
# (raylib, fftw3f, flac, libusb-1.0, soxr) are resolvable. Mirrors CI's
# `...:${MINGW_PREFIX}/lib/pkgconfig` in the windows-exe job. ucrt64 first
# (CI toolchain; MINGW64 is deprecated).
foreach ($msys in @("C:\msys64\ucrt64", "C:\msys64\mingw64")) {
    $pcDir = Join-Path $msys "lib/pkgconfig"
    if (Test-Path $pcDir) {
        $pkgPaths += $pcDir
    }
}
$existingPkgPath = if ($env:PKG_CONFIG_PATH) { $env:PKG_CONFIG_PATH } else { "" }
$joinedPkgPath = (($pkgPaths + @($existingPkgPath)) | Where-Object { $_ -and $_.Trim().Length -gt 0 }) -join ';'
$env:PKG_CONFIG_PATH = $joinedPkgPath

if ($env:CMAKE_PREFIX_PATH) {
    $env:CMAKE_PREFIX_PATH = "$depsPrefix;$env:CMAKE_PREFIX_PATH"
} else {
    $env:CMAKE_PREFIX_PATH = $depsPrefix
}

& pkg-config --exists fftw3f
if ($LASTEXITCODE -ne 0) {
    Fail "Missing fftw3f pkg-config module. Install FFTW3 dev files and retry."
}

& pkg-config --exists libzstd
if ($LASTEXITCODE -ne 0) { Fail "Missing libzstd; install mingw-w64-ucrt-x86_64-zstd" }
$fftwVersion = (& pkg-config --modversion fftw3f).Trim()
Write-Log "fftw3f: $fftwVersion"

if ($Clean -and (Test-Path $buildPath)) {
    Write-Log "Cleaning build directory: $buildPath"
    Remove-Item -Recurse -Force $buildPath
}

$coreDataPath = Join-Path $buildPath "meson-private/coredata.dat"
if (Test-Path $coreDataPath) {
    Invoke-Meson -PythonExe $pythonExe -MesonArgs @("setup", $buildPath, $sourceRoot, "--buildtype", $BuildType, "--wipe")
} else {
    Invoke-Meson -PythonExe $pythonExe -MesonArgs @("setup", $buildPath, $sourceRoot, "--buildtype", $BuildType)
}

if ($All) {
    Invoke-Meson -PythonExe $pythonExe -MesonArgs @("compile", "-C", $buildPath, "misrc_capture", "misrc_extract", "misrc_gui")
} else {
    Invoke-Meson -PythonExe $pythonExe -MesonArgs @("compile", "-C", $buildPath, "misrc_gui")
}

if (-not $SkipSmoke) {
    $guiExe = Join-Path $buildPath "misrc_gui.exe"
    if (-not (Test-Path $guiExe)) {
        $guiExe = Join-Path $buildPath "misrc_gui"
    }
    if (-not (Test-Path $guiExe)) {
        Fail "Built GUI executable not found under $buildPath."
    }
    Write-Log "Smoke test: $guiExe --smoke-test"
    & $guiExe --smoke-test
    if ($LASTEXITCODE -ne 0) {
        Fail "Smoke test failed."
    }
}

Write-Log "OK: build path is connected and local build succeeded."
