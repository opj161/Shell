# Fails the build when a defect or pattern the refactor removed comes back.
#
#   powershell -NoProfile -File scripts\check-invariants.ps1
#
# Keep -NoProfile. build.ps1 passes it; running without it costs whatever the
# machine's PowerShell profile costs, which on the development box here is
# about forty seconds and looks exactly like this script being slow.
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
    # A list of roots rather than one, because rule 11 has to cover both the
    # DLL and the harness: the trap it bans was live in src\tests\hostprobe
    # while production was already clean. A single string still binds - a rule
    # that names one directory does not have to say so twice.
    param([string[]]$Dir, [string[]]$Include)

    $out = @()
    foreach($one in $Dir)
    {
        $full = Join-Path $root $one
        if(-not (Test-Path $full)) { continue }
        $out += Get-ChildItem -LiteralPath $full -Recurse -File -Include $Include -ErrorAction SilentlyContinue
    }
    return $out
}

# Returns the file with /* */ and // comments blanked out, so a rule matches
# code rather than prose about code. Crude but sufficient: it only has to stop
# a mention inside a comment from reading as a call. String literals are left
# alone - no rule so far needs that, and doing it properly means tracking
# escapes and raw strings.
#
# Newlines are preserved exactly, so an offset into the result still maps to
# the right line of the original file.
#
# Returned whole rather than split into lines, because a rule has to be able to
# match a call that is written across several lines. The [^;]* rules are bounded
# by the statement terminator either way, so matching whole-file does not let
# them run past the call they are looking at - it only stops them missing one
# whose arguments were wrapped. Memoized because the rules overlap: four of them
# scan all of src for *.h and *.cpp.
$codeCache = @{}

function Get-CodeText
{
    param([string]$Path)

    if($codeCache.ContainsKey($Path)) { return $codeCache[$Path] }

    $text = Get-Content -LiteralPath $Path -Raw -ErrorAction SilentlyContinue
    if(-not $text) { $codeCache[$Path] = ''; return '' }

    $text = [regex]::Replace($text, '(?s)/\*.*?\*/', {
        param($m) ($m.Value -replace '[^\r\n]', ' ')
    })
    $text = [regex]::Replace($text, '//[^\r\n]*', '')

    $codeCache[$Path] = $text
    return $text
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

    @{ Name  = 'MIIM_STRING must not be cleared from a menu item (docs/refactor/05 section 3)'
       Dir   = 'src\dll\src'; Include = @('*.cpp', '*.h')
       Regex = 'fMask\s*&=\s*~\s*\(?\s*MIIM_STRING'
       Why   = 'measured: an owner-drawn item is read by a screen reader only because MIIM_STRING carries its title alongside MFT_OWNERDRAW - dropping it silently makes the menu unreadable' },

    # Reading the borrowed root's style is required (docs/refactor/09 R2): a host
    # that set MNS_NOTIFYBYPOS is owed WM_MENUCOMMAND, and Shell cannot know it is
    # owed without asking. What must never happen is Shell *applying* the style to
    # the menu it composed and tracks itself. So the ban is on the operation, not
    # on the token.
    #
    # Two things this used to miss, both found by asking what the tree actually
    # writes rather than what a violation would look like in the abstract.
    #
    # This codebase never calls SetMenuInfo directly. It calls MENU::set
    # (src\dll\src\Include\MenuItem.h), which is also how the required *read* is
    # written - MENU::get in ContextMenu.cpp. So only the dwStyle alternative could
    # ever have caught a real violation, and R2's own 'prove the gate' step planted
    # a SetMenuInfo shape: the one alternative this tree would never produce.
    #
    # And MNS_NOTIFYBYPOS is 0x08000000. A rule that only knows the symbolic name
    # is defeated by writing the number.
    #
    # Still defeated by an intermediate variable - `auto style = MNS_NOTIFYBYPOS;`
    # then assigning that - and that is a real limit of a lexical gate rather than
    # an oversight. The harness scenario
    # takeover.a_by_position_host_is_told_which_position is what would catch it
    # behaviourally.
    @{ Name  = "Shell's composed menu must not be given MNS_NOTIFYBYPOS (docs/refactor/01 section 3a)"
       Dir   = 'src\dll\src'; Include = @('*.cpp', '*.h')
       Regex = '(SetMenuInfo|MENU::set)\s*\([^;]*(MNS_NOTIFYBYPOS|0x0?8000000)|dwStyle\s*(\|)?=\s*[^;]*(MNS_NOTIFYBYPOS|0x0?8000000)'
       Why   = 'measured: with TPM_RETURNCMD a by-position menu returns 1 and sends no WM_MENUCOMMAND, so the selection is lost - see src\tests\hostprobe\fixtures\question.notifybypos_with_returncmd.trace. Reading the style off the host''s own menu with GetMenuInfo(MIM_STYLE) is required and is not this.' },

    @{ Name  = 'SystemParametersInfo on the menu path must not broadcast (docs/refactor/02 section 4a)'
       Dir   = 'src\dll\src'; Include = @('*.cpp', '*.h')
       Regex = 'SystemParametersInfoW?\s*\([^;]*SPIF_(SENDCHANGE|SENDWININICHANGE|UPDATEINIFILE)'
       Why   = 'broadcasts WM_SETTINGCHANGE to every top-level window twice per menu; pass fWinIni 0' },

    @{ Name  = 'GetState(TRUE) before first paint is forbidden (docs/refactor/02 section 2)'
       Dir   = 'src\dll\src'; Include = @('*.cpp', '*.h')
       Regex = 'GetState\s*\([^)]*,\s*TRUE\s*,'
       Why   = 'fOkToBeSlow TRUE lets a verb handler stall the menu thread without bound' }
,

    # The third member of the 'silent wrong answer' family AGENTS.md names, and
    # the only one that had no gate. release(n - 1) has one and the MB_*/WC_*
    # flags have one; this did not, and the trap came back in new harness code
    # after the project had already solved it twice.
    #
    # GetMenuItemInfo truncates to whatever cch it was given and reports success,
    # so a fixed buffer does not fail, it lies. The documented pattern is two
    # calls: dwTypeData null to learn cch, then a buffer of cch + 1.
    # https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmenuiteminfow
    #
    # Keyed on cch being sized from a compile-time array rather than from what
    # Windows just reported. The correct pattern always writes cch from the
    # query's own cch, which no ARRAYSIZE/_countof/std::size/sizeof expression
    # can produce.
    @{ Name  = 'Menu item text must use the documented two-call GetMenuItemInfo (AGENTS.md)'
       Dir   = @('src\dll\src', 'src\tests\hostprobe'); Include = @('*.cpp', '*.h')
       Regex = '\bcch\s*=\s*(ARRAYSIZE|_countof|std::size|sizeof)\b'
       Why   = 'GetMenuItemInfo truncates silently to the cch it was given; a fixed buffer loses everything past it and still returns TRUE. Third-party extension titles cross 260 characters routinely. Ask with dwTypeData = nullptr first, then read into cch + 1 - Include/MenuText.h in the DLL, hostprobe/Probe.h and hostprobe/MenuReader.h in the harness.' }
)

# Enable each of these as its phase lands; see docs/refactor/06 and 07.
$deferredRules = @()

function Test-Rules
{
    param($Rules, [switch]$WarnOnly)

    $count = 0
    foreach($rule in $Rules)
    {
        foreach($file in (Get-Sources -Dir $rule.Dir -Include $rule.Include))
        {
            $text = Get-CodeText -Path $file.FullName
            if(-not $text) { continue }

            # IgnoreCase to match what PowerShell's -match did before this
            # scanned whole files instead of single lines. Note that matching
            # the whole text also lets a [^;]* rule span lines, so a call
            # broken across several lines is caught rather than missed.
            foreach($match in [regex]::Matches($text, $rule.Regex, 'IgnoreCase'))
            {
                # Line number from the match offset: comment blanking preserves
                # every newline, so this is the line in the real file.
                $before = $text.Substring(0, $match.Index)
                $number = ([regex]::Matches($before, "`n")).Count + 1
                $lineStart = $before.LastIndexOf("`n") + 1
                $lineEnd = $text.IndexOf("`n", $match.Index)
                if($lineEnd -lt 0) { $lineEnd = $text.Length }
                $line = [pscustomobject]@{
                    Number = $number
                    Text = $text.Substring($lineStart, $lineEnd - $lineStart)
                }

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
