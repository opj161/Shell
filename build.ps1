<#
.SYNOPSIS
    Canonical multi-architecture build and test script for Nilesoft Shell.
.PARAMETER Platform
    Architecture to build: 'all', 'x64', 'x86', or 'arm64'. Default: 'x64'.
.PARAMETER Configuration
    Build configuration: 'Release' or 'Debug'. Default: 'Release'.
.PARAMETER AllowNoVCLTL
    Build without VC-LTL, linking the static CRT instead. Skips the package
    restore. The result is roughly 361 KB larger per binary and is NOT what CI
    ships, so use this only to build offline or to bisect a CRT-sensitive bug.
.EXAMPLE
    .\build.ps1 -Platform x64
    .\build.ps1 -Platform all
    .\build.ps1 -Platform x64 -AllowNoVCLTL
#>
[CmdletBinding()]
param(
    [ValidateSet("all", "x64", "x86", "arm64")]
    [string]$Platform = "x64",

    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",

    [switch]$AllowNoVCLTL
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

# VC-LTL is declared in three packages.config files, and `msbuild /restore` does
# not restore that format: RestorePackagesConfig is "An opt-in switch, that
# restores projects with packages.config. Support with MSBuild -t:restore only."
# https://learn.microsoft.com/en-us/nuget/reference/msbuild-targets
#
# This script used to run no restore at all, so local builds silently linked the
# static CRT while CI - which does run `nuget restore` - shipped an msvcrt.dll
# build 361 KB smaller. src\shared\VC-LTL.props now fails the build rather than
# letting that pass unnoticed, so the restore has to happen here. It also covers
# setup.wixproj, which is SDK-style and restores through the same target.
$extraProps = @()
if ($AllowNoVCLTL) {
    Write-Host "`nSkipping restore: building without VC-LTL by request." -ForegroundColor Yellow
    Write-Host "The static CRT will be linked. This is not what CI ships." -ForegroundColor Yellow
    $extraProps += "/p:ShellAllowNoVCLTL=true"
}
else {
    Write-Host "`nRestoring packages..." -ForegroundColor Green
    & $msbuildPath -nologo -t:restore -p:RestorePackagesConfig=true $solutionPath
    if ($LASTEXITCODE -ne 0) {
        throw "Package restore failed. Re-run with -AllowNoVCLTL to build against the static CRT instead (offline builds, or bisecting a CRT-sensitive bug)."
    }
}

foreach ($p in $platforms) {
    Write-Host "`n========================================================" -ForegroundColor Cyan
    Write-Host " Building Platform: $p | Configuration: $Configuration" -ForegroundColor Cyan
    Write-Host "========================================================" -ForegroundColor Cyan

    & $msbuildPath /m /p:Configuration=$Configuration /p:Platform=$p @extraProps $solutionPath
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
