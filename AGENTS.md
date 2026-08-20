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

This is not ceremony. Every substantive fix in the 1.9.20 latency and hardening
work came from a documentation page contradicting an assumption in the tree:

| What the code assumed | What the documentation says |
| --- | --- |
| `WM_INITMENUPOPUP` could be sent with `lParam = 0xFFFFFFFF` for any popup | The low word is the opening item's position and the high word is `TRUE` only for the window menu — `0xFFFFFFFF` claims position 65535 in a window menu |
| The whole native menu tree had to be initialised up front | It is sent "when a drop-down menu or submenu is **about to become active**", i.e. one popup at a time |
| UI Automation could be called from the taskbar's message handler | A client that inspects its own UI from the UI thread can see "very slow performance, or even cause the application to stop responding"; use a separate MTA thread that owns no windows |
| An interface pointer could be stored process-wide and used from any thread | "Interface pointers must be marshaled when passed between apartments" |
| The Global Interface Table was the way to do that | For new code Microsoft recommends `RoGetAgileReference` instead — refcounted, eager-marshaling, no cookie to revoke |
| Only the binary component needed an architecture-specific code | A new code is required for *any* change of a resource's target location, and `ProgramFiles6432Folder` resolves differently per architecture — so every component under `INSTALLFOLDER` needed one |

Cite the URL in the commit message and, where the reasoning is not obvious from
the code, in a comment next to it. A future reader needs to be able to check the
claim, not take it on faith.

A citation is a deep link to the specific API or topic page, plus the sentence you
are relying on, quoted. A landing page, a search result, or a paraphrase from
memory is not a citation. Every finding you report — including every "reviewed
clean" — carries a doc URL, a probe, or the explicit word *unverified*. If nothing
documents the behaviour, say so and cite the probe instead: `NtUserTrackPopupMenu`
has no page, and what `TrackPopupMenu` puts in `WM_INITMENUPOPUP`'s `lParam` was
established by measurement, not by reading. If the page and the machine disagree,
the machine decides what the code does and the divergence goes in a comment.

Check both directions. *Code → documentation*: take the API call in front of you
and read what it promises. *Documentation → code*: take a documented edge
condition — required-size semantics, MSI sequencing, registry element limits —
and go looking for the places that fail to implement it. The second direction is
what finds the defect nobody suspected — the `release(n - 1)` shape described
below was live in several files at once, and reads as ordinary code right up
until you know what the API returns in the empty case.

### The plan is not the specification either

Implementation plans in this repo are starting points. Two of their proposals
were superseded by the documentation once it was actually read — the GIT (use an
agile reference) and the scope of the MSI component change (wider than proposed).
One proposal — never blocking the taskbar thread on the UIA worker — would have
broken the first right-click of every sequence; the documentation supplied the
correct primitive (`CoWaitForMultipleHandles`, which enters the COM modal loop on
a single-threaded apartment) instead.

Assess each item on its merits. Implementing something because a document listed
it, when measurement says it does not matter, is as much a mistake as skipping
something that does.

## Verify empirically, then pin it with a test

Documentation tells you the contract; the machine tells you what actually
happens. Both, in that order.

Small throwaway probes are cheap and have repeatedly changed the design. Build
them in the scratchpad, not the tree:

```bash
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

Then encode the invariant in `src/tests`. The suites are dependency-free and
self-registering; see `src/tests/test.h`. Prefer testing a real invariant over a
mock: `test_native_menu_lazy` drives a real owner window whose child popups sleep
60 ms each, and asserts that opening the root pays none of it.

For anything the tests cannot reach — installers especially — inspect the
artifact instead of assuming. `scripts/` has no MSI tooling; use the
`WindowsInstaller.Installer` COM object from PowerShell to read the `Component`,
`RemoveFile` and `InstallExecuteSequence` tables out of a built package.

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

## Things that are easy to get wrong here

**Structured exception handling.** `NtUserTrackPopupMenu` and several hooks use
`__try`/`__finally`. MSVC refuses (C2712) to compile a function that mixes SEH
with objects that need unwinding, so those functions use plain-old-data helpers —
`menu_perf_begin`/`menu_perf_end` rather than a scope timer, `ShellExtCapture::has`
rather than `match`.

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
This shape was in four places at once (`Registry.cpp` ×3, `Environment.h`,
`Windows.h`, `FuncExpression.cpp`); `grep -rn "release(.*- *1)"` finds them.

**Windows string APIs that will not terminate for you.** Two separate traps,
both of which had been live in the tree for years:

- `RegQueryValueEx` documents that a `REG_SZ` "is NOT guaranteed to be
  null-terminated". Size from the byte count the *second* call reports, and drop
  a terminator only after checking one is there. `src/exe/src/Main.cpp` still
  carries the comment from the first time this bit, when the `TreatAs` ownership
  check could not recognise Shell's own redirect.
- `GetMenuItemInfo` truncates silently to whatever `cch` you passed. Use the
  documented two-call pattern (`dwTypeData = nullptr` to learn `cch`, then a
  buffer of `cch + 1`) — `Include/MenuText.h` wraps it. A fixed `MAX_PATH`
  buffer loses everything past 259 characters, and third-party extensions cross
  that line routinely.

**`MB_*` versus `WC_*` conversion flags.** `MultiByteToWideChar` takes `MB_*`;
`WideCharToMultiByte` takes `WC_*`. They are different numbers, and passing an
`MB_` flag to `WideCharToMultiByte` with `CP_UTF8` fails the call outright with
`ERROR_INVALID_FLAGS` — it does not degrade, it returns 0 for every input. Two
converters in `Encoding.h` did exactly this, which is why `sel.tofile()` wrote a
zero-byte file. Neither the compiler nor `/analyze` says a word.

**`Environment::Expand` returns; it does not modify.** `Expand(value).move()` as
a statement compiles, does the work, and throws the result away. Two call sites
read `REG_EXPAND_SZ` values this way and handed the caller the raw `%VARIABLES%`.

**Line endings.** Some committed blobs are CRLF while `.gitattributes` asks for LF
in the index, so the first edit to such a file shows as a whole-file diff. Say so
in the commit message and compare with `--ignore-space-at-eol`. Never normalise
files you are not otherwise changing.

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
(`Microsoft.VisualStudio.Component.VC.Tools.ARM64`) are installed, so
`.\build.ps1 -Platform arm64` compiles and packages. Only the test executable
cannot run, because the host is x64 — `build.ps1` skips it and says so.
