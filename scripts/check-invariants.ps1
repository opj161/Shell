# Fails the build when a defect or pattern the refactor removed comes back.
#
#   powershell -File scripts\check-invariants.ps1
#
# Paths are resolved relative to this script, so it runs from anywhere. It is
# invoked by build.ps1 after a successful build; run it directly when iterating.
#
# Two lists:
#   $rules          invariants that hold in the tree today -> violations FAIL.
#   $deferredRules  invariants whose enabling work has not landed yet
#                   (docs/refactor/06, Phase 1+) -> violations WARN.
#
# Matching ignores comments. Every rule below names something the code must not
# *do*; a comment explaining why it no longer does it is not a violation, and a
# gate that cannot tell the difference fires on the commit that satisfies it.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# Files are listed by recursive search from a root directory. PowerShell's `**`
# is not a recursive glob - 'src\dll\src\**\*.h' resolves exactly one directory
# level, which silently skipped src\dll\src\*.h and src\dll\src\Include\*\*.h.
function Get-Sources
{
    param([string]$Dir, [string[]]$Include)

    $full = Join-Path $root $Dir
    if(-not (Test-Path $full)) { return @() }

    Get-ChildItem -LiteralPath $full -Recurse -File -Include $Include -ErrorAction SilentlyContinue
}

# Strips // line comments, /* */ block comments and string literals, so a rule
# matches code rather than prose about code. Crude but sufficient: it only has
# to stop a mention inside a comment from reading as a call.
function Get-CodeLines
{
    param([string]$Path)

    $text = Get-Content -LiteralPath $Path -Raw -ErrorAction SilentlyContinue
    if(-not $text) { return @() }

    # Block comments first, replaced by newlines so line numbers survive.
    $text = [regex]::Replace($text, '(?s)/\*.*?\*/', {
        param($m) ($m.Value -replace '[^\r\n]', '')
    })

    $lines = $text -split "`n"
    for($i = 0; $i -lt $lines.Count; $i++)
    {
        $line = $lines[$i] -replace '//.*$', ''
        if($line.Trim()) { [pscustomobject]@{ Number = $i + 1; Text = $line } }
    }
}

$rules = @(
    @{ Name  = 'Recycle Bin query must not return to menu construction (docs/refactor/02 section 4)'
       Dir   = 'src\dll\src'; Include = @('ContextMenu.cpp')
       Regex = 'SHQueryRecycleBin[AW]?"?\s*[,)]'
       Why   = 'enumerates every drive synchronously before first paint' },

    @{ Name  = 'Direct2D/DirectWrite link dependencies stay removed (dead renderer, docs/refactor/04 section 2)'
       Dir   = 'src\dll\src'; Include = @('*.cpp', '*.h')
       Regex = '#pragma\s+comment\s*\(\s*lib\s*,\s*"(d2d1|dwrite)'
       Why   = 'adds system DLLs to the import table of every host process' },

    @{ Name  = 'DllGetClassObjectHook machinery stays deleted (it was never installed)'
       Dir   = 'src\dll\src'; Include = @('*.cpp', '*.h')
       Regex = 'DllGetClassObjectHook'
       Why   = 'dead hook that was declared, detoured and never attached' },

    @{ Name  = 'explicit destructor calls are forbidden (lifetime ends, members keep being used)'
       Dir   = 'src'; Include = @('*.h', '*.cpp')
       Regex = 'this\s*->\s*~\w+\s*\('
       Why   = 'use a private reset()/clear() helper and call it from the destructor' },

    @{ Name  = 'memcpy into this is forbidden (silently wrong the moment the type gains a member)'
       Dir   = 'src'; Include = @('*.h', '*.cpp')
       Regex = 'memcpy\s*\(\s*this\s*,'
       Why   = 'use the compiler-generated copy assignment' },

    @{ Name  = 'MB_* conversion flags must not be passed to WideCharToMultiByte'
       Dir   = 'src'; Include = @('*.h', '*.cpp')
       Regex = 'WideCharToMultiByte\s*\([^;]*\bMB_(PRECOMPOSED|COMPOSITE|ERR_INVALID_CHARS|USEGLYPHCHARS)\b'
       Why   = 'WideCharToMultiByte takes WC_*; an MB_* flag with CP_UTF8 fails outright with ERROR_INVALID_FLAGS' },

    @{ Name  = 'SystemParametersInfo on the menu path must not broadcast (docs/refactor/02 section 4a)'
       Dir   = 'src\dll\src'; Include = @('*.cpp', '*.h')
       Regex = 'SystemParametersInfoW?\s*\([^;]*SPIF_(SENDCHANGE|SENDWININICHANGE|UPDATEINIFILE)'
       Why   = 'broadcasts WM_SETTINGCHANGE to every top-level window twice per menu; pass fWinIni 0' }
)

# Enable each of these as its phase lands; see docs/refactor/06 and 07.
$deferredRules = @(
    @{ Name  = '[DEFERRED Phase 1] GetState(TRUE) before first paint is forbidden (docs/refactor/02 section 2)'
       Dir   = 'src\dll\src'; Include = @('ExplorerCommand.cpp')
       Regex = 'GetState\s*\([^)]*,\s*TRUE\s*,'
       Why   = 'fOkToBeSlow TRUE lets a verb handler stall the menu thread without bound' }
)

function Test-Rules
{
    param($Rules, [switch]$WarnOnly)

    $count = 0
    foreach($rule in $Rules)
    {
        foreach($file in (Get-Sources -Dir $rule.Dir -Include $rule.Include))
        {
            foreach($line in (Get-CodeLines -Path $file.FullName))
            {
                if($line.Text -notmatch $rule.Regex) { continue }

                $count++
                $relative = $file.FullName.Substring($root.Length + 1)
                if($WarnOnly)
                {
                    Write-Host ("WARN  {0}" -f $rule.Name) -ForegroundColor Yellow
                }
                else
                {
                    Write-Host ("INVARIANT VIOLATION  {0}" -f $rule.Name) -ForegroundColor Red
                }
                Write-Host ("    {0}:{1}: {2}" -f $relative, $line.Number, $line.Text.Trim())
                Write-Host ("    why: {0}" -f $rule.Why) -ForegroundColor DarkGray
            }
        }
    }
    return $count
}

$failures = Test-Rules -Rules $rules
$null = Test-Rules -Rules $deferredRules -WarnOnly

if($failures -gt 0)
{
    Write-Host ""
    Write-Host ("check-invariants: {0} violation(s)" -f $failures) -ForegroundColor Red
    exit 1
}

Write-Host ("check-invariants: OK ({0} rules, {1} deferred)" -f $rules.Count, $deferredRules.Count) -ForegroundColor Green
exit 0
