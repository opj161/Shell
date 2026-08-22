<#
.SYNOPSIS
    Canonical multi-architecture build and test script for Nilesoft Shell.
.PARAMETER Platform
    Architecture to build: 'all', 'x64', 'x86', or 'arm64'. Default: 'x64'.
.PARAMETER Configuration
    Build configuration: 'Release' or 'Debug'. Default: 'Release'.
.EXAMPLE
    .\build.ps1 -Platform x64
    .\build.ps1 -Platform all
#>
[CmdletBinding()]
param(
    [ValidateSet("all", "x64", "x86", "arm64")]
    [string]$Platform = "x64",

    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

# Locate MSBuild
$msbuildCandidates = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
)

$msbuildPath = $msbuildCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $msbuildPath) {
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { $msbuildPath = $cmd.Source }
}

if (-not $msbuildPath) {
    throw "MSBuild.exe was not found. Please install Visual Studio / Build Tools or add MSBuild to PATH."
}

$solutionPath = Join-Path $PSScriptRoot "src\Shell.sln"
$platforms = if ($Platform -eq "all") { @("x64", "x86", "arm64") } else { @($Platform) }

Write-Host "Using MSBuild: $msbuildPath" -ForegroundColor DarkGray

foreach ($p in $platforms) {
    Write-Host "`n========================================================" -ForegroundColor Cyan
    Write-Host " Building Platform: $p | Configuration: $Configuration" -ForegroundColor Cyan
    Write-Host "========================================================" -ForegroundColor Cyan

    & $msbuildPath /m /p:Configuration=$Configuration /p:Platform=$p $solutionPath
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed for platform $p ($Configuration)."
    }

    # Run unit tests on compatible host architectures
    $testExe = Join-Path $PSScriptRoot "src\bin\$p\tests.exe"
    if (Test-Path $testExe) {
        $canRun = ($p -eq "x64" -and [Environment]::Is64BitOperatingSystem) -or 
                  ($p -eq "x86" -and ([Environment]::Is64BitOperatingSystem -or -not [Environment]::Is64BitOperatingSystem))

        if ($canRun) {
            Write-Host "`nRunning Unit Tests for $p..." -ForegroundColor Green
            & $testExe
            if ($LASTEXITCODE -ne 0) {
                throw "Unit tests failed for platform $p."
            }
        } else {
            Write-Host "Skipping execution of $p tests on current host architecture." -ForegroundColor Yellow
        }
    }
}

# Source-level invariants: patterns the refactor removed and that must not come
# back. Cheap, host-independent, and unlike the unit suite it runs on every
# platform - so a cross-compiled-only build still gets checked.
$invariants = Join-Path $PSScriptRoot "scripts\check-invariants.ps1"
if (Test-Path $invariants) {
    Write-Host "`nChecking source invariants..." -ForegroundColor Green
    & powershell -NoProfile -ExecutionPolicy Bypass -File $invariants
    if ($LASTEXITCODE -ne 0) {
        throw "Source invariant check failed."
    }
}

Write-Host "`nAll requested builds and tests completed successfully!" -ForegroundColor Green
