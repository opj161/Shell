# 06 — Phases, test harness, regression gates

Execution order merges A1§24 and A2§30 with the validated backlog; every phase ends
shippable. Machine-dependent verification is called out explicitly (this repo's AGENTS
rule: unit + emitted-artifact checks never substitute for host testing).

---

## Phase 0 — correctness floor (days)

**0.0 comes first (§07 A6).** The harness in §2 below was described as "the single
highest-leverage testing investment" and gates six other items — Phase 2.3, §05.4
Stage 1, the backend-coverage experiment, the NONOTIFY question, the UNINIT
divergence, and, by this section's own note, items 0.1–0.4 — yet it appeared in no
phase's work list. It is Phase 0 item 1.

Note on what 0.1–0.4 actually need: the harness records *host-observable message
streams*. The expression fixes do not touch that stream; what pins them is unit
coverage against the real evaluator, which is what `test_expression.cpp` now
provides (`tests.vcxproj` links the shipping `Expression/*.cpp`, so the tests drive
the same code the DLL runs). Build the harness first because Phase 2 is blocked
without it, not because Phase 0 is.

Commit-sized work items (each independently revertible; suite green after every commit):

| # | Commit | Contents | Tests/gates |
|---|---|---|---|
| 0.0 | `test(hostprobe): record host-observable menu message traces` | probe window + message recorder + scenario runner (§2) | baseline trace recorded and committed as a fixture |
| 0.1 | `fix(expression): numeric less-than` | `FuncExpression.cpp:532` one-line flip | `test_expression`: `less_numeric_is_less_not_greater`, `less_numeric_handles_negatives_and_fractions`, `less_on_strings_still_compares_length`; grep `src/bin/imports/**` for `<` in numeric contexts first |
| 0.2 | `fix(expression): Copy protocols` | Ternary (`Expression.h:140-148`) + FuncExpression Array (`IdentExpression.h:174-189`) + Array2Expression type | `test_expression`: `ternary_copy_*` ×3, `func_copy_*` ×3, `array_literal_copy_preserves_the_node_type`, `number_literal_copy_still_yields_a_number` |
| 0.3 | `fix(parser): duplicate imports actually skipped` | `Parser.cpp` early-return | `test_parser_imports` ×4, driving the real `Parser` over real temp files via the new `Parser(const string&)` constructor |
| 0.4 | `fix(msg): msg.right is MB_RIGHT` | `Constants.h:21` | `test_expression.msg_right_maps_to_the_alignment_flag`, `msg_button_results_are_unchanged` |
| 0.5 | `chore(dll): remove dead links and hooks` | d2d1/dwrite/Winmm pragmas + `OnDrawItem_D2D` decl + DllGetClassObjectHook machinery + commented Hooker blocks | link check; import-table size diff |
| 0.6 | `chore(shared): delete zero-user subsystems` | StringBuffer/TString/Buffer/MemoryManager/commented collections/auto_ptr block; GC→vector<unique_ptr> | build + full suite |
| 0.7 | `fix(shared): disarm latent defects` | string.h assign/operator[]/conversion-op; PlutoVGWrap ×3; CommandLine dtor-reuse; non-copyable auto_handle/File; RegistryKey refcount | **new** `test_string_index` ×9 — the claim that existing suites pinned these was wrong; nothing covered indexing or copy assignment. Contract per §04.2: reads at or beyond `m_length` are `L'\0'` by all three routes |
| 0.8 | `chore(encoding): single validator path` | delete 4 duplicate validators, defective `Utf16ToUtf8`, and dead `UTF8::From(const std::string&)` (§04.2) | `test_encoding` already covers `Encoding::GetType` including the strict BOM-less cases; deletions are covered by the suite staying green |
| 0.9 | `fix(menu): drop Recycle Bin query from enumeration` | remove `ContextMenu.cpp:4579` query, trust native state | rg gate + the probe below |

**0.9's premise was checked, not assumed (§07 §2.3).** The removed block only ran
when the native item was *already* `disabled` — it existed to override a false
negative from Explorer, so "trust the native state" is a change in exactly the case
the workaround targeted. Probe run 2026-08-22 on Windows 11 26200.8875 x64: bound
the Recycle Bin folder via `SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder)` →
`BindToObject` → `CreateViewObject(IID_IContextMenu)` → `QueryContextMenu`, and read
`MENUITEMINFO.fState` for the `empty` verb alongside `SHQueryRecycleBinW(nullptr)`.
Result: bin **not** empty (36 items, 567 MB) and the verb reported **enabled** — the
guarded-against state did not reproduce. The complementary direction (bin empty ⇒
verb disabled) was not tested, because producing it means destroying the tester's
recycled files; if Explorer got *that* one wrong the consequence is a live item that
does nothing, which is benign. Re-run the probe when this is next touched.

Items 0.1–0.4 alter evaluation semantics and are pinned by `test_expression.cpp`
against the real evaluator. Each fix was verified to be *caught* by its test: the
defect was re-introduced, the suite rebuilt, and the specific test observed to fail,
before being restored. A green test that also passes with the bug present is worth
nothing, and four of these defects were the kind that reads as ordinary code.

Items: §04.1 expression fixes (#1–#5) · §04.2 deletions/fixes · §02.4 Recycle-Bin
removal. Gates: full suite x64 (+x86 compile), rg gates green, trace harness (§2 below)
green *before* any behavior-affecting fix lands so every later diff is measured against
a recorded baseline.

## Status, 2026-08-24

Phase 0 is complete apart from item 0.0. Phases 1–3 have been entered out of
order, taking the items that are verifiable on this machine first; what is left
in each is stated below rather than in a separate tracker.

| Landed | Item | Phase |
|---|---|---|
| ✅ | `GetState(TRUE)` retry deleted; E_PENDING resolves to a provisional item | 1.2 |
| ✅ | `PackageCatalogService` in memory — async warm, coalesced stale-while-revalidate, atomic publish | 1.1 |
| ✅ | `NativeMenuBridge` INIT/UNINIT pairing | 2.2 |
| ✅ | `CoCreateInstanceHook` policy-before-diagnostics (Alt-held blocklist bypass) | 3 |
| ✅ | `SPI_SETMENUSHOWDELAY` `fWinIni = 0` | 3 |
| ✅ | Config StaleWithError — a failed parse serves the live generation | 3 |
| ✅ | Persisted last-known-good shadow + recovery on a fresh process | 3 |
| ✅ | **0.0 trace harness** — `src/tests/hostprobe/`, 19 scenarios, baselines committed; Phase 2.3 and the four items gated on it are unblocked | 0 |
| ⬜ | Provider deadline and deferral (§02.2a) — the remaining unbounded work on the menu thread | 1.3 |
| ✅ | Diagnostics ring (§02.6) — always-on phase timing, 47.8 ns per phase; the registry value now gates only the log file | 1.4 |
| ⬜ | Cold-start measurement, then the persistence decision for §02.1 step 3 | 1.5 |
| ✅ | TPM normalization — `TPM_RETURNCMD` always added, native replay posted, synthetic identifiers no longer reach the host | 2.3 |
| ⬜ | `TakeoverSession`, WinEvent lifecycle, flicker A/B | 2 |
| ⬜ | `shell.exe -check`, circuit breaker, bypass gesture, taskbar Stage 2 | 3 |

### What the harness settled (2026-08-24)

The four questions §2 below listed as blocking design rules were recorded
against untouched Windows, and two of them came back the opposite way round
from the plan's assumption. Full detail and the deep links are in
`src/tests/hostprobe/fixtures/README.md`; the consequences are folded into
§01.3a and §05.4.

| Question | Answer | What it changed |
|---|---|---|
| Does `TPM_RETURNCMD` leave a duplicate `WM_COMMAND`? | **No.** RETURNCMD is what suppresses it | `TPM_NONOTIFY` is never needed; its `HostProfile` opt-in is dropped rather than built |
| What does `TPM_NONOTIFY` suppress? | The whole menu **lifecycle** — ENTERMENULOOP, INITMENU, INITMENUPOPUP, UNINITMENUPOPUP, EXITMENULOOP. `WM_COMMAND`, `WM_MENUSELECT`, `WM_MEASUREITEM` and `WM_DRAWITEM` all survive | Adding it would suppress the very `WM_INITMENUPOPUP` the bridge exists to deliver. The synthesised-`WM_MENUSELECT` design is dropped: nothing suppressed it |
| Is the `WM_MENUCHAR` low word an index or an identifier? | **Index**, and an out-of-range one is refused rather than reinterpreted | Mnemonics (§05.4 Stage 1) unblocked as specified |
| Is native replay sent or posted? | **Posted**, after `WM_EXITMENULOOP` and after the tracking call returns | QA-03 inverted: replay must post, not send |

Plus one the plan had not asked: `MNS_NOTIFYBYPOS` and `TPM_RETURNCMD` do not
compose — with both set the call returns 1 and no `WM_MENUCOMMAND` is sent, so
the selection is lost. Shell's composed menu must never carry the style.

Three measurements were taken rather than assumed, and all are recorded where
the code that depends on them lives:

- The packaged-verb scan, on this machine: 289 packages, 244 manifests read,
  23 registrations; **111.6 ms cold, 63–68 ms warm**, of which the registry
  enumeration is 2 ms. That is what one right-click in every thirty seconds
  used to pay before anything was drawn (`Include/PackageCatalogService.h`).
- The Recycle Bin premise behind item 0.9, probed rather than assumed (§0.9
  below).
- The whole host-observable message stream, for 19 scenarios, recorded as
  committed fixtures rather than reasoned about (`src/tests/hostprobe/`).

Every fix above was verified to be *caught* by its test: the defect was
reintroduced in a copy of the header, the suite rebuilt, and the specific test
observed to fail while the others kept passing. That pass found two tests that
passed for the wrong reason (a timestamp comparison that `CopyFileW` makes
vacuous, and a manifest check that refused for a different reason than the one
under test) and one that crashed the suite instead of reporting — all three
fixed rather than left green.

## Phase 1 — first paint (1–2 weeks)

Re-ordered by §07: the unbounded cost is provider *activation*, not catalog scanning,
and persistence is deferred until cold start has been measured without it.

1. `PackageCatalogService`, **in memory only** — async warm, `shared_ptr<const>`
   publish, both consumers migrated (§02.1 steps 1–2 and 4). No on-disk cache yet.
2. `GetState` policy — delete the `TRUE` retry (§02.2).
3. **Provider deadline + deferral, and presentation caching (§02.2a).** The item that
   actually stops a slow extension freezing the menu.
4. Diagnostics ring replaces opt-in-only perf (§02.6); keep registry flag as flush
   switch.
5. Measure cold start. *Then* decide whether §02.1 step 3 (persistence) earns its
   trust boundary and on-disk format.

~~4. Taskbar stage 1 zero-wait~~ — **withdrawn (§07 A2).** Taskbar work is Stage 2
only and moves to Phase 3.

Acceptance: R1/R1a assertions active in debug builds (no package I/O, no
`GetState(TRUE)`, and no unbounded COM activation on the menu thread); a fake
provider sleeping 2 s in `GetState` does not delay first paint and appears on the
next menu; cold-start right-click p95 ≤ warm p95 + 10 ms on reference machine; ring
shows provider table populated with a `deferred` count.

## Phase 2 — takeover contract (2 weeks)

1. `TakeoverSession` consolidation (§01.2) — no behavior change.
2. `NativeMenuBridge` INIT/UNINIT pairing (§01.5) + tests.
3. ✅ HostContract normalization (§01.3), landed as `Include/HostContract.h`
   after the harness answered its gating questions. `TPM_RETURNCMD` is now
   always added and `TPM_NONOTIFY` never is — probe 2 showed no duplicate
   `WM_COMMAND` to suppress, so the `HostProfile` opt-in for NONOTIFY was
   dropped rather than built. Native replay is posted rather than sent, per the
   reversed QA-03.

   One thing the plan did not anticipate: adding `TPM_RETURNCMD` makes a
   cancelled menu and a *failed* tracking call return the same 0, which would
   have made Shell answer a notifying host TRUE for a menu that never appeared.
   A twentieth scenario established that the last-error code separates them
   (`question.a_failed_track_sets_a_last_error`).
4. WinEvent lifecycle state machine (§01.6).
5. Flicker-hack A/B benchmark → gate/delete (§02.4). *(SPI: see Phase 3.)*

Acceptance: §01.10 checklist; harness traces equivalent modulo intentional transforms.

## Phase 3 — safety product (1 week)

**Persisted** last-known-good + `shell.exe -check` (§03.1b) and in-memory stale-serve
(§03.2) · circuit breaker + bypass gesture (§01.7, §05.2) · CoCI policy compile &
conditional attach, **including the Alt-held blocklist bypass** (§04.9) + router
de-dup of Win11 suppression (§01.9) · taskbar Stage 2 layout snapshot (§02.5) ·
`SPI_SETMENUSHOWDELAY` `fWinIni = 0` (§02.4a).

Acceptance: typo-survival scenario scripted in a VM **including an Explorer restart**
(the case the in-memory design missed); quarantine of a test CLSID demonstrable
end-to-end and not defeated by holding Alt; first taskbar right-click of a sequence
still shows Shell's menu; no `WM_SETTINGCHANGE` broadcast observable around a menu.

## Phase 4 — seams and scale (ongoing, interleavable)

Seam steps 5–7 of §04.4 (selection layering, MenuModel, presenter) · targeted moveto
(§04.6) · icon-cache extension + memoization whitelist behind measurement (§04.7) ·
config watcher (§03.3). Each step independent; order flexible after Phase 3.

## Phase 5 — capability wave (per §05)

MSAA exposure → mnemonics/type-ahead → smart columns → Reliability Center UI →
favorites/recents → inspector. MSAA/mnemonics/columns can pull forward anytime (small,
independent).

## 2. Windows trace harness (`src/tests/hostprobe/`) — **built**

```powershell
src\bin\x64\hostprobe.exe                       # every scenario, printed
src\bin\x64\hostprobe.exe question              # just the ones that assert
src\bin\x64\hostprobe.exe --verify src\tests\hostprobe\fixtures
```

It builds on every platform and **is deliberately not run by `build.ps1`**: it
creates a window and shows real popup menus, which does not belong in a
developer's build. Execution is the interactive VM job.

Three mechanics were established by probing rather than chosen, and each of the
first attempts was wrong:

- **Keys are posted to the owner's thread queue, not injected.** `SendInput`
  would put real keystrokes into whatever holds the foreground. Measured
  instead: the menu's modal loop reads keyboard messages off the *thread*
  queue, so `PostThreadMessage` drives it identically to posting at the
  `#32768` window — and depends on nothing undocumented, not even that class
  name.
- **Navigation is closed-loop.** Counting key presses is wrong three ways: a
  separator is skipped silently, a press before the loop is reading is
  discarded, and a `TPM_NONOTIFY` menu emits nothing at all until something is
  selected. The first version counted presses and produced a scenario with an
  empty trace that read as a discovery about Windows rather than as a harness
  fault. The driver now steps and watches `WM_MENUSELECT` until the highlight is
  where the script asked, and reports a navigation failure as a failure.
- **A watchdog is always armed.** A menu left standing blocks the tracking call
  forever. `EndMenu` ends "the calling thread's active menu" and the driver is
  not that thread, so the watchdog uses the documented alternative — posting
  `WM_CANCELMODE` to the owner.

Two smaller things that keep runs identical: the popup is placed in the screen
quadrant furthest from the live cursor (the first run drew it under the mouse
and filled the trace with `MF_MOUSESELECT` hovers), and consecutive identical
lines collapse, because an owner-drawn item is redrawn as often as the
compositor likes.

Original specification, for reference:

- Creates real popup menus under a probe window whose WndProc records every message:
  WM_INITMENU, WM_INITMENUPOPUP(+lParam), WM_MENUSELECT, WM_MENUCHAR, WM_MEASUREITEM,
  WM_DRAWITEM, WM_COMMAND, WM_MENUCOMMAND, WM_UNINITMENUPOPUP, WM_EXITMENULOOP.
- Runs each scenario twice — **native** (menu tracked directly) vs **takeover**
  (same menu through Shell's hook with identity config) — normalizes handles, diffs
  traces.
- Scenario matrix: {TrackPopupMenu, Ex} × {RETURNCMD ±} × {NONOTIFY ±} × {MNS_NOTIFYBYPOS}
  × {mouse select, keyboard mnemonic, Enter, cancel} × item origin {native, custom,
  ExplorerCommand-fake} × popup nesting {root, lazy submenu, nested}.
- Also hosts the **backend coverage experiment**: same scenarios with each
  PopupInterceptionBackend to decide primary backend empirically (§01.9), and the
  TPM-replay ordering probe that gates Phase 2.3.

Dedicated probes added from QA validation:

1. ✅ **`WM_MENUCHAR` position-vs-ID** (QA-01) —
   `question.menuchar_low_word_is_an_index` and
   `..._is_not_an_identifier`. Answer: index; out-of-range refused. §05.4
   Stage 1 unblocked.
2. ✅ **`TPM_NONOTIFY` suppression set** (QA-02) — `select.nonotify.*`,
   `question.nonotify_still_measures_ownerdraw` and its control. Answer: the
   lifecycle, not the notifications. §01.3a. Also settled the RETURNCMD
   duplicate-`WM_COMMAND` question the same table was gating.
3. ⬜ **UNINIT tolerance** — host receives `WM_UNINITMENUPOPUP` for a popup it never
   really tracked (LegacyEager descendants, §01.5 divergence note); must be non-fatal
   for representative handlers. Needs a Shell-side scenario, not a native one, so it
   lands with the takeover half of the harness.
4. ⬜ **Gesture non-interference** — bypass (`Ctrl+Alt+RClick`) and config-reload
   (`Shift+Ctrl+RClick`) can never both fire from one click (QA-04). Also
   Shell-side; lands with the bypass gesture in Phase 3.
5. ✅ **Replay delivery ordering** (QA-03) — answered the other way round.
   `select.plain.classic` and `question.notifybypos_reports_a_position` show
   Windows **posts** `WM_COMMAND`/`WM_MENUCOMMAND` after `WM_EXITMENULOOP` and
   after the tracking call returns, so the replay design is posted, not
   synchronous, and delivery-before-destroy is Windows' problem rather than one
   Shell introduces.

**Still to build: the takeover half.** Everything above records what *untouched
Windows* does, which is the baseline and was the blocking half. Running the same
scenarios through Shell's hook and diffing needs a deployed, injected build, so
it is scheduled with the Phase 2.3 replay work rather than here.

CI role: builds the probe on all platforms but *execution* requires an interactive
desktop — run in the scheduled VM job (below), not PR CI.

## 3. CI additions (cheap, immediate)

- Run `scripts/validate-msi-lifecycle.ps1` post-build (currently manual only).
- **`scripts/check-invariants.ps1` — landed and wired into `build.ps1`**, which runs
  it after every successful platform build and fails the build on a violation. Six
  enforced rules, two deferred (warn-only) that turn on with their phase:
  `GetState(…, TRUE)` with Phase 1, `SPIF_SENDCHANGE` with Phase 3.

  Three things the first version got wrong, worth remembering when adding rules
  (§07 §1.1):
  1. It **failed on its own tree** — the Recycle-Bin rule matched the comment that
     the same change added to explain the removal. A rule names something the code
     must not *do*; it must not fire on prose about it. The script now strips block
     comments, line comments and string literals before matching.
  2. `Get-ChildItem -Path 'src\dll\src\**\*.h'` — **PowerShell's `**` is not a
     recursive glob.** It resolved exactly one directory level, covering 43 of 46
     headers and silently skipping `pch.h`, `dija.h` and everything under
     `Include\Diagnostics\`. Use `-Recurse -Include`.
  3. The explicit-destructor and `memcpy(this, …)` rules scanned `src\dll` only,
     while the fixes they guard are in **`src\shared`** (`Library/PlutoVGWrap.h`,
     `System/CommandLine.h`). A gate that does not cover the file it was written for
     is decoration.
- Keep x64/x86 test execution; ARM64 stays build+package only (host constraint,
  documented in build.ps1). The invariant check is source-level, so it runs for every
  platform including the ones whose test binary cannot execute here.

## 4. Measurement protocol & budgets

Baseline first, then per-phase re-measure (AGENTS.md "measure before optimising"):

```powershell
HKCU\SOFTWARE\Nilesoft\Shell  perf  REG_DWORD  <floor-ms>   # existing phase timers
```

Plus ring export (`shell.exe -report perf`) for distributions. Budgets set from measured
p95/p99 on the reference machine, not invented: initial targets — pre-display added by
Shell ≤ 15 ms p95 Explorer file context; taskbar hit-test added ≤ 2 ms; catalog refresh
never on menu thread (hard gate, not budget).

## 5. Windows acceptance matrix (VM/manual; from A1§25, trimmed to what this plan changes)

Per release of Phases 1–3:

- OS: Win10 22H2, current Win11 stable, one Insider ring (early warning).
- Contexts: file/folder/mixed/large-selection/background/desktop/taskbar(±secondary)/Home/
  QuickAccess/RecycleBin/third-party host (Total Commander or Everything at minimum).
- Lifecycle: install/upgrade/uninstall/repair incl. TreatAs foreign-state preservation
  (existing validator covers tables; VM covers behavior).
- Accessibility: Narrator over owner-drawn menus before/after MSAA change.
- Config: LKG typo scenario; watcher live-edit; reload-during-open-menu.

## 6. Risk register deltas (vs master plan §2 sources)

| Risk | Mitigation |
|---|---|
| TPM replay regressions in exotic hosts | probe-gated landing (Phase 2.3) + HostProfile escape hatch + bypass gesture |
| Backend swap reduces coverage | experiment stays opt-in until matrix proves parity; private win32u route remains default |
| Async services introduce lifetime bugs | reuse proven worker pattern (TaskbarUiaWorker); snapshot publish is atomic shared_ptr like Initializer |
| Quarantine misfires on legit-but-slow extensions | telemetry-only tier first; manual quarantine default; one-click undo |
| Scope creep in seam extraction | move-only commits; each seam revertible; features wait for MenuModel |
