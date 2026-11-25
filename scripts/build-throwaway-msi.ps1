<#
.SYNOPSIS
    Builds a throwaway installer from the real WiX sources, for real install and
    upgrade tests on this machine without touching the installed product.
.DESCRIPTION
    The package keeps every action and table of the real one and changes only its
    identity: product name "Nilesoft Shell Test" (so INSTALLFOLDER and the Start
    Menu entry differ), a fixed throwaway UpgradeCode (so two builds are one
    product line), a ProductCode generated per build, and component codes whose
    first group is rewritten (so they are identical across builds and shared
    with nothing the real install registered).

    -Legacy shapes it like upstream before 1.9.20: CONFIG is not Permanent and
    uninstall removes every file in the folder - the package the legacy config
    bridge exists to upgrade from.

    What it cannot isolate: registration. The throwaway writes the same CLSID
    keys and the same Windows 11 TreatAs redirect as the real product, so
    uninstalling it unregisters Shell machine-wide. Run
    .\scripts\backup-and-upgrade.ps1 afterwards to put the real one back.

    Needs src\bin\<Platform> already built (.\build.ps1).
.EXAMPLE
    .\scripts\build-throwaway-msi.ps1 -Out $env:TEMP\msitest -Version 1.9.19 -Legacy
    .\scripts\build-throwaway-msi.ps1 -Out $env:TEMP\msitest -Version 1.9.21
    msiexec /i "$env:TEMP\msitest\shelltest-x64-1.9.19-legacy.msi" /qn INSTALLFOLDER="C:\ShellTest\Custom\" /l*v old.log
    msiexec /i "$env:TEMP\msitest\shelltest-x64-1.9.21.msi" /qn /l*v upgrade.log
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Out,
    [Parameter(Mandatory)] [string]$Version,
    [ValidateSet('x64', 'x86', 'arm64')] [string]$Platform = 'x64',
    [switch]$Legacy
)
$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot -Parent
$work = Join-Path $Out "build-$Platform-$Version"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
$wix = Join-Path $work 'src\setup\wix'
$bin = Join-Path $work 'src\bin'
New-Item -ItemType Directory -Force -Path $wix, (Join-Path $bin $Platform) | Out-Null

Copy-Item (Join-Path $repo 'src\setup\wix\*') $wix -Recurse -Exclude 'obj'
foreach ($item in 'imports', 'license.txt', 'readme.txt', 'shell.nss') {
    Copy-Item (Join-Path $repo "src\bin\$item") $bin -Recurse
}
foreach ($item in 'shell.dll', 'shell.exe', 'ca.dll') {
    Copy-Item (Join-Path $repo "src\bin\$Platform\$item") (Join-Path $bin $Platform)
}

# A pattern that no longer matches is an error, never a silently unchanged copy.
function Edit-File([string]$path, [scriptblock]$edit) {
    $text = [IO.File]::ReadAllText($path)
    $new = & $edit $text
    if ($new -eq $text) { throw "build-throwaway-msi: no change made to $path" }
    [IO.File]::WriteAllText($path, $new, [Text.UTF8Encoding]::new($false))
}

Edit-File (Join-Path $wix 'setup.wxs') {
    param($t)
    $t = $t -replace "(<\?define UpgradeCode = ')[^']+", '${1}{D2A1F6E4-5B3C-4E0A-9C11-7A3E00000001}'
    $t = $t -replace "(<\?define Name = ')Shell(')", '${1}Shell Test${2}'
    # The upstream component code the arm64/x86 previous-folder search names, so
    # a throwaway never seeds INSTALLFOLDER from the real installation.
    $t = $t -replace "\{ED9EFE4B-", '{D2A1F6E4-'
    if ($Legacy) {
        $t = $t -replace "(<Component Id='CONFIG' Guid='\`$\(var\.ConfigComponentGuid\)') Permanent='yes'", '$1'
        $t = $t -replace "(<RemoveFile Id='PurgeLog')", "<RemoveFile Id='PurgeAllLegacy' Name='*.*' On='uninstall' Directory='INSTALLFOLDER' />`r`n`t`t`t`t`t`$1"
    }
    $t
}

Edit-File (Join-Path $wix 'identity.wxi') {
    param($t)
    $t -replace "(ComponentGuid\s*=\s*'\{)[0-9A-Fa-f]{8}", '${1}D2A1F6E4'
}

Edit-File (Join-Path $wix 'version.wxi') {
    param($t)
    $t -replace "(<\?define Version = ')[^']+", ('${1}' + $Version)
}

$msbuild = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild.exe was not found.' }

& $msbuild -nologo -restore /nr:false /v:minimal /p:Configuration=Release /p:Platform=$Platform (Join-Path $wix 'setup.wixproj')
if ($LASTEXITCODE -ne 0) { throw "build-throwaway-msi: the package did not build ($Platform $Version)" }

$dest = Join-Path $Out ("shelltest-$Platform-$Version" + $(if ($Legacy) { '-legacy' } else { '' }) + '.msi')
Copy-Item (Join-Path $bin "setup-$Platform.msi") $dest -Force
Write-Host "built $dest"
