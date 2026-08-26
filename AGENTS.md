# Working in this repository

Nilesoft Shell is a DLL injected into `explorer.exe` and into third-party file
managers. It hooks `TrackPopupMenu`, subclasses host windows, and hands the shell
a COM context-menu handler. Almost every interesting bug here is a Win32, COM or
Windows Installer contract being broken — not a logic error you can reason out
from the source alone.

That shapes how to work on it.

## Official documentation is the specification

**Read the vendor's reference page for every documented contract you touch —
Win32, COM, shell, MSI/WiX, Detours, MSVC — before you change the code, and
before you state a conclusion about it.** Assessment counts: "this is correct" is
a claim about a contract exactly as much as "this is a bug" is. Not from memory,
and not from what the surrounding code appears to assume — the surrounding code
is frequently the thing that is wrong.

### Query Microsoft documentation through Microsoft Learn MCP

Use the Microsoft Learn MCP tools for any Win32, COM, shell, UIA, MSI or MSVC
question here. `microsoft_docs_search` is for *discovery* — its snippets are not
evidence. `microsoft_docs_fetch` on the canonical URL is, and it is what you read
before changing code or stating a conclusion: the full section, including
parameter, return-value, remarks, threading and lifetime, for **every** contract
the conclusion rests on. `microsoft_code_sample_search` illustrates usage; a
sample never overrides the reference.

Tool names are namespaced differently by different clients and the advertised set
changes — discover them rather than concluding one is missing. If the MCP route
is genuinely unavailable, fetch the same pages from `learn.microsoft.com` and say
so; never fall back to memory or third-party summaries. For WiX, Detours, or
anything Learn does not cover, use the maintainer's own reference.

### Query WiX documentation from the local official mirror

`.bin/wix-docs` is the primary WiX reference: `rg` for the exact element,
attribute, command or warning, then read the whole page. Prefer
`.bin/wix-docs/schema` over tutorials or generated API pages for `.wxs`
authoring, and cite it by repository-relative path
(`.bin/wix-docs/schema/wxs/component.mdx`).

It is maintainer documentation, not the Windows Installer specification: a WiX
page says what the authoring *emits*, and the MSI, registry or COM contract
underneath it still comes from Learn. When both matter, cite both. If the mirror
lacks the page, use WiX's current official docs and record that it did not cover
it.

This is not ceremony. Every substantive fix in the 1.9.20 latency and hardening
work came from a documentation page contradicting an assumption in the tree:

| What the code assumed | What the documentation says |
| --- | --- |
| `WM_INITMENUPOPUP` could be sent with `lParam = 0xFFFFFFFF` for any popup | The low word is the opening item's position and the high word is `TRUE` only for the window menu — `0xFFFFFFFF` claims position 65535 in a window menu |
| UI Automation could be called from the taskbar's message handler | A client that inspects its own UI from the UI thread can see "very slow performance, or even cause the application to stop responding"; use a separate MTA thread that owns no windows |
| Only the binary component needed an architecture-specific code | A new code is required for *any* change of a resource's target location, and `ProgramFiles6432Folder` resolves differently per architecture — so every component under `INSTALLFOLDER` needed one |

The three shapes to expect: a field whose meaning you guessed, a threading rule
stated only in a remark, and a requirement that turns out to be wider than the
one case you were looking at.

Cite a canonical deep link to the specific API or topic page and quote the short
passage on which the conclusion depends. A landing page, search result, MCP search
snippet, AI summary, code sample or paraphrase from memory is not a contract
citation. For a conclusion that crosses several APIs, cite every material
contract, not just the most convenient page.

Put those citations in every finding you report — including every "reviewed
clean" — and in the commit message when making a commit. Where the reasoning is
not obvious from the code, leave the deep link and the relevant invariant in a
nearby comment. If nothing documents the behaviour, label the conclusion
*unverified* and cite a reproducible probe instead: `NtUserTrackPopupMenu` has no
page, and what `TrackPopupMenu` puts in `WM_INITMENUPOPUP`'s `lParam` was
established by measurement, not by reading.

If documentation and a probe disagree, record the Windows build, architecture,
toolchain, exact probe and observed result. The probe establishes what that
machine did; the documentation establishes what the code may portably rely on.
Do not silently replace a documented contract with an implementation accident.
If compatibility requires undocumented behaviour, isolate it, explain the
divergence in a comment and pin the observation with a focused test.

Check both directions. *Code → documentation*: take the API call in front of you
and read what it promises. *Documentation → code*: take a documented edge
condition — required-size semantics, MSI sequencing, registry element limits —
and go looking for the places that fail to implement it. The second direction is
what finds the defect nobody suspected — the `release(n - 1)` shape described
below was live in several files at once, and reads as ordinary code right up
until you know what the API returns in the empty case.

### The plan is not the specification either

Implementation plans in this repo are starting points. Proposals have repeatedly
been superseded once the documentation and runtime ownership were checked: a GIT
or agile reference became one-shot stream marshaling after the single-consumer
invariant was proved, and the MSI component-identity change was wider than first
proposed. One proposal — never blocking the taskbar thread on the UIA worker —
would have broken the first right-click of every sequence; the documentation
supplied the correct primitive (`CoWaitForMultipleHandles`, which enters the COM
modal loop on a single-threaded apartment) instead.

Assess each item on its merits. Implementing something because a document listed
it, when measurement says it does not matter, is as much a mistake as skipping
something that does.

## Verify empirically, then pin it with a test

Documentation tells you the contract; the machine tells you what actually
happens. Both, in that order.

Small throwaway probes are cheap and have repeatedly changed the design. Build
them in the scratchpad, not the tree:

```powershell
cl /nologo /std:c++20 /EHsc /O2 /I <repo>\src\dll\src /Fe:probe.exe probe.cpp <libs>
```

Worked examples from this codebase:

- Tracking what `TrackPopupMenu` puts in `WM_INITMENUPOPUP`'s `lParam` for a
  tracked root (`0`) versus a submenu at position 2 (`2`), which is what the
  synthesised notification now matches exactly.
- Timing the old package scan against the new index on the real registry:
  78 ms → 2.3 ms, and the answer changed from wrong to right.
- Measuring `IUIAutomation` on the taskbar: ~28 ms for the first query in a
  process, ~2–3 ms after — which showed the win is bounding the pathological
  case, not shaving milliseconds.
- Checking whether reconstructing `@{PackageFullName? ms-resource://…}` would fix
  `appx.name`: it resolved 2 of 38 packages, so it was not worth adding.

Three things reliably waste a probe's first attempt on this machine, all of
them avoidable:

- **Write source files with a file-writing tool, never a shell heredoc.** The
  Windows shells here are reached through enough layers that a backslash in
  C++ does not survive: `L'\\'` arrives as `L'\'`, which fails to compile with
  `C2017: illegal escape sequence` two lines away from anything that looks
  wrong. Nesting a heredoc inside another language multiplies the problem.
  Where a literal separator is genuinely needed, `const wchar_t sep = 0x5C;`
  sidesteps the question entirely.

  **The same applies to *editing*, not only to creating.** A `python - <<'PY'`
  patch script loses a backslash level exactly as a C++ heredoc does, and it
  fails in both directions: a search string containing an escape silently
  matches nothing, and a replacement containing one writes a *real* control
  character into a string literal, giving `C2001: newline in constant` at a line
  that looks fine. That happened four times in one session, including in the
  edit that added this paragraph. Use the file-editing tool for anything
  containing a backslash, or build the string with `chr(92)` so there is no
  escape left to eat.
- **Pass `-NoProfile` to every `powershell` invocation.** `build.ps1` already
  does for `check-invariants.ps1`. Without it the cost is whatever the
  machine's PowerShell profile costs — about forty seconds here — and it looks
  exactly like the script being slow, which is a long way to chase for
  nothing.
- **Do not check line endings with `sed`, `cat -A` or `file`.** The MSYS tools
  in Git Bash are in text mode and will show you clean `$` line ends on a file
  that is CRLF, or the reverse. `.gitattributes` checks source out as CRLF, so
  when a change might have introduced bare LF, count the bytes:
  `python -c "d=open(p,'rb').read(); print(d.count(b'\n')-d.count(b'\r\n'))"`.
  Zero is the answer you want.

A probe that links repository sources needs the same include roots and
libraries the owning project lists, not a minimal guess — `src\dll\src` for
`Include/...` paths, `src\shared` for `System.h`, and `user32.lib` for
`Packages.cpp`, whose `icontains` calls `CharUpperW`. `RegistryConfig.h` also
needs `Resource.h` and `Globals.h` ahead of it, for `APP_NAME` and the CLSID
literals, and `shlwapi.lib` for `Environment::Expand`.

## Ways an experiment can test something other than what you think

Each cost real time before it was found, and each produces a result that looks
like a finding about Windows — or, worse, like a regression in the change you
just made.

**A registry value set from an agent's shell is not visible to `explorer.exe`.**
Measured 2026-08-24: a key created here took `HKCU\SOFTWARE`'s subkey count to
101 in that shell while Explorer still counted 100 and answered
`ERROR_FILE_NOT_FOUND` for the key by name. Elevating with
`Start-Process -Verb RunAs` does not help — it stays inside the same tree. The
same `reg add` run from a **scheduled task** took Explorer's count to 101 and
its read to the value written, so that is the way to set one:

```powershell
$a = New-ScheduledTaskAction -Execute 'reg.exe' -Argument 'add "HKCU\SOFTWARE\Nilesoft\Shell" /v perf /t REG_DWORD /d 1 /f'
Register-ScheduledTask -TaskName 'SetHkcu' -Action $a -Force | Out-Null
Start-ScheduledTask -TaskName 'SetHkcu'; Start-Sleep 3
Unregister-ScheduledTask -TaskName 'SetHkcu' -Confirm:$false
```

This is the whole of the "the `perf` value reads back correctly but Explorer
logs nothing" puzzle that `docs/refactor/08-handoff.md` recorded as unexplained
and warned the next reader not to sink time into. The registry read, the log
path and the file permissions had all been ruled out correctly. Explorer had
simply never seen the value. Note that **files** and **HKLM** are not affected:
editing the installed `shell.nss` under elevation reaches Explorer, and so does
`shell.exe -register -treat`.

**Explorer restarts about a second after it is killed, so deploying while it is
down gives it the previous build.** `scripts/backup-and-upgrade.ps1` used to
stop Explorer, then copy; the Explorer that came back had already mapped the
old binary, which was then renamed aside underneath it —
`GetModuleFileName` still reports the name the file had at load, so the module
list looked right. Every "verified in a real Explorer" result on the takeover
branch was one build stale. The script now deploys first and restarts last, and
checks that the Explorer which came back started *after* the copy. Do not
reorder it. And do not read the installed file's creation time to decide: NTFS
file tunneling puts back the old one when a file reappears at a path it just
left, which rotate-then-copy does — the script stamps it explicitly.

**A context menu read seconds after an Explorer restart measures Explorer
settling, not your change.** Measured 2026-08-24 while checking a selection-path
change: the desktop menu read 28 items before deploying, **29 immediately after
the restart**, and 27 on every read from a few seconds later onwards. The
29-item read contained two Directory Opus items and, decisively, the *native*
`New` where the settled menu has Shell's own `New+` — so it was taken while
Shell's rules had not been applied and the third-party handler set had not
settled. Read that way, a no-op change looks like it rewrote the menu.

Two things make this safe to work with. Wait for the reading to repeat
identically — the extension set stabilises within a few seconds, and on this
machine one OneDrive item ("Move to OneDrive") still comes and goes between
otherwise identical menus, so a one-item diff is noise rather than signal. And
check that Shell composed the menu at all before comparing anything: the
presence of config-driven items (`New+`, `Terminal`, `File manage`, `Go To` in
the stock configuration) is the cheap proof, because their absence means you are
diffing Windows' own menu against Shell's.

**A test that only ever calls a function the way the test calls it proves
nothing about the way the product calls it** (2026-08-25).
`ConfigWatcher::start` is re-entered *by its own reload callback, on its own
thread* — that is the shipping path, because every successful load re-points the
watch set. `start()` began with `stop()`, which joined the calling thread;
`join()` on the current thread is `resource_deadlock_would_occur`, so it threw,
and the stop event was already signalled. The watcher died after exactly one
reload, `watching()` went on answering true, and `load_generation`'s `catch(...)`
swallowed the exception. Thirteen tests covered this class; every one of them
called `start()` from the test's own thread, which is the single caller the code
was correct for. It was found by editing a real `shell.nss` twice and noticing
only the first edit arrived. **When a function is re-entered from a callback it
owns, write the test that calls it from the callback.**

**A failure reported in different words from every other failure is a failure
nobody can find** (2026-08-25). The trace harness failed about one
run in four for three sessions and was written up twice as an unexplained
transient, because a fixture mismatch printed the offending line and *no*
`FAIL` and *no* scenario name — so a run said `23 scenario(s), 1 failure(s)`
with nothing in its output that a filter for the word every other failure uses
would show. It read as a bug in the counter. Making it say
`FAIL <name> does not match its recorded baseline` is what turned three
sessions of folklore into a bug report on the next run; the cause was a
documented `TrackPopupMenu` contract the harness had never met. When something
is hard to reproduce, check first that it is reported the same way as
everything else.

**A test that builds a path from `%TEMP%` and compares it against one Windows
handed back is also asserting that this machine has no 8.3 short names**
(2026-08-26). Every GitHub runner's temp directory is
`C:\Users\RUNNER~1\AppData\Local\Temp`. `Path::Full` keeps the spelling it was
given; a shell link read through `IShellLink` answers the canonical long form —
so two paths to one file compare unequal. Build the tree from the long form
(`GetLongPathNameW`; the tilde is *not* a shortcut for deciding whether to call
it). Reproduce by pointing `TEMP` at a short path, which takes a minute.

The same disagreement was live in production underneath it: `ConfigShadow::save`
compared the caller's `root` against the parser's `loaded` by string prefix,
matched nothing, and silently wrote no shadow at all.

**A green step that verifies nothing is worse than a red one** (2026-08-26).
The CI step meant to install the ARM64 toolset tested for *any*
directory named `arm64` under *any* MSVC `bin`. One matched, belonging to an
unrelated toolset version, so the step reported "already present", exited 0,
installed nothing — and the build went on failing with the identical error the
step existed to remove. Test for the thing the error is actually about: MSB8020
names a *platform toolset*, so the check is now the
`MSBuild\Microsoft\VC\v170\Platforms\ARM64\PlatformToolsets\v143` directory.

Then encode the invariant in `src/tests`. The suites are dependency-free and
self-registering; see `src/tests/test.h`. Prefer testing a real invariant over a
mock: `test_native_menu_lazy` drives a real owner window whose child popups sleep
60 ms each, and asserts that opening the root pays none of it.

For anything the tests cannot reach — installers especially — inspect the
artifact instead of assuming. Use the read-only `scripts/msi-query.ps1` wrapper
around `WindowsInstaller.Installer` to inspect the `Component`, `RemoveFile` and
`InstallExecuteSequence` tables in a built package.

## Measure before optimising

The menu path has opt-in phase timers. Turn them on with:

```
HKCU\SOFTWARE\Nilesoft\Shell    perf    REG_DWORD    1
```

The value doubles as the reporting floor in milliseconds. They are off by default
because `Logger` opens, appends and closes the log file for every line — an
always-on timer would put file I/O on the path being measured.

Do not start from SIMD, painting loops or allocator micro-tuning. The wins in
this codebase have all been *removing synchronous work from before the first
pixel*: a whole native menu tree that was initialised but never shown, a package
repository that was fully enumerated to answer one boolean.

## Deploying a local build

```powershell
.\scripts\backup-and-upgrade.ps1              # host architecture
.\scripts\backup-and-upgrade.ps1 -ResetConfig # also take the stock shell.nss
```

It self-elevates, and it restarts Explorer, so ask before running it.

The one thing worth knowing: **`shell.dll` can never be overwritten in place.**
Every process that has ever raised a shell context menu has it loaded, and Shell
pins its own module for the life of that process on purpose — two dozen holders
on a normal desktop (Chrome, Notepad, OneDrive, PowerToys, dllhost, svchost).
Stopping Explorer does not release it. What works is renaming: a mapped image
can be renamed within its volume, so the installed binary is rotated aside under
a name that cannot already exist and the new one is written at the canonical
path. Processes that already loaded the old file keep running it until they
exit, which is why a freshly deployed change may not appear in an app that was
already open.

Registration for a per-machine install lands in **HKLM**, not HKCU:
`HKLM\SOFTWARE\Classes\CLSID\{BAE3934B-…}\InprocServer32`. On Windows 11 the
menu only becomes the primary one via the `TreatAs` redirect that
`shell.exe -register -treat` writes for `{86ca1aa0-…}`; without it Shell lives
under "Show more options".

## Build and test

```powershell
.\build.ps1 -Platform x64     # also runs the unit suite
.\build.ps1 -Platform x86
.\build.ps1 -Platform arm64   # needs the v143 ARM64 cross tools
```

Building a single project needs `SolutionDir` passed explicitly:

```powershell
msbuild /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="<repo>\src\" src\dll\Shell.vcxproj
```

`/p:RunTestsAfterBuild=false` skips the post-build test run. Warnings are errors
in the test project.

Report suites individually rather than the aggregate check count.

[`.github/workflows/build.yml`](.github/workflows/build.yml) enforces
`check-invariants.ps1`, three-platform builds, the unit suite on x64/x86, and
`validate-msi-lifecycle.ps1` per package. Two things before you read a red run:

- **The runner is not this machine.** Visual Studio 18, which resolves the
  pinned `v143` toolset for Win32 and x64 but not ARM64 (the arm64 leg installs
  that component first), and a `%TEMP%` carrying an 8.3 short name.
- **hostprobe is not in CI and cannot be** — both modes need an interactive
  desktop, takeover mode a per-user COM override. It is a manual pre-merge step
  with a named owner ([`06`](docs/refactor/06-phases-and-tests.md) §3), it is the
  gate that has caught the most, and the recipe with its mandatory cleanup is in
  [`13`](docs/refactor/13-implementation-handoff.md) §4. Run it for anything
  touching the menu path.

## Things that are easy to get wrong here

**A `Release()` that happens only on the failure paths is usually a transfer,
not a leak.** An `IExplorerCommand` provider is handed two references — the
thread cache keeps one, the caller gets one — and the resolution loop released
the caller's only on `Hidden` and `Failed`. That reads as a leak on the
succeeding path and is not: `fill_menuitem_from_explorer_command` stores the
pointer and sets `explorer_command_owned`, and `~menuitem_t` releases it. The
item is the second owner.

"Fixing" it into an unconditional release was a **double-release** on every shown
verb, in objects still asked for `EnumSubCommands` and `Invoke` after
composition — a use-after-free, not a miscount. `Include/ProviderCache.h` makes
the handover explicit (`detach()`), keyed on what the item *actually took*, not
on `FillResult::Shown`: an `ECF_ISSEPARATOR` command returns `Shown` from a
branch that stores no pointer, and that path leaked for real.

The suite passed throughout, because the fake knew about the cache and the
handle and not about the third owner. What caught it was the **takeover
harness**, failing one to five scenarios a run with a *different set each time* —
which is what memory corruption looks like from outside, and is indistinguishable
from the desktop flakiness `fixtures/README.md` warns about. When the harness is
erratic rather than repeatably wrong, build the last known-green commit in a
worktree and run both under the same conditions before blaming the machine.

**Structured exception handling.** `NtUserTrackPopupMenu` and several hooks use
`__try`/`__finally`. MSVC refuses (C2712) to compile a function that mixes SEH
with objects that need unwinding, so those functions use plain-old-data helpers —
`menu_perf_begin`/`menu_perf_end` rather than a scope timer, `ShellExtCapture::has`
rather than `match`.

The trap is that a call which *looks* like a plain call can be one of those
objects. `_log.write(...)` is a variadic template that builds `string::Argument`
temporaries in the **caller's** frame, so a single log line added to the hook's
`__finally` fails the whole function with C2712 — and the error points at the
`__try`, not at the line that caused it. Put the call in its own function;
`log_breaker_opened()` in `Main.cpp` is exactly that and nothing else.

**Re-entrancy.** `SendMessageW(WM_INITMENUPOPUP)` runs arbitrary host and
third-party extension code synchronously. Hold no lock across it — not the hook
map, not a cache snapshot, not the capture registry. Copy what you need first.

**Shell's own subclass hook.** A `WM_INITMENUPOPUP` sent to the host comes back
through `ContextMenu::WindowSubclassProc` first. The target handle is published in
`_native_notify` so the hook passes it through instead of treating a host menu as
one of Shell's own popups.

**Custom actions and `INSTALLFOLDER`.** Directory properties are not resolved
until `CostFinalize` (sequence 1000). An immediate custom action scheduled before
that reads `INSTALLFOLDER` as empty whenever no UI sequence set it — a silent
install, for instance — and `InstallFolder()` in `src/setup/ca/dllmain.cpp` then
falls back to locating Shell by its registered CLSID. That silently services
*whatever Shell is registered on the machine* rather than the one being installed:
it was caught renaming the live installation's `shell.dll` aside while an entirely
different product was installing. Schedule after `CostFinalize`; a deferred action
gets the path through `CustomActionData` instead and never sees the property.

**`string::release(n - 1)` on an API's returned count.** `terminate()` clamps
`m_length` to `m_capacity - 1` rather than rejecting an out-of-range index, and
the minimum allocation is 16. So when the count is 0 — an empty registry value,
a failed `ExpandEnvironmentStrings` — `release(n - 1)` underflows to `SIZE_MAX`
and hands back a **15-character string of NULs**: `length()` is 15, `c_str()[0]`
is `'\0'`, and `empty()` is false. Every length-driven caller downstream is then
wrong, and nothing crashes to tell you. Guard the zero case before subtracting.

The six sites this was found in are all guarded now, so `rg 'release\(.*-\s*1\)'`
returns *correct* code — check the guard before concluding you have found
something. The rule is for the next one written.

**`string::c_str()` answers `nullptr` for an empty string, and a
`std::wstring_view` built from one is undefined.** `return valid() ? m_data :
nullptr` (`System/Text/string.h`), and `std::wstring_view`'s pointer constructor
calls `char_traits::length` on whatever it is given — so `wstring_view(s.c_str())`
faults on every empty `s`. Nothing warns: it compiles, `/analyze` says nothing,
and it reads like the most ordinary line in the file.

What makes this worse than it sounds is which strings are empty. A top-level
menu item's `path` is empty, so `signature_of(item->path.c_str(), …)` faulted on
the *first item of every menu* — not an edge case, the common case. The symptom
was a menu that never appeared, with `explorer.exe` still running and nothing in
any log, because the access violation happened inside the hook's SEH region and
`catch(...)` under `/EHsc` does not catch SEH. Convert through a helper that maps
null to an empty view (`MenuIdentity::view`), or take `const wchar_t *` and check.
Same family as `release(n - 1)` and the `MB_*`/`WC_*` flags: a silent wrong
answer from a line that looks right.

**`RegQueryValueEx` will not terminate a `REG_SZ` for you** — it "is NOT
guaranteed to be null-terminated". Size from the byte count the *second* call
reports, and drop a terminator only after checking one is there.

Two more of this family are now caught by `check-invariants.ps1`, so they need
watching only in review of the rule itself: an `MB_*` flag passed to
`WideCharToMultiByte` (which fails outright with `ERROR_INVALID_FLAGS` and
returns 0 for every input — it does not degrade), and a fixed-buffer
`GetMenuItemInfo` read (which truncates silently; use the two-call pattern that
`Include/MenuText.h` wraps).

**`Environment::Expand` returns; it does not modify.** `Expand(value).move()` as
a statement compiles, does the work, and throws the result away. Two call sites
read `REG_EXPAND_SZ` values this way and handed the caller the raw `%VARIABLES%`.

**Line endings.** `.gitattributes` says `* text=auto eol=crlf`: the stored blob
is LF, the checkout is CRLF. Never normalise files you are not otherwise
changing — in particular do not run `git add --renormalize .`, which restages
every stale-blob file in the repository at once.

If a file suddenly shows as a whole-file diff you did not make, it is a blob
committed before that attribute settled, surfacing because something
invalidated git's stat cache. `git diff --ignore-cr-at-eol` (empty) and the
working bytes against `git show HEAD:<path>` (identical) confirm it; then leave
it. Fixing one is its own commit touching nothing else.

**Namespaces.** `Nilesoft::Diagnostics` and `Nilesoft::Shell::Diagnostics` both
exist. At global scope in `Main.cpp` an unqualified `Diagnostics` is ambiguous;
use the `perf` alias or qualify fully.

## Things that need a machine this one is not

Be explicit in the commit message when you could not verify something, rather
than implying you did.

- **Third-party host smoke tests.** Total Commander, Directory Opus, Everything -
  the hosts that reach Shell through `IShellExtInit`/`IContextMenu` rather than
  `IShellBrowser`. Nothing here exercises that path.
- **The MSI upgrade matrix.** Component identity and sequencing changes need real
  installs on clean VMs across the full set of upgrade combinations. Reading the
  emitted tables proves the package is authored as intended; it does not prove an
  upgrade behaves.

  One row of it *is* verifiable here, and worth repeating whenever the setup
  changes. Build a throwaway pair from the real WiX sources with a different
  `UpgradeCode`, `ProductCode` and product name — so `INSTALLFOLDER` and the Start
  Menu shortcut differ — and with the `OnInstall`/`OnUninstall` actions stripped,
  because those run `shell.exe -register` and restart Explorer. Keep the *component*
  codes identical across the pair: that is what a same-architecture upgrade looks
  like. Shape the older one like whatever package you are upgrading from. Then
  install, edit its `shell.nss`, upgrade, and read the verbose log rather than the
  outcome alone — `Component: CONFIG;` and `Executing op: FileRemove` say what
  Windows Installer decided, which is the part that keeps being surprising.

ARM64 is not one of these: the v143 ARM64 cross tools
(`Microsoft.VisualStudio.Component.VC.Tools.ARM64`) are installed here, so
`.\build.ps1 -Platform arm64` compiles and packages. Only the test executable
cannot run, because the host is x64 — `build.ps1` skips it and says so. The CI
runner needs the same component and does not ship with it; the workflow adds it
on that leg.
