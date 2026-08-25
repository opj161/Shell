# 10 — QA assessment of the 09 remediation

**Independent verification pass, 2026-08-25**, against
`refactor/takeover-master-plan` with HEAD at `450985f` and the remediation
present as **uncommitted working-tree changes**.

This document audits [`09-remediation-plan.md`](09-remediation-plan.md) — both
the plan and its §0a claim that the plan was carried out. Every statement below
is one of:

- **(m)** measured on this machine, with the command and the output;
- **(c)** checked against the tree, with file and line;
- **(d)** quoted from the vendor's reference page, with a canonical deep link.

Nothing here is inferred from the remediation document's own account of itself.

---

## 0. Verdict

**The engineering is sound and the gates are genuinely green. The bookkeeping
around it is not, and the work is not committed.**

Every claim in §0a that can be re-measured, re-ran clean: three platforms build
with zero warnings, 33,102 checks pass, `check-invariants` reports OK on all
three, both harness modes pass against the build under test, all three MSIs
validate, and the two ~64 KiB stack frames are gone. The three headline
workstreams — R0's harness gate, R2's by-position replay, R6.7's detour
enlistment — are well designed, correctly cited and verified working end to end.

Sixteen findings follow. One is process-blocking (**F1**: nothing is committed).
Six are Medium and concentrate in one place — **R3, the package unification, is
the weakest workstream on the branch**: it lost a memoization, contradicts
itself in a comment, dropped six of its own required tests, dropped one of its
own design steps, and left the rewritten class untestable by construction. The
rest are documentation drift, including the specific drift the plan was written
to stop.

| | Count |
|---|---|
| Verified green, no finding | 11 gates |
| Blocking (process) | 1 |
| Medium | 8 |
| Low | 7 |

---

## 1. What was re-measured, and what it said

All commands run on this machine on 2026-08-25 against the working tree.

| # | Gate | Command | Result |
|---|---|---|---|
| G1 | x64 build + suite | `.\build.ps1 -Platform x64` | **33,102 checks / 0 failures**, `0 Warning(s) 0 Error(s)`, exit 0 (m) |
| G2 | x86 build + suite | `.\build.ps1 -Platform x86` | **33,102 checks / 0 failures**, 0 warnings, exit 0 (m) |
| G3 | arm64 build + package | `.\build.ps1 -Platform arm64` | builds and packages, 0 warnings, tests skipped (host is x64), exit 0 (m) |
| G4 | invariants | run by `build.ps1` on each platform | **`check-invariants: OK (10 rules, 0 deferred)`** ×3 (m) |
| G5 | harness, native | `src\bin\x64\hostprobe.exe --verify src\tests\hostprobe\fixtures` | **23 scenario(s), 0 failure(s), 9 skipped**, `verified against …\fixtures`, exit 0 (m) |
| G6 | harness, takeover | per-user CLSID override + `--takeover --verify …\fixtures` | **32 scenario(s), 0 failure(s)**, exit 0, `takeover: C:\Users\j_opp\Projects\Shell\src\bin\x64\shell.dll` — the build under test (m) |
| G7 | R0 exit codes | seven malformed invocations | `--verify`/`--record`/`--shell` with no operand → **122**; `--verify --takeover` → **122**; `--bogus` → **122**; `zzz_no_such` → **121**; `--shell x.dll` → **123** (m) |
| G8 | MSI lifecycle | `scripts\validate-msi-lifecycle.ps1 -Path @(x64,x86,arm64)` | **`ok` ×3** (m) |
| G9 | `/analyze`, DLL | `MSBuild /p:RunCodeAnalysis=true /t:Rebuild src\dll\Shell.vcxproj` | **0 × C6262** — the two 64 KiB frames are gone. **6 lock warnings**, up from the plan's baseline of 4 — see **F7** (m) |
| G10 | `/analyze`, exe + CA | same, `src\exe\exe.vcxproj` and `src\setup\ca\ca.vcxproj` | **clean, zero warnings** (m) |
| G11 | wire-version gate, live | `.\src\bin\x64\shell.exe -report perf` against the running Explorer | `2 processes had a ring this build cannot read - restart them to pick up this Shell.` — the documented version-8 consequence, reported rather than silently empty (m) |

Two counts the documents assert, re-derived: `ContextMenu.cpp` is **6,182**
lines and `MenuPresenter.cpp` **1,795**, exactly as
[`08-handoff.md`](08-handoff.md) now states (m). The suite is **63**
`test_*.cpp` files carrying **758** `TEST(...)` macros, against the plan's
`E-10` baseline of 59/678 at `450985f` (m).

Per-suite counts claimed in §0a: `test_hostprobe_args` **13** ✓,
`test_detour_enlistment` **11** ✓, `test_explorer_command_state` **9** ✓,
`test_provider_schedule` claimed 15, actually **16** (m — understated, harmless).

### 1a. Contract citations, verified verbatim

Every load-bearing quotation in the changed code was fetched from Microsoft
Learn and compared word for word. All five check out:

| Quoted in | Passage | Source |
|---|---|---|
| `HostContract.h:99`, `ContextMenu.h:806`, `ShellMenu.h` | "MNS_NOTIFYBYPOS … Menu owner receives a **WM_MENUCOMMAND** message instead of a **WM_COMMAND** message when the user makes a selection. **MNS_NOTIFYBYPOS** is a menu header style and has no effect when applied to individual sub menus." | [MENUINFO](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-menuinfo) (d) |
| `Main.cpp:1432`, `Scenarios.cpp:666` | "*wParam* — The zero-based index of the item selected. *lParam* — A handle to the menu for the item selected." | [WM_MENUCOMMAND](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menucommand) (d) |
| `ProviderSchedule.h:47`, `00-master-plan.md` §4a | "None of the methods of this interface should communicate with network resources. These methods are called on the UI thread…" | [IExplorerCommand](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorercommand) (d) |
| `ExplorerCommandState.h:16` | "the verb object should not perform any memory intensive computations that could cause the UI thread to stop responding. The verb object should return E_PENDING in that case" | [GetState](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getstate) (d) |
| `PerfExport.h:936` | "All bytes must be within the maximum size specified by CreateFileMapping" and "If this parameter is 0 (zero), the mapping extends from the specified offset to the end of the file mapping" | [MapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile) (d) |

**R6.2 is the model of how this should be done.** The old comment claimed a size
check that did not exist; the new one names the mechanism that actually enforces
it, quotes both halves of the `dwNumberOfBytesToMap` contract, and says which
one-character change (`sizeof(...)` → `0`) would silently break it. That is a
contract citation, not a decoration.

**One documented divergence, correctly handled.** `WM_MENUCOMMAND` is documented
as *sent*; Shell **posts** it (`Main.cpp:1439`). The justification is measured
rather than asserted: `question.notifybypos_reports_a_position.trace:9` records
Windows delivering it *after* `WM_EXITMENULOOP`, and
`question.notifybypos_with_returncmd.trace` records that with `TPM_RETURNCMD` no
notification is delivered at all. That is AGENTS.md's documentation-versus-probe
rule applied correctly, and the divergence is explained in a comment beside the
call (c).

---

## 2. Workstream-by-workstream verdict

| | §0a claim | Verified verdict |
|---|---|---|
| **R0** harness gate | done | **Confirmed, fully.** `Arguments.h` is a pure parser; all seven malformed shapes exit non-zero with `FAIL` in the same word every other failure uses; `ran == 0` fails (121). The fixture directory now prints in the summary. (m, c) |
| **R1.1** two-phase scheduler | done | **Confirmed as designed, unverified on the machine.** `ProviderSchedule.h` is pure, deterministic, honest about its bound ("at most one in-flight overrun"), and covered by 16 tests. Ordinal→slot mapping, registration-order publication and duplicate-winner order are correct (c). But see **F17**: the live 52.2 ms session it exists to fix has not been re-measured, because the build is not deployed. |
| **R1.2** truthful outcomes | done | **Confirmed.** `classify_command_state` implements §02.2's rule including the previously missing failure branch; `Pending` is emitted; `DeferredSlow`/`DeferredBudget` are distinct on the wire; `PERF_EXPORT_VERSION` 7→8 with an exact-match reader, pinned by `the_version_is_the_one_this_meaning_belongs_to` and `a_block_from_the_previous_version_is_refused`; the live refusal was observed (G11). (m, c) |
| **R1.3** report the breach | done | **Confirmed.** `explorer.commands.over_budget` is recorded only on a breach and annotated with the refused count. Ring timing is always-on (`MenuPerf.h`), so it does not depend on the opt-in `perf` value. (c) |
| **R1.4** reconcile budgets | done, option (b) | **Confirmed.** §06.4 is split into two numbers, §00.4a states what is and is not bounded, R1a's literal form is retired in §00.1 rather than quietly weakened. Option (c) correctly left to the maintainer. (c) |
| **R1.5** selection fallback | blocked on R8 | **Correct.** `selection.rebuild_array` instrumentation is present (`ExplorerCommand.cpp:330`); no cap was invented without the number. (c) |
| **R2** by-position replay | done | **Confirmed, with one coverage gap.** Style read once on the root with the return value checked; separate `[0x60000000, 0x6fffffff)` tracking range that `ident.equals()` and `is_synthetic_id()` both reject; `_native_origins` maps back to `{position, menu}`; `WM_MENUCOMMAND` posted. Live proof: the takeover run emits `WM_MENUCOMMAND position=33 menu=menu#1` and zero `WM_COMMAND` (m). Gap in **F9**. |
| **R3** package unification | done | **Weakest workstream. Six findings** — F3, F4, F5, F6, plus its share of F12. The architecture is right; the execution left a memoization regression, a self-contradicting comment, an untestable class, and a dropped design step. |
| **R4** `Win32MenuPresenter` | done | **Confirmed.** `PresenterContext` is a closed 15-member struct of references, the five anonymous member structs were named first (which is what had made this look hard), the eight paint functions are `Win32MenuPresenter::` members, and `ContextMenu` is the client. Gated by the four `render.*` scenarios, all green through the override, all four reading the same 239×820 popup of 33 items (m, c). |
| **R5** invariant rewrite | done | **Confirmed.** Invariants 1, 2 and 7 rewritten to what the code guarantees; §4a names the two exceptions; the `SPI_*` parameter shapes are pinned with their citations. (c) |
| **R6** small fixes | done | **6.1 ✓ · 6.2 ✓ · 6.3 ✓ · 6.5 half ✓ · 6.6 ✓ · 6.7 ✓ · 6.4 one third.** See **F7** (analysis regressed) and **F10** (6.4's tests and atomic-write decision missing). |
| **R7** documentation | done | **Mostly ✓, three drifts introduced** — **F12** (the tally), **F13** (the gate counts), **F14** (a corrected sentence naming a function that does not exist). |
| **R8** environment validation | open | **Correct, unchanged.** |

---

## 3. Findings

### F1 — Blocking (process): none of the remediation is committed

**HEAD is `450985f`** — the exact commit
[`09-remediation-plan.md`](09-remediation-plan.md) says it was written and
audited against. `git rev-list --count main..HEAD` is **83**, unchanged. The
entire remediation exists as 44 modified files (+2,895 / −389) plus 8 untracked
files in the working tree (m).

§0a's table is headed "**where it landed**". Nothing landed. That has concrete
consequences for the plan's own §4 gates, every one of which is written
per-commit:

- **Gate 5** — "Cite the contract and quote the passage, in the code **and in
  the commit message**" — cannot have been satisfied. The in-code half is done
  well (§1a); the commit-message half does not exist.
- **Gate 1–2** — three platforms and both harness modes *per commit* — cannot be
  evidenced per change. They pass for the aggregate, which is what this pass
  measured, but "which change broke it" is unanswerable.
- **Gate 3** — "re-introduce the bug, rebuild, watch *that* test fail" — §0a
  claims this was done for three changes. Unverifiable and unrepeatable without
  the commits.
- **R4's own design** mandates **two commits** ("Introduce the surface, change
  nothing else" then "Move the functions"). One working tree cannot be two
  commits, and the byte-identical-bodies claim can no longer be checked the way
  §04.4 requires.
- **R2 step 7** requires the invariant-rule change "**in the same commit**" as
  the style read.
- **R7.9** requires the whitespace cleanup be "a formatting-only commit; no
  behavior belongs in that commit". It is currently mixed into the R2 change to
  `ContextMenu.cpp` and the R4 change to `MenuPresenter.cpp` (**F16**).

Practical risk: a single `git checkout -- .` destroys roughly 2,900 lines of
work that no reflog can recover, and `docs/refactor/09-remediation-plan.md`
itself is untracked.

**Recommended:** commit as the plan sequences it (R0 → R6.7 → R7.1–7.2 → R2 →
R1 → R3 → R5 → R4 → R6), each with its contract citations, before anything else
on this list.

---

### F2 — Medium: `ProviderHealth::consider()` is dead production code with 30 live tests

After R1.1, `ExplorerCommand.cpp` calls `classify` / `note_slow_deferral` /
`note_reprobe_started` / `note_budget_deferral`. `consider()` has **no shipping
caller** — `rg '\.consider\(|->consider\('` finds only
`src/tests/test_provider_health.cpp`, 30 call sites (c). `ProviderVerdict`
(`Try`/`DeferSlow`/`DeferBudget`) is now reachable only through it.

So the tree carries two admission policies that can disagree, one of them
untested against the product and the other tested only against itself. This is
precisely the shape AGENTS.md warns about — *"a test that only ever calls a
function the way the test calls it proves nothing about the way the product
calls it"* — and it inflates the headline check count with assertions about
code no user reaches.

The semantics were preserved carefully in the port (verified line by line:
unknown → try while budget remains; known → refused on estimate; slow not due →
`since_probe++` then defer; slow and due → probe granted, counter reset before
the call), with two deliberate improvements — the estimate is `max(best, last)`
rather than `best`, and a due re-probe refused for budget is now recorded as a
budget deferral and stays due. Those improvements exist **only** in the new
path; `consider()` still encodes the old ones.

**Recommended:** delete `consider()` and `ProviderVerdict`, and either retire or
re-point `test_provider_health.cpp`'s 30 assertions at the live pair
(`classify` + `plan_providers` + `provider_step_fits`).

---

### F3 — Medium: R3 lost `display_name` memoization

`PackageIndex::display_name` cached the resolved name per entry and returned it
on every subsequent call (`Packages.cpp:487-491`, `entry.display_resolved`) (c).

`PackagesCache::display_name` now constructs a fresh `RegistryPackageSource` and
calls `resolve_display_name` **on every evaluation** (`Cache.h:98-115`), with no
cache anywhere in the path (c).

So `appx.name` / `package.name` moved from *resolve once per process* to
*resolve every time*, on the menu thread, where resolution can call
`SHLoadIndirectString` — which for the `@{PackageFullName?ms-resource:…}` form
loads the package's `Resources.pri` — and then walk the MrtCache tree
([SHLoadIndirectString](https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-shloadindirectstring)) (d).

R3.5 offered exactly two acceptable outcomes: queue resolution on the catalog
worker and publish a name cache, **or** "keep the synchronous compatibility
behavior, **measure it**, name it as an exception to R1". What shipped is a
third: keep it synchronous, name it as an exception, drop the cache, and do not
measure. The measurement is still listed open in R8.

No shipped configuration uses these functions —
`src/bin/imports/terminal.nss:8,12` uses only `package.exists` and
`package.path` (c) — so there is no user-visible effect today. It is a
regression against an explicit invariant nonetheless.

---

### F4 — Medium: a comment contradicts the code it introduces, 30 lines apart in the same file

`Include/Cache.h:59-60`, in the class header block:

> Only `appx.name`/`package.name` reach it, no shipped configuration uses them,
> **and it is timed under its own phase so a report says when somebody's does.**

`Include/Cache.h:98-107`, in the function that block describes:

> **Not wrapped in a MenuPerfScope**, and the reason is worth recording…
> The cost of this call is therefore attributed to whichever phase is evaluating
> the expression, which for a menu is `native.modify_rules`.

The second is correct; there is no phase named for it (c). The reason given for
not adding one is legitimate and well argued (including `MenuPerf.h` from
`Cache.h` would make an unqualified `Diagnostics` ambiguous inside
`namespace Nilesoft::Shell` — the exact hazard AGENTS.md §"Namespaces" records).
The header block simply was not updated to match.

This is the same defect class as **R6.2**, the finding whose whole point was
that "a comment names a check that is not there" — reintroduced by R3 in the
same pass that fixed it in `PerfExport.h`.

---

### F5 — Medium: `PackagesCache` was rewritten wholesale, has zero tests, and cannot be tested as written

Every method of `PackagesCache` — `exists`, `find_identity`, `path`,
`display_name`, `all` — was replaced. **No `.cpp` file under `src/tests`
includes `Cache.h` or names `PackagesCache`** (c).

It is not merely untested; it is untestable in its current shape.
`PackagesCache::catalog()` hard-codes
`PackageCatalogService::instance().snapshot_for_menu()` (`Cache.h:141-144`) —
a process-wide singleton that owns a real worker thread, with no injection seam.
That is the opposite of the direction every other service on this branch took:
`ProviderHealth` takes an injected clock, `PackageIndex` an injected
`IPackageSource`, `ConfigWatcher` an injected `WaitForObjects` (added by R6.1 in
this very pass), `InlineDetourTransaction` a whole injected API table.

Six of R3's own required tests did not land:

| R3 required | Landed? |
|---|---|
| a menu-path query never causes the fake source to enumerate | no |
| `CACHE::clear()` leaves the index intact (fails today) | no |
| **a cold query waits at most the budget, then answers from whatever exists** | **no** |
| `path` returns the published value and makes no AppModel call on the reader thread | no |
| a query miss coalesces refresh requests | no (see **F6**) |
| whichever `display_name` policy is chosen is pinned, timed and visible | no |

What did land is five tests of the *store* (`a_publish_carries_packages_as_well_as_commands`,
`both_halves_are_replaced_together_by_a_later_scan`,
`a_scan_invalidated_while_running_publishes_neither_half`) and of the shared
matcher (`the_snapshot_matcher_is_the_index_matcher`,
`a_full_name_and_its_path_come_out_of_one_walk`). Those are good tests of the
plumbing either side of the class. The class itself is uncovered.

The third row is the one that matters. R3's own design step 4 names the failure:
*"If a cold `package.exists()` answered false, the stock config's Terminal item
would vanish from the first menu of every process — a worse defect than the one
being fixed."* The mitigation (`snapshot_for_menu()`'s bounded wait) is
implemented and looks right on inspection; nothing asserts it.

Two secondary observations on the same code, both review-only:

- R3 **widened** the set of call sites that can enter a 400 ms
  `CoWaitForMultipleHandles` on the menu thread — previously only
  `append_explorer_commands`, now any `package.*` expression during rule
  evaluation. On an STA that wait "enters the COM modal loop", i.e. pumps
  messages, during menu composition. The window is narrow (first menu of a
  process, before `warm_async`'s scan lands) and pre-existing in kind, but it is
  new in reach and was not analysed in the plan.
- Only the DLL is affected: `src/exe/exe.vcxproj` does not compile the
  expression engine, so `shell.exe -check` cannot reach this path (c).

---

### F6 — Medium: R3 design step 6 was dropped without being recorded

R3's own §"What is wrong" item 4 reads: *"`PackageCatalogService::invalidate()`
has no shipping caller; freshness is TTL-only despite the original plan's
opportunistic invalidation/miss hint."* Design step 6 answers it: *"Make refresh
reachable. A package-query miss may request one coalesced refresh."*

`invalidate()` **still has no shipping caller** —
`rg 'invalidate\(\)' src/dll/src src/exe/src` returns the definitions plus
`TaskbarUiaWorker`'s unrelated cache (c). Freshness is TTL-only. A query miss
requests nothing.

§0a marks R3 **done** and enumerates three deliverables, none of which is this
one. A design step that is dropped for good reason is a fine outcome; a design
step that disappears between the plan and the status table is how §1 rule 6 says
decisions come to be made by nobody.

---

### F7 — Medium: static analysis regressed, 4 lock warnings → 6, against gate 7

Measured (m):

```text
ProviderQuarantineStore.h(84):  C26110  reload
ProviderQuarantineStore.h(98):  C26117  reload
ProviderQuarantineStore.h(144): C26117  refresh_if_stale
ProviderQuarantineStore.h(150): C26110  refresh_if_stale
ProviderQuarantineStore.h(159): C26117  refresh_if_stale
ProviderQuarantineStore.h(162): C26117  refresh_if_stale
```

The plan's §0 baseline was *"4 lock-analysis warnings plus 2 real ~64 KiB stack
frames"*. The stack frames are gone — **C6262 count is now zero, and the exe and
custom-action projects are entirely clean** (m), which is R6.5's first half
delivered exactly as specified, with C6262's 16 KiB user-mode default threshold
correctly cited
([C6262](https://learn.microsoft.com/en-us/cpp/code-quality/c6262?view=msvc-170)) (d).

But R6.4's `reload()` mutex fix introduced the same analyzer pattern in a second
function, taking the count to six, and R6.5's second half — *"Restructure the
early returns into small locked helpers so the analyzer and a human see the same
ownership"* — was not done. Gate 7 ("triage every new warning") was therefore
not applied to the pass's own output.

The warnings do appear to be false positives (the `lock_guard` scopes are
lexical and correct), which is what the plan concluded. The instruction was to
restructure rather than to accept, precisely so the count stays meaningful.

---

### F8 — Low: three dead helpers from R2, one of them a latent trap

`ContextMenu::ID::is_native_tracking()` (`ContextMenu.h:552`) has **no caller
anywhere in the tree** (c). The mapping is done by `_native_origins.find()`
instead.

Beside it, a hazard worth writing down even though it cannot fire today:

```cpp
static constexpr auto start_sys    = 0x5fffffff;
static constexpr auto start_native = 0x60000000;
uint32_t get_sys()    { return sys++; }        // no caller
uint32_t get_native() { return native++; }
```

`get_sys()` also has no caller (c). If one were ever added, the **second**
allocation would be `0x60000000` — the first value in the new native tracking
range — and a host item would be confusable with a system item. The two ranges
are adjacent with no gap and nothing asserts they stay disjoint.

**Recommended:** delete `is_native_tracking()`, and either delete `get_sys()` or
give the two ranges a compile-time separation assertion.

---

### F9 — Medium: R2's nested-submenu replay has no end-to-end test

R2's test section is explicit: *"select both the root and a nested submenu
item"*, and *"remove the containing submenu handle and only the nested case must
fail."*

The harness scenario `takeover.a_by_position_host_is_told_which_position`
selects a **root-level** item only. The live trace confirms it:
`WM_MENUCOMMAND position=33 menu=menu#1`, where `menu#1` is the host root (m).
`ShellMenu::apply_notify_by_position` appends its target items to the root and
sets `_target_position = count - 3`; no submenu is built (c).

The nested case exists as `test_host_contract.cpp:a_nested_selection_names_its_own_submenu`,
which asserts `done.notify_menu == SUB_MENU`. But `complete_host_contract` only
copies `sel.containing_menu` through — the test cannot distinguish a
`ContextMenu` that fills that field correctly from one that always reports the
root. The assertion that R2 named as necessary is the one that is missing.

The code path reads correct: `enumerate_native_menu_level` stores
`item->native_source.menu = hMenu` per level (`ContextMenu.cpp:3770`), and
`prepare_system_items` — which populates `_native_origins` — runs for each menu
level after `materialize_native_children` (`ContextMenu.cpp:1074-1082`) (c). That
is review, not a test, on the one field the plan singled out.

What the scenario *does* cover well, and deserves saying: it appends a duplicate
identifier and four zero-identifier items on purpose, which is exactly what
defeats the naive "key the table on the host's `wID`" implementation, and the
target is deliberately **not** last so a presenter replaying its own index
cannot pass by coincidence.

---

### F10 — Medium: R6.4 is marked done having delivered one of its three parts

R6.4 asks for three things:

1. *"Add direct real-file integration tests, including a same-process concurrent
   refresh"* for `FavoritesStore.h` and `ProviderQuarantineStore.h` — **not
   done.** No test file includes either header (c). Both still expose
   `set_path_for_testing` that nothing uses.
2. *"take `_path_override` under the mutex in `ProviderQuarantineStore::reload`"*
   — **done** (`ProviderQuarantineStore.h:80-91`) (c).
3. *"Measure/decide an atomic temp+replace write once for both formats rather
   than allowing the two copies to drift"* — **not done.** Both still use
   `CREATE_ALWAYS` (c).

§0a's cell reads "6.4 the store mutex", which is literally accurate about what
was done, while the workstream row reads **done**. The item's own §"While
there," framing makes the mutex the incidental part and the tests the point.

---

### F11 — Low–Medium: rewritten invariant rule 8 does not cover the codebase's own idiom

R2 correctly required replacing the lexical `MNS_NOTIFYBYPOS` ban with a gate on
*setting* the style. The replacement is:

```powershell
Regex = 'SetMenuInfo\s*\([^;]*MNS_NOTIFYBYPOS|dwStyle\s*(\|)?=\s*[^;]*MNS_NOTIFYBYPOS'
```

The first alternative matches a direct `SetMenuInfo(...)` call. **This tree does
not call `SetMenuInfo` directly** — it goes through `MENU::set(hMenu, mi)`
(`Include/MenuItem.h:1333-1336`), which is also how the required read is
written (`MENU::get`, `ContextMenu.cpp:3935`) (c). So only the second
alternative can catch a real violation in this codebase, and it is defeated by
an intermediate variable (`DWORD s = MNS_NOTIFYBYPOS; mi.dwStyle = s;`) or by
the numeric literal `0x08000000`.

R2 step 7 says to *"Prove the gate by temporarily adding the forbidden
`SetMenuInfo` shape"* — which exercises the alternative the tree would never
produce. The gate is necessarily weaker than the ban it replaced; it is weaker
than it needed to be.

Comment stripping is confirmed working (`Get-CodeText`, check-invariants.ps1:54),
so the new `MNS_NOTIFYBYPOS` mentions in `HostContract.h` and `ContextMenu.h`
prose correctly do not fire (c).

**Recommended:** add `MENU::set` to the first alternative, and consider matching
`0x08000000` beside the symbolic name.

---

### F12 — Medium: the tally written to stop tallies drifting, drifted upward

R7.1 is unambiguous about what to publish:

> Add the strict §1.3 table to `00-master-plan.md` §3a, `06` §Status and `08`
> §3.6: **11 implemented, 5 partial (2, 5, 9, 11, 17), 4 resolved differently
> (7, 8, 10, 20)**.

What was published in `00-master-plan.md` §3b is **"15 implemented, 5 resolved
differently, 0 partial, 0 untouched"** (c), echoed in `06` and `08`.

Of the five partials:

| Item | Closed by | Legitimate? |
|---|---|---|
| 2 | code — R3's snapshot unification | yes, with F3/F4/F5/F6 as residue |
| 5 | code — R2's by-position replay | **yes**, cleanly |
| 17 | code — R4's presenter boundary | **yes**, cleanly |
| 9 | **relabelling** — conditional attach re-described from "deferred" to "declined", moving it partial → *resolved differently* | arguable |
| 11 | **relabelling** — closed because §4 now *enumerates* the two `SPI_*` mutations that are still there | arguable |

Three items were closed by writing code. Two were closed by writing prose, by
the same pass that reports the score, in the document whose §1 rule 6 exists
because *"a tally that drifts is how a governing decision came to be made by
nobody."* Both relabellings may well be the right call — item 9's decline has
standing reasons in §01.9a, and enumerating the SPI mutations is genuinely what
R5 asked for. The problem is who decided and when it was recorded, not the
conclusion.

**Recommended:** keep 15/5/0 if it is defended, but say in §3b that items 9 and
11 were **reclassified** in the §09 pass and by what argument, so the next
reader can see the two kinds of closure apart.

---

### F13 — Low: `09-remediation-plan.md` disagrees with itself about its own gate

| Location | Says |
|---|---|
| §0a, "Gate at completion" | "23 harness scenarios native and **32** through takeover" |
| §0, verification table | "23 scenarios, 0 failures (**8 skipped**)" and "**31** scenarios" |
| R0, "Acceptance" | "the canonical commands run exactly **23/31** scenarios" |
| §4, gate 2 | "exactly **23** native and **31** takeover scenarios" |
| §0, harness-warning paragraph | "The claimed **23/31** counts…" |
| **Measured here** | **23 native, 9 skipped, 32 takeover** (m) |

R2 added the thirty-second scenario, so 32 is correct and §0a is right; three
other places in the same file were not updated.
[`06-phases-and-tests.md`](06-phases-and-tests.md) has it right ("23 scenarios
native (9 skipped …) and 32 through takeover"). The document that defines the
gate is the one that states it wrongly, in the section a reader would copy from.

---

### F14 — Low: a correction that names a function which does not exist

`06-phases-and-tests.md` §3, corrected by R7.7:

> **Not string literals** — this said it did, and it does not: `strip_code`
> replaces comment bodies with spaces and leaves literals alone.

There is no `strip_code` in `scripts/check-invariants.ps1`. The function is
`Get-CodeText` (line 54) (c). The substance of the correction is right — the
script strips `/* */` and `//` and deliberately leaves string literals alone,
and says so in its own header comment.

---

### F15 — Low: orphaned comment block in `PerfExport.h`

The block beginning *"Diagnostics::ProviderResult, as the word a report prints"*
documents `perf_export_result_name`. R1.2 inserted
`perf_export_header_understood` — with its own block comment — directly between
them, so the file now reads: comment A, comment B, function B, function A
(`PerfExport.h:746-786`) (c). Cosmetic, and the kind of thing that becomes wrong
rather than merely untidy the next time either function moves.

---

### F16 — Low: R7.9's formatting-only commit can no longer exist

`git diff --check` against the working tree is **clean** (m) — the trailing
whitespace and the EOF blank line are genuinely fixed. But
`git diff --check main...HEAD` still reports **30** hits (m), because nothing is
committed, and the fixes live inside the same working-tree changes as R2
(`ContextMenu.cpp`) and R4 (`MenuPresenter.cpp`) (c).

R7.9's instruction — *"clean the current failures in a formatting-only commit;
no behavior belongs in that commit"* — is now only satisfiable by splitting the
whitespace hunks back out when **F1** is addressed.

---

### F17 — Medium (open verification): R1's headline claim is unmeasured on this machine

R1 exists to fix one observed thing: the 52.2 ms session in which six providers
were dropped, four of them costing 0.3–1.6 ms, purely for sitting behind a
13.9 ms one. Its acceptance criterion is *"the observed 52.2 ms session's four
cheap providers are no longer dropped."*

That has not been re-measured. The scheduler is proved against numbers — 16
tests, including `a_runtime_overrun_can_no_longer_starve_the_cheap_tail`, which
encodes the recorded session's shape (c) — but not against the machine, because
the branch build has not been deployed. Deployment restarts Explorer, which
AGENTS.md says to ask about first.

This matters more than an ordinary open item because of the plan's own gate 4:
*"Anything touching the menu path: deploy it, drive it, and read the menu back.
Both of the last session's most valuable findings were defects in work already
marked done, and neither had a crash, a log line or a failing test."* R1 changes
which providers get called, in what order, and which items appear in the menu.
It is the single change on this branch most likely to be wrong in a way only a
real Explorer can show.

The `render.*` harness scenarios through the per-user CLSID override do read a
live composed menu back through MSAA (239×820, 33 items, all four agreeing) (m),
which covers *painting*. They do not cover the provider set, because the harness
menu is not an Explorer file menu with 37 registered handlers.

**Recommended, and it is the highest-value remaining action:** deploy
(`scripts\backup-and-upgrade.ps1`, with the user's go-ahead), right-click a file
in Explorer a dozen times, and run `shell.exe -report perf:all`. Three things to
read off it: whether any session still shows `deferred(budget)`; whether
`explorer.commands.over_budget` ever appears; and whether the per-provider list
still contains the four cheap handlers in the sessions where it used to lose
them. That also exercises the version-8 export end to end from the writing side,
which G11 could only test from the refusing side.

---

## 4. Things that are right and worth not losing

Five pieces of this pass are better than the brief required, and a future reader
should not undo them:

- **`shared/DetourEnlistment.h`** is the best-engineered file in the change. It
  takes every Win32 and Detours call through an injected table so all four
  failure shapes are exercised by 11 tests without a process to break; it
  distinguishes the one `OpenThread` failure it is entitled to ignore (a thread
  that ended during the snapshot race) from the ones it is not, and proves the
  distinction from the documented lifetime of a thread identifier rather than
  from an undocumented `GetLastError` value; and it fails **open**, which is the
  branch's existing contract everywhere else. The comment explaining why the
  test seam is not a test-only affordance — *"a transaction whose failure paths
  cannot be run is a transaction whose failure paths are not known to work, and
  this one rewrites live code in Explorer"* — is the right argument.
- **`ProviderSchedule.h` states what it does not promise.** *"Not 'at most two
  providers are dropped', and not a hard bound on the menu… the honest bound is
  still one in-flight overrun."* An optimisation header that refuses to
  overclaim is rare and load-bearing.
- **The version-8 wire change was done the hard, correct way.** The layout did
  not change; only the *meaning* of result value 3 did. Bumping the version for
  that, refusing on exact match rather than treating the version as a floor, and
  stating the cost where the constant is defined, is a stricter reading of
  compatibility than most codebases apply — and G11 shows the resulting message
  is useful rather than merely correct.
- **R5's invariant rewrite chose honesty over the comfortable wording.** Three
  invariants that read stronger than the code were rewritten to say what is
  actually guaranteed, and §00.1's original thesis sentence was amended rather
  than left standing beside a weakened §06 number. The reasoning given — that a
  stated invariant which is not true is worse than a weaker one that is, because
  it is what a later session "restores" — is correct and is the argument the
  next reader will need.
- **`01-takeover-contract.md` §3-0** explains *why* item 5 stayed closed for
  three sessions while half of it was never built: the harness proved what
  Windows does with `MNS_NOTIFYBYPOS` and nothing asserted what Shell does, and
  the two are indistinguishable in a status table. That generalises well beyond
  item 5, and it is the finding this whole remediation was worth having.

---

## 5. Recommended order

1. **F1** — commit the work, sequenced as §4 of the plan specifies, with the
   contract citations in the messages. Split the whitespace hunks out (**F16**).
   Nothing else on this list survives losing the working tree.
2. **F17** — deploy and re-measure the provider set on a real Explorer. This is
   the one gate the plan itself calls the most valuable, and the one change most
   able to be silently wrong.
3. **F3 / F4 / F5 / F6** — the R3 cluster. Restore memoization or move
   resolution to the worker; fix the contradicting comment; give `PackagesCache`
   an injection seam and land the cold-query test; either implement the
   miss-driven refresh or record its decline in §0a.
4. **F2** — delete `consider()` and `ProviderVerdict`, re-point or retire the 30
   tests.
5. **F7 / F10** — restructure `ProviderQuarantineStore` so the analyzer count
   goes to zero, and land R6.4's store tests and the atomic-write decision.
6. **F9** — extend the by-position scenario to a nested submenu selection, and
   check it goes red when `native_source.menu` is forced to the root.
7. **F11** — widen invariant rule 8 to `MENU::set` and the numeric literal.
8. **F12 / F13 / F14 / F15 / F8** — the documentation and dead-code tidy-ups.

R8 remains open and correctly reported; nothing in this assessment changes it.
