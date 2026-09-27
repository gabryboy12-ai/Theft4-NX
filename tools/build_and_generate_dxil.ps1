# =============================================================================
# build_and_generate_dxil.ps1
# Full Windows pipeline: build rage_fxc_extractor + XenosRecomp, extract
# Xbox 360 shaders from fxl_final, then generate shader_cache.cpp with DXIL.
#
# Run from repo root (no VS Developer Prompt required):
#   powershell -ExecutionPolicy Bypass -File .\tools\build_and_generate_dxil.ps1
# =============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot

# --- Locate cmake -----------------------------------------------------------
$CMake = $null
$CMakeCandidates = @(
    "C:\tools\cmake\bin\cmake.exe",
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\ProgramData\chocolatey\bin\cmake.exe"
)
foreach ($candidate in $CMakeCandidates) {
    if (Test-Path $candidate) { $CMake = $candidate; break }
}
if (-not $CMake) {
    $cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmakeCmd) { $CMake = $cmakeCmd.Source }
}
if (-not $CMake) {
    Write-Error "cmake not found. Install from https://cmake.org/download/ and tick 'Add to PATH'."
}
Write-Host "  Using cmake: $CMake"

# --- Locate VS install via vswhere -------------------------------------------
$VSInstallPath = $null
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $VSInstallPath = (& $vswhere -latest -property installationPath 2>$null).Trim()
    Write-Host "  VS install: $VSInstallPath"
}
if (-not $VSInstallPath) {
    foreach ($p in @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
    )) {
        if (Test-Path $p) { $VSInstallPath = $p; break }
    }
}
if (-not $VSInstallPath) {
    Write-Error "Visual Studio 2022 not found. Install it with the 'Desktop development with C++' workload."
}

# --- Locate Ninja (bundled with VS) ------------------------------------------
$Ninja = $null
$NinjaCandidates = @(
    "$VSInstallPath\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "C:\tools\ninja\ninja.exe",
    "C:\ProgramData\chocolatey\bin\ninja.exe"
)
foreach ($candidate in $NinjaCandidates) {
    if (Test-Path $candidate) { $Ninja = $candidate; break }
}
if (-not $Ninja) {
    $ninjaCmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($ninjaCmd) { $Ninja = $ninjaCmd.Source }
}
if (-not $Ninja) {
    Write-Error "ninja.exe not found under VS install. Reinstall VS 2022 with 'Desktop development with C++' workload."
}
Write-Host "  Using ninja: $Ninja"

# --- Locate vcvars64.bat -----------------------------------------------------
$VcVars = "$VSInstallPath\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $VcVars)) {
    Write-Error "vcvars64.bat not found at: $VcVars"
}
Write-Host "  Using vcvars: $VcVars"

# --- Paths ------------------------------------------------------------------
$FxcExtractorBuild = Join-Path $RepoRoot "tools\rage_fxc_extractor\build"
$FxcExtractorExe   = Join-Path $FxcExtractorBuild "Release\rage_fxc_extractor.exe"

$XenosRecompBuild  = Join-Path $RepoRoot "tools\XenosRecomp\build_win"
# Ninja puts exe directly in the target subdir (no Release/ subfolder)
$XenosRecompExe    = Join-Path $XenosRecompBuild "XenosRecomp\XenosRecomp.exe"

$FxlFinalDir         = Join-Path $RepoRoot "LibertyRecompLib\private\fxl_final"
$ExtractedShadersDir = Join-Path $RepoRoot "extracted_shaders"
$ShaderCacheCpp      = Join-Path $RepoRoot "LibertyRecompLib\shader\shader_cache.cpp"
$ShaderCommonH       = Join-Path $RepoRoot "tools\XenosRecomp\XenosRecomp\shader_common.h"
$DxilDll             = Join-Path $RepoRoot "tools\XenosRecomp\thirdparty\dxc-bin\bin\x64\dxil.dll"

# --- Helpers ----------------------------------------------------------------
function Step([string]$msg) {
    Write-Host ""
    Write-Host "--- $msg ---" -ForegroundColor Cyan
}

function Run {
    param([string]$exe, [string[]]$arguments)
    Write-Host "  > $exe $($arguments -join ' ')"
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "Command failed (exit $LASTEXITCODE): $exe" }
}

# Run cmake inside a vcvars64 + Ninja environment.
# Uses pushd to map the UNC working directory to a drive letter so cmd.exe
# can use it as CWD (cmd.exe refuses UNC paths as working directory).
function Run-InVsEnv {
    param([string]$workDir, [string[]]$cmakeArgs)
    $argStr = $cmakeArgs -join ' '
    $bat = [System.IO.Path]::GetTempFileName() -replace '\.tmp$', '.bat'
    @"
@echo off
pushd "$workDir"
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
call "$VcVars" >nul 2>&1
set PATH=$([System.IO.Path]::GetDirectoryName($Ninja));%PATH%
"$CMake" $argStr
set EC=%ERRORLEVEL%
popd
exit /b %EC%
"@ | Set-Content $bat -Encoding ASCII
    Write-Host "  > [vcvars] cmake $argStr"
    cmd /c $bat
    $ec = $LASTEXITCODE
    Remove-Item $bat -ErrorAction SilentlyContinue
    if ($ec -ne 0) { throw "cmake command failed (exit $ec)" }
}

function Reset-BuildDirIfStale {
    param([string]$buildDir, [string]$expectedGenerator)
    $cacheFile = Join-Path $buildDir "CMakeCache.txt"
    if (-not (Test-Path $cacheFile)) { return }
    $cacheContent = Get-Content $cacheFile -Raw
    if ($cacheContent -notmatch [regex]::Escape($expectedGenerator)) {
        Write-Host "  Stale CMakeCache (wrong generator). Wiping $buildDir ..." -ForegroundColor Yellow
        Remove-Item -Recurse -Force $buildDir
    }
}

# --- 0. Validate fxl_final --------------------------------------------------
Step "Checking shader source directory"
if (-not (Test-Path $FxlFinalDir)) { Write-Error "fxl_final not found at: $FxlFinalDir" }
$FxcCount = (Get-ChildItem -LiteralPath $FxlFinalDir -Filter "*.fxc").Count
Write-Host "  Found $FxcCount .fxc files" -ForegroundColor Green

# --- 1. Build rage_fxc_extractor (VS generator - no PCH) --------------------
Step "Building rage_fxc_extractor"
if (Test-Path $FxcExtractorExe) {
    Write-Host "  Already built, skipping." -ForegroundColor DarkGray
} else {
    Reset-BuildDirIfStale $FxcExtractorBuild "Visual Studio"
    New-Item -ItemType Directory -Force -Path $FxcExtractorBuild | Out-Null
    Push-Location $FxcExtractorBuild
    try {
        $cmakeHelp = & $CMake --help 2>&1 | Out-String
        $vsGen = "Visual Studio 17 2022"
        foreach ($g in @("Visual Studio 17 2022","Visual Studio 16 2019","Visual Studio 15 2017")) {
            if ($cmakeHelp -match [regex]::Escape($g)) { $vsGen = $g; break }
        }
        Run $CMake @("..", "-G", $vsGen, "-A", "x64", "-DCMAKE_BUILD_TYPE=Release")
        Run $CMake @("--build", ".", "--config", "Release")
    } finally {
        Pop-Location
    }
}
Write-Host "  $FxcExtractorExe" -ForegroundColor Green

# --- 2. Build XenosRecomp with Ninja + MSVC ----------------------------------
Step "Building XenosRecomp (DXIL enabled, Ninja generator)"
if (Test-Path $XenosRecompExe) {
    Write-Host "  Already built, skipping." -ForegroundColor DarkGray
} else {
    Reset-BuildDirIfStale $XenosRecompBuild "Ninja"
    New-Item -ItemType Directory -Force -Path $XenosRecompBuild | Out-Null
    $xenosSrc = Join-Path $RepoRoot "tools\XenosRecomp"
    Run-InVsEnv $XenosRecompBuild @("$xenosSrc", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DXENOS_RECOMP_DXIL=ON", "-DCMAKE_SYSTEM_PROCESSOR=x86_64", "-DCMAKE_SYSTEM_NAME=Windows")
    Run-InVsEnv $XenosRecompBuild @("--build", ".", "--target", "XenosRecomp")
}
Write-Host "  $XenosRecompExe" -ForegroundColor Green

# --- 3. dxil.dll beside the process -----------------------------------------
Step "Setting up dxil.dll"
$dxilDest = Join-Path $RepoRoot "dxil.dll"
if (-not (Test-Path $dxilDest)) {
    Copy-Item $DxilDll $dxilDest
    Write-Host "  Copied dxil.dll to repo root" -ForegroundColor Green
} else {
    Write-Host "  dxil.dll already present" -ForegroundColor DarkGray
}

# --- 4. Extract Xbox 360 shaders --------------------------------------------
Step "Extracting Xbox 360 shaders from fxl_final"
$existingFiles = 0
if (Test-Path $ExtractedShadersDir) {
    $existingFiles = (Get-ChildItem -LiteralPath $ExtractedShadersDir -Recurse -File).Count
}
if ($existingFiles -gt 0) {
    Write-Host "  extracted_shaders\ already has $existingFiles files, skipping." -ForegroundColor DarkGray
} else {
    New-Item -ItemType Directory -Force -Path $ExtractedShadersDir | Out-Null
    Run $FxcExtractorExe @("--batch", $FxlFinalDir, $ExtractedShadersDir)
    $extracted = (Get-ChildItem -LiteralPath $ExtractedShadersDir -Recurse -File).Count
    Write-Host "  Extracted $extracted shader files" -ForegroundColor Green
}

# --- 5. Generate shader_cache.cpp with DXIL ---------------------------------
Step "Generating shader_cache.cpp"
if (Test-Path $ShaderCacheCpp) {
    Copy-Item $ShaderCacheCpp "$ShaderCacheCpp.bak" -Force
    Write-Host "  Backed up existing shader_cache.cpp"
}
# dxil.dll must be next to the process at runtime
$env:PATH = "$RepoRoot;$env:PATH"
Run $XenosRecompExe @($ExtractedShadersDir, $ShaderCacheCpp, $ShaderCommonH)
$sizeMB = [math]::Round((Get-Item $ShaderCacheCpp).Length / 1MB, 1)
Write-Host "  Generated shader_cache.cpp ($sizeMB MB)" -ForegroundColor Green

# --- 6. Compile post-process HLSL -> .dxil.h --------------------------------
Step "Compiling post-process HLSL shaders"
$ps1 = Join-Path $RepoRoot "tools\compile_dxil_shaders.ps1"
powershell -ExecutionPolicy Bypass -File $ps1
if ($LASTEXITCODE -ne 0) { throw "compile_dxil_shaders.ps1 failed" }

# --- Done -------------------------------------------------------------------
Write-Host ""
Write-Host "All done! Run the build:" -ForegroundColor Green
Write-Host "  cmake . --preset x64-Clang-Release"
Write-Host "  cmake --build .\out\build\x64-Clang-Release --target LibertyRecomp"
Write-Host ""
