
param(
    [switch]$Rebuild,
    [switch]$GenCommon
)

$ErrorActionPreference = "Stop"

$shaderSrc = Join-Path $PSScriptRoot "vkpt\Source\Shaders"
$shaderOut = Join-Path $PSScriptRoot "vkpt\Build"
$destDir   = Join-Path $PSScriptRoot "build\Debug\id1\shaders"

if ($env:VULKAN_SDK) {
    $sdkBin = Join-Path $env:VULKAN_SDK "Bin"
    if (Test-Path (Join-Path $sdkBin "glslc.exe")) {
        $env:PATH = "$sdkBin;$env:PATH"
    }
}
if (-not (Get-Command glslc -ErrorAction SilentlyContinue)) {
    throw "glslc not found. Install the Vulkan SDK or set VULKAN_SDK."
}

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

Push-Location $shaderSrc
try {
    python GenerateShaders.py @genArgs
    if ($LASTEXITCODE -ne 0) { throw "GenerateShaders.py failed (exit $LASTEXITCODE)." }
}
finally {
    Pop-Location
}

if (-not (Test-Path $destDir)) {
    New-Item -ItemType Directory -Path $destDir -Force | Out-Null
}

$srcNames = @(Get-ChildItem -Path (Join-Path $shaderOut "*.spv") | Select-Object -ExpandProperty Name)

$stale = Get-ChildItem -Path (Join-Path $destDir "*.spv") | Where-Object { $srcNames -notcontains $_.Name }
foreach ($f in $stale) {
    Write-Host "Removing stale shader: $($f.Name)" -ForegroundColor Yellow
    Remove-Item $f.FullName -Force
}

$copied = Copy-Item -Path (Join-Path $shaderOut "*.spv") -Destination $destDir -Force -PassThru
Write-Host "Deployed $($copied.Count) shader(s) to $destDir" -ForegroundColor Green
