<#
.SYNOPSIS
    Deploys a locally built Nilesoft Shell over the installed one.

.DESCRIPTION
    shell.dll cannot be overwritten in place. Every process that has ever raised
    a shell context menu has it loaded, and Shell pins its own module for the
    life of that process on purpose, so the file stays mapped until the process
    exits. Stopping Explorer does not release it - on a normal desktop a couple
    of dozen processes hold it.

    What does work is renaming: Windows allows a mapped image to be renamed
    within its volume, just not deleted or overwritten. So the installed binary
    is rotated aside under a name that cannot already exist, and the new one is
    written at the canonical path. Processes that already loaded the old file
    keep running it until they exit; new menus get the new one.

    An earlier version of this script rotated to the fixed name "shell.dll.old",
    which is itself still mapped from the previous deployment, and silenced the
    failure - so the rename quietly did nothing and the copy that followed died
    with a sharing violation, after Explorer had already been stopped and Shell
    unregistered. Nothing here silences a failure, and Explorer is restarted
    from a finally block whatever happens.

.PARAMETER Platform
    Target to deploy. Defaults to the host architecture, and a source binary
    built for anything else is refused.

.PARAMETER ResetConfig
    Overwrite shell.nss with the stock one. By default an existing shell.nss is
    left alone - it is the user's file, the same contract the MSI gives it.

.PARAMETER NoTreat
    Skip the Windows 11 context-menu redirect. Without this, registration uses
    "-register -treat" on Windows 11 so Shell owns the modern menu rather than
    living under "Show more options".

.EXAMPLE
    .\scripts\backup-and-upgrade.ps1
    .\scripts\backup-and-upgrade.ps1 -ResetConfig
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'arm64')]
    [string]$Platform,

    [switch]$ResetConfig,
    [switch]$NoTreat
)

$ErrorActionPreference = 'Stop'

# --- Elevation -------------------------------------------------------------
# Writing to Program Files needs it, and so does renaming out of it.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Elevating...' -ForegroundColor Yellow
    $argv = @('-ExecutionPolicy', 'Bypass', '-NoProfile', '-File', "`"$PSCommandPath`"")
    if ($Platform)    { $argv += @('-Platform', $Platform) }
    if ($ResetConfig) { $argv += '-ResetConfig' }
    if ($NoTreat)     { $argv += '-NoTreat' }
    $elevated = Start-Process powershell.exe -ArgumentList $argv -Verb RunAs -Wait -PassThru
    exit $elevated.ExitCode
}

$repoRoot   = Split-Path $PSScriptRoot -Parent
$installDir = 'C:\Program Files\Nilesoft Shell'
$backupDir  = Join-Path $repoRoot 'backup_installed_shell'

function Get-PeArchitecture([string]$path) {
    $fs = [IO.File]::OpenRead($path)
    try {
        $br = New-Object IO.BinaryReader($fs)
        $fs.Position = 0x3C
        $fs.Position = $br.ReadInt32() + 4
        switch ($br.ReadUInt16()) {
            0x014C  { 'x86' }
            0x8664  { 'x64' }
            0xAA64  { 'arm64' }
            default { 'unknown' }
        }
    }
    finally { $fs.Dispose() }
}

function Test-Locked([string]$path) {
    try { $s = [IO.File]::Open($path, 'Open', 'ReadWrite', 'None'); $s.Close(); $false }
    catch { $true }
}

# --- 1. Source ------------------------------------------------------------
if (-not $Platform) {
    $Platform = if (-not [Environment]::Is64BitOperatingSystem) { 'x86' }
                elseif ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') { 'arm64' }
                else { 'x64' }
}

$sourceDir = Join-Path $repoRoot "src\bin\$Platform"
$sourceDll = Join-Path $sourceDir 'shell.dll'
$sourceExe = Join-Path $sourceDir 'shell.exe'

if (-not (Test-Path $sourceDll)) {
    throw "No shell.dll at '$sourceDll'. Build it first: .\build.ps1 -Platform $Platform"
}

# The binary decides, not the folder it was found in. Deploying the wrong
# architecture leaves an install that loads in nothing.
$dllArch = Get-PeArchitecture $sourceDll
if ($dllArch -ne $Platform) {
    throw "'$sourceDll' is $dllArch, not $Platform. Rebuild: .\build.ps1 -Platform $Platform"
}

Write-Host "Deploying $Platform from $sourceDir" -ForegroundColor Cyan
Write-Host "  shell.dll  $((Get-Item $sourceDll).LastWriteTime)  ($dllArch)" -ForegroundColor DarkGray

# --- 2. Back up -----------------------------------------------------------
Write-Host "`n1. Backing up the current installation..." -ForegroundColor Cyan
if (Test-Path $installDir) {
    if (-not (Test-Path $backupDir)) { New-Item -ItemType Directory -Path $backupDir -Force | Out-Null }
    Copy-Item -Path "$installDir\*" -Destination $backupDir -Recurse -Force
    Write-Host "[OK] $backupDir" -ForegroundColor Green
}
else {
    New-Item -ItemType Directory -Path $installDir -Force | Out-Null
    Write-Host '[INFO] No existing installation; creating one.' -ForegroundColor Yellow
}

# --- 3. Who is holding the old binary ------------------------------------
Write-Host "`n2. Processes holding the installed shell.dll..." -ForegroundColor Cyan
$holders = @()
foreach ($p in (Get-Process -ErrorAction SilentlyContinue)) {
    try {
        foreach ($m in $p.Modules) {
            if ($m.FileName -like "$installDir\*") { $holders += $p.ProcessName; break }
        }
    }
    catch { }        # protected or exited between the enumeration and the read
}
if ($holders) {
    $names = ($holders | Sort-Object -Unique) -join ', '
    Write-Host "[INFO] $($holders.Count) process(es): $names" -ForegroundColor Yellow
    Write-Host '       These keep the previous build until they exit. Expected - the' -ForegroundColor DarkGray
    Write-Host '       module is pinned for process lifetime, so it is renamed aside' -ForegroundColor DarkGray
    Write-Host '       rather than overwritten.' -ForegroundColor DarkGray
}

try {
    # --- 4. Explorer ------------------------------------------------------
    Write-Host "`n3. Stopping Explorer..." -ForegroundColor Cyan
    Get-Process -Name explorer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2

    # --- 5. Rotate and copy ----------------------------------------------
    Write-Host "`n4. Deploying binaries..." -ForegroundColor Cyan
    $stamp = (Get-Date -Format 'yyyyMMddHHmmss') + '_' + (Get-Random -Minimum 1000 -Maximum 9999)

    foreach ($name in @('shell.dll', 'shell.exe')) {
        $target = Join-Path $installDir $name
        if (-not (Test-Path $target)) { continue }

        # A name that cannot already exist, so a still-mapped previous rotation
        # can never make this a no-op.
        Move-Item -Path $target -Destination (Join-Path $installDir "$name.old.$stamp") -Force

        if (Test-Path $target) { throw "Could not rotate '$target' out of the way." }
    }

    Copy-Item -Path $sourceDll -Destination (Join-Path $installDir 'shell.dll') -Force
    if (Test-Path $sourceExe) { Copy-Item -Path $sourceExe -Destination (Join-Path $installDir 'shell.exe') -Force }

    foreach ($extra in @('license.txt', 'readme.txt')) {
        $from = Join-Path $repoRoot "src\bin\$extra"
        if (Test-Path $from) { Copy-Item -Path $from -Destination $installDir -Force }
    }

    $imports = Join-Path $repoRoot 'src\bin\imports'
    if (Test-Path $imports) {
        New-Item -ItemType Directory -Path (Join-Path $installDir 'imports\lang') -Force | Out-Null
        Copy-Item -Path "$imports\*" -Destination (Join-Path $installDir 'imports') -Recurse -Force
    }

    # The deployed file has to be the one that was just built.
    $want = (Get-FileHash $sourceDll -Algorithm SHA256).Hash
    $got  = (Get-FileHash (Join-Path $installDir 'shell.dll') -Algorithm SHA256).Hash
    if ($want -ne $got) { throw 'Deployed shell.dll does not match the source.' }
    Write-Host "[OK] shell.dll verified ($($want.Substring(0,16))...)" -ForegroundColor Green

    # --- 6. Configuration -------------------------------------------------
    Write-Host "`n5. Configuration..." -ForegroundColor Cyan
    $stockNss     = Join-Path $repoRoot 'src\bin\shell.nss'
    $installedNss = Join-Path $installDir 'shell.nss'

    if ($ResetConfig -or -not (Test-Path $installedNss)) {
        Copy-Item -Path $stockNss -Destination $installedNss -Force
        Write-Host '[OK] Stock shell.nss installed.' -ForegroundColor Green
    }
    else {
        # shell.nss belongs to whoever edited it. Same contract the MSI gives it
        # (NeverOverwrite + Permanent): never clobbered, never silently rewritten.
        Write-Host '[OK] Existing shell.nss kept.' -ForegroundColor Green

        $a = (Get-FileHash $installedNss -Algorithm SHA256).Hash
        $b = (Get-FileHash $stockNss -Algorithm SHA256).Hash
        if ($a -ne $b) {
            Copy-Item -Path $stockNss -Destination "$installedNss.stock-new" -Force
            Write-Host '     The stock config has changed. A copy is beside yours as' -ForegroundColor DarkGray
            Write-Host '     shell.nss.stock-new; diff it if you want the new defaults.' -ForegroundColor DarkGray
        }
    }

    # --- 7. Housekeeping --------------------------------------------------
    # Rotations from earlier deployments, once nothing has them mapped.
    $freed = 0
    Get-ChildItem -Path $installDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'shell.dll.old*' -or $_.Name -like 'shell.exe.old*' -or $_.Name -eq 'shell.old' } |
        ForEach-Object {
            if (-not (Test-Locked $_.FullName)) {
                Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
                if (-not (Test-Path $_.FullName)) { $freed++ }
            }
        }
    if ($freed) { Write-Host "[OK] Removed $freed unmapped backup binar$(if($freed -eq 1){'y'}else{'ies'})." -ForegroundColor Green }

    # --- 8. Register ------------------------------------------------------
    Write-Host "`n6. Registering..." -ForegroundColor Cyan
    $isWin11 = [Environment]::OSVersion.Version.Build -ge 22000
    $regArgs = @('-register')
    if ($isWin11 -and -not $NoTreat) { $regArgs += '-treat' }

    & (Join-Path $installDir 'shell.exe') @regArgs
    Start-Sleep -Seconds 1

    $clsid = 'HKCU:\Software\Classes\CLSID\{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}'
    if (-not (Test-Path $clsid)) { throw 'Registration did not create the context-menu CLSID.' }
    Write-Host "[OK] Registered ($($regArgs -join ' '))" -ForegroundColor Green

    if ($isWin11 -and -not $NoTreat) {
        # -treat sets this, but assert it: without the redirect Shell only shows
        # up under "Show more options".
        $treatAs = 'HKCU:\Software\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\TreatAs'
        $expected = '{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}'
        if ((Get-ItemProperty -Path $treatAs -Name '(default)' -ErrorAction SilentlyContinue).'(default)' -ne $expected) {
            New-Item -Path $treatAs -Force | Out-Null
            Set-ItemProperty -Path $treatAs -Name '(default)' -Value $expected
        }
        Write-Host '[OK] Windows 11 context-menu redirect in place.' -ForegroundColor Green
    }

    Write-Host "`nDeployment complete ($Platform)." -ForegroundColor Green
}
finally {
    # Whatever went wrong above, the user gets their desktop back.
    if (-not (Get-Process -Name explorer -ErrorAction SilentlyContinue)) {
        Write-Host "`nRestarting Explorer..." -ForegroundColor Cyan
        Start-Process explorer.exe
        Start-Sleep -Seconds 2
    }
}
