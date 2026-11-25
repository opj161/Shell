# Working in this repository

Nilesoft Shell is a DLL that Windows loads into `explorer.exe` and into
third-party file managers **by COM activation, not by injection** — `DllMain`
does nothing but record the instance, and `BootstrapOnce()` runs from
`DllGetClassObject`. Once activated it pins its own module, hooks
`TrackPopupMenu`, subclasses host windows, and hands the shell a COM
context-menu handler. Almost every interesting bug here is a Win32, COM or
Windows Installer contract being broken — not a logic error you can reason out
from the source alone.

Which host it can actually serve follows from that, and the distinction matters
before you debug a "broken menu" in one:

- **Explorer** — recognised by window class in `Selections::QueryShellWindow`.
- **A host that tracks the HMENU it handed the handlers** — `QueryContextMenu`
  binds that handle in `ShellExtCapture` and inserts *zero items*, and the
  `TrackPopupMenu` hook recognises it later and composes. **Everything** works
  this way; verified 2026-08-28.
- **A host that builds its own menu instead** cannot be served at all, and no
  change on this side fixes it. **Directory Opus** is the worked example: it
  "searches the system registry and its own file type settings for the commands
  to display on the menu"
  ([Context Menu](https://www.gpsoft.com.au/help/opus12/documents/Context_Menu.htm)),
  so a handler that adds no items contributes nothing. Its vendor says it does
  this deliberately, "largely in order to provide the same sort of functionality
  Nilesoft Shell does". Shell's value is *removing, moving and regrouping*
  existing items, which `QueryContextMenu` cannot express — through that door a
  handler can only add. Do not try to take Opus over; record it and move on.
- **A host that repaints its own menu windows** gets its own menus, by design.
  **WizTree** is the worked example (diagnosed 2026-11-25). It is Delphi, and
  VCL Styles' `TSysPopupStyleHook` replaces the window procedure of every
  `#32768` in the process with `SetWindowLongPtr`. Shell's owner-drawn items
  were then never drawn: a blank rectangle that only closing WizTree removed.
  Restoring that procedure on teardown also cut Shell's comctl32 subclass out
  before `WM_NCDESTROY`, so the backdrop windows were never destroyed.
  `Include/HostMenuPainter.h` detects the replacement, `WinEventProc` ends
  Shell's menu unseen, and the hook tracks the host's own menu for that click
  and every later one in the process. `Uninitialize()` now also destroys any
  backdrop a popup left behind. The same applies to WizFile, TreeSize and
  WinSCP whenever a VCL style (their dark modes) is active.

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
Re-confirmed 2026-08-28, and the asymmetry is specifically about *writes*:
reading agrees in both directions — this shell and a scheduled task both counted
101 subkeys under `HKCU\SOFTWARE` and both saw the same keys — but a value
written from here was still absent when a scheduled task read it back four
seconds later. So reading configuration to find out what a host sees is fine;
setting one for a host to pick up is not, and needs the task below.

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

**Inside an installer custom action, a registry write the ACL forbids can
"succeed" into a volatile store** (2026-11-25, Windows 11 26300 x64, undocumented).
In msiexec's custom-action server, opening the TrustedInstaller-owned
`{86ca1aa0-…}` key for `KEY_CREATE_SUB_KEY` succeeds and `RegCreateKeyExW` creates
`TreatAs` — but `NtQueryKey(KeyNameInformation)` on the returned handle names
`\REGISTRY\MACHINE\SOFTWARE\Volatile\00\MACHINE\SOFTWARE\Classes\...`, and the real
key never changes. A rerun then "opens the existing key" (disposition 2) while the
real one is absent. The same calls from an elevated shell or a SYSTEM scheduled
task fail with `ERROR_ACCESS_DENIED` as the ACL says, and the custom action's token
reports UAC virtualization neither allowed nor enabled. So a fallback keyed on
`ERROR_ACCESS_DENIED` never runs there: `src/shared/TreatAsRegistry.h` judges a
write by reading the real key back. `NtQueryKey` on the handle is how to see where
a registry write actually went; a reboot clears the store.

**Windows Installer's `WindowsBuild` is 9600 on Windows 10 and 11.** Under msiexec
`VersionNT` is 603 and `WindowsBuild` 9600 (verbose logs on 26200 and 26300), so a
condition of `WindowsBuild >= 22000` is never true. A session opened through the
`WindowsInstaller.Installer` COM object from PowerShell reports the real values,
because they follow the calling process's manifest — only a real install shows the
problem. Read `CurrentBuildNumber` with a `RegistrySearch` (`WINDOWSCURRENTBUILD`
in `setup.wxs`).

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

**And `GetModuleFileName` will not tell you, so do not ask it.** It reports the
name the file had at load, which after the rotation is the canonical path — so
a stale host looks identical to a current one in every module listing. This cost
a session on 2026-08-28: `Everything.exe` had been running since before the
deploy, its menu misbehaved, and the module list said
`C:\Program Files\Nilesoft Shell\shell.dll`. Compare the **mapped image size**
against the candidates on disk instead, which differs whenever the link did:

```powershell
(Get-Process Everything).Modules |
    Where-Object ModuleName -eq 'shell.dll' | Select-Object ModuleMemorySize
```

against each `shell.dll*`'s `SizeOfImage` in the install directory — the rotated
copies are still there. Explorer is restarted by the deploy script; every other
host that has ever raised a shell menu is not, so restart it before believing
anything it shows you.

**`SizeOfImage` often cannot discriminate, so check the PE link timestamp too.**
Measured 2026-11-25: all six `shell.dll*` in the install directory — file sizes
2 049 536 through 2 054 144 bytes, builds three months apart — reported
`SizeOfImage` 2 072 576, because section alignment rounds every difference away.
A later pair, 2 054 144 and 2 054 656 bytes, did the same. The mapped size is
still worth reading, but when it matches everything, the discriminator is
`IMAGE_FILE_HEADER.TimeDateStamp`:
`docs/upgrade/26h2/capture-state.ps1` records it per file, and it differed by
hours where the sizes were identical.

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

Building `tests.vcxproj` does not run it; `build.ps1` and CI run `tests.exe`
explicitly, which takes a suite-name substring as its one argument. Warnings are
errors in the test project.

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

**Rule out the harness's own race first, because it has one and it looks
identical.** Measured 2026-08-28: 3 failures in 21 native runs and 3 in 17
takeover runs, always one scenario, always a different one, always in the
`select.*` family, and always the trailing `WM_COMMAND` missing from the trace.
It reproduces in **native** mode, where Shell is never loaded into the process,
so it cannot be the product. `TrackPopupMenuEx` promises only that the owner
"does not receive a WM_COMMAND message from the menu until the function
returns" — a floor, not a delivery guarantee, and the message is *posted* — so
`Probe::track`'s drain stopping at the first empty `PeekMessage` dropped it
whenever the poster was descheduled, which is why the failures cluster under
load. The drain now waits for a quiet queue rather than one empty read. Two
consequences worth keeping: a single green hostprobe run is not a gate result,
report it as *N of M*; and an erratic `select.*` failure is this, not the
paragraph above.

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
install, for instance. `InstallFolder()` in `src/setup/ca/dllmain.cpp` used to
fall back to locating Shell by its registered CLSID, which silently serviced
*whatever Shell is registered on the machine* rather than the one being installed:
it was caught renaming the live installation's `shell.dll` aside while an entirely
different product was installing. The fallback is gone and an empty folder fails
the action; keep it that way. Schedule after `CostFinalize`; a deferred action
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

**`string::c_str()` used to answer `nullptr` for an empty string. It no longer
does — and the replacement rule is the opposite of the old one.** The history is
worth keeping, because it is the sharpest example in this file of a line that
looks ordinary and is not.

It was `return valid() ? m_data : nullptr` (`System/Text/string.h`) where
`valid()` is `m_data && m_length > 0`, so every legitimately empty string
answered `nullptr`. `std::wstring_view`'s pointer constructor calls
`char_traits::length` on whatever it is given, so `wstring_view(s.c_str())`
faulted on every empty `s`. Nothing warned: it compiled, `/analyze` said nothing.
And a top-level menu item's `path` is empty, so `signature_of(item->path.c_str(),
…)` faulted on the *first item of every menu* — not an edge case, the common
case. The symptom was a menu that never appeared, with `explorer.exe` still
running and nothing in any log, because the access violation happened inside the
hook's SEH region and `catch(...)` under `/EHsc` does not catch SEH.

`c_str()` and `operator wchar_t const *()` now answer `L""`. Of 532 `c_str()`
call sites, none tested the result against null, so nothing depended on it.
`operator wchar_t *()` — the **mutable** one — still answers `nullptr`, because
handing out a writable pointer into a string literal is worse than handing out
null.

So the rule now: **a sink that means "absent" must test emptiness, not the
pointer.** `if(x && *x)`, which is what this codebase already does in eight
places. Two sinks did not, and both were fixed with the conversion rather than
left to fail later: `Selections.h`'s `PathItem::get` tested `if(sep)` before
`replace(L"\\", sep)` and would have stripped every backslash from a path, and
`Shell.h`'s `OpenAndWait` assigned `sei.lpDirectory` unconditionally where
`ShellExecuteEx` documents `NULL` as "the current directory is used" and does not
define the empty string. Implicit conversions cannot be grepped, so if a
null-means-absent sink was missed it is still out there; the sweep covered
`if(<ptr>)` tests, NULL-sensitive Win32 sinks and `ptr ? a : b` ternaries.

**`ReplaceFileW` fails on a file you still hold open without
`FILE_SHARE_DELETE`.** It opens the replaced file with `DELETE` access, so a handle
of yours that shares only reading makes it fail with `ERROR_SHARING_VIOLATION`
(measured). `RestoreLegacyConfig` did exactly that, and every legacy upgrade with a
`shell.nss` to carry over failed with 1603 until the throwaway pair was actually
installed.

**Every reader of a file the user edits shares read, write and delete.** Two
dozen hosts reload `shell.nss` the moment it is saved. `_wfopen_s` "can't open a
file for sharing": measured 2026-11-25, while it held the file an editor's
`GENERIC_WRITE` open and a rename-replace's `DELETE` both failed with error 32, and
an editor holding the file for write made the parser's open fail. `File::Open`'s
binary read and `ConfigShadow::open_for_read` now open with all three share modes.
With those, an in-place save, `ReplaceFileW` and a POSIX-semantics rename land
while a host reads. A classic `MoveFileEx(REPLACE_EXISTING)` over an open file
still fails with `ERROR_ACCESS_DENIED` whatever the reader shares (measured).
The stamps catch a write that lands mid-read. A root that exists but cannot be
opened leaves `Status.Refresh` set, so the next menu retries, because releasing a
lock changes nothing the watcher can see.

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

- **Third-party host smoke tests**, for the hosts that reach Shell through
  `IShellExtInit`/`IContextMenu` rather than `IShellBrowser`. Partly closed on
  2026-08-28: **Everything** was driven by hand and composes correctly, which is
  the first real confirmation of the capture path outside the harness.
  **Directory Opus** is now known to be out of scope by its vendor's design (see
  the host classes at the top of this file) — do not spend time on it.
  **Total Commander** remains untested, and the question to answer there is the
  only one that matters: does it track the same `HMENU` it handed the handlers?
- **The MSI upgrade matrix.** Component identity and sequencing changes need real
  installs on clean VMs across the full set of upgrade combinations. Reading the
  emitted tables proves the package is authored as intended; it does not prove an
  upgrade behaves.

  One row of it *is* verifiable here, and worth repeating whenever the setup
  changes. `scripts/build-throwaway-msi.ps1` builds a throwaway pair from the real
  WiX sources: a different product name (so `INSTALLFOLDER` and the Start Menu
  entry differ), its own `UpgradeCode`, and component codes identical across the
  pair — that is what a same-architecture upgrade looks like — but shared with
  nothing the real install registered. `-Legacy` shapes the older one like
  upstream before 1.9.20. Install it into a custom folder, edit its `shell.nss`,
  upgrade, uninstall, reinstall, and read the verbose logs rather than the outcome
  alone — `Component: CONFIG;`, `Executing op: FileRemove` and each custom action's
  `Return value` say what Windows Installer decided, which is the part that keeps
  being surprising. Its registration is the real one, so redeploy afterwards.

  Run on 2026-11-25, it found two defects that every static check had passed:
  the legacy config bridge failed *every* upgrade that had a `shell.nss` to carry
  over (`RestoreLegacyConfig` held the file open without `FILE_SHARE_DELETE` while
  `ReplaceFileW` replaced it), and the Windows 11 redirect was never created on a
  fresh install (the redirected-write case below).

ARM64 is not one of these: the v143 ARM64 cross tools
(`Microsoft.VisualStudio.Component.VC.Tools.ARM64`) are installed here, so
`.\build.ps1 -Platform arm64` compiles and packages. Only the test executable
cannot run, because the host is x64 — `build.ps1` skips it and says so. The CI
runner needs the same component and does not ship with it; the workflow adds it
on that leg.
