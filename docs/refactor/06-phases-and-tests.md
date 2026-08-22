# 06 — Phases, test harness, regression gates

Execution order merges A1§24 and A2§30 with the validated backlog; every phase ends
shippable. Machine-dependent verification is called out explicitly (this repo's AGENTS
rule: unit + emitted-artifact checks never substitute for host testing).

---

## Phase 0 — correctness floor (days)

Commit-sized work items (each independently revertible; suite green after every commit):

| # | Commit | Contents | Tests/gates |
|---|---|---|---|
| 0.1 | `fix(expression): numeric less-than` | `FuncExpression.cpp:532` one-line flip | `test_expression.less_numeric`; grep `src/bin/imports/**` for `<` in numeric contexts first |
| 0.2 | `fix(expression): Copy protocols` | Ternary (`Expression.h:140-148`) + FuncExpression Array (`IdentExpression.h:174-189`) + Array2Expression type | clone-and-eval tests for all three |
| 0.3 | `fix(parser): duplicate imports actually skipped` | `Parser.cpp:1150-1162` early-return | diamond-import counter test |
| 0.4 | `fix(msg): msg.right is MB_RIGHT` | `Constants.h:21` | constant-mapping test |
| 0.5 | `chore(dll): remove dead links and hooks` | d2d1/dwrite/Winmm pragmas + `OnDrawItem_D2D` decl + DllGetClassObjectHook machinery + commented Hooker blocks | link check; import-table size diff |
| 0.6 | `chore(shared): delete zero-user subsystems` | StringBuffer/TString/Buffer/MemoryManager/commented collections/auto_ptr block; GC→vector<unique_ptr> | build + full suite |
| 0.7 | `fix(shared): disarm latent defects` | string.h assign/operator[]/conversion-op; PlutoVGWrap ×3; CommandLine dtor-reuse; non-copyable auto_handle/File; RegistryKey refcount | existing suites stay green |
| 0.8 | `chore(encoding): single validator path` | delete 4 duplicate validators + defective Utf16ToUtf8 | test_encoding extended |
| 0.9 | `fix(menu): drop Recycle Bin query from enumeration` | remove `ContextMenu.cpp:4579` query, trust native state | rg gate + manual recycle-bin menu pass |

Items 0.1–0.4 land **after** the trace-harness baseline exists (§2) or on a branch with
harness comparison before merge, since they alter evaluation semantics.

Items: §04.1 expression fixes (#1–#5) · §04.2 deletions/fixes · §02.4 Recycle-Bin
removal. Gates: full suite x64 (+x86 compile), rg gates green, trace harness (§2 below)
green *before* any behavior-affecting fix lands so every later diff is measured against
a recorded baseline.

## Phase 1 — first paint (1–2 weeks)

1. `PackageCatalogService` + persistent cache; migrate both consumers (§02.1).
2. `GetState` policy — delete the `TRUE` retry (§02.2).
3. Diagnostics ring replaces opt-in-only perf (§02.6); keep registry flag as flush
   switch.
4. Taskbar stage 1 zero-wait (§02.5).

Acceptance: R1 assertions active in debug builds (no package I/O, no `GetState(TRUE)`
on menu thread); cold-start right-click p95 ≤ warm p95 + 10 ms on reference machine;
ring shows provider table populated.

## Phase 2 — takeover contract (2 weeks)

1. `TakeoverSession` consolidation (§01.2) — no behavior change.
2. `NativeMenuBridge` INIT/UNINIT pairing (§01.5) + tests.
3. HostContract normalization behind the probe gate (§01.3): land internal
   RETURNCMD|NONOTIFY tracking + replay only after harness equivalence is demonstrated
   for native items across the TPM matrix.
4. WinEvent lifecycle state machine (§01.6).
5. SPI mutations removed; flicker-hack A/B benchmark → gate/delete (§02.4).

Acceptance: §01.10 checklist; harness traces equivalent modulo intentional transforms.

## Phase 3 — safety product (1 week)

Last-known-good states + error surfacing (§03.1–2) · circuit breaker + bypass gesture
(§01.7, §05.2) · CoCI policy compile & conditional attach + router de-dup of Win11
suppression (§01.9). Acceptance: typo-survival scenario scripted in VM; quarantine of a
test CLSID demonstrable end-to-end.

## Phase 4 — seams and scale (ongoing, interleavable)

Seam steps 5–7 of §04.4 (selection layering, MenuModel, presenter) · targeted moveto
(§04.6) · icon-cache extension + memoization whitelist behind measurement (§04.7) ·
config watcher (§03.3). Each step independent; order flexible after Phase 3.

## Phase 5 — capability wave (per §05)

MSAA exposure → mnemonics/type-ahead → smart columns → Reliability Center UI →
favorites/recents → inspector. MSAA/mnemonics/columns can pull forward anytime (small,
independent).

## 2. Windows trace harness (`src/tests/hostprobe/`, new)

The single highest-leverage testing investment (A1§25, A2§25). A small standalone
exe + a hosted-test mode:

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

Dedicated probes added from QA validation (all block their dependent design rules):

1. **`WM_MENUCHAR` position-vs-ID** — record what untouched Windows does with a
   returned LOWORD on current builds; *Using Menus* documents zero-based index
   (QA-01). Gates §05.4 Stage 1.
2. **`TPM_NONOTIFY` suppression set** — which of `WM_MENUSELECT`, `WM_MEASUREITEM`,
   `WM_DRAWITEM`, `WM_COMMAND` still reach the owner with NONOTIFY set. The pages are
   silent ("does not send notification messages", unenumerated). Gates the HostContract
   rule freeze and any synthesized-notification design in §01.3.
3. **UNINIT tolerance** — host receives `WM_UNINITMENUPOPUP` for a popup it never
   really tracked (LegacyEager descendants, §01.5 divergence note); must be non-fatal
   for representative handlers.
4. **Gesture non-interference** — bypass (`Ctrl+Alt+RClick`) and config-reload
   (`Shift+Ctrl+RClick`) can never both fire from one click (QA-04).
5. **Replay delivery-before-destroy** — if any posted-message replay variant is ever
   considered, prove the host processes it before its own `DestroyMenu` runs
   (QA-03); default design is synchronous precisely to avoid needing this.

CI role: builds the probe on all platforms but *execution* requires an interactive
desktop — run in the scheduled VM job (below), not PR CI.

## 3. CI additions (cheap, immediate)

- Run `scripts/validate-msi-lifecycle.ps1` post-build (currently manual only).
- rg-based invariant gates script (`scripts/check-invariants.ps1`): forbidden patterns
  from Phases 1–3 (`GetState\(\s*selection\s*,\s*TRUE`, `SHQueryRecycleBinW` in dll src,
  `d2d1` pragma, …). Fails PRs.
- Keep x64/x86 test execution; ARM64 stays build+package only (host constraint,
  documented in build.ps1).

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
