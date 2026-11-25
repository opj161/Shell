<#
.SYNOPSIS
    Records the context menus on this machine, normalised so two runs diff
    cleanly. Run before the 26H2 upgrade and again afterwards.
.DESCRIPTION
    Risk #2 in 00-pre-upgrade-baseline.md - 26H2 turning on File Explorer
    features that add verbs to the menu Shell composes - is the one thing the
    registry/file snapshot cannot see. This sees it.

    Two censuses, from menucensus.cpp beside this script:

      live    the menu Explorer actually shows on the desktop background, read
              out of the popup window's own HMENU. The only reading that proves
              Shell composed anything. Repeated until two consecutive readings
              agree, because a menu read just after an Explorer restart measures
              Explorer settling: 26, 27, 29, 29 on this machine on 2026-11-25.

      static  what the registered handlers contribute through
              IContextMenu::QueryContextMenu, with no UI at all, for six
              surfaces. Deterministic. Shell contributes zero items this way by
              design, so this is the *input* to its composition.

    Normalisation drops the command id. Shell renumbers its own items on every
    invocation - measured 2026-11-25, two readings identical in title, kind and
    position while every Shell id had shifted by one - so the stable identity of
    a row is (depth, position, kind, title). Diffing on the id reports a
    completely changed menu every time.

    Nothing here writes outside -Out. The live census posts WM_CONTEXTMENU to the
    desktop view and dismisses the menu with ESC, so a menu flashes on screen;
    it synthesises no input and needs no elevation.
.PARAMETER Out
    Directory for this run's census. Defaults to baseline\census.
.PARAMETER Diff
    Compare two census directories instead of recording. Pass the earlier one
    here and the later one in -Against.
.PARAMETER Against
    The later census directory, with -Diff.
.PARAMETER MaxLiveRuns
    How many live readings to take while waiting for two to agree. Default 10.
.EXAMPLE
    .\docs\upgrade\26h2\census.ps1 -Out .\docs\upgrade\26h2\baseline\census-post
.EXAMPLE
    .\docs\upgrade\26h2\census.ps1 -Diff .\docs\upgrade\26h2\baseline\census -Against .\docs\upgrade\26h2\baseline\census-post
#>
[CmdletBinding()]
param(
    [string]$Out,
    [string]$Diff,
    [string]$Against,
    [int]$MaxLiveRuns = 10
)

$ErrorActionPreference = 'Stop'

# (depth, position, kind, title). The id is deliberately absent - see above.
#
# Titles get one substitution as well. PowerToys' New+ submenu names its template
# entries with a counter that changes on every invocation - two censuses taken a
# minute apart read "tmp-255407" and "tmp-253576" - so a trailing run of four or
# more digits becomes -N. The prefix is kept, so a template appearing or
# disappearing still shows up; only the counter is discarded. Found by diffing
# two consecutive runs of this script, which is why it is worth doing before
# trusting any diff.
function ConvertTo-CensusRows([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return @() }
    @(Get-Content -LiteralPath $path |
        Where-Object { $_ -and $_ -notmatch '^#' } |
        ForEach-Object {
            $c = $_ -split "`t"
            $title = ($c[4] -replace '-\d{4,}$', '-N')
            '{0}|{1}|{2}|{3}' -f $c[0], $c[1], $c[3], $title
        })
}

if ($Diff) {
    if (-not $Against) { throw '-Diff needs -Against' }
    $names = @(Get-ChildItem -LiteralPath $Diff -Filter '*.tsv' |
        ForEach-Object { $_.Name }) | Sort-Object -Unique
    $changed = 0
    foreach ($name in $names) {
        $a = ConvertTo-CensusRows (Join-Path $Diff $name)
        $bPath = Join-Path $Against $name
        if (-not (Test-Path -LiteralPath $bPath)) {
            Write-Output "MISSING  $name"
            $changed++
            continue
        }
        $b = ConvertTo-CensusRows $bPath
        $delta = @(Compare-Object $a $b)
        if ($delta.Count -eq 0) {
            Write-Output ("same     {0,-26} {1} rows" -f $name, $a.Count)
            continue
        }
        $changed++
        Write-Output ("CHANGED  {0,-26} {1} -> {2} rows" -f $name, $a.Count, $b.Count)
        foreach ($row in $delta) {
            $mark = if ($row.SideIndicator -eq '=>') { '  + ' } else { '  - ' }
            Write-Output "$mark$($row.InputObject)"
        }
    }
    Write-Output ''
    Write-Output "$changed of $($names.Count) surfaces changed"
    return
}

# --- locate or build the probe ---------------------------------------------
$source = Join-Path $PSScriptRoot 'menucensus.cpp'
if (-not (Test-Path -LiteralPath $source)) { throw "menucensus.cpp not found beside $PSScriptRoot" }

$buildDir = Join-Path ([IO.Path]::GetTempPath()) 'nilesoft-menucensus'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$exe = Join-Path $buildDir 'menucensus.exe'

$needsBuild = -not (Test-Path -LiteralPath $exe)
if (-not $needsBuild) {
    $needsBuild = (Get-Item -LiteralPath $source).LastWriteTimeUtc -gt (Get-Item -LiteralPath $exe).LastWriteTimeUtc
}

if ($needsBuild) {
    # Same editions build.ps1 looks for, one directory deeper.
    $vcvars = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $vcvars) { throw 'vcvars64.bat not found; install the C++ build tools.' }

    Write-Host "Building the probe with $vcvars" -ForegroundColor DarkGray
    $libs = 'shell32.lib shlwapi.lib ole32.lib user32.lib'
    # Compile from the build directory rather than passing /Fo. A quoted path
    # ending in a backslash - /Fo"C:\dir\" - escapes its own closing quote in
    # cmd, and cl then reports "D8003: missing source filename" with the source
    # plainly on the command line.
    & cmd /c "call `"$vcvars`" >nul 2>&1 && cd /d `"$buildDir`" && cl /nologo /std:c++20 /EHsc /O2 /W4 /Fe:`"$exe`" `"$source`" $libs"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $exe)) { throw 'probe build failed' }
}

if (-not $Out) { $Out = Join-Path $PSScriptRoot 'baseline\census' }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# --- live census, repeated until two consecutive readings agree -------------
Write-Host 'Live desktop menu (waiting for two identical readings)...' -ForegroundColor Cyan
# The individual readings go in a subdirectory: only the settled census belongs
# at the top level, or -Diff walks the unsettled ones too and reports noise.
$runDir = Join-Path $Out 'runs'
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$settled = $false
$previous = $null
for ($i = 1; $i -le $MaxLiveRuns; $i++) {
    $path = Join-Path $runDir "live-desktop-run$i.tsv"
    & $exe live-desktop 2>$null | Out-File -LiteralPath $path -Encoding utf8
    $rows = ConvertTo-CensusRows $path
    Write-Host ("  run {0}: {1} rows" -f $i, $rows.Count)

    if ($previous -and -not (Compare-Object $previous $rows)) {
        Copy-Item -LiteralPath $path -Destination (Join-Path $Out 'live-desktop.tsv') -Force
        Write-Host ("  settled at {0} rows after {1} readings" -f $rows.Count, $i) -ForegroundColor Green
        $settled = $true
        break
    }
    $previous = $rows
    Start-Sleep 6
}
if (-not $settled) {
    Write-Warning "live census never produced two identical readings in $MaxLiveRuns tries - the menu is genuinely unstable, do not treat a diff against it as signal"
}

# --- static census, one reading per surface ---------------------------------
Write-Host 'Static handler census...' -ForegroundColor Cyan
$desktop = [Environment]::GetFolderPath('Desktop')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$targets = @(
    @{ Name = 'static-bg-desktop';  Args = @('bg', $desktop) },
    @{ Name = 'static-bg-repo';     Args = @('bg', $repo) },
    @{ Name = 'static-bg-drive-c';  Args = @('bg', 'C:\') },
    @{ Name = 'static-item-readme'; Args = @('item', (Join-Path $repo 'README.md')) },
    @{ Name = 'static-item-folder'; Args = @('item', (Join-Path $repo 'docs')) },
    @{ Name = 'static-multi';       Args = @('item', (Join-Path $repo 'README.md'), (Join-Path $repo 'LICENSE')) }
)
foreach ($t in $targets) {
    $path = Join-Path $Out "$($t.Name).tsv"
    & $exe @($t.Args) 2>$null | Out-File -LiteralPath $path -Encoding utf8
    Write-Host ("  {0,-22} {1} rows" -f $t.Name, (ConvertTo-CensusRows $path).Count)
}

Write-Host "census written to $Out" -ForegroundColor Green
