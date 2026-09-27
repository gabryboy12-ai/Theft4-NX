# =============================================================================
# compile_dxil_shaders.ps1
# Compiles all LibertyRecomp HLSL shaders to DXIL .h headers using the
# bundled DXC. Run this from the repo root on a Windows machine before
# building with the x64-Clang-Release CMake preset.
#
# Usage:
#   cd C:\path\to\LibertyRecomp
#   .\tools\compile_dxil_shaders.ps1
#
# The generated *.dxil.h files are written next to their source .hlsl and
# are #included directly by video.cpp / postprocess_renderer.cpp at build time.
# =============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ── Locate DXC ────────────────────────────────────────────────────────────────
$RepoRoot  = $PSScriptRoot | Split-Path -Parent
$DxcExe    = Join-Path $RepoRoot "tools\XenosRecomp\thirdparty\dxc-bin\bin\x64\dxc.exe"
$DxilDll   = Join-Path $RepoRoot "tools\XenosRecomp\thirdparty\dxc-bin\bin\x64\dxil.dll"
$ShaderDir = Join-Path $RepoRoot "LibertyRecomp\gpu\shader\hlsl"

if (-not (Test-Path $DxcExe)) {
    Write-Error "dxc.exe not found at: $DxcExe`nMake sure submodules are initialised (git submodule update --init --recursive)"
}

# dxil.dll must be beside dxc.exe for signing to work — copy to CWD if needed
if (-not (Test-Path (Join-Path (Get-Location) "dxil.dll"))) {
    Copy-Item $DxilDll (Get-Location)
    Write-Host "[setup] Copied dxil.dll to working directory for DXIL signing" -ForegroundColor Cyan
}

# ── Include paths ─────────────────────────────────────────────────────────────
# shader_common.h lives in tools/XenosRecomp/XenosRecomp/
# imgui_common.h lives in LibertyRecomp/gpu/imgui/
$IncludeShaderCommon = Join-Path $RepoRoot "tools\XenosRecomp\XenosRecomp"
$IncludeImgui        = Join-Path $RepoRoot "LibertyRecomp\gpu"
$IncludeHlsl         = $ShaderDir   # for local .hlsli includes

# ── Helper: derive C array name from file stem ────────────────────────────────
# e.g. "taa_ps" -> "g_taa_ps_dxil"
function Get-ArrayName([string]$stem) {
    return "g_${stem}_dxil"
}

# ── Helper: compile one shader ────────────────────────────────────────────────
function Compile-Shader {
    param(
        [string]$File,       # filename without .hlsl, relative to ShaderDir
        [string]$Target,     # e.g. ps_6_0 / vs_6_0
        [string[]]$Defines   # extra -D flags
    )

    $hlslPath = Join-Path $ShaderDir "$File.hlsl"
    $dxilPath = Join-Path $ShaderDir "$File.hlsl.dxil.h"
    $arrayName = Get-ArrayName ($File -replace '[/\\]', '_' -replace '\.', '_')

    if (-not (Test-Path $hlslPath)) {
        Write-Warning "  SKIP  $File.hlsl  (file not found)"
        return
    }

    # Build argument list
    $args = @(
        "-T",  $Target,
        "-HV", "2021",
        "-all-resources-bound",
        "-Wno-ignored-attributes",
        "-E",  "shaderMain",
        "-Fh", $dxilPath,
        "-Vn", $arrayName,
        "-I",  $IncludeShaderCommon,
        "-I",  $IncludeImgui,
        "-I",  $IncludeHlsl,
        "-DLIBERTY_RECOMP"
    )

    foreach ($def in $Defines) { $args += "-D$def" }
    $args += $hlslPath

    Write-Host "  compile  $File  [$Target]" -NoNewline
    $result = & $DxcExe @args 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAILED" -ForegroundColor Red
        Write-Host $result -ForegroundColor Red
        $script:FailCount++
    } else {
        Write-Host "  OK" -ForegroundColor Green
        $script:OkCount++
    }
}

# ── Shader table ──────────────────────────────────────────────────────────────
# Each entry: File, Target, ExtraDefines[]
# Mirrors the compile_vertex_shader / compile_pixel_shader calls in CMakeLists.txt

$Shaders = @(
    # Core video shaders (video.cpp)
    @{ File="blend_color_alpha_ps";    Target="ps_6_0"; Defines=@() }
    @{ File="copy_vs";                 Target="vs_6_0"; Defines=@() }
    @{ File="copy_color_ps";           Target="ps_6_0"; Defines=@() }
    @{ File="copy_depth_ps";           Target="ps_6_0"; Defines=@() }
    @{ File="csd_filter_ps";           Target="ps_6_0"; Defines=@() }
    @{ File="csd_no_tex_vs";           Target="vs_6_0"; Defines=@() }
    @{ File="csd_vs";                  Target="vs_6_0"; Defines=@() }
    @{ File="enhanced_burnout_blur_vs";Target="vs_6_0"; Defines=@() }
    @{ File="enhanced_burnout_blur_ps";Target="ps_6_0"; Defines=@() }
    @{ File="gaussian_blur_3x3";       Target="ps_6_0"; Defines=@() }
    @{ File="gaussian_blur_5x5";       Target="ps_6_0"; Defines=@() }
    @{ File="gaussian_blur_7x7";       Target="ps_6_0"; Defines=@() }
    @{ File="gaussian_blur_9x9";       Target="ps_6_0"; Defines=@() }
    @{ File="gamma_correction_ps";     Target="ps_6_0"; Defines=@() }
    @{ File="hdr_tonemap_ps";          Target="ps_6_0"; Defines=@() }
    @{ File="imgui_ps";                Target="ps_6_0"; Defines=@() }
    @{ File="imgui_vs";                Target="vs_6_0"; Defines=@() }
    @{ File="resolve_msaa_color_2x";   Target="ps_6_0"; Defines=@() }
    @{ File="resolve_msaa_color_4x";   Target="ps_6_0"; Defines=@() }
    @{ File="resolve_msaa_color_8x";   Target="ps_6_0"; Defines=@() }
    @{ File="resolve_msaa_depth_2x";   Target="ps_6_0"; Defines=@() }
    @{ File="resolve_msaa_depth_4x";   Target="ps_6_0"; Defines=@() }
    @{ File="resolve_msaa_depth_8x";   Target="ps_6_0"; Defines=@() }

    # Post-process shaders (postprocess_renderer.cpp)
    @{ File="fullscreen_vs";           Target="vs_6_0"; Defines=@() }
    @{ File="taa_ps";                  Target="ps_6_0"; Defines=@() }
    @{ File="smaa_edge_detect_ps";     Target="ps_6_0"; Defines=@("SMAA_HLSL_4_1", "SMAA_PRESET_HIGH") }
    @{ File="smaa_blend_ps";           Target="ps_6_0"; Defines=@("SMAA_HLSL_4_1", "SMAA_PRESET_HIGH") }
    @{ File="smaa_neighborhood_blend_ps"; Target="ps_6_0"; Defines=@("SMAA_HLSL_4_1", "SMAA_PRESET_HIGH") }
    @{ File="fsr1_easu_ps";            Target="ps_6_0"; Defines=@() }
    @{ File="fsr1_rcas_ps";            Target="ps_6_0"; Defines=@() }

    # Stubbed / in-progress shaders (compile but don't enable in renderer yet)
    @{ File="ssao_gtao_ps";            Target="ps_6_0"; Defines=@() }
    @{ File="ssao_blur_ps";            Target="ps_6_0"; Defines=@() }
    @{ File="ssao_composite_ps";       Target="ps_6_0"; Defines=@() }
    @{ File="dof_ps";                  Target="ps_6_0"; Defines=@() }
    @{ File="dof_bokeh_ps";            Target="ps_6_0"; Defines=@() }
    @{ File="dof_combine_ps";          Target="ps_6_0"; Defines=@() }
    @{ File="dof_postfilter_ps";       Target="ps_6_0"; Defines=@() }
    @{ File="dof_prefilter_ps";        Target="ps_6_0"; Defines=@() }
    @{ File="ssr_raytrace_ps";         Target="ps_6_0"; Defines=@() }
    @{ File="ssr_composite_ps";        Target="ps_6_0"; Defines=@() }
    @{ File="film_grain_ps";           Target="ps_6_0"; Defines=@() }
    @{ File="chromatic_aberration_ps"; Target="ps_6_0"; Defines=@() }
    @{ File="motion_blur_camera_ps";   Target="ps_6_0"; Defines=@() }
    @{ File="sunshafts_prepass_ps";    Target="ps_6_0"; Defines=@() }
    @{ File="sunshafts_radial_ps";     Target="ps_6_0"; Defines=@() }
    @{ File="sunshafts_composite_ps";  Target="ps_6_0"; Defines=@() }
    @{ File="vignette_ps";             Target="ps_6_0"; Defines=@() }
)

# ── Run ───────────────────────────────────────────────────────────────────────
$script:OkCount   = 0
$script:FailCount = 0

Write-Host ""
Write-Host "LibertyRecomp DXIL shader compiler" -ForegroundColor White
Write-Host "  DXC:       $DxcExe"
Write-Host "  Shader dir: $ShaderDir"
Write-Host "  Shaders:   $($Shaders.Count)"
Write-Host ""

foreach ($s in $Shaders) {
    Compile-Shader -File $s.File -Target $s.Target -Defines $s.Defines
}

Write-Host ""
Write-Host "Done: $($script:OkCount) succeeded, $($script:FailCount) failed." -ForegroundColor $(if ($script:FailCount -eq 0) { "Green" } else { "Red" })

if ($script:FailCount -gt 0) { exit 1 }
