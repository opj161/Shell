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
| ✅ | Provider budget and deferral (§02.2a) — live providers reused per thread, whole-menu budget, slow ones remembered. Warm menu ~170 ms → ~41 ms | 1.3 |
| ✅ | Diagnostics ring (§02.6) — always-on phase timing, 47.8 ns per phase; the registry value now gates only the log file | 1.4 |
| ✅ | Cold-start measured, and §02.1 step 3 **declined**: `catalog.first_wait` never fires, even on a menu raised the moment a restarted Explorer's desktop view exists | 1.5 |
| ✅ | TPM normalization — `TPM_RETURNCMD` always added, native replay posted, synthetic identifiers no longer reach the host | 2.3 |
| ✅ | WinEvent lifecycle (§01.6) — the six-state machine **measured for and declined**; the real defect was the popup stack and the window map removing by two different keys. `TakeoverSession` as an object is not buildable in an SEH hook body (C2712) and the consolidation happened in plain-old-data form instead. §01.6a | 2 |
| ✅ | Mnemonics (§05.4 Stage 1) — `WM_MENUCHAR` was unanswered, so a typed letter beeped | 5 |
| ✅ | Type-ahead (§05.4 Stage 2) — typing a name selects it; mnemonics keep precedence on the first character | 5 |
| ✅ | Packaged verbs no longer leak a GDI bitmap per right-click (§02.2a-ii) | 1 |
| ✅ | `shell.exe -check` (§03.1b step 4) — parses, reports, publishes nothing; a missing file no longer reports ok | 3 |
| ✅ | Taskbar (§02.5) — one cached UIA round trip instead of four, and the bounded wait is now counted. The rectangle model was measured and declined | 3 |
| ✅ | Circuit breaker and one-shot bypass gesture (§01.7, §05.2) — one gesture classifier, so reload and bypass cannot both fire | 3 |
| ◐ | CoCI policy compile (§01.9) — the fast path landed; conditional attach deferred with reasons in §01.9a | 3 |
| ✅ | **Takeover half of the harness** — `--takeover` loads Shell into the probe process; all 23 scenarios byte-identical, and the one that was not found a real defect | 0 |
| ✅ | The hook no longer destroys the host's last-error code (found by the above) | 2 |
| ✅ | **Replay half of the harness** — four shell-namespace scenarios; `b63fdc2` and `a634ab6` verified against a real borrowed menu | 0 |
| ✅ | The circuit breaker no longer counts deliberate declines, which had switched takeover off after three popups in any non-Explorer host | 3 |
| ✅ | MSAA (§05.3) — confirmed against Shell's own composed menu in a real `explorer.exe`; closed as already satisfied | 5 |
| ✅ | Smart columns (§05.5) — `settings columns = N`; measured on a real menu, 239×1031 scrolling → 938×990 in four columns | 5 |
| ✅ | **Ring export** (§4) — `shell.exe -report perf` reads menu timings out of any host on the desktop. Pre-display measured in a real Explorer at **10.3 ms p50 warm**, inside the 15 ms budget | 1 |
| ✅ | **Deploy ordering** — Explorer was mapping the build *before* the one just deployed, so every real-host result on this branch had been one build stale | 0 |
| ✅ | `priority` versus the `TreatAs` redirect (§01.9b) — measured four ways; de-duplicated, and `shell.exe -check` now says when the setting is inert | 3 |
| ✅ | Host tracking flags in the report — which half of `complete_host_contract` a real host exercises. Explorer and Everything both set `TPM_RETURNCMD` | 2 |
| ✅ | Flicker A/B (§02.4a) — 7.0 ms per menu, landing *after* `popup.total_pre_display` stops. Gated on `flicker`, reported as its own phase | 2 |
| ✅ | Targeted moveto (§04.6) — a location-bearing moveto rule opens the submenu it named, not all of them. Measured on a real Explorer: 85.5 ms → 20.2 ms, same menu | 4 |
| ◐ | Third-party hosts (§3.1) — Everything driven and the breaker fix confirmed there. The non-`RETURNCMD` code path *is* covered by `takeover.a_native_item_replays_its_own_identifier` against a real borrowed shell menu; what is open is whether any shipping host chooses those flags, which needs a person with a lister open | 5 |
| ✅ | Provider names in the report (§05.1a) — names live beside the records, so the measured path stays string-free and the identifier printed is the CLSID `-quarantine:add` takes | 5 |
| ✅ | Quarantine (§05.1b) — `shell.exe -quarantine`, per-user, effective next menu. Skips the provider rather than refusing its activation. Verified through MSAA on a real Explorer: 29 menu items against 30 | 5 |
| ✅ | Takeover status in the report (§05.1) — the row that matters most when nothing has been measured | 5 |
| ✅ | Popup stack (§01.6a) — `_level` removes by handle, so it can no longer disagree with `_map` about which window went away | 2 |
| ✅ | `shell.exe -check` with no argument — could never find a configuration on any machine, because the export skips the bootstrap that sets the paths up | 3 |
| ✅ | **§03.5's last acceptance criterion** — invalid config, Explorer restarted, menus served from the persisted shadow. Verified in a real Explorer; the menu came back at 213 × 680, identical to baseline | 3 |
| ✅ | Reliability Center **window** (§05.1d) — `shell.exe -reliability`. Provider list merged across hosts with quarantine beside it, over the exact text `-report perf` prints. Two defects found by looking at the real window: six CLSIDs rendering alike, and a raw mnemonic marker | 5 |
| ✅ | Seam step 5 (§04.4a, §04.4b) — selection layering. Not blocked by the rendering harness after all: it lives in `Selections.cpp`, which `tests.vcxproj` already links. The split exposed a real defect — `has_IShellBrowser` is set from a window class hash, so a host embedding the shell view lost the selection it had already handed Shell | 4 |
| ✅ | Seam step 6 of §04.4a — superseded by the rows below; step 7 remains. The claim that *both* gated favorites and the inspector was wrong: §05.6 needs origin-stable identity from `MenuModel` and §05.7 needs that plus parser provenance. Neither needs the presenter | 4 |
| ✅ | Interception backend (§01.9c, backlog item 8) — **found missing by the 2026-08-24 audit**, having been absent from the handoff's own list of remaining work. The interface is declined (the two mechanisms are not interchangeable — the PE format makes an import table per-image) and so is the per-hook-entry health check (it cannot fail). What was built is the `intercept` line: which mechanism is live, per host | 3 |
| ✅ | Provider identity in the report (§05.1c) — the slowest provider on the reference machine printed a hash, so the flagship's diagnosis could not be handed to its own treatment. Verified in a real Explorer | 5 |
| ✅ | **Selection array reuse (§02.3a)** — a menu over 200 selected files cost 645 ms, of which 616 ms was `ensure_selection_array` rebuilding an array one `SHParseDisplayName` at a time. 645 ms → 30 ms | 1 |
| ✅ | Provider health keyed by selection shape (§02.2a-iii) — §02.2a specified `(clsid, selection_shape)` and the implementation dropped the shape | 1 |
| ✅ | Lazy large-selection (§04.7) — **measured and declined** on its own gate | 4 |
| ✅ | Per-session memoization (§04.7) — **measured and declined**, which closes item 20. `native.modify_rules` is 0.1 ms across eleven consecutive menus, for ~240 rule evaluations against 27-35 native items: about 0.4 us each. The whitelist's purity claim is the real cost, and a stale menu item is a silent wrong answer | 4 |
| ✅ | **Composed-menu rendering coverage** (§08.3.8) — four `render.*` scenarios read the live menu back through MSAA and assert readability, order against the composed HMENU, layout containment and submenu placement. The gate seam steps 6-7 had been waiting on for three sessions | 0 |
| ✅ | Seam step 6 (§04.4) — `MenuModel`, the origin table §01.4 specified. Three parallel vectors held one fact between them and `_main_popup` was read by nothing at all | 4 |
| ✅ | Seam step 7 (§04.4) — the painting and the layer compositing leave `ContextMenu.cpp` for `MenuPresenter.cpp`, 1,606 lines, both halves verified byte-identical. 7,542 → 5,852. **Closes item 17.** Naming the `Win32MenuPresenter` class remains, and is design work over a surface the split has just made readable | 4 |
| ✅ | The harness transient — diagnosed and fixed. A documented `TrackPopupMenu` contract the harness never met, a blind wait `TPM_NONOTIFY` makes impossible, and no settle between menus. 26 clean runs against 2-in-14. Found only once a fixture mismatch started printing `FAIL` like every other failure | 0 |
| ✅ | **Rule provenance** (§05.7) — every `NativeMenu` records the file and line it was written on; `CACHE::files` carries the list into the generation. The inspector's long pole, and half of what favorites needed | 5 |
| ✅ | **Favorites** (§05.6, §05.6a) — `settings { favorites = N }` lifts pinned and most-used items into a section at the top. Per-user file, `shell.exe -favorites`. Verified in a real Explorer: `Refresh \| NanaZip \| ———` promoted, both origins, cap honoured. Three departures from the plan, each with its reason | 5 |
| ✅ | **Rule inspector** (§05.7, §05.7a) — Shift+Alt+right-click puts each item's origin, matched rule locations and identity in its tooltip. One of the plan's four fields declined: re-evaluating `where=` to display it could disagree with what the menu did. **Closes item 19, and the backlog** | 5 |
| ✅ | **The config watcher died after one reload** (§03.3b) — `start()` re-pointing from its own callback joined its own thread, threw, and left the stop event signalled. "Save shell.nss and the menu follows" had worked once per Explorer lifetime since it landed. Found by needing two consecutive live edits | 3 |

### The 2026-08-24 backlog audit

The status table above is maintained per session and had drifted from the
backlog it tracks. Reconciled item by item against `00-master-plan.md` §3:
**fourteen closed** (built, or measured and declined with numbers recorded),
**three partial** (item 9 conditional attach, item 14 the window, item 20
memoization and lazy selection), **three open** (items 8, 17, 19).

Item 8 was closed the same day (§01.9c). By the end of the second 2026-08-25
session the tally was **eighteen closed** (items 1–8, 10–18, 20), **one
partial** (9) and **one open** (19).

**Item 19 closed in the third 2026-08-25 session**, which closes the backlog:
favorites (§05.6a) and the rule inspector (§05.7a), over the parser provenance
that landed first. After §09 the tally is **all twenty closed** as outcomes, and
**15 implemented / 5 resolved differently** on the strict view in §00.3b — item
9's conditional attach remains declined, not deferred. Two tallies rather than
one, because an item that reads "closed" in every document stops being looked
at, and that is exactly how item 5 kept a never-built replay path for three
sessions (§01.3-0). `08-handoff.md` §3.6 is the running version of this.

Two things worth carrying forward from how item 19 landed, because neither is
about the feature:

- **Building it found a defect in something already shipped.** Verifying the
  pinned section needed two consecutive live configuration edits, and the second
  never arrived — the config watcher had been dying after exactly one reload per
  Explorer lifetime since it landed (§03.3b). Item 16 had been marked done on a
  test that made one edit.
- **The plan was wrong about one thing in a way only the product's own usage
  shows.** §05.6 keys a custom item's identity on where its rule sits in a file;
  for an audience that edits `shell.nss` constantly, that un-pins everything
  below an inserted line. §05.6a records the departure and the reason.

Item 20 is closed by measurement rather than by building, in all three of its
parts: the icon cache was declined in §04.7; the lazy large-selection item on
2026-08-25 against its own stated gate — `selection.preparing` 0.0 ms, metadata
1.3 ms for 200 items; and per-session memoization the same day, also against its
own gate — `native.modify_rules` 0.1 ms across eleven consecutive menus, roughly
0.4 microseconds per rule evaluation. Taking the second of those found §02.3a, a
645 ms menu of which 616 ms was Shell rebuilding an `IShellItemArray` the view
had already built.

Two of those had been invisible for several sessions because every individual
update to the table was itself accurate — the failure was that nothing was
reconciling the table against the plan. `08-handoff.md` §1 rule 6 now says to.

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
- The whole host-observable message stream, for 20 scenarios, recorded as
  committed fixtures rather than reasoned about (`src/tests/hostprobe/`).
- The packaged-verb *providers*, which turned out to cost far more than the
  scan did: **~700 ms on the first menu in a process and ~170 ms on every menu
  after it**, of which ~46 ms is `CoCreateInstance` alone, warm. That number is
  what re-shaped §02.2a (`Include/ProviderHealth.h`).
- The diagnostics ring's own overhead, against the budget set for it: 47.8 ns
  per phase, ~0.6 µs per menu (`Include/Diagnostics/DiagnosticsRing.h`).

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

Acceptance, **restated 2026-08-25** against what was built (§09 R7.8). A
checkbox is a gate, not history: one of these could never be met and saying so
is the correction.

- ✅ No package I/O and no `GetState(TRUE)` on the menu thread — enforced by
  `check-invariants` rules 1 and 10 rather than by a debug assertion, which is
  stronger: it fails the build rather than a run somebody has to perform.
  Package *enumeration* joined them in §09 R3.
- ❌ **superseded** — "a fake provider sleeping 2 s in `GetState` does not delay
  first paint and appears on the next menu." Not met, and not achievable
  without moving `IExplorerCommand` calls off the menu thread, which §02.2a-i
  declined on the documented grounds that "these methods are called on the UI
  thread". What is delivered instead: the *first* such menu pays once, the
  provider is remembered as slow and skipped from then on, and the exclusion
  expires so a handler that gets fixed comes back. See §00.4a for the governing
  wording this replaces R1a's literal form with.
- ✅ Ring shows the provider table populated, with the deferral counted — and
  since §09 R1.2 with the two deferrals told apart: `deferred(slow)` is a
  judgement about the handler, `deferred(budget)` is a statement about the
  menu, and they ask the user for opposite remedies.
- ⏳ cold-start right-click p95 ≤ warm p95 + 10 ms on the reference machine —
  open; the cold outlier is the first menu in a process and is bounded by the
  budget rather than by this ratio.

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
~~config watcher (§03.3)~~ **landed 2026-08-24, see §03.3a**. Each step
independent; order flexible after Phase 3.

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

**`--verify`, `--record` and `--shell` each require their operand**, and a run
that selects no scenario fails. Both rules exist because the abbreviated
`hostprobe.exe --verify` used to fall through to the substring filter, match
nothing, and report `0 scenario(s), 0 failure(s)` with exit code 0 — a gate that
can be asked to exercise nothing and still say yes gates nothing. Exit codes
above the failure range say why a run never reached an expectation: 121 nothing
ran, 122 malformed command line, 123 `--shell` without `--takeover`, 124 Shell
would not load, 125 no window. See `src/tests/hostprobe/Arguments.h` and §09 R0.

**The expected counts are part of the gate**: 23 scenarios native (9 skipped,
which are the takeover-only set — a native run reporting 0 skipped is not a
native run) and 32 through takeover.

**`--shell` cannot redirect the shell-namespace scenarios.** Those reach Shell
through COM, and COM loads the copy named in the registry — so `--shell` points
this process's *loader* at a build while `QueryContextMenu` activates the
registered one. Use the per-user `InprocServer32` override in §08.3.7, and check
the `takeover:` line names the build under test.

It builds on every platform and **is deliberately not run by `build.ps1`**: it
creates a window and shows real popup menus, which does not belong in a
developer's build. **Execution is a manual pre-merge step**, not a job — see
§3.

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
3. ◐ **UNINIT tolerance** — the pairing itself is now verified against a real
   borrowed menu: `takeover.every_borrowed_popup_is_told_it_is_finished` builds
   its menu through the shell namespace, so the popup Shell borrows is one that
   whichever handlers this machine has installed filled, and every INIT is
   matched by exactly one UNINIT. Removing the UNINIT loop fails that scenario
   and no other.

   What is still open is the narrower §01.5 divergence: an UNINIT for a
   `LegacyEager` descendant the user never opened, which untouched Windows would
   never send. That needs a configuration with a location-bearing `moveto` rule
   to force the eager policy, and representative third-party handlers to receive
   it.
4. ✅ **Gesture non-interference** — bypass (`Ctrl+Alt+RClick`) and config-reload
   (`Shift+Ctrl+RClick`) can never both fire from one click (QA-04). **Answered
   without the harness, and better.** A harness run could only have shown that
   two independent keyboard reads happened to agree once; the rules are now a
   pure function of one snapshot (`Include/TakeoverGesture.h`), the hook
   classifies once and passes the result to both consumers, and
   `test_takeover_gesture.cpp` enumerates the whole reachable table. See
   §01.7a.
5. ✅ **Replay delivery ordering** (QA-03) — answered the other way round.
   `select.plain.classic` and `question.notifybypos_reports_a_position` show
   Windows **posts** `WM_COMMAND`/`WM_MENUCOMMAND` after `WM_EXITMENULOOP` and
   after the tracking call returns, so the replay design is posted, not
   synchronous, and delivery-before-destroy is Windows' problem rather than one
   Shell introduces.

### The takeover half — built 2026-08-24, and it needed no deployed build

This section used to say the takeover half was "scheduled with the Phase 2.3
replay work" because "running the same scenarios through Shell's hook and
diffing needs a deployed, injected build". That prerequisite was wrong. Shell is
a COM in-process server and `BootstrapOnce()` is called from
`DllGetClassObject`, so **any process that asks Shell for a class object gets
the popup hook installed in itself** — no injection, no deployment, no Explorer
restart. `src/tests/hostprobe/Takeover.h` does exactly that:

```powershell
src\bin\x64\hostprobe.exe --takeover --verify src\tests\hostprobe\fixtures
src\bin\x64\hostprobe.exe --takeover --shell <path> ...   # which Shell to load
```

There is no second fixture set, because the result is stronger than one: **all
23 scenarios are byte-identical through the hook**, so the native baselines are
the takeover gate.

What that verifies is the **fail-open and circuit-breaker paths**, exercised in
a real host for the first time — not the replay. `QueryShellWindow` does not
recognise the probe's window class, so Shell declines every one of these menus
and the breaker opens after three; the traces prove the decline is
host-observably invisible, which is §01.2's safety property and was previously
supported only by reading.

**It found a defect on its first run.** One scenario in twenty-three came back
different: the hook was handing the host whatever last-error value teardown
happened to leave, destroying the failed-versus-cancelled distinction that
`Include/HostContract.h` itself consumes. Fixed; re-introducing it fails that
scenario and no other.

### The replay half — built the same day, and it found the worse bug

Four `takeover.*` scenarios build their menu the way a file manager does —
`SHParseDisplayName` → `SHBindToParent` → `IShellFolder::GetUIObjectOf` →
`IContextMenu::QueryContextMenu` → track (`src/tests/hostprobe/ShellMenu.h`).
That is what sets `loader.contextmenuhandler` and makes `QueryShellWindow` take
its `goto ui` branch, so it is the only shape Shell will take over from a host
that is not Explorer — and it is the path `b63fdc2` fixed.

Their traces are printed and never stored: a shell menu's items come from
whichever handlers the machine has installed. They assert properties instead —
Shell substitutes a menu of its own, every borrowed popup that received an INIT
receives exactly one UNINIT, a non-`RETURNCMD` host is told which of *its* items
was chosen by wID exactly once, and a `RETURNCMD` host gets the identifier back
and is not notified as well.

**They exposed a defect worse than the last-error one.** Run in the same process
as the 23 declining scenarios, the first of them failed: three declines had
opened the circuit breaker, because `ContextMenu::Initialize` returned the same
`nullptr` for "not a window I handle" as for a real failure. A file manager
raising three of its own internal popups would have lost Shell for the session.
Fixed in `5085b93`; §01.7a has the reasoning. **The scenario ordering is
load-bearing** — the `takeover.*` cases must stay last.

Two things to know before relying on this:

- **`--shell` cannot redirect them.** COM activates Shell by the path in the
  registry, so they exercise the *installed* copy whatever `--shell` says. Two
  knowingly broken builds passed every assertion before that was noticed; the
  harness now refuses the configuration and lists every `shell.dll` mapped in
  the process. Deploy the build you want to test.
- **Each assertion was checked to catch its defect** — removing the UNINIT loop
  fails the pairing scenario and nothing else, disabling the `WM_COMMAND` replay
  fails the identifier scenario and nothing else, and the breaker regression
  fails the first. Doing that needed the defective build to *be* the registered
  one, which a per-user `HKCU\Software\Classes\CLSID\...\InprocServer32`
  override achieves without touching HKLM or restarting Explorer.

CI role: builds the probe on all platforms. **Execution is manual.** Earlier
revisions of this document routed it to "the scheduled VM job (below)"; there
was no such job, here or anywhere in the repository, and a gate that exists
only in prose is worse than an acknowledged manual one. §3 names the step and
its owner.

## 3. CI additions

**Landed 2026-08-26.** Until then `.github/workflows/build.yml` ran `msbuild` on
the solution and `tests.exe`, and nothing else — so every rule below was
enforced on a developer's machine only, and this branch had never been through
CI at all, because the workflow triggered on `main` and PRs to `main`.

- **`scripts/check-invariants.ps1` runs in CI**, as its own `invariants` job
  rather than a matrix step. The check reads sources only, so running it once
  per platform proves nothing extra; as a separate job it still reports when a
  platform build fails, which is when a reintroduced pattern is most likely to
  be the cause. It remains wired into `build.ps1` for the developer loop.
- **`scripts/validate-msi-lifecycle.ps1` runs in CI**, per platform, after the
  build. It reads the emitted package read-only, so it needs no elevation and
  no desktop. The build step gained `-restore` so the WiX SDK is restored
  deterministically and the package is actually produced.

  One defect had been hiding behind the manual-only status:
  `validate-msi-lifecycle.ps1` passed under PowerShell 7 and threw on its
  first assertion under Windows PowerShell 5.1 — the edition `build.ps1` and
  `AGENTS.md` both invoke. `Query-Msi` returned an array, which a function
  [unrolls into the pipeline](https://learn.microsoft.com/powershell/module/microsoft.powershell.core/about/about_return#return-values-and-the-pipeline),
  handing back a bare `[pscustomobject]` for any one-row query — and in Windows
  PowerShell those
  [have no `Count` property](https://learn.microsoft.com/powershell/module/microsoft.powershell.core/about/about_pscustomobject#notes).
  Fixed with the unary comma the same page prescribes.
- **The working branch is in the push trigger** while it is live.
- **hostprobe execution stays manual.** It creates a window and shows real
  popup menus, and the takeover half needs a per-user COM override (§08.3.7);
  a hosted runner is not a reliable interactive desktop, and a gate that
  flakes teaches people to ignore it. Both canonical commands are a
  **pre-merge step owned by the branch owner (`@opj161`)**, run at the
  enforced counts with the `takeover:` line naming the build under test:

  ```powershell
  .\src\bin\x64\hostprobe.exe --verify .\src\tests\hostprobe\fixtures
  .\src\bin\x64\hostprobe.exe --takeover --verify .\src\tests\hostprobe\fixtures
  ```

  This is a real gap, stated rather than disguised as a job.
- **`scripts/check-invariants.ps1` enforces ten rules, none deferred.** This
  section said "six enforced rules, two deferred (warn-only) that turn on with
  their phase: `GetState(…, TRUE)` with Phase 1, `SPIF_SENDCHANGE` with
  Phase 3". Both of those are now enforced, as rules 9 and 10, and the script
  reports `check-invariants: OK (10 rules, 0 deferred)`.

  Three things the first version got wrong, worth remembering when adding rules
  (§07 §1.1):
  1. It **failed on its own tree** — the Recycle-Bin rule matched the comment that
     the same change added to explain the removal. A rule names something the code
     must not *do*; it must not fire on prose about it. The script now strips block
     comments and line comments before matching. **Not string literals** - this
     said it did, and it does not: `strip_code` replaces comment bodies with
     spaces and leaves literals alone. A rule whose pattern could match a string
     has to be written to exclude one. Corrected 2026-08-25
     (docs/refactor/09-remediation-plan.md R7.7).
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

**The ring export exists now** — `shell.exe -report perf`, and `perf:all` for every
recorded session rather than the slowest. It needs no registry value and reads every
host on the desktop, which is what makes the budget below checkable where it matters.

First measurement against that budget, a real Explorer, Windows 11 26200.8875 x64,
2026-08-24: **pre-display p50 10.3 ms warm, 60 ms on the first menu in a process.**
Inside the 15 ms p95 target for the warm case; the cold outlier is the first
activation of every packaged verb handler, and is what §02.1 step 5's persistence
decision is about.

One correction the export forced. `popup.total_pre_display` stops before Shell starts
tracking, so it does not include the vertical-blank wait in `WM_NCCALCSIZE` — another
**7.0 ms on average** (§02.4a). Right-click to pixels is therefore ~23 ms, not the
~16 ms the phase alone reports, and a budget written against that phase is measuring
about two thirds of what the user waits for.

Two things about measuring on a real Explorer that silently invalidate a run, both now
in `AGENTS.md`: a registry value set from an agent's shell is not visible to Explorer
at all, and a deploy that stops Explorer before copying gives it the previous build.

Budgets set from measured p95/p99 on the reference machine, not invented: initial
targets — pre-display added by Shell ≤ 15 ms p95 Explorer file context; taskbar
hit-test added ≤ 2 ms; catalog refresh never on menu thread (hard gate, not budget).

**Split into two numbers, 2026-08-25 (§09 R1.4).** One budget covering both
Shell's own work and third-party handlers could not be met and could not be
enforced, and `ProviderHealth.h` was tuned so that it never fired — so the
document and the code stated incompatible targets and the code won silently.
What replaces it:

| | Target | Measured 2026-08-25, 37 handlers |
|---|---|---|
| **Shell's own pre-paint work** | ≤ 15 ms p95 | **~1.1 ms** — met with room to spare |
| **Third-party provider work** | reported, ordered cheapest-first, capped per menu at `MENU_BUDGET_US`; a breach appears as `explorer.commands.over_budget` | 36.6 ms of a 37.7 ms menu |

The second is deliberately not a pass/fail number on this branch: a handler's
cost is the handler author's, Shell cannot interrupt a call already running, and
the only lever that would lower it — condemning routine outliers rather than
pathological ones — removes a menu item the user has today. That trade is
costed in §09 R1.4 and belongs to the maintainer, not to a threshold edit.

## 5. Windows acceptance matrix (VM/manual; from A1§25, trimmed to what this plan changes)

Per release of Phases 1–3:

- OS: Win10 22H2, current Win11 stable, one Insider ring (early warning).
- Contexts: file/folder/mixed/large-selection/background/desktop/taskbar(±secondary)/Home/
  QuickAccess/RecycleBin/third-party host (Total Commander or Everything at minimum).
- Lifecycle: install/upgrade/uninstall/repair incl. TreatAs foreign-state preservation
  (existing validator covers tables; VM covers behavior).
- ~~Accessibility: Narrator over owner-drawn menus before/after MSAA change.~~
  **Done 2026-08-24** — read back through `IAccessible` against Shell's own
  composed menu in a real `explorer.exe`, which is the surface Narrator reads.
  22 named items, 6 correctly-unnamed separators, states right. See §05.3.
- Config: LKG typo scenario; watcher live-edit; reload-during-open-menu.

## 6. Risk register deltas (vs master plan §2 sources)

| Risk | Mitigation |
|---|---|
| TPM replay regressions in exotic hosts | probe-gated landing (Phase 2.3) + HostProfile escape hatch + bypass gesture |
| Backend swap reduces coverage | experiment stays opt-in until matrix proves parity; private win32u route remains default |
| Async services introduce lifetime bugs | reuse proven worker pattern (TaskbarUiaWorker); snapshot publish is atomic shared_ptr like Initializer |
| Quarantine misfires on legit-but-slow extensions | telemetry-only tier first; manual quarantine default; one-click undo |
| Scope creep in seam extraction | move-only commits; each seam revertible; features wait for MenuModel |
