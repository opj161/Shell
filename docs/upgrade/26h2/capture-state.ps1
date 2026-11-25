<#
.SYNOPSIS
    Read-only snapshot of everything on this machine that Nilesoft Shell's
    behaviour depends on, as JSON, for diffing across a Windows feature update.
.DESCRIPTION
    Shell is a COM-activated context-menu handler that is only the *primary*
    Windows 11 menu because of a TreatAs redirect on a TrustedInstaller-owned
    CLSID key, and it only composes a menu because it hooks
    win32u!NtUserTrackPopupMenuEx and subclasses host windows. None of that is
    visible in the project tree: it is machine state. This captures it.

    Nothing here writes. Run it before the upgrade and again afterwards, then
    diff the two JSON files:

        .\docs\upgrade\26h2\capture-state.ps1 -Out pre.json
        # ... upgrade, reboot ...
        .\docs\upgrade\26h2\capture-state.ps1 -Out post.json
        .\docs\upgrade\26h2\capture-state.ps1 -Diff pre.json -Against post.json

    Elevation is not required: every key read here grants BUILTIN\Users read.

    Contracts the capture rests on:
      * IMAGE_OPTIONAL_HEADER.SizeOfImage is what a loaded module reports as its
        mapped size, which is why both are recorded - a stale host cannot be
        told from a current one by GetModuleFileName.
        https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-image_optional_header32
      * TreatAs is the registry entry that makes one CLSID emulate another;
        CoGetTreatAsClass / CoTreatAsClass read and write it.
        https://learn.microsoft.com/en-us/windows/win32/com/treatas
      * Shell extensions are found through shellex\ContextMenuHandlers under the
        file class, which is why the whole handler set is recorded and not only
        Shell's own entry.
        https://learn.microsoft.com/en-us/windows/win32/shell/context-menu-handlers
.PARAMETER Out
    Where to write the JSON snapshot. Defaults to a timestamped file beside this
    script under baseline\.
.PARAMETER Diff
    Compare two snapshots instead of capturing. Pass the earlier file here and
    the later one in -Against.
.PARAMETER Against
    The later snapshot, with -Diff.
#>
[CmdletBinding()]
param(
    [string]$Out,
    [string]$Diff,
    [string]$Against
)

$ErrorActionPreference = 'Continue'

$ShellDir = 'C:\Program Files\Nilesoft Shell'
$ShellClsids = @(
    '{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}',
    '{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF2}',
    '{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF3}')
$Win11MenuClsid = '{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}'
$MenuClasses = @(
    '*', 'AllFilesystemObjects', 'Directory', 'Directory\background',
    'DesktopBackground', 'Drive', 'Folder', 'LibraryFolder',
    'LibraryFolder\background', 'UserLibraryFolder')
$SystemFiles = @(
    'Windows.UI.FileExplorer.dll', 'win32u.dll', 'user32.dll', 'shell32.dll',
    'windows.storage.dll', 'twinui.dll', 'twinui.pcshell.dll',
    'uiautomationcore.dll', 'combase.dll', 'comctl32.dll')

function Get-PeHeader([string]$file) {
    try {
        $bytes = [IO.File]::ReadAllBytes($file)
        $e = [BitConverter]::ToInt32($bytes, 0x3C)
        [pscustomobject]@{
            SizeOfImage = [BitConverter]::ToUInt32($bytes, $e + 24 + 56)
            Machine     = '0x{0:X4}' -f [BitConverter]::ToUInt16($bytes, $e + 4)
            LinkTimeUtc = [DateTimeOffset]::FromUnixTimeSeconds(
                [BitConverter]::ToUInt32($bytes, $e + 8)).UtcDateTime.ToString('s')
        }
    }
    catch { $null }
}

function Get-FileFacts([string]$file) {
    if (-not (Test-Path -LiteralPath $file)) {
        return [pscustomobject]@{ Path = $file; Present = $false }
    }
    $item = Get-Item -LiteralPath $file
    $pe = Get-PeHeader $file
    [pscustomobject]@{
        Path          = $file
        Present       = $true
        Length        = $item.Length
        LastWriteUtc  = $item.LastWriteTimeUtc.ToString('s')
        Sha256        = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
        FileVersion   = $item.VersionInfo.FileVersion
        SizeOfImage   = $pe.SizeOfImage
        Machine       = $pe.Machine
        LinkTimeUtc   = $pe.LinkTimeUtc
    }
}

function Get-KeyValues([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $out = [ordered]@{}
    $item = Get-Item -LiteralPath $path
    foreach ($name in $item.GetValueNames()) {
        $shown = if ($name -eq '') { '(Default)' } else { $name }
        $out[$shown] = [string]$item.GetValue($name)
    }
    $out
}

function Get-Snapshot {
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'

    # --- the redirect that decides who owns the Windows 11 menu --------------
    $menuKey = "HKLM:\SOFTWARE\Classes\CLSID\$Win11MenuClsid"
    $treatAs = [ordered]@{
        KeyPresent      = Test-Path -LiteralPath $menuKey
        InprocServer32  = Get-KeyValues "$menuKey\InProcServer32"
        TreatAs         = (Get-KeyValues "$menuKey\TreatAs")
        Owner           = $null
        UsersRights     = @()
        HkcuOverride    = $null
    }
    if ($treatAs.KeyPresent) {
        $acl = Get-Acl -Path $menuKey
        $treatAs.Owner = $acl.Owner
        $treatAs.UsersRights = @(
            $acl.Access |
                Where-Object { "$($_.IdentityReference)" -eq 'BUILTIN\Users' } |
                ForEach-Object {
                    '{0} inherited={1} flags={2}' -f $_.RegistryRights, $_.IsInherited, $_.InheritanceFlags
                })
    }
    $hkcuMenu = "HKCU:\SOFTWARE\Classes\CLSID\$Win11MenuClsid"
    if (Test-Path -LiteralPath $hkcuMenu) {
        $k = Get-Item -LiteralPath $hkcuMenu
        $treatAs.HkcuOverride = [ordered]@{
            SubKeyCount    = $k.SubKeyCount
            ValueCount     = $k.ValueCount
            InprocServer32 = Get-KeyValues "$hkcuMenu\InprocServer32"
            TreatAs        = Get-KeyValues "$hkcuMenu\TreatAs"
        }
    }

    # --- Shell's own COM registration ---------------------------------------
    $registration = foreach ($clsid in $ShellClsids) {
        foreach ($hive in 'HKLM', 'HKCU') {
            $path = "${hive}:\SOFTWARE\Classes\CLSID\$clsid"
            if (Test-Path -LiteralPath $path) {
                [pscustomobject]@{
                    Hive           = $hive
                    Clsid          = $clsid
                    Default        = (Get-KeyValues $path).'(Default)'
                    InprocServer32 = Get-KeyValues "$path\InprocServer32"
                }
            }
        }
    }

    # --- every context-menu handler on the classes Shell is registered for ---
    $handlers = foreach ($class in $MenuClasses) {
        foreach ($hive in 'HKLM', 'HKCU') {
            $path = "${hive}:\SOFTWARE\Classes\$class\shellex\ContextMenuHandlers"
            if (Test-Path -LiteralPath $path) {
                foreach ($sub in (Get-ChildItem -LiteralPath $path -ErrorAction SilentlyContinue)) {
                    [pscustomobject]@{
                        Hive    = $hive
                        Class   = $class
                        Handler = $sub.PSChildName
                        Clsid   = [string]$sub.GetValue('')
                    }
                }
            }
        }
    }

    # --- approval and blocking ----------------------------------------------
    $approvedKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved'
    $approved = @()
    if (Test-Path -LiteralPath $approvedKey) {
        $k = Get-Item -LiteralPath $approvedKey
        $approved = @($k.GetValueNames() | Where-Object { $ShellClsids -contains $_ } |
            ForEach-Object { '{0} = {1}' -f $_, $k.GetValue($_) })
    }
    $blockedKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Blocked'
    $blocked = $null
    if (Test-Path -LiteralPath $blockedKey) { $blocked = Get-KeyValues $blockedKey }

    # --- installed package ---------------------------------------------------
    $product = $null
    foreach ($root in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
                      'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*') {
        $hit = Get-ItemProperty $root -ErrorAction SilentlyContinue |
            Where-Object { $_.Publisher -eq 'Nilesoft' -or $_.DisplayName -eq 'Nilesoft Shell' }
        if ($hit) {
            $product = $hit | Select-Object -First 1 DisplayName, DisplayVersion, InstallDate,
                InstallSource, PSChildName, UninstallString
        }
    }
    $cachedMsi = $null
    $products = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UserData\S-1-5-18\Products'
    foreach ($p in (Get-ChildItem $products -ErrorAction SilentlyContinue)) {
        $ip = Join-Path $p.PSPath 'InstallProperties'
        if (Test-Path -LiteralPath $ip) {
            $v = Get-ItemProperty -LiteralPath $ip
            if ($v.DisplayName -eq 'Nilesoft Shell') {
                $cachedMsi = [ordered]@{
                    LocalPackage = $v.LocalPackage
                    Present      = [bool]($v.LocalPackage -and (Test-Path -LiteralPath $v.LocalPackage))
                    Sha256       = if ($v.LocalPackage -and (Test-Path -LiteralPath $v.LocalPackage)) {
                                       (Get-FileHash -LiteralPath $v.LocalPackage).Hash } else { $null }
                }
            }
        }
    }

    # --- hosts that currently hold the module -------------------------------
    $hosts = foreach ($proc in (Get-Process -ErrorAction SilentlyContinue)) {
        $mods = $null
        try { $mods = $proc.Modules | Where-Object { $_.FileName -like "$ShellDir\shell.dll" } } catch {}
        foreach ($m in $mods) {
            $started = $null
            try { $started = $proc.StartTime.ToUniversalTime().ToString('s') } catch {}
            [pscustomobject]@{
                Process     = $proc.ProcessName
                Pid         = $proc.Id
                StartedUtc  = $started
                MappedSize  = $m.ModuleMemorySize
            }
        }
    }

    [ordered]@{
        CapturedUtc = (Get-Date).ToUniversalTime().ToString('s')
        Os = [ordered]@{
            ProductName          = $cv.ProductName
            DisplayVersion       = $cv.DisplayVersion
            CurrentBuild         = $cv.CurrentBuild
            Ubr                  = $cv.UBR
            EditionId            = $cv.EditionID
            CompositionEditionId = $cv.CompositionEditionID
            BuildBranch          = $cv.BuildBranch
            BuildLabEx           = $cv.BuildLabEx
        }
        ShellFiles   = @(
            if (Test-Path -LiteralPath $ShellDir) {
                Get-ChildItem -LiteralPath $ShellDir -Recurse -File -Force |
                    Where-Object { $_.Name -ne 'shell.log' } |
                    ForEach-Object { Get-FileFacts $_.FullName }
            })
        SystemFiles  = @($SystemFiles | ForEach-Object { Get-FileFacts (Join-Path $env:SystemRoot "System32\$_") })
        Explorer     = Get-FileFacts (Join-Path $env:SystemRoot 'explorer.exe')
        Win11Menu    = $treatAs
        Registration = @($registration)
        Handlers     = @($handlers | Sort-Object Class, Handler)
        Approved     = $approved
        Blocked      = $blocked
        Product      = $product
        CachedMsi    = $cachedMsi
        UserConfig   = [ordered]@{
            HkcuNilesoftShell = Get-KeyValues 'HKCU:\SOFTWARE\Nilesoft\Shell'
            ShellNssSha256    = if (Test-Path -LiteralPath "$ShellDir\shell.nss") {
                                    (Get-FileHash -LiteralPath "$ShellDir\shell.nss").Hash } else { $null }
        }
        LoadedHosts  = @($hosts | Sort-Object Process, Pid)
    }
}

if ($Diff) {
    if (-not $Against) { throw '-Diff needs -Against' }
    $a = Get-Content -LiteralPath $Diff -Raw | ConvertFrom-Json
    $b = Get-Content -LiteralPath $Against -Raw | ConvertFrom-Json

    Write-Output "OS   : $($a.Os.DisplayVersion) $($a.Os.CurrentBuild).$($a.Os.Ubr)  ->  $($b.Os.DisplayVersion) $($b.Os.CurrentBuild).$($b.Os.Ubr)"
    Write-Output ''

    # The three questions that decide whether Shell still works at all.
    Write-Output '--- is Shell still the primary Windows 11 menu? ---'
    $ta = $a.Win11Menu.TreatAs.'(Default)'
    $tb = $b.Win11Menu.TreatAs.'(Default)'
    Write-Output "  TreatAs : '$ta'  ->  '$tb'"
    if ($ta -ne $tb) { Write-Output '  CHANGED - the redirect was altered or removed by the upgrade' }
    Write-Output "  owner   : $($a.Win11Menu.Owner)  ->  $($b.Win11Menu.Owner)"

    Write-Output ''
    Write-Output '--- is Shell still registered as a handler? ---'
    $ha = @($a.Handlers | ForEach-Object { '{0}|{1}|{2}|{3}' -f $_.Hive, $_.Class, $_.Handler, $_.Clsid })
    $hb = @($b.Handlers | ForEach-Object { '{0}|{1}|{2}|{3}' -f $_.Hive, $_.Class, $_.Handler, $_.Clsid })
    foreach ($row in (Compare-Object $ha $hb)) {
        $mark = if ($row.SideIndicator -eq '=>') { 'ADDED  ' } else { 'REMOVED' }
        Write-Output "  $mark $($row.InputObject)"
    }
    if (-not (Compare-Object $ha $hb)) { Write-Output '  no change' }

    Write-Output ''
    Write-Output '--- did the files Shell depends on change? ---'
    foreach ($fa in $a.SystemFiles) {
        $fb = $b.SystemFiles | Where-Object { $_.Path -eq $fa.Path }
        if (-not $fb) { Write-Output "  GONE    $($fa.Path)"; continue }
        if ($fa.Sha256 -ne $fb.Sha256) {
            Write-Output ("  CHANGED {0}`n            {1} -> {2}" -f (Split-Path $fa.Path -Leaf), $fa.FileVersion, $fb.FileVersion)
        }
    }
    $ea = $a.Explorer; $eb = $b.Explorer
    if ($ea.Sha256 -ne $eb.Sha256) {
        Write-Output ("  CHANGED explorer.exe`n            {0} -> {1}" -f $ea.FileVersion, $eb.FileVersion)
    }

    Write-Output ''
    Write-Output '--- is the installed Shell itself intact? ---'
    foreach ($fa in $a.ShellFiles) {
        $fb = $b.ShellFiles | Where-Object { $_.Path -eq $fa.Path }
        if (-not $fb) { Write-Output "  GONE    $($fa.Path)" }
        elseif ($fa.Sha256 -ne $fb.Sha256) { Write-Output "  CHANGED $($fa.Path)" }
    }
    foreach ($fb in $b.ShellFiles) {
        if (-not ($a.ShellFiles | Where-Object { $_.Path -eq $fb.Path })) {
            Write-Output "  NEW     $($fb.Path)"
        }
    }
    return
}

if (-not $Out) {
    $dir = Join-Path $PSScriptRoot 'baseline'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $Out = Join-Path $dir ("state-{0}.{1}-{2}.json" -f $cv.CurrentBuild, $cv.UBR, (Get-Date -Format 'yyyyMMdd-HHmmss'))
}

$snapshot = Get-Snapshot
$snapshot | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Out -Encoding utf8

Write-Output "wrote $Out"
Write-Output ("  OS            {0} {1}.{2}" -f $snapshot.Os.DisplayVersion, $snapshot.Os.CurrentBuild, $snapshot.Os.Ubr)
Write-Output ("  TreatAs       {0}" -f $snapshot.Win11Menu.TreatAs.'(Default)')
Write-Output ("  handlers      {0} across {1} classes" -f $snapshot.Handlers.Count, (@($snapshot.Handlers.Class | Sort-Object -Unique)).Count)
Write-Output ("  loaded hosts  {0}" -f $snapshot.LoadedHosts.Count)
Write-Output ("  shell files   {0}" -f $snapshot.ShellFiles.Count)
