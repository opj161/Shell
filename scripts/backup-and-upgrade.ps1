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

    Explorer is restarted *after* the binaries are in place, not before. Windows
    brings the shell back about a second after it is killed, so stopping it
    first only guaranteed the Explorer that came back had mapped the previous
    build - the deploy landed one restart late, every time, while looking like
    it had worked.

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
    [switch]$NoTreat,

    # Set by the elevated relaunch below. The elevated process gets its own
    # console, which closes with it, so it transcribes to a file the launching
    # session can read back - otherwise a failed deployment says nothing at all.
    [string]$LogPath
)

$ErrorActionPreference = 'Stop'

# --- Elevation -------------------------------------------------------------
# Writing to Program Files needs it, and so does renaming out of it.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Elevating...' -ForegroundColor Yellow

    $transcript = Join-Path $env:TEMP ("nilesoft-deploy-{0}.log" -f (Get-Date -Format 'yyyyMMddHHmmss'))
    $argv = @('-ExecutionPolicy', 'Bypass', '-NoProfile', '-File', "`"$PSCommandPath`"", '-LogPath', "`"$transcript`"")
    if ($Platform)    { $argv += @('-Platform', $Platform) }
    if ($ResetConfig) { $argv += '-ResetConfig' }
    if ($NoTreat)     { $argv += '-NoTreat' }

    $elevated = Start-Process powershell.exe -ArgumentList $argv -Verb RunAs -Wait -PassThru

    if (Test-Path $transcript) {
        Get-Content $transcript |
            Where-Object { $_ -notmatch '^\*{10,}$' -and $_ -notmatch '^(Windows PowerShell transcript|Start time|End time|Username|RunAs User|Configuration Name|Machine|Host Application|Process ID|PSVersion|PSEdition|PSCompatibleVersions|BuildVersion|CLRVersion|WSManStackVersion|PSRemotingProtocolVersion|SerializationVersion|Transcript started|Transcript stopped)' }
        Write-Host "`n(full transcript: $transcript)" -ForegroundColor DarkGray
    }

    exit $elevated.ExitCode
}

if ($LogPath) { Start-Transcript -Path $LogPath -Force | Out-Null }

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

# Deploying bits that are already installed would stop Explorer and rotate a
# copy of the running DLL aside for nothing - and every rotation stays on disk
# until whatever mapped it exits.
$installedDll = Join-Path $installDir 'shell.dll'
$alreadyCurrent = (Test-Path $installedDll) -and
                  ((Get-FileHash $installedDll -Algorithm SHA256).Hash -eq (Get-FileHash $sourceDll -Algorithm SHA256).Hash)

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
    # --- 4. Rotate and copy ----------------------------------------------
    #
    # Explorer is restarted *after* this, not before, and that ordering is the
    # whole point.
    #
    # This used to stop Explorer first, on the reasonable-sounding theory that
    # a running Explorer is in the way of replacing the file it has mapped. It
    # is not - rotation exists precisely so it does not have to be - and
    # stopping it first was actively wrong: Windows restarts the shell within
    # about a second of it dying, so by the time the copy finished, a new
    # Explorer had already started and mapped the *old* binary. Every deploy
    # therefore landed one Explorer restart late, and the machine looked like it
    # was running the build that had just been deployed while running the one
    # before it.
    #
    # Measured 2026-08-24: Explorer started 17:09:08 and the rotation it was
    # supposed to precede is stamped 17:09:09. That is the whole of the puzzle
    # docs/refactor/08-handoff.md section 3.2 recorded as unexplained - phase
    # timings that appeared from every host except explorer.exe - because the
    # explorer.exe under test did not have the build that produced them.
    Write-Host "`n3. Deploying binaries..." -ForegroundColor Cyan
    if ($alreadyCurrent) {
        Write-Host '[OK] shell.dll unchanged; nothing rotated.' -ForegroundColor Green
    }
    else {
        $stamp = (Get-Date -Format 'yyyyMMddHHmmss') + '_' + (Get-Random -Minimum 1000 -Maximum 9999)

        foreach ($name in @('shell.dll', 'shell.exe')) {
            $target = Join-Path $installDir $name
            if (-not (Test-Path $target)) { continue }

            # A name that cannot already exist, so a still-mapped previous
            # rotation can never make this a no-op.
            Move-Item -Path $target -Destination (Join-Path $installDir "$name.old.$stamp") -Force

            if (Test-Path $target) { throw "Could not rotate '$target' out of the way." }
        }

        Copy-Item -Path $sourceDll -Destination (Join-Path $installDir 'shell.dll') -Force
        if (Test-Path $sourceExe) { Copy-Item -Path $sourceExe -Destination (Join-Path $installDir 'shell.exe') -Force }

        # Stamp when this landed, and stamp it explicitly.
        #
        # The creation time cannot simply be read: NTFS file tunneling puts back
        # the *old* creation time when a file reappears at a path it recently
        # left, which is exactly what rotate-then-copy does. Observed here on
        # 2026-08-24 - a shell.dll deployed at 17:09 reported a creation time
        # from four days earlier - and it matters because the restart check
        # below asks whether Explorer started after the deployment.
        (Get-Item (Join-Path $installDir 'shell.dll')).CreationTime = Get-Date
    }

    # The MSI installs the licence as LICENSE; match it rather than leaving both
    # that and a license.txt behind.
    $licence = Join-Path $repoRoot 'src\bin\license.txt'
    if (Test-Path $licence) { Copy-Item -Path $licence -Destination (Join-Path $installDir 'LICENSE') -Force }
    Remove-Item (Join-Path $installDir 'license.txt') -Force -ErrorAction SilentlyContinue

    $readme = Join-Path $repoRoot 'src\bin\readme.txt'
    if (Test-Path $readme) { Copy-Item -Path $readme -Destination $installDir -Force }

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

    # --- 5. Configuration -------------------------------------------------
    Write-Host "`n4. Configuration..." -ForegroundColor Cyan
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

    # --- 6. Housekeeping --------------------------------------------------
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

    # --- 7. Register ------------------------------------------------------
    Write-Host "`n5. Registering..." -ForegroundColor Cyan
    $isWin11 = [Environment]::OSVersion.Version.Build -ge 22000
    $regArgs = @('-register')
    if ($isWin11 -and -not $NoTreat) { $regArgs += '-treat' }

    & (Join-Path $installDir 'shell.exe') @regArgs
    Start-Sleep -Seconds 1

    # A per-machine install registers under HKLM. Both hives are checked because
    # that is what COM resolves against - asserting one of them is how a
    # successful deployment previously got reported as a failure.
    # Not "| Select-Object -First 1": stopping a pipeline early makes PowerShell
    # 5.1 write "The pipeline has been stopped" into the transcript, which reads
    # like a failure in a deployment log.
    function Get-FirstRegistryDefault([string[]]$paths) {
        foreach ($path in $paths) {
            if (Test-Path $path) {
                $value = (Get-ItemProperty $path -Name '(default)' -ErrorAction SilentlyContinue).'(default)'
                if ($value) { return $value }
            }
        }
        return $null
    }

    $contextMenu = '{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}'
    $inproc = Get-FirstRegistryDefault @(
        "HKLM:\SOFTWARE\Classes\CLSID\$contextMenu\InprocServer32",
        "HKCU:\Software\Classes\CLSID\$contextMenu\InprocServer32"
    )

    if (-not $inproc) { throw 'Registration did not create the context-menu CLSID in either hive.' }
    Write-Host "[OK] Registered ($($regArgs -join ' '))" -ForegroundColor Green
    Write-Host "     InprocServer32 -> $inproc" -ForegroundColor DarkGray

    if ($inproc -ne (Join-Path $installDir 'shell.dll')) {
        Write-Host "[WARN] Registered DLL is not the one just deployed." -ForegroundColor Yellow
    }

    if ($isWin11 -and -not $NoTreat) {
        # Verified, not written. -treat owns this key; writing it here by hand
        # would put a copy in HKCU that shadows the real one in HKLM, and a
        # stale value there outlives any future unregister.
        $treatAs = Get-FirstRegistryDefault @(
            'HKLM:\SOFTWARE\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\TreatAs',
            'HKCU:\Software\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\TreatAs'
        )

        if ($treatAs -eq $contextMenu) {
            Write-Host '[OK] Windows 11 context-menu redirect in place.' -ForegroundColor Green
        }
        else {
            Write-Host "[WARN] TreatAs redirect missing or unexpected ('$treatAs'); Shell will" -ForegroundColor Yellow
            Write-Host '       appear under "Show more options" rather than as the main menu.' -ForegroundColor Yellow
        }
    }

    # --- 8. Restart Explorer, last -----------------------------------------
    #
    # After the binaries and the registration, never before. See the note at
    # step 3: Windows restarts the shell about a second after it is killed, so
    # stopping it first only guaranteed that the Explorer which came back had
    # mapped the previous build.
    #
    # And it is verified rather than assumed, because "the deploy silently did
    # not take" is exactly the failure that cost a whole session: an Explorer
    # whose start time predates the copy is running the old DLL no matter what
    # its module list says, since the file it mapped was renamed aside
    # afterwards and GetModuleFileName still reports the name it had at load.
    # Two different questions, and conflating them is what let a stale Explorer
    # survive a deploy that reported success. The binaries can already be this
    # build - a re-run, or a second deploy of an unchanged tree - while the
    # running Explorer still has the one before it mapped.
    $deployedAtStamp = (Get-Item (Join-Path $installDir 'shell.dll')).CreationTime
    $explorerCurrent = @(Get-Process -Name explorer -ErrorAction SilentlyContinue |
                         Where-Object { $_.StartTime -gt $deployedAtStamp }).Count -gt 0

    if ($explorerCurrent) {
        Write-Host "`n7. Explorer left alone - it already started after this shell.dll was installed." -ForegroundColor Cyan
    }
    else {
        Write-Host "`n7. Restarting Explorer so it maps the new binary..." -ForegroundColor Cyan
        $deployedAt = Get-Date
        Get-Process -Name explorer -ErrorAction SilentlyContinue |
            Stop-Process -Force -ErrorAction SilentlyContinue

        # Wait for the shell to come back rather than sleeping a fixed span -
        # the watchdog is usually about a second, and a fixed sleep is how this
        # check becomes decorative on a slow machine.
        $back = $null
        for ($i = 0; $i -lt 60; $i++) {
            Start-Sleep -Milliseconds 500
            $back = Get-Process -Name explorer -ErrorAction SilentlyContinue |
                    Sort-Object StartTime | Select-Object -Last 1
            if ($back -and $back.StartTime -gt $deployedAt) { break }
            $back = $null
        }

        if (-not $back) {
            Start-Process explorer.exe
            Start-Sleep -Seconds 2
            $back = Get-Process -Name explorer -ErrorAction SilentlyContinue |
                    Sort-Object StartTime | Select-Object -Last 1
        }

        if ($back -and $back.StartTime -gt $deployedAt) {
            Write-Host "[OK] Explorer restarted (pid $($back.Id)) after the new shell.dll was in place." -ForegroundColor Green
        }
        else {
            Write-Host '[WARN] Could not confirm Explorer restarted after the copy. It may still be' -ForegroundColor Yellow
            Write-Host '       running the previous build; sign out and back in, or restart it by hand.' -ForegroundColor Yellow
        }
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
    if ($LogPath) { try { Stop-Transcript | Out-Null } catch { } }
}
