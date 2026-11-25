# Windows 11 26H2: pre-upgrade baseline and risk assessment

Captured 2026-11-25 on this machine, before any upgrade. Everything below is
either read off the machine, read out of the tree, or cited to the vendor. Where
a claim is an inference rather than a measurement it says so.

Companion files, all in `docs/upgrade/26h2/`:

| File | What it is |
| --- | --- |
| `capture-state.ps1` | Read-only snapshot of every piece of machine state Shell depends on, as JSON. Also diffs two snapshots. |
| `census.ps1` | Records the context menus on six surfaces plus the live desktop menu, normalised so two runs diff cleanly. Also diffs two censuses. |
| `menucensus.cpp` | The probe `census.ps1` builds and drives. Kept in the tree rather than the scratchpad because it has to survive the upgrade's reboot. |
| `baseline/pre-26h2-state.json` | Machine snapshot, 2026-11-25 at 26200.9974, taken **after** the prep below. **This is the thing to diff against.** |
| `baseline/census/` | The settled menu census. `live-desktop.tsv` plus six `static-*.tsv`; `runs/` holds the individual readings, and `live-desktop.volatile.txt` the rows known to come and go. |
| `baseline/tests-x64-26200.9974.txt` | Full x64 unit-suite output, 80 suites, `34794 checks, 0 failure(s)`. |
| `baseline/check-invariants.txt` | `check-invariants: OK (15 rules, 0 deferred)`. |
| `baseline/hostprobe-native.txt`, `baseline/hostprobe-takeover.txt` | Ten runs each, full output. |
| `baseline/reg-*.reg` | Registry exports of the three Shell CLSIDs, the Windows 11 menu CLSID with its `TreatAs`, and all eight `ContextMenuHandlers` registrations. |

---

## 1. The decisive finding: 26H2 is not an OS swap

This changes the whole shape of the risk, so it goes first.

Windows 11 26H2 ships as an **enablement package** on the servicing branch this
machine is already on. Microsoft Learn, *What's new in Windows 11, version 26H2*:

> "Devices running eligible versions of Windows 11 can move to version 26H2
> through a small enablement package rather than a full operating system
> upgrade."

and

> "Because Windows 11, versions 26H2, 25H2, and 24H2 share a common servicing
> branch, many new features and improvements have already been delivered through
> monthly servicing updates. Version 26H2 enables selected features and resets
> the support lifecycle for the new release."

— <https://learn.microsoft.com/en-us/windows/whats-new/whats-new-windows-11-version-26h2>

The release-health tables make the shared payload concrete. The same KB produces
the same revision on both versions:

| KB | 25H2 | 26H2 |
| --- | --- | --- |
| KB5132061 (2026-11-10) | 26200.9974 | 26300.9974 |
| KB5132057 (2026-11-19) | 26200.10075 | 26300.10075 |

— <https://learn.microsoft.com/windows/release-health/windows11-release-information>

**This machine is at 26200.9974 — the 26H2-equivalent revision of the same
cumulative.** Going to 26H2 moves it to 26300.9974 (or .10075 if the November D
update lands with it). It does not replace `explorer.exe`, `shell32.dll`,
`twinui.pcshell.dll`, `win32u.dll` or `Windows.UI.FileExplorer.dll`: those are
already the files 26H2 runs. Note that every one of them already reports
`10.0.26100.xxxx`, because 26100 is the branch base for all three versions.

### What Windows Update is actually offering, as of 2026-11-25

Read out of the Windows Update agent on this machine (`Microsoft.Update.Session`,
a read-only search):

```
Title       : Windows 11, version 26H2
KB          : 5132061
Categories  : Upgrades
IsMandatory : False
RebootBehavior : 2 (canRequest)
```

**KB5132061 is the same KB already installed here as the November cumulative** —
the machine is at 26200.9974, which that KB produced. That is the enablement
path, in the machine's own words.

Do **not** use the download size to judge this. The agent reports
`MinDownloadSize 0` and `MaxDownloadSize 96 818 MB` for it, and 1 509 MB for a
Defender signature update — both are bundle aggregates, not what gets
transferred. An earlier draft of this document suggested judging eKB versus full
upgrade by "tens of megabytes versus gigabytes"; that was wrong and is removed.
The discriminators that do work are the two above (category `Upgrades`, KB equal
to the installed cumulative) and, afterwards, the snapshot diff below.

### Testable prediction

Re-running `capture-state.ps1` after the upgrade and diffing should report
**no change at all** under *"did the files Shell depends on change?"*, unless a
cumulative update lands in the same session. If that section shows changed
binaries, something larger than an enablement package happened and the rest of
this assessment needs revisiting.

### Known issues, checked 2026-11-25

Microsoft's 26H2 known-issues page lists three items: SQL Server linked-server
connections leaking (open), USB Audio Class 1.0 devices failing (resolved in
KB5132061, which this machine already has), and Remote Desktop failing for
non-admin users where Administrator protection is enabled (resolved in
KB5127936). **Nothing involving Explorer, the shell, context menus or shell
extensions.** No safeguard hold is described for this class of device.
<https://learn.microsoft.com/windows/release-health/status-windows-11-26h2>

Re-read that page after the upgrade before blaming Shell for anything.

### What *does* change, and why it still matters

Three feature sets come on by default in 26H2, and two of them are in Shell's
blast radius:

- **App-specific actions from the taskbar.** Shell hooks the taskbar menu
  (`Shell_TrayWnd` / `Shell_SecondaryTrayWnd` in
  `src/shared/System/Windows/Window.h:10`) and queries the taskbar through UI
  Automation on a separate MTA thread. `AppActions.exe` is *already* running on
  this machine with `shell.dll` loaded into it, so the feature is partly live
  here; 26H2 turns it on fully.
- **File Explorer enhancements**, described by Learn only as focused "on
  improving navigation and usability within File Explorer". The per-feature list
  on the same page includes new archive formats (`uu`, `cpio`, `xar`, `nupkg`),
  AI actions, and hover quick actions in Home — all of which are new *verbs and
  handlers*, i.e. new rows in the menu Shell composes.
- Windows settings backup (not relevant here).

So the realistic failure mode is not "Shell stops loading". It is "the menu
Shell composes gained or lost items", which is precisely what a menu census
diff detects and nothing else does.

---

## 2. The machine, as of 2026-11-25

### OS

| | |
| --- | --- |
| Product | Windows 11 Pro for Workstations (`ProductName` still reads "Windows 10 Pro for Workstations") |
| Version | 25H2, build **26200.9974** |
| Branch / lab | `ge_release`, `BuildLabEx 26100.1.amd64fre.ge_release.240331-1435` |
| Edition | `ProfessionalWorkstation`, composition `Enterprise` |
| Installed | 2026-08-27 · last boot 2026-11-22 00:18 |
| Latest quality updates | KB5132061 (2026-11-21), KB5132688 (2026-11-14), KB5127690 (2026-10-25) |
| WU policy | no `TargetReleaseVersion`, no deferral policy, no `C:\$WINDOWS.~BT` staging |

Nothing is holding 26H2 back. Learn's 26H2 status page says it "is now available
for all eligible devices" and that automatic delivery began expanding on
2026-11-18 (<https://learn.microsoft.com/windows/release-health/status-windows-11-26h2>),
so this machine will be offered it whether or not it is asked for.

### Installed Shell

Per-machine install under `C:\Program Files\Nilesoft Shell`, 37 files.

| | |
| --- | --- |
| `shell.dll` | 2 054 656 bytes, SHA256 `F3A7F31CB8B52A466FABB6FE141551AEE2AE37118445F109B7754CF765FA80D8` |
| | PE: `SizeOfImage` 2 072 576, machine `0x8664`, linked **2026-11-25 02:00:24 UTC** |
| `shell.exe` | 378 368 bytes, SHA256 `7F4CBE2D…`, FileVersion 1.9.20 |
| `shell.nss` | 1 010 bytes, SHA256 `07D4F706…` — a customised config, not the stock one, and the deploy kept it |
| Rotated copy | one, `shell.dll.old.20261125030255_8746` (the 2026-09-06 build the prep replaced); the deploy removed six older unmapped ones |
| ARP entry | "Nilesoft Shell" 1.9.20, ProductCode `{7BCA6512-254B-456F-8F08-AAD7508BE640}`, installed 2026-08-20 |
| Cached MSI | `C:\Windows\Installer\1b0c368.msi`, 1 314 816 bytes, SHA256 `080A9BB8…` |
| MSI source | `C:\Users\j_opp\Projects\Shell\src\bin\` — a locally built package, not a release |

### Registration

```
HKLM\SOFTWARE\Classes\CLSID\{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}   (and …BAF2, …BAF3)
    (Default)                       = Nilesoft.Shell
    InprocServer32\(Default)        = C:\Program Files\Nilesoft Shell\shell.dll
    InprocServer32\ThreadingModel   = Apartment

HKLM\SOFTWARE\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}       "File Explorer Context Menu"
    InProcServer32\(Default)        = C:\Windows\System32\Windows.UI.FileExplorer.dll
    TreatAs\(Default)               = {BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}      <-- the takeover
```

`TreatAs` is the single registry value that makes Shell the *primary* Windows 11
menu rather than something under "Show more options". Learn, *TreatAs*:

> "Specifies the CLSID of a class that can emulate the current class."

— <https://learn.microsoft.com/en-us/windows/win32/com/treatas>

Eight `shellex\ContextMenuHandlers\ @nilesoft.shell` registrations, all in HKLM,
all pointing at `…BAF1`: `*`, `Directory`, `Directory\background`,
`DesktopBackground`, `Drive`, `Folder`, `LibraryFolder`,
`LibraryFolder\background`. That is the documented discovery path
(<https://learn.microsoft.com/en-us/windows/win32/shell/context-menu-handlers>).
The snapshot records **70 handlers across 10 classes** in total, so Shell's eight
sit among 62 other registrations whose comings and goings are the main source of
menu-census noise.

Approved: `{BAE3934B-…BAF1} = @nilesoft.shell` under
`HKLM\…\Shell Extensions\Approved`. Blocked list contains only an unrelated
Epson entry.

### Hardening state is currently correct

`{86ca1aa0-…}` is owned by `NT SERVICE\TrustedInstaller` and grants
`BUILTIN\Users` `ReadKey` only. That is the stock shape, i.e. the machine-wide
hole described in `scripts/repair-treatas-acl.ps1` is **not** present here.
Worth re-checking after the upgrade, because a repair or re-register under an
older code path is exactly what would widen it again.

### Configuration

- `HKCU\SOFTWARE\Nilesoft\Shell` exists with **zero values** — so the `perf`
  phase timers are off, which is the default and the right state for a baseline.
- `HKCU\SOFTWARE\Classes\CLSID\{86ca1aa0-…}` existed with **zero subkeys and zero
  values** — a leftover from a hostprobe takeover run, inert because with no
  `InprocServer32` or `TreatAs` beneath it `HKEY_CLASSES_ROOT` merging still
  resolves the HKLM entries. **Removed during the prep**, so a post-upgrade
  reading of that path is unambiguous. It had to be deleted through a scheduled
  task, because a registry write from an agent shell is not visible to
  `explorer.exe` — the asymmetry `AGENTS.md` records, re-confirmed here.
- No per-user override under
  `HKCU\SOFTWARE\Classes\CLSID\{BAE3934B-…BAF1}` either: the hostprobe takeover
  runs below set one and the cleanup removed it, verified absent afterwards.
- Nothing re-applies the registration: no `Run` key, no `RunOnce`, no scheduled
  task references Nilesoft. **Whatever state the registry is in after the upgrade
  is the state it stays in.**

### Hosts holding the module

31 processes, every one mapping `C:\Program Files\Nilesoft Shell\shell.dll` at
`ModuleMemorySize` 2 072 576: `explorer`, `Everything`, `Code`, `claude`,
`OneDrive`, `PowerToys`, `WhatsApp.Root`, `StartMenuExperienceHost`,
`AppActions`, `WidgetBoard`, `TextInputHost`, `PhoneExperienceHost`, several
`dllhost` and `svchost`, and others. The deploy counted **42** holders before it
rotated the old binary aside.

`explorer.exe` started 2026-11-25 03:02:57, after the new `shell.dll` was written
at 03:00:24, so Explorer runs the deployed build — the deploy script checks that
ordering itself and reported it. Every *other* host in that list still maps the
pre-prep image until it exits, which is the normal state and not a fault; it does
mean none of them should be believed about Shell's behaviour until restarted.

---

### Gates, green, on the deployed build

All run 2026-11-25 against the binary now installed.

| Gate | Result |
| --- | --- |
| x64 build | 0 warnings, 0 errors |
| Unit suite | 80 suites, `34794 checks, 0 failure(s)` |
| `check-invariants.ps1` | `OK (15 rules, 0 deferred)` |
| hostprobe native | **10 of 10** clean, 23 scenarios each (10 skipped, as native mode skips the takeover family) |
| hostprobe takeover | **10 of 10** clean, 33 scenarios each |

Reported as *N of M* deliberately. The harness used to fail about one run in
seven — 3 of 21 native and 3 of 17 takeover, measured 2026-08-28, always one
`select.*` scenario, always a different one — because its post-tracking drain
stopped at the first empty `PeekMessage` and a *posted* `WM_COMMAND` could land
after it. The `Probe.h` fix in §3.1 is in this build. Twenty clean runs is
consistent with that fix working but does not prove it: at the old rate, ten
clean native runs would happen about one time in five anyway.

The takeover runs set the per-user COM override the recipe in
`docs/refactor/13-implementation-handoff.md` §4 requires, and the `takeover:`
line named `src\bin\x64\shell.dll` on every run — so they exercised the build
under test rather than the installed copy. The override and the staged
`shell.nss`/`imports` were removed afterwards and verified absent.

### Menu census

`baseline/census/`, recorded after the deploy and after Explorer settled.

| Surface | Rows | What it is |
| --- | --- | --- |
| `live-desktop` | **27** | what Explorer actually shows on the desktop background |
| `static-bg-desktop` | 19 | handler contributions, Desktop folder background |
| `static-bg-repo` | 20 | handler contributions, a normal folder background |
| `static-bg-drive-c` | 20 | handler contributions, `C:\` background |
| `static-item-readme` | 40 | one file selected |
| `static-item-folder` | 51 | one folder selected |
| `static-multi` | 35 | two files selected |

The live reading settles as `AGENTS.md` warns: consecutive readings after the
Explorer restart gave **26, 27, 29, 29**, then five more at 29.

**Two agreeing readings are not enough, and finding that out is the most useful
thing in this section.** An hour later the same recorder settled just as
convincingly at **27**, five readings running, because `NanaZip` and PowerToys'
`Unlock with File Locksmith` had stopped contributing to the desktop background
menu. Nothing was upgraded in between. So the recorder now requires **three**
consecutive identical readings, and writes any row that was not present in every
reading to `<surface>.volatile.txt`; `-Diff` annotates those rows and does not
count them as a change. Those two rows are recorded there explicitly, because a
single lucky run would not have produced them.

Read that as the accuracy limit of this instrument: a diff of one or two
third-party rows on this machine is noise. A new *Microsoft* verb is not.

Timing, from posting `WM_CONTEXTMENU` to the menu being readable and then to its
item count holding still, across the recorded readings:

| | popup appears | HMENU readable | count stable |
| --- | --- | --- | --- |
| desktop background | 141-235 ms | 172-235 ms | 375-438 ms |

Those are end-to-end numbers for the whole host, not Shell's share of it, and
they include the probe's own 25 ms polling granularity and its six-poll quiet
period — so the *floor* is about 150 ms by construction. They are worth
re-measuring after the upgrade only as a gross check: a jump to seconds means
something real, a 30 ms difference means nothing.

Shell demonstrably composed that menu: `Repomix`, `Terminal`, `File manage`,
`Develop` and `Go To` are all items from this machine's `shell.nss`, and they
carry command ids at or above `0x10000000` while Explorer's own items are in the
30 000s. (`New+` in the same menu is *PowerToys*' module, not Shell's — worth
knowing, because `AGENTS.md` lists `New+` among the cheap proofs that Shell
composed a menu, and on this machine it would be a false positive.)

Two normalisations were needed before a census could be diffed at all, both found
by diffing two consecutive runs of the recorder:

- **Shell renumbers its own command ids on every invocation.** Two readings were
  identical in title, kind and position while every Shell id had shifted by one.
  So a row's identity is `(depth, position, kind, title)`; diffing on the id
  reports a completely changed menu every time.
- **PowerToys' New+ submenu suffixes its template entries with a counter** —
  `tmp-255407` in one run, `tmp-253576` in the next. A trailing run of four or
  more digits is normalised to `-N`, keeping the prefix so a template appearing
  or disappearing is still visible.

Without those two rules the first post-upgrade diff would have shown three of
seven surfaces "changed" with nothing actually different.

## 3. Three things found during the inventory that were wrong

These are not 26H2 risks. They are defects that would be misread as 26H2 damage
if they were still in place when the upgrade happens. The first is now fixed; the
other two are properties of the install to keep in mind during triage.

### 3.1 The deployed binary was missing the divide-by-zero fix — fixed

The installed `shell.dll` had been linked **2026-09-06 02:45:32 UTC** (04:45:32
local). The commit log:

```
303c6fc 2026-09-06 04:48:26  perf(gates): the invariant check spent six minutes matching build output
469d35d 2026-09-06 04:48:49  fix(menu): the flicker wait could divide by zero and take explorer.exe with it
2a12e3a 2026-09-06 04:50:42  docs(17): three things this document asserted were not true
```

`src/bin/x64/shell.dll` had the same 04:45:32 timestamp and the same SHA256 as
the installed file, so no rebuild had happened after that link: **469d35d was not
in the deployed binary.** Its subject says what the consequence is. Going into an
upgrade that turns on new File Explorer and taskbar surfaces — i.e. new code
paths through menu composition — with a known Explorer-killing divide-by-zero
deployed was the wrong starting position.

Rebuilt and redeployed 2026-11-25. The deployed binary is now
`F3A7F31CB8B52A46…`, linked 2026-11-25 02:00:24 UTC, built from the working tree
at `2a12e3a` plus three uncommitted fixes that were already in it:

| File | What the edit does |
| --- | --- |
| `src/setup/ca/dllmain.cpp` | `BorrowReportScope` parks the `MSIHANDLE` for one custom action instead of assigning a global at entry and relying on every return path to clear it. |
| `src/shared/BorrowedKeyAccess.h` | Refuses to borrow a key whose DACL is null, because restoring a null DACL grants everyone full access — `SetSecurityInfo`'s documented behaviour, i.e. the machine-wide hole the class exists to remove. |
| `src/tests/hostprobe/Probe.h` | The drain after a tracking call waits for a quiet queue rather than one empty `PeekMessage`, and `item_title_at` compares against `GetMenuItemCount` in `int` rather than `UINT` so a `-1` return is not read as 4 294 967 295. |

Build clean on x64: 0 warnings, 0 errors.

### 3.2 The package and the files on disk disagree

The installed package is the **2026-08-20** generation. Its custom-action set is:

```
OnDowngrading, OnUpdate, OnUpdateElevatedData, OnUpdateElevated, OnInstall, OnUninstall
```

The current authoring in the tree is a different generation entirely:

```
OnDowngrading, BackupLegacyConfig, RestoreLegacyConfigRollback, RestoreLegacyConfig,
CleanupLegacyConfig, PrepareTreatAs, TreatAsRollback, TreatAsApply, TreatAsCommit,
NotifyShellChanged
```

So the `TreatAs` transaction that 1142e66 fixed is **not** in the installed
package, and the files it believes it installed are the 2026-08-20 payload, while
what is actually on disk is the 2026-09-06 build that
`scripts/backup-and-upgrade.ps1` rotated in.

**Consequence: never run `msiexec /f` (repair) on this install.** Repair restores
the package's own payload and would silently revert `shell.dll` to the August
build.

### 3.3 `SizeOfImage` does not discriminate the deploys on this machine

`AGENTS.md` says to tell a stale host from a current one by comparing the mapped
image size against each candidate on disk, because `GetModuleFileName` reports
the pre-rotation name. On this machine that test cannot distinguish anything.
Before the prep, **all six** `shell.dll*` files had `SizeOfImage` 2 072 576:

| File | Bytes | SizeOfImage | Linked (UTC) |
| --- | --- | --- | --- |
| `shell.dll` | 2 054 144 | 2 072 576 | 2026-09-06 02:45:32 |
| `…old.20260905235516_4109` | 2 054 144 | 2 072 576 | 2026-09-05 21:48:30 |
| `…old.20260906044931_6023` | 2 054 144 | 2 072 576 | 2026-09-05 21:55:04 |
| `…old.20260905232750_8380` | 2 052 096 | 2 072 576 | 2026-09-05 13:20:35 |
| `…old.20260905150843_3828` | 2 051 584 | 2 072 576 | 2026-09-05 12:48:25 |
| `…old.20260905150108_3591` | 2 049 536 | 2 072 576 | 2026-08-27 14:43:12 |

The section-alignment rounding absorbed every size difference. The prep did not
change this: the new build is 2 054 656 bytes and the one it replaced is
2 054 144, and **both** still round to `SizeOfImage` 2 072 576, which is also
what the restarted Explorer reports as its mapped size.

So for triage on *this* install the discriminator has to be the PE **link
timestamp** — 2026-11-25 02:00:24 for the deployed image, 2026-09-06 02:45:32 for
the rotated one — which `capture-state.ps1` records per file. Worth folding back
into `AGENTS.md`.

---

## 4. Should Shell be uninstalled before upgrading?

**No. Upgrade in place, with Shell installed.** The reasoning, in order of weight:

1. **There is no binary swap to protect against.** The classic reason to remove a
   shell extension before a feature update is that Setup replaces Explorer and
   the shell DLLs and migrates registration across. 26H2 does neither — it is an
   enablement package on the branch this machine already runs, and the
   `Windows.UI.FileExplorer.dll` that hosts the menu CLSID is already the 26H2
   one (§1).

2. **There is direct evidence the redirect survives servicing of exactly that
   component.** `C:\Windows\System32\Windows.UI.FileExplorer.dll` has
   `LastWriteTime 2026-11-14 21:33` — the November cumulative rewrote it. The
   `TreatAs` value is still present today, and nothing on this machine re-applies
   it (no Run key, no scheduled task, §2). So a CBS operation that *replaced the
   DLL* left the redirect alone. An enablement package does less than that, not
   more. This is measured, not assumed; the residual uncertainty is that an eKB
   re-stages component manifests and no vendor page states what that does to a
   third-party `TreatAs` on an inbox CLSID. Hence the post-upgrade check in §6.

3. **Uninstalling exercises the stale package, through two documented hazards.**
   `OnUninstall` in the installed package runs
   `<INSTALLFOLDER>\shell.exe -u -s -t -restart` at **sequence 27**, with type
   385 (immediate, asynchronous). Sequence 27 is before `CostFinalize` (1000), so
   `INSTALLFOLDER` is unresolved and `InstallFolder()` falls back to locating
   Shell by registered CLSID — the exact hazard `AGENTS.md` records as having
   been "caught renaming the live installation's `shell.dll` aside while an
   entirely different product was installing". It happens to resolve correctly
   here, because the registered CLSID does point at this install. It is still an
   uninstall that depends on a fallback rather than on the property.

4. **The failure mode of a bad uninstall is the worst available state.** If the
   redirect removal does not land, `{86ca1aa0-…}\TreatAs` is left pointing at
   `{BAE3934B-…BAF1}`, whose registration the uninstall just deleted: Windows 11's
   primary context-menu CLSID redirected to a class that no longer exists. That
   produces a broken right-click that looks exactly like 26H2 damage, and it is
   reached *by* the precaution.

5. **Reinstalling afterwards is not a clean path either.** The current repo
   package has the *same* `ProductCode {7BCA6512-…BE640}` and the same
   `ProductVersion 1.9.20` as the installed one, and its own `Upgrade` table has
   a `NEWPRODUCTFOUND` row at `VersionMin 1.9.20` with attributes 258
   (`VersionMinInclusive | OnlyDetect`) wired to `OnDowngrading`. A plain
   `msiexec /i` of it therefore meets "This or a newer version of [ProductName]
   is already installed" and exits. Getting back to a working install after an
   uninstall means either the same stale package or a deliberate
   `REINSTALL=ALL REINSTALLMODE=vamus`, decided under pressure with no menu.

6. **Recovery, if the redirect is lost, is one elevated command** —
   `shell.exe -register -treat` — which is cheaper than any uninstall/reinstall
   cycle. That asymmetry is the whole argument.

The one case that would change this answer: if Windows Update offers a *full*
24H2/25H2→26H2 upgrade on this machine rather than the enablement package. The
offer read in §1 says it is the enablement path, and the snapshot diff detects it
after the fact. Then the removal argument becomes real again, and it should be
reconsidered with the generation-B package built and ready first.

### There is probably no going back, and that is the real argument for a backup

The usual safety net for a feature update is *Settings > System > Recovery > Go
back*, within ten days. It very likely does not apply here. Microsoft's own
guidance on rolling back feature updates states:

> "Uninstallation will not be successful when the feature update was applied
> using an Enablement Package."

— <https://learn.microsoft.com/intune/device-updates/windows/manage-update-rings>
(the *Uninstall* tab)

That page is about Intune's Uninstall action, which drives the same
feature-update uninstall mechanism the consumer "Go back" button and
`setup.exe /uninstall` use. An enablement package also creates no `Windows.old`
to roll back *to*, which is what that mechanism needs — that part is inference,
not a cited contract, and it is cheap to settle after the upgrade by looking for
`C:\Windows.old`.

The honest position, then: **26H2 is very low risk and very hard to undo.** Those
are not in tension — little changes, so little should break, but if something
does, the way out is forward (a fix, or a repair install) rather than back.

Two consequences, neither of them done:

- **System Protection state here is unknown.** `RPSessionInterval` is 1, but
  `Get-ComputerRestorePoint` and the `SystemRestore` CIM class both answer
  *Access denied* without elevation, so whether protection is on for `C:` and
  whether any restore point exists could not be read. Worth checking in an
  elevated prompt, and worth creating one — or a full image — before upgrading,
  precisely because "Go back" probably is not available.
- **The user's `shell.nss` is not in version control.** It exists at
  `C:\Program Files\Nilesoft Shell\shell.nss` and in `backup_installed_shell\`,
  which is in `.gitignore` — two copies, one disk, nothing off-machine. An
  enablement package will not touch `C:\Program Files`, so this is not a 26H2
  risk; it is a standing one, and a repair install or a reset would take both
  copies. It was left out of this commit deliberately: `opj161/Shell` is a public
  fork, and a personal config is the owner's call to publish, not this
  document's.

---

## 5. Risk register

Ranked by expected cost, with the check that detects each and the fix.

| # | Risk | Likelihood | Detect | Fix |
| --- | --- | --- | --- | --- |
| 1 | `TreatAs` cleared → Shell drops under "Show more options" | Low (§4.2) but highest impact | `capture-state.ps1 -Diff`, first section | elevated `shell.exe -register -treat` |
| 2 | New 26H2 File Explorer verbs / handlers change the composed menu | **High — by design** | settled menu census vs baseline; `Handlers` diff in the snapshot | config rules in `shell.nss`; `modify.remove` |
| 3 | Taskbar app-actions surface changes the taskbar menu or the UIA query | Medium | taskbar right-click census; `perf` timers for the UIA worker | code change in the taskbar path |
| 4 | Deployed DLL's divide-by-zero kills `explorer.exe` under new code paths | **Closed 2026-11-25** | Explorer restarts on right-click | was: rebuild + redeploy (§3.1) |
| 5 | `Approved` entry or the CLSID registration dropped | Low | snapshot `Registration` / `Approved` | `shell.exe -register` |
| 6 | `{86ca1aa0-…}` ACL widened again by a re-register | Low | `scripts/repair-treatas-acl.ps1 -WhatIfOnly` | same script without the flag |
| 7 | IAT hook on `win32u!NtUserTrackPopupMenuEx` stops matching | Very low (no binary change) | menu does not appear at all | investigate; `src/dll/src/Main.cpp:1712` |
| 8 | Shell extensions disabled by a policy the upgrade turns on | Low | snapshot `Blocked` | remove the block |
| 9 | Something does break and there is no way back | Low, but **not mitigable after the fact** | `C:\Windows.old` absent, "Go back" greyed out | restore point or image taken *before* (§4) |

On #7: `IsWindows11OrGreater()` is `Major > 10 || (Major == 10 && Build >= 22000)`
(`src/shared/System/Windows/Windows.h:191`), so build 26300 keeps every Windows 11
branch taken. The expression functions compare against 22000 only
(`src/dll/src/Expression/FuncExpression.cpp:4277`, `:4329`, `:4341`, `:4344`).
**No code in the tree gates on 26200 or on any build above 22000**, so the build
number crossing 26200 → 26300 changes no decision. The 26200 references in the
tree are all in comments recording where a measurement was taken.

Related, and worth knowing before reading any post-upgrade measurement: every
empirical note in the tree is pinned to **26200.8875**
(`src/dll/src/Include/HostContract.h:33`, `PopupLifecycle.h:21`, `IconResource.h:30`,
`ProviderHealth.h:7`, `DiagnosticsRing.h:24`, and others). The machine is already
at **26200.9974**, eleven cumulative revisions on. Those numbers were stale before
26H2 entered the picture.

---

## 6. Procedure

### Before the upgrade — done 2026-11-25

1. ~~Rebuild and redeploy so the deployed binary contains 469d35d (§3.1).~~
   Done: `F3A7F31C…`, Explorer restarted after the copy.
2. ~~Delete the inert HKCU override.~~ Done, via a scheduled task.
3. ~~Re-run the snapshot so the baseline describes what is deployed.~~ Done.
4. ~~Record a settled menu census.~~ Done, 7 surfaces, §2.
5. ~~Record a hostprobe result as _N of M_.~~ Done, 10 of 10 both modes, §2.
6. ~~Commit `docs/upgrade/26h2/`~~ so a `git clean` cannot take the baseline with
   it. Done. (Untracked files survive a reboot perfectly well — the reason to
   commit is that the diff is worthless if half of it is missing.)

Still open, and all three need either elevation or a human at the keyboard:

7. **Create a restore point, or an image.** "Go back" is probably unavailable for
   an eKB (§4), and System Protection state could not be read without elevation.
8. **Census the taskbar menu**, before and after. The probe cannot open it, but
   `menucensus read-open` can read one you open: start it, then right-click the
   taskbar and leave the menu up (§7). It is the surface 26H2 most directly
   changes.
9. **Put `shell.nss` somewhere off this machine.** Two copies on one disk, both
   outside version control (§4).

### The upgrade itself

Settings → Windows Update → *Check for updates* → 26H2 → Download & install.
Note the download size: tens of megabytes means the enablement package and this
assessment holds; gigabytes means a full upgrade and §4's exception applies.
Close `Everything`, `Code`, `PowerToys` and the other long-running hosts first —
not for the upgrade's sake, but so that nothing is left holding a pre-upgrade
mapping of `shell.dll` afterwards and misreporting its own behaviour.

### After the upgrade

1. Snapshot and diff:

   ```
   .\docs\upgrade\26h2\capture-state.ps1 -Out .\docs\upgrade\26h2\baseline\post-26h2-state.json
   .\docs\upgrade\26h2\capture-state.ps1 -Diff .\docs\upgrade\26h2\baseline\pre-26h2-state.json -Against .\docs\upgrade\26h2\baseline\post-26h2-state.json
   ```

   The diff answers the four questions that matter in order: is Shell still the
   primary menu, is it still a registered handler, did the system binaries move,
   is the install itself intact.

2. **Wait before measuring anything.** A menu read seconds after a restart
   measures Explorer settling, not the upgrade — 26, 27, 29, 29 on this machine
   after the prep's restart. `census.ps1` handles this itself by reading until two
   consecutive readings agree; if it warns that they never did, the menu is
   genuinely unstable and a diff against it is not signal.

3. Re-census and diff. **This is where 26H2's real effect shows up** (risk #2),
   and nothing else will show it:

   ```
   .\docs\upgrade\26h2\census.ps1 -Out .\docs\upgrade\26h2\baseline\census-post
   .\docs\upgrade\26h2\census.ps1 -Diff .\docs\upgrade\26h2\baseline\census -Against .\docs\upgrade\26h2\baseline\census-post
   ```

   A clean result prints `0 of 7 surfaces changed`. Anything else names the
   surface and the rows, added and removed. Read a new row's title against the
   26H2 feature list in §1 before assuming it is Shell's doing — new archive
   formats and AI actions are expected to appear here.

4. Re-run the gates: `.\build.ps1 -Platform x64` (unit suite), and hostprobe
   native + takeover as *N of M*. Compare against
   `baseline/tests-x64-26200.9974.txt` — 80 suites, 0 failures — per suite, not
   on the aggregate check count, and against 10 of 10 for both hostprobe modes.

5. Check `scripts/repair-treatas-acl.ps1 -WhatIfOnly` (risk #6).

6. Read <https://learn.microsoft.com/windows/release-health/status-windows-11-26h2>
   and the resolved/known-issues pages before concluding that anything observed
   is Shell's fault.

### If the menu is gone

In this order, because each step is cheaper than the next:

1. `capture-state.ps1 -Diff` — is `TreatAs` still there?
2. If not: elevated `"C:\Program Files\Nilesoft Shell\shell.exe" -register -treat`,
   then restart Explorer. That is the whole fix for risk #1.

   **Do not try to restore it with `baseline/reg-clsid-86ca1aa0-treatas.reg`.**
   That export exists to show what the key held, not to put it back:
   `{86ca1aa0-…}` is owned by `NT SERVICE\TrustedInstaller` and grants
   `BUILTIN\Users` only `ReadKey` (§2), so `reg import` fails even elevated.
   `shell.exe -register -treat` goes through `BorrowedKeyAccess`, which borrows
   the access, writes, and puts the security descriptor back.
3. If the registration is gone too: elevated `shell.exe -register`, then the
   `-treat` step.
4. If `shell.dll` itself is gone: redeploy with
   `.\scripts\backup-and-upgrade.ps1`. **Not `msiexec /f`** (§3.2).
5. Only then start reading logs. `C:\Program Files\Nilesoft Shell\shell.log` is
   UTF-16 and appends; it was **85 226 bytes** when this baseline was finished,
   so anything past that offset is new. Phase timers are off
   (`HKCU\SOFTWARE\Nilesoft\Shell` has no `perf` value); if you turn them on,
   set the value through a scheduled task or Explorer will never see it.

---

## 7. What this baseline does not cover

- **The live census covers the desktop background only**, and the two attempts to
  widen it both failed for reasons worth recording rather than retrying:

  - **The taskbar cannot be reached by posting a message.** `Shell_TrayWnd`
    ignores a posted `WM_CONTEXTMENU` — ten attempts, no popup window ever
    appeared — and so does the real child under the point, which is a
    `Windows.UI.Composition.DesktopWindowContentBridge`. The Windows 11 taskbar
    is a XAML surface. Reaching its menu needs synthetic input, and if Windows
    answers with a XAML flyout rather than an `HMENU` there is no menu handle to
    read at all. Since 26H2 turns taskbar app actions on by default, this is the
    gap that matters most. (Shell does track a popup for the taskbar —
    `src/dll/src/Main.cpp` passes `hTaskbar` to the tracking hook — so a real
    right-click probably does produce a readable `HMENU`; that is untested.)

    **There is a way to census it anyway, with one right-click from a human.**
    `menucensus read-open` reads whatever popup menu is already open and leaves
    it exactly as found, so:

    ```
    <probe> read-open > taskbar-before.tsv     # then right-click the taskbar
                                               # and leave the menu up
    ```

    It waits up to 20 s for a menu to appear. Verified 2026-11-25 against a menu
    opened by a separate process: the reading is **identical** to the settled
    `live-desktop` one, row for row. It needed the same count-stability wait as
    the live modes to get there — read the instant the handle became readable, it
    caught the menu mid-composition and returned 27 rows that were a *different*
    27, with Shell's items present and Explorer's `Paste` and `New+` not yet
    added. The same trick censuses a folder or a selection, which is the other
    surface this shell cannot drive.
  - **A folder window could not be opened from this shell at all.** Neither the
    probe's `ShellExecuteW` nor `Start-Process explorer.exe <path>` produced a
    `CabinetWClass` window, across ten attempts; the process started and no
    window arrived. The probe's window lookups are fine — it finds `Progman`,
    `SHELLDLL_DefView` and `Shell_TrayWnd` in the same run — so this is about UI
    launched from an agent shell not reaching the user's desktop. That rhymes
    with the registry-write asymmetry `AGENTS.md` records for this shell, but the
    two are not established to share a cause. Run `menucensus live-folder <path>`
    from an ordinary terminal if the folder surface is wanted.

  So for folders, files and multi-selections the only reading available here is
  the `IContextMenu` one, which is deterministic and is the right instrument for
  "what do the handlers contribute" — but it is not what Explorer shows, because
  Shell contributes nothing through that interface. A change in how Shell
  *composes* those menus will not appear in it.
- **Twenty clean hostprobe runs do not prove the drain fix.** See §2.
- **The MSI upgrade matrix** remains unverifiable here, per `AGENTS.md`. Nothing
  in §4 claims an uninstall/reinstall *would* fail; it claims the path is
  untested on this generation and that the in-place route avoids needing it.
- **What an enablement package does to component registration** is not documented
  by the vendor at the level this question needs. §4.2 substitutes a measurement
  (the November cumulative replaced the DLL and left `TreatAs` intact) for a
  contract. If the redirect does disappear, that is new information and belongs
  in `AGENTS.md`.
- **`NtUserTrackPopupMenu` has no documentation page at all**, so what
  `TrackPopupMenu` puts in `WM_INITMENUPOPUP`'s `lParam` is established on this
  machine by measurement only. If 26H2 changed it — unlikely, since `win32u.dll`
  is unchanged — the hostprobe fixtures under `src/tests/hostprobe/fixtures/` are
  what detect it.
