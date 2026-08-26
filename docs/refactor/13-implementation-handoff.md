# 13 — Implementation handoff

**Written 2026-08-26, at the end of the QA sessions and before implementation
begins.** Its only purpose is to carry what was in the auditing session's head
into the implementing session — the operational facts, the measured baselines,
and the decisions already taken — so none of it is re-derived or re-litigated.

[`12-closure-plan.md`](12-closure-plan.md) is the specification: **what** to
change and **why**, with the contract citations. This document is the **how**,
and it assumes the plan has been read.

> **Superseded for numbers, still current for method.** The implementation is
> complete: [`14-post-implementation-qa.md`](14-post-implementation-qa.md) §1
> carries the final measurements. Every baseline in §3 below is the *starting*
> state and none of them still hold — the suite, the harness counts and the
> `/analyze` figure all moved by design. §4 (running the harness), §5 (toolchain
> traps) and §7 (settled decisions) are unchanged and still worth reading.

---

## 1. Start here

1. Read [`12-closure-plan.md`](12-closure-plan.md) in full. Parts C and D are the
   work; Part A is why each item is trusted; Part F is what not to do.
2. Read `AGENTS.md`. It is not optional context — §"Three ways an experiment can
   test something other than what you think" and §"Things that are easy to get
   wrong here" both bear directly on several workstreams.
3. **Do W0 before anything else.** The remediation is uncommitted. Until it is
   committed, one bad `git checkout` or `git restore` destroys ~2,900 lines that
   no reflog can recover.

Read [`11-qa-deep-pass.md`](11-qa-deep-pass.md) §N7 before investigating anything
that looks suspicious in the areas it covers — six plausible defects were checked
and cleared there, with reasons.

---

## 2. State of the tree as of this handoff

```text
branch  refactor/takeover-master-plan
HEAD    450985f            (83 commits ahead of main)
tree    44 modified tracked files, +2,895 / −389
        10 untracked source/test/doc files (listed in 12 §W0)
```

Nothing from `09` is committed. `HEAD` is the same commit `09` was written and
audited against.

**Not part of the remediation, do not commit:** `src/bin/x64/` (build output,
only partly ignored), the six `2026-08-*.txt` session transcripts at the
repository root, and — pending a decision — `external-audit.md`.

The three `*.lastcodeanalysissucceeded` files the QA session's `/analyze` runs
left in `src/bin/x64/` have been deleted. They come back on every `/analyze` run;
W7.4 re-runs it, so add `*.lastcodeanalysissucceeded` to `.gitignore` while you
are there.

---

## 3. Measured baselines

Every number below was measured on this machine on 2026-08-25/26 **against the
current working tree**. They are the comparison set: after each workstream, these
should hold except where the plan says a number changes.

| Gate | Baseline | Command |
|---|---|---|
| x64 build + suite | **33,102 checks / 0 failures**, 0 warnings | `.\build.ps1 -Platform x64` |
| x86 build + suite | **33,102 checks / 0 failures**, 0 warnings | `.\build.ps1 -Platform x86` |
| arm64 | builds + packages, 0 warnings, **tests skipped** (host is x64) | `.\build.ps1 -Platform arm64` |
| invariants | `check-invariants: OK (10 rules, 0 deferred)` | run by `build.ps1` |
| suite size | **63** `test_*.cpp`, **758** `TEST(...)` | `ls src/tests/test_*.cpp \| wc -l` |
| harness, native | **23 scenarios, 0 failures, 9 skipped** | §4 below |
| harness, takeover | **32 scenarios, 0 failures** | §4 below |
| harness exit codes | 121 zero-match · 122 usage · 123 `--shell` without `--takeover` | |
| `/analyze`, DLL | **6 warnings** — C26110 @84,150; C26117 @98,144,159,162, all `ProviderQuarantineStore.h`. **0 × C6262** | §5 below |
| `/analyze`, exe + CA | **clean** | §5 below |
| MSI lifecycle | **ok ×3** | §6 below |
| line counts | `ContextMenu.cpp` **6,182** · `MenuPresenter.cpp` **1,795** | `wc -l` |
| `git diff --check` | working tree **clean**; `main...HEAD` **30 hits** (nothing committed) | |
| packaged handlers here | **55** distinct context-menu CLSIDs; ring/export cap is **32** | §7 below |

**Targets that must change:** `/analyze` DLL goes 6 → **0** (W7.4). Suite counts
rise as W1/W2/W4/W6 add tests. Harness counts rise to **23 / 33** when W6.3 adds
the nested scenario — update `Scenarios.h`'s constants (W6.2) and the docs in the
same commit.

---

## 4. Running the harness

`build.ps1` does **not** run hostprobe, and neither does CI (that is D4/W6.4).
Both commands must be run by hand until W6.4 lands.

```powershell
.\src\bin\x64\hostprobe.exe --verify .\src\tests\hostprobe\fixtures
```

Takeover mode needs the per-user COM override — `--shell` alone cannot redirect
shell-namespace COM activations. Full recipe, and **the cleanup is mandatory**:

```powershell
Copy-Item src\bin\shell.nss src\bin\x64\shell.nss -Force
Copy-Item src\bin\imports src\bin\x64\imports -Recurse -Force
$k = 'HKCU\SOFTWARE\Classes\CLSID\{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}\InprocServer32'
reg add $k /ve /t REG_SZ /d "$PWD\src\bin\x64\shell.dll" /f
reg add $k /v ThreadingModel /t REG_SZ /d Apartment /f

.\src\bin\x64\hostprobe.exe --takeover --verify .\src\tests\hostprobe\fixtures

reg delete 'HKCU\SOFTWARE\Classes\CLSID\{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}' /f
Remove-Item src\bin\x64\shell.nss, src\bin\x64\imports -Recurse -Force
```

The QA session set this override and removed it; the machine is clean. Leaving it
in place points every context-menu activation on the desktop at a dev build.

**Check the `takeover:` line names the build under test.** Without the override
the run still executes 32 scenarios but
`takeover.a_by_position_host_is_told_which_position` fails with
`got 0 WM_MENUCOMMAND (position 0, "&Open", menu 0000000000000000)` — that is the
signature of "the installed DLL composed the menu", not a regression in your
change. Explorer is unaffected either way; it has `shell.dll` pinned from before.

---

## 5. Toolchain facts that cost time

- **`msbuild` is not on `PATH`.** Use
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe`.
  `build.ps1` probes for it; direct invocations must not assume it.
- **`/analyze`**, exactly as it worked:

  ```powershell
  $mb = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
  & $mb /nologo /v:m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\src\" `
        /p:RunCodeAnalysis=true /p:EnablePREfast=true /p:RunTestsAfterBuild=false `
        /t:Rebuild src\dll\Shell.vcxproj
  ```
  The production projects are `src\dll\Shell.vcxproj`, `src\exe\exe.vcxproj`
  (**not** `Manager.vcxproj` — that name does not exist) and
  `src\setup\ca\ca.vcxproj`.
- **`-NoProfile` on every direct `powershell` invocation.** `AGENTS.md` explains
  why: about forty seconds here, and it looks exactly like the script being slow.
- **Do not pipe `build.ps1` through `Select-Object -Last N` and read that as the
  result.** The tee'd file gets everything but the displayed tail is truncated
  mid-suite and looks like the test list is short. Write the full log to a file,
  then grep it for `checks,`, `Warning(s)`, `check-invariants:`.
- **`validate-msi-lifecycle.ps1` takes an array**:
  `& scripts\validate-msi-lifecycle.ps1 -Path @('src\bin\setup-x64.msi','src\bin\setup-x86.msi','src\bin\setup-arm64.msi')`.
  A comma-joined string fails in `Resolve-Path`.
- **Throwaway probes**: write the `.cpp` with a file-writing tool, never a shell
  heredoc (`AGENTS.md` — a backslash does not survive). The `vcvars64.bat` route
  prints a `vswhere.exe is not recognized` error on this machine but still
  compiles; run the produced `.exe` by full path rather than relying on `cd`.
  Build probes in the scratchpad, not in the tree.
- **Counting packaged handlers** (used for W5's cap argument):

  ```powershell
  $ids=@{}; Get-AppxPackage | ForEach-Object {
    $m = Join-Path $_.InstallLocation 'AppxManifest.xml'
    if(Test-Path $m){ $t = Get-Content $m -Raw
      if($t -match 'fileExplorerContextMenus'){
        foreach($x in [regex]::Matches($t,'Clsid="\{?([0-9A-Fa-f\-]{36})\}?"')){ $ids[$x.Groups[1].Value.ToUpper()]=1 } } } }
  $ids.Count
  ```

---

## 6. Code coordinates

Anchors verified at this tree. Line numbers drift as soon as you edit — treat
them as starting points, not addresses.

| Workstream | Where |
|---|---|
| **W1** Detours | `src/shared/DetourEnlistment.h` — `InlineDetourApi` :49, `thread_still_present` :108, `enlist_process_threads` :150, walk end :221, `begin()` :246. Real API table in `src/shared/Library/detours.h`. Tests `src/tests/test_detour_enlistment.cpp` (11) |
| **W2** scheduler | `ProviderSchedule.h` — `provider_estimate_us` :137, `plan_providers` :153, `provider_step_fits` :250. `ProviderHealth.h` — `ProviderTiming` :118, thresholds :175-178, `classify` :268, `note_*` :275-311, `next_exploration_cursor` :352. Planning loop `ExplorerCommand.cpp` :505-590. Dead `consider()` :203 + 30 tests in `test_provider_health.cpp` |
| **W3** cost boundary | `ExplorerCommand.cpp` :634 (`spent_before`) → :652 (`cost`) → :686 (`GetCanonicalName`) |
| **W4** package scan | `Packages.cpp` — `enumerate_full_names` :176. `PackageCatalogService.cpp` — `scan_package_catalog` :78, worker publish :281, `SetEvent(_published)` :291. `PackageCatalogService.h` — `publish` :160, `abandon_refresh` :186, `DefaultTtlMs` :109. `Cache.h` — `PackagesCache` :66, `catalog()` :141, `find_entry` :155 |
| **W5** telemetry | `ExplorerCommand.cpp` :593 (deferral batch — move it after the resolution loop ending ~:688). Caps: `DiagnosticsRing.h` :153, `PerfExport.h` :173. Report comment `exe/src/Main.cpp` :1168 |
| **W6.1/6.2** harness | `hostprobe/Arguments.h` — `take_operand` :85. `hostprobe/main.cpp` — `ran == 0` :~530. Counts belong in `hostprobe/Scenarios.h` |
| **W6.3** nested | `hostprobe/ShellMenu.h` — `apply_notify_by_position` :~130, `title_at` :210. `Scenarios.cpp` — scenario :~1055, result capture :~662. Origin capture `ContextMenu.cpp` :3770; ID assignment :752; style read :3935 |
| **W6.5** rule 8 | `scripts/check-invariants.ps1` :108-117. The idiom it misses: `MENU::set` / `MENU::get`, `MenuItem.h` :1333-1341 |
| **W6.6** menu text | `hostprobe/ShellMenu.h` :213. Correct pattern already in `hostprobe/Probe.h` :494 and `hostprobe/MenuReader.h` :296 |
| **W7** stores | `FavoritesStore.h` — `record_use` :104, `reload` :126, `write_time` :~165, the "one increment" comment :37. `Favorites.h` — `load` :261, `save` :313. `ProviderQuarantine.h` — :326, :379. `ProviderQuarantineStore.h` — `reload` :74, `refresh_if_stale` :~135 |
| **W8** COM cache | `ExplorerCommand.cpp` :88 (`CachedProvider`), :95 (`provider_cache`), :101 (`acquire_explorer_command`), :125 (`forget_explorer_command`) |
| **W9** package policy | `Cache.h` :59 (claims a phase) vs :98 (says there is none); `display_name` :98-115. Old memoization `Packages.cpp` :476-513. `invalidate()` `PackageCatalogService.cpp` :176 |
| **W10** docs | `06-phases-and-tests.md` :490 (VM job), :497 (rule count); `HostContract.h` :42; `PerfExport.h` :746; `check-invariants.ps1` :54 (`Get-CodeText`) |

---

## 7. Decisions already taken — do not re-open

Each was reached with evidence in [`11`](11-qa-deep-pass.md) or
[`12`](12-closure-plan.md) Part A. Re-deriving them costs a session.

- **Windows *posts* `WM_MENUCOMMAND`**, despite the page saying "Sent" — measured
  directly (Windows 11 26200 x64, three runs, `wParam = 1`, `IsMenu(lParam)` true
  at delivery). **R2's `PostMessageW` is correct; do not "fix" it.** What is wrong
  is only the sentence in `HostContract.h:42` claiming the traces measured it —
  they cannot, because `Probe::track` drains its own queue first (W10.2).
- **`/analyze` is measured, not unknown**: 6 lock warnings, 0 × C6262, exe and CA
  clean. The external assessment's "UNVERIFIED" is superseded.
- **`_published` semantics stay as they are** (12 §A.2). Setting the event after a
  failed attempt is deliberate and documented; `snapshot_for_menu` already handles
  a null snapshot. Only add the clarifying comment.
- **F3/D5 is Medium and pre-existing**, not a R1 regression — the same ordering is
  on the `−` side of the diff. Still worth fixing.
- **W0 is one commit, not eight** (12 §W0), because six files carry hunks from
  two or more workstreams and every commit must build on three platforms.
- **No `PERF_EXPORT_VERSION` bump for W2.** `ProviderTiming` is in-process only;
  `PerfExportProvider` is a separate layout. Version 8 is already correct.
- **No naive TLS destructor for W8** — it can run after the thread's
  `CoUninitialize`.
- **`PackageIndex` stays** for now: it is dead production code but is the tested
  implementation of identity matching and display-name resolution, and
  `RegistryPackageSource` under it is live. Deleting it is a separate decision and
  needs W9.1 to have given `display_name` a home first.

---

## 8. Needs the user — do not proceed unattended

- **Deploying.** `scripts\backup-and-upgrade.ps1` self-elevates and restarts
  Explorer. `AGENTS.md` says to ask first. It is needed for W11's live R1
  re-measurement. Do not reorder the script: it deploys first and restarts last
  on purpose.
- **§09 R1.4 option (c)** — lowering `SLOW_PROVIDER_US` trades a real menu entry
  for ~14 ms. Explicitly the maintainer's call. W2 fixes the liveness defect
  without needing it.
- **W9.1** — worker-resolved name cache vs. measured-synchronous. The second
  option needs R8's cold/warm PRI/MrtCache measurement first.
- **W6.4** — whether to write the scheduled desktop harness job or delete the
  references to it is a process decision, not a code one.
- **`external-audit.md`** — move under `docs/refactor/` or drop it.

---

## 9. Per-workstream close-out

For each workstream from W1 onward, before moving on:

1. `.\build.ps1 -Platform x64` — checks up, 0 failures, 0 warnings, invariants OK.
2. Both harness commands where the workstream touches the menu path, replay or
   composition (W1, W2, W3, W5, W6.3, W8) — with the COM override, and cleaned up
   after.
3. **Re-introduce the defect, rebuild, watch *that named test* fail, restore.**
   This is the branch's gate 3 and it is the only thing that proves a new test
   tests anything.
4. `/analyze` on the touched production project; triage every new warning.
5. Commit with the contract quoted in the message, and `git diff --check` clean.
6. Before the final close-out: x86 and arm64 too, plus
   `validate-msi-lifecycle.ps1` if setup or shared lifecycle code changed
   (W7 touches `src/shared`, so it does).

Report suites individually rather than the aggregate check count — `AGENTS.md`
asks for that, and an aggregate is how a regression in one suite hides.

---

## 10. On resetting context

Resetting before implementation is the right call, and this document plus
[`12`](12-closure-plan.md) is what makes it safe. What does **not** survive a
reset and is not written down anywhere else has been captured above: the
baselines in §3, the toolchain traps in §5, the coordinates in §6, and the
settled decisions in §7.

Nothing was written to persistent memory. Everything durable is now in the
repository, which is where the next reader will look — and `AGENTS.md`'s own rule
is that a fact the repo already records does not belong in a second place.
