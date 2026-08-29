# Compiles GLSL shaders to SPIR-V and deploys them to build\Debug\id1\shaders.
# Usage: .\build_shaders.ps1 [-Rebuild] [-GenCommon]
#   -Rebuild   : clear the shader cache and recompile everything
#   -GenCommon : also regenerate the shader-common headers (GenerateShaderCommon.py)
#
# Requires the Vulkan SDK (glslc). Prefers $env:VULKAN_SDK\Bin if set.

param(
    [switch]$Rebuild,
    [switch]$GenCommon
)

$ErrorActionPreference = "Stop"

$shaderSrc = Join-Path $PSScriptRoot "vkpt\Source\Shaders"
$shaderOut = Join-Path $PSScriptRoot "vkpt\Build"
$destDir   = Join-Path $PSScriptRoot "build\Debug\id1\shaders"

# Make glslc available (Vulkan SDK). Prefer the SDK's Bin dir when VULKAN_SDK is set.
if ($env:VULKAN_SDK) {
    $sdkBin = Join-Path $env:VULKAN_SDK "Bin"
    if (Test-Path (Join-Path $sdkBin "glslc.exe")) {
        $env:PATH = "$sdkBin;$env:PATH"
    }
}
if (-not (Get-Command glslc -ErrorAction SilentlyContinue)) {
    throw "glslc not found. Install the Vulkan SDK or set VULKAN_SDK."
}

# Build the shader-common headers first when requested.
if ($GenCommon) {
    Push-Location (Join-Path $PSScriptRoot "vkpt\Source\Generated")
    try {
        python GenerateShaderCommon.py --path .
        if ($LASTEXITCODE -ne 0) { throw "GenerateShaderCommon.py failed (exit $LASTEXITCODE)." }
    }
    finally {
        Pop-Location
    }
}

$genArgs = @()
if ($GenCommon) { $genArgs += "-gencomm" }
if ($Rebuild)  { $genArgs += "-rebuild" }
$genArgs += "-psout"

# GenerateShaders.py uses cwd-relative paths; it must run from vkpt\Source\Shaders.
Push-Location $shaderSrc
try {
    python GenerateShaders.py @genArgs
    if ($LASTEXITCODE -ne 0) { throw "GenerateShaders.py failed (exit $LASTEXITCODE)." }
}
finally {
    Pop-Location
}

# Deploy: mirror the freshly built SPIR-V into the game override folder,
# removing any stale .spv that no longer exist in vkpt\Build.
if (-not (Test-Path $destDir)) {
    New-Item -ItemType Directory -Path $destDir -Force | Out-Null
}

$srcNames = @(Get-ChildItem -Path (Join-Path $shaderOut "*.spv") | Select-Object -ExpandProperty Name)

# Remove stale .spv files from the destination.
$stale = Get-ChildItem -Path (Join-Path $destDir "*.spv") | Where-Object { $srcNames -notcontains $_.Name }
foreach ($f in $stale) {
    Write-Host "Removing stale shader: $($f.Name)" -ForegroundColor Yellow
    Remove-Item $f.FullName -Force
}

# Copy fresh .spv files.
$copied = Copy-Item -Path (Join-Path $shaderOut "*.spv") -Destination $destDir -Force -PassThru
Write-Host "Deployed $($copied.Count) shader(s) to $destDir" -ForegroundColor Green
