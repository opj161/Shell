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
    [int]$MaxLiveRuns = 10,
    [switch]$Sets
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

if ($Diff -and $Sets) {
    # The second pass the positional diff needs. A row's identity there includes
    # its position, so one inserted item marks everything below it as changed:
    # static-item-readme reported 40 -> 55 rows with 31 added and 24 removed
    # lines, of which 15 were genuinely new and the rest had merely moved down.
    # Comparing titles as unordered sets answers "what appeared and disappeared",
    # which is the question a release-notes diff actually asks.
    if (-not $Against) { throw '-Diff needs -Against' }
    foreach ($f in (Get-ChildItem -LiteralPath $Diff -Filter '*.tsv')) {
        $a = @(ConvertTo-CensusRows $f.FullName | ForEach-Object { ($_ -split '\|', 4)[3] } | Where-Object { $_ })
        $bPath = Join-Path $Against $f.Name
        if (-not (Test-Path -LiteralPath $bPath)) { Write-Output ("MISSING  {0}" -f $f.Name); continue }
        $b = @(ConvertTo-CensusRows $bPath | ForEach-Object { ($_ -split '\|', 4)[3] } | Where-Object { $_ })

        $delta = @(Compare-Object $a $b)
        if ($delta.Count -eq 0) {
            Write-Output ("same set {0,-26} {1} -> {2} titles (order/position only)" -f $f.Name, $a.Count, $b.Count)
            continue
        }
        Write-Output ("CHANGED  {0,-26} {1} -> {2} titles" -f $f.Name, $a.Count, $b.Count)
        foreach ($row in ($delta | Sort-Object SideIndicator, InputObject)) {
            $mark = if ($row.SideIndicator -eq '=>') { '    NEW  ' } else { '    GONE ' }
            Write-Output "$mark$($row.InputObject)"
        }
    }
    return
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
        # Rows a recorder saw come and go are annotated rather than hidden: they
        # are still differences, they are just not evidence of anything.
        #
        # Matched on the TITLE, not the whole row. A volatile row changes the
        # position of everything below it, so matching `depth|pos|kind|title`
        # recognised the volatile row itself and nothing it displaced - measured
        # 2026-11-25, when NanaZip appearing and disappearing moved 45 rows and
        # only one of them was annotated. Separators are excluded: their title is
        # empty, and treating "" as volatile would suppress every separator diff.
        $volatileTitles = @()
        foreach ($dir in @($Diff, $Against)) {
            $vp = Join-Path $dir (($name -replace '\.tsv$', '') + '.volatile.txt')
            if (Test-Path -LiteralPath $vp) {
                $volatileTitles += @(Get-Content -LiteralPath $vp |
                    ForEach-Object { ($_ -split '\|', 4)[3] } |
                    Where-Object { $_ })
            }
        }
        function Test-Volatile([string]$row) {
            $t = ($row -split '\|', 4)[3]
            return ($t -and $volatileTitles -contains $t)
        }

        $real = @($delta | Where-Object { -not (Test-Volatile $_.InputObject) })
        if ($real.Count -eq 0) {
            Write-Output ("same*    {0,-26} {1} rows (differences are all known-volatile)" -f $name, $a.Count)
            continue
        }

        $changed++
        Write-Output ("CHANGED  {0,-26} {1} -> {2} rows" -f $name, $a.Count, $b.Count)
        foreach ($row in $delta) {
            $mark = if ($row.SideIndicator -eq '=>') { '  + ' } else { '  - ' }
            $note = if (Test-Volatile $row.InputObject) { '   (known volatile)' } else { '' }
            Write-Output "$mark$($row.InputObject)$note"
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
$desktop = [Environment]::GetFolderPath('Desktop')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path

# The individual readings go in a subdirectory: only the settled census belongs
# at the top level, or -Diff walks the unsettled ones too and reports noise.
$runDir = Join-Path $Out 'runs'
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

# One live surface, not three, and the reason is worth keeping.
#
# menucensus also has live-taskbar and live-folder modes, and neither can be
# recorded from an agent shell - measured 2026-11-25, ten attempts each:
#
#   live-taskbar  Shell_TrayWnd ignores a posted WM_CONTEXTMENU, and so does the
#                 real child under the point, which is a
#                 Windows.UI.Composition.DesktopWindowContentBridge. The Windows
#                 11 taskbar is a XAML surface; reaching its menu needs synthetic
#                 input, and if Windows answers with a XAML flyout rather than an
#                 HMENU there is no menu handle to read at all. Check the taskbar
#                 by hand.
#   live-folder   no new CabinetWClass window ever appeared, from the probe's own
#                 ShellExecuteW or from Start-Process explorer.exe. The probe
#                 finds Progman, SHELLDLL_DefView and Shell_TrayWnd in the same
#                 breath, so its window lookups work; a folder window launched
#                 from here simply does not arrive. Consistent with the
#                 registry-write asymmetry AGENTS.md records for this shell,
#                 though that is not established as the same cause.
#
# Run those two modes by hand from an ordinary terminal if you want them.
$live = @(
    @{ Name = 'live-desktop'; Args = @('live-desktop') }
)

# Three consecutive identical readings, not two. Two agreeing readings only
# means nothing changed in the five seconds between them: measured 2026-11-25,
# the desktop menu settled at 29 rows across seven readings and then settled
# just as convincingly at 27 an hour later, because NanaZip and PowerToys' "Unlock
# with File Locksmith" had stopped contributing. Rows that did not appear in
# every reading are written to <surface>.volatile.txt, so a later diff can tell
# a real change from a handler that comes and goes.
foreach ($surface in $live) {
    Write-Host "$($surface.Name) (waiting for three identical readings)..." -ForegroundColor Cyan
    $settled = $false
    $previous = $null
    $agree = 1
    $seen = @()
    $lastPath = $null
    for ($i = 1; $i -le $MaxLiveRuns; $i++) {
        $path = Join-Path $runDir "$($surface.Name)-run$i.tsv"
        & $exe @($surface.Args) 2>$null | Out-File -LiteralPath $path -Encoding utf8
        $rows = ConvertTo-CensusRows $path
        $timing = (Select-String -LiteralPath $path -Pattern '^# timing-ms').Line
        Write-Host ("  run {0}: {1} rows   {2}" -f $i, $rows.Count, $timing)
        if ($rows.Count -gt 0) { $seen += , $rows }
        $lastPath = $path

        if ($null -ne $previous -and $rows.Count -gt 0 -and -not (Compare-Object $previous $rows)) {
            $agree++
            if ($agree -ge 3) {
                Copy-Item -LiteralPath $path -Destination (Join-Path $Out "$($surface.Name).tsv") -Force
                Write-Host ("  settled at {0} rows after {1} readings" -f $rows.Count, $i) -ForegroundColor Green
                $settled = $true
                break
            }
        }
        else {
            $agree = 1
        }
        $previous = $rows
        Start-Sleep 5
    }

    if (-not $settled) {
        if ($seen.Count -eq 0) {
            Write-Warning "$($surface.Name): produced no readings at all in $MaxLiveRuns tries - see $lastPath for why"
        }
        else {
            Write-Warning "$($surface.Name): no three identical readings in $MaxLiveRuns tries - treat a diff against it as noise, not signal"
        }
    }

    if ($seen.Count -gt 1) {
        $union = @($seen | ForEach-Object { $_ } | Sort-Object -Unique)
        $stable = @($union | Where-Object { $row = $_; -not ($seen | Where-Object { $_ -notcontains $row }) })
        $volatileRows = @($union | Where-Object { $stable -notcontains $_ })
        if ($volatileRows.Count -gt 0) {
            $volatileRows | Set-Content -LiteralPath (Join-Path $Out "$($surface.Name).volatile.txt") -Encoding utf8
            Write-Host ("  {0} row(s) were not present in every reading - recorded as volatile" -f $volatileRows.Count) -ForegroundColor Yellow
        }
    }
}

# --- static census, one reading per surface ---------------------------------
Write-Host 'Static handler census...' -ForegroundColor Cyan
$targets = @(
    @{ Name = 'static-bg-desktop';  Args = @('bg', $desktop) },
    @{ Name = 'static-bg-repo';     Args = @('bg', $repo) },
    @{ Name = 'static-bg-drive-c';  Args = @('bg', 'C:\') },
    @{ Name = 'static-item-readme'; Args = @('item', (Join-Path $repo 'README.md')) },
    # An archive, because 26H2's release notes name new archive formats (uu,
    # cpio, xar, nupkg) and the first baseline censused no archive at all - so
    # when "Extract All..." turned up on a .nupkg afterwards there was nothing to
    # compare it against and no way to tell a new verb from an old one.
    @{ Name = 'static-item-nupkg';  Args = @('item', (Join-Path $repo 'packages\choco\nilesoft-shell.1.9.nupkg')) },
    @{ Name = 'static-item-folder'; Args = @('item', (Join-Path $repo 'docs')) },
    @{ Name = 'static-multi';       Args = @('item', (Join-Path $repo 'README.md'), (Join-Path $repo 'LICENSE')) }
)
foreach ($t in $targets) {
    $path = Join-Path $Out "$($t.Name).tsv"
    & $exe @($t.Args) 2>$null | Out-File -LiteralPath $path -Encoding utf8
    Write-Host ("  {0,-22} {1} rows" -f $t.Name, (ConvertTo-CensusRows $path).Count)
}

Write-Host "census written to $Out" -ForegroundColor Green
