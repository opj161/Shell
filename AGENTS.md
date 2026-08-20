# Working in this repository

Nilesoft Shell is a DLL injected into `explorer.exe` and into third-party file
managers. It hooks `TrackPopupMenu`, subclasses host windows, and hands the shell
a COM context-menu handler. Almost every interesting bug here is a Win32, COM or
Windows Installer contract being broken — not a logic error you can reason out
from the source alone.

That shapes how to work on it.

## Official documentation is the specification

**Read the Microsoft Learn page for every Win32, COM, shell or MSI contract you
touch, before you change the code.** Not from memory, and not from what the
surrounding code appears to assume — the surrounding code is frequently the thing
that is wrong.

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
  installs on clean VMs across the upgrade combinations. Reading the emitted
  tables proves the package is authored as intended; it does not prove an upgrade
  behaves.

ARM64 is not one of these: the v143 ARM64 cross tools
(`Microsoft.VisualStudio.Component.VC.Tools.ARM64`) are installed, so
`.\build.ps1 -Platform arm64` compiles and packages. Only the test executable
cannot run, because the host is x64 — `build.ps1` skips it and says so.
