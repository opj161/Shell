# 09 — Remediation plan

**Written and independently re-audited 2026-08-25 against
`refactor/takeover-master-plan` @ `450985f`** — 83 commits ahead of `main`, 26
after `6b26e61`. It merges an external audit
([`external-audit.md`](external-audit.md)) with a first-hand QA pass run on
this machine, keeps only what survived verification, and turns the remainder
into work items.

Every claim below is either (a) checked against the tree at this HEAD with the
file and line named, (b) measured on this machine with the numbers given, or
(c) quoted from the vendor's reference page with a canonical deep link. Where a
claim is none of those it says so.

---

## 0a. Implementation status (2026-08-25, after execution)

**Committed 2026-08-26 as `95ffd86`** — one commit for the whole audited
tree, for the reasons in [`12-closure-plan.md`](12-closure-plan.md) §W0.
Every gate result quoted below was re-run against that tree immediately
before the commit: 33,102 checks / 0 failures / 0 warnings on x64, and
`check-invariants: OK (10 rules, 0 deferred)`. `git diff --check main...HEAD`
is clean at `95ffd86`; the 30 hits reported earlier were an artefact of
nothing being committed, not of trailing whitespace in the tree.

This document was a plan. It has since been carried out; what follows is the
record of what landed, so a reader is not left guessing which half they are
looking at. Everything below §1 is preserved as written — the findings, the
reasoning and the citations are the evidence for the changes, and rewriting them
into the past tense would destroy the audit trail.

| Workstream | State | Where it landed |
|---|---|---|
| **R0** harness gate | **done** | `src/tests/hostprobe/Arguments.h` (pure parser), `main.cpp` refuses an empty run; `test_hostprobe_args.cpp` ×13. The three malformed invocations now exit 122/123 and a filter matching nothing exits 121 |
| **R1.1** two-phase scheduler | **done** | `Include/ProviderSchedule.h`, driven from `ExplorerCommand.cpp`; `test_provider_schedule.cpp` ×15, including the recorded 52.2 ms session's shape |
| **R1.2** truthful outcomes | **done** | `ProviderResult::DeferredSlow`/`DeferredBudget`, `Pending` now emitted; `Include/ExplorerCommandState.h` + `test_explorer_command_state.cpp` ×9; `PERF_EXPORT_VERSION` 7 → 8 |
| **R1.3** report the breach | **done** | `explorer.commands.over_budget`, annotated with the refused count, recorded only on a breach |
| **R1.4** reconcile the budgets | **done, option (b)** | §06.4 split into two numbers; §00.4a states what is and is not bounded. Option (c) — lowering `SLOW_PROVIDER_US` — remains **the maintainer's call** and was not taken |
| **R1.5** selection-array fallback | **blocked on R8** | The instrumentation the measurement needs already exists (`selection.rebuild_array`, annotated with the count). No cap was invented without the number |
| **R2** by-position replay | **done** | `HostNotification::MenuCommand` + `HostSelection`; tracking-identifier range and origin table in `ContextMenu`; `WM_MENUCOMMAND` posted from `Main.cpp`; invariant rule 8 rewritten from a lexical ban to a gate on *setting* the style |
| **R3** package unification | **done** | `CatalogSnapshot` carries packages and paths from the same scan; `PackagesCache` is a snapshot reader; `CACHE::clear()` no longer touches packages. `display_name` is the one named exception |
| **R4** `Win32MenuPresenter` | **done** | `Include/Win32MenuPresenter.h`; the five anonymous member structs named first; bodies verified byte-identical (zero removed lines) |
| **R5** invariant rewrite | **done** | `00-master-plan.md` §4 and the new §4a |
| **R6** small fixes | **done** | 6.1 watcher state · 6.2 the `MapViewOfFile` comment · 6.3 one name for one extension · 6.4 the store mutex · 6.5 two 64 KiB stack frames to the heap · 6.6 the threshold comment · 6.7 detour enlistment, with `DetourEnlistment.h` and `test_detour_enlistment.cpp` ×11 |
| **R7** documentation | **done** | Both tallies, §01.3-0 on why item 5 was missed, the line-count commits, C-4, the live measurement, harness cardinality, the invariant-script description, the Phase 1 acceptance boxes, and `git diff --check` clean |
| **R8** environment validation | **open** | Unchanged: it needs a machine or a person this one is not |

**Gate at completion:** 33,102+ checks / 0 failures on x64 and x86, arm64 builds
and packages, 0 warnings, `check-invariants: OK (10 rules, 0 deferred)`,
**23 harness scenarios native and 32 through takeover, 0 failures** (the
takeover run through the per-user CLSID override, with the `takeover:` line
naming the build under test), and `git diff --check main` clean.

Three changes were checked by re-introducing the defect and watching the named
test fail, per §4 gate 3: the watcher's dead-thread state (3 tests red), the
by-position remap (the harness scenario reports zero `WM_MENUCOMMAND`), and
invariant rule 8 (a planted `SetMenuInfo` shape is rejected).

---

## 0. How this was verified

Run on this machine on 2026-08-25 at `450985f`, first by the authoring pass and
then independently reproduced/extended where noted:

| Check | Result |
|---|---|
| `.\build.ps1 -Platform x64` | **32,837 checks / 0 failures**, 0 warnings |
| `.\build.ps1 -Platform x86` | **32,837 checks / 0 failures**, 0 warnings |
| `.\build.ps1 -Platform arm64` | builds and packages, 0 warnings (tests skipped — host is x64) |
| `check-invariants.ps1` after the multi-platform build | **OK (10 rules, 0 deferred)** |
| `hostprobe.exe --verify src\tests\hostprobe\fixtures` | **23 scenarios, 0 failures** (9 skipped — need `--takeover`) |
| per-user COM override + `hostprobe.exe --takeover --verify src\tests\hostprobe\fixtures` | **`kTakeoverScenarios` scenarios, 0 failures** (32 when audited; 33 since W6.3), with the `takeover:` line naming the `src\bin\x64\shell.dll` under test |
| `shell.exe -report perf` against the live `explorer.exe` | 70 menus, 16 held — see §2.1 |
| `-check`, `-quarantine:list`, `-favorites:list` | all behave as documented, including the `app.dir` 9-vs-10-files nuance of [§03.1b](03-config-safety.md) |
| `validate-msi-lifecycle.ps1` over x64/x86/ARM64 | **all three OK**; emitted Component, Registry, CustomAction, RemoveFile and sequence invariants hold |
| MSVC `/analyze`, production x64 projects | manager and hostprobe clean; DLL: 4 lock-analysis warnings plus 2 real ~64 KiB stack frames; custom action repeats one stack warning — see R6.5 |
| `git diff --check main...HEAD` | **fails** on pre-existing trailing whitespace in the presenter split and one blank line at EOF — see R7 |

No tracked production code was changed while producing this plan. The audit
documents themselves are untracked at this point, so “working tree clean” would
not be a reproducible statement.

**Harness invocation warning found by the independent pass.** `--verify` takes a
mandatory directory argument. Running the abbreviated commands previously shown
in this table produces **`0 scenario(s), 0 failure(s)`** and exits 0. The claimed
23/31 counts therefore did not come from the commands as recorded — and were
also the wrong numbers: the harness was 23/32 at that point (9 skipped
natively, not 8), and the table above now names the constants instead of a
number. The rerun used the full invocation and verified
that the loaded DLL was this HEAD. R0 makes that class of false green
impossible, and W6.2 makes the counts themselves enforced rather than merely
recorded: they now live in `src/tests/hostprobe/Scenarios.h` as
`kNativeScenarios` and `kTakeoverScenarios`, and an unfiltered run that does
not hit them exits 120.

> **The counts moved again, and this is the last time this document repeats
> them.** W6.3 added `takeover.a_nested_by_position_selection_names_its_submenu`,
> so the table is **23 native / 33 takeover**, with **10** skipped natively.
> Every number in this file that named a cardinality has been replaced by a
> reference to `kNativeScenarios` / `kTakeoverScenarios`
> ([`Scenarios.h`](../../src/tests/hostprobe/Scenarios.h)), because a count
> written into prose drifts away from the thing it counts and the drift is
> invisible precisely because the run still passes — which is exactly what
> happened here twice: 23/31 recorded against a 23/32 harness, then 23/32
> recorded against a 23/33 one, by the very commit that added the scenario.

**So the ordinary build and behavioral suites are green, but the QA surface is
not itself green yet.** Everything below is a gap between what the documents
guarantee and what the code does, a defect no existing test was shaped to see,
or a validation gate that can report success without exercising its subject.

---

## 1. Verdict on the external audit

### 1.1 Confirmed, verbatim

Each of these was re-checked at this HEAD and is correct as stated.

| # | External claim | Verified at |
|---|---|---|
| E-1 | `Cache.h` still owns `PackagesCache Packages`, and `clear()` clears it | [Cache.h:251](../../src/dll/src/Include/Cache.h#L251), [Cache.h:312](../../src/dll/src/Include/Cache.h#L312) |
| E-2 | `PackageIndex::ensure_index()` can synchronously enumerate | [Packages.h:174](../../src/dll/src/Include/Packages.h#L174), [Packages.cpp](../../src/dll/src/Packages.cpp) `ensure_index` — true at that HEAD; `PackageIndex` has since been deleted (D-01), so both anchors are past end-of-file |
| E-3 | `FuncExpression.cpp` still reaches `cache->Packages` | [FuncExpression.cpp:231](../../src/dll/src/Expression/FuncExpression.cpp#L231) |
| E-4 | Provider `GetState`/`GetTitle`/`GetFlags`/`GetIcon` remain synchronous on the menu thread | [ExplorerCommand.cpp:157](../../src/dll/src/ExplorerCommand.cpp#L157) onward |
| E-5 | CoCI detour still attaches on `if(rt.loader.explorer)`, no `TakeoverRouter` | [Main.cpp:1819](../../src/dll/src/Main.cpp#L1819) |
| E-6 | Taskbar keeps `BUDGET_MS = 250` and `CoWaitForMultipleHandles` | [Main.cpp:342](../../src/dll/src/Main.cpp#L342), [Main.cpp:400](../../src/dll/src/Main.cpp#L400) |
| E-7 | `SPI_SETMENUSHOWDELAY` and `SPI_SETSELECTIONFADE` mutations remain; `SPIF_SENDCHANGE` is gone | [ContextMenu.cpp:2799](../../src/dll/src/ContextMenu.cpp#L2799), [:4341](../../src/dll/src/ContextMenu.cpp#L4341), [:5673](../../src/dll/src/ContextMenu.cpp#L5673), [:5675](../../src/dll/src/ContextMenu.cpp#L5675) |
| E-8 | No `Win32MenuPresenter` class; the extracted functions are still `ContextMenu::` members | [MenuPresenter.cpp](../../src/dll/src/MenuPresenter.cpp) — 8 functions, all `ContextMenu::` |
| E-9 | Line counts 6,110 / 1,652 | `wc -l` at this HEAD |
| E-10 | 59 `test_*.cpp`, 678 `TEST(...)` | counted at this HEAD |
| E-11 | HEAD `450985f`, 26 after `6b26e61`, 83 ahead of `main` | `git rev-list --count` |

### 1.2 Corrected or re-scoped

**C-1 — Item 5 (HostContract / TPM normalization) is *not* Done.** The audit
marks it complete and lists `MNS_NOTIFYBYPOS` under harness coverage. The harness
covers what *Windows* does with that style; it does not cover what *Shell* does.
[§01.3](01-takeover-contract.md)'s replay table requires
`PostMessage(owner, WM_MENUCOMMAND, position, borrowed_HMENU)` when the borrowed
root carried the style. [`HostContract.h`](../../src/dll/src/Include/HostContract.h)
has only `HostNotification::Command`; no shipping path requests `MIM_STYLE` from
the borrowed root (the generic `MenuItem.h` wrapper is not that path).
See finding **A** in §2 and workstream **R2**.

**C-2 — the package gap is real but smaller, and its blast radius is larger.**
The audit implies manifest reads survive on the menu thread. They do not:
`RegistryPackageSource::enumerate_full_names`
([Packages.cpp:176](../../src/dll/src/Packages.cpp#L176)) reads *subkey names
only*, and its own comment records why — "opening every package key and reading
every value is what used to make this scan expensive". [§06](06-phases-and-tests.md)
measured the registry half at **2 ms** against 111.6 ms for the manifest scan
that was removed. So the residual is a bounded ~2 ms, not a manifest walk.

Larger, though, in *who pays it*: the audit treats this as a power-user path. The
**stock configuration** takes it on every menu —
[`src/bin/imports/terminal.nss:8`](../../src/bin/imports/terminal.nss#L8) is
`where=package.exists("WindowsTerminal")`, with `package.path(...)` on line 12.
With a 30 s TTL ([Packages.h:143](../../src/dll/src/Include/Packages.h#L143)) and
`CACHE::clear()` discarding the index on every config reload, a default install
re-enumerates on the menu thread at least twice a minute of active use, and once
after every save the config watcher picks up.

**C-3 — first-paint boundedness: the audit diagnoses the wrong half.** It frames
the gap as "a provider may freeze one first paint, after which Shell learns to
defer it", and calls that an accepted product limitation. That is true and it is
accepted. It is not what is actually missing the budget. **The routine cost is,
not the pathological one** — measured below. See findings **B**/**C** and **R1**.

**C-4 — config safety: one "untested" criterion is partly tested.**
[§03.5](03-config-safety.md) and [§3.9](08-handoff.md) file "a shadow whose
manifest fails verification is refused" under *needs a machine state this one is
not in*. The refusal is unit-tested four ways —
`test_config_shadow.cpp`: `a_shadow_whose_content_changed_is_refused`,
`a_shadow_missing_a_file_is_refused`, `a_shadow_with_no_manifest_is_refused`,
`a_manifest_this_build_does_not_understand_is_refused`. Only the *end-to-end*
route (a fresh process falling back through `Initializer::init`) is untested.

**C-5 — the audit could not run the binaries; this pass did.** Its §11 says so
explicitly. §0 above supplies the missing half: three platforms green, both
harness modes clean, and a live Explorer read.

**C-6 — percentages.** Not adopted or disputed. A percentage cannot be acted on;
§2's register can. The audit's substantive conclusion — *late stage, no
workstream unstarted, remaining divergence concentrated rather than broad* — is
consistent with what this pass found, with the one correction in C-1.

**C-7 — the later independent pass found that R1, R2 and R3 themselves needed
remediation.** The first version of this document correctly found the three
areas, but its proposed remedies did not yet close them:

- R1 admitted providers by predicted cost and then still *called* them in
  registration order. A live overrun therefore still makes the tail pay, and
  the claimed transformation of the observed 52.2 ms session does not follow
  from the algorithm. See **M** and the rewritten R1.1.
- R2 preserved a native item's original position but did not give zero-ID or
  duplicate-ID native items a unique internal tracking identity. With
  `TPM_RETURNCMD`, Shell still cannot tell which such item was selected. See
  **N** and the rewritten R2.
- R3 called `package.path` and `appx.name` “single-key registry reads.” The
  former calls `GetPackagePathByFullName`; the latter can call
  `SHLoadIndirectString` and enumerate MrtCache. See **O** and the rewritten R3.

### 1.3 Exact implementation progress against the twenty-item backlog

This is the strict implementation view, not the more generous “built or
measured-and-declined means closed” project tally. A redesigned or declined item
is a valid outcome, but it is not counted as literal implementation of the
original item.

| # | Strict state at `450985f` | Evidence / remainder |
|---|---|---|
| 1 | **Done** | expression fixes and dead-code deletion are built and pinned |
| 2 | **Partial** | async ExplorerCommand catalog landed; NSS package index remains config-owned and synchronous; persistence declined |
| 3 | **Done, diagnostics gap** | `GetState(TRUE)` retry removed; non-`E_PENDING` failure and pending telemetry still wrong (P) |
| 4 | **Done** | borrowed INIT/UNINIT pairing, real-menu coverage |
| 5 | **Partial** | return/notification normalization landed; by-position replay did not (A/N) |
| 6 | **Done, environment gates open** | persisted LKG and real restart verified; two fresh-state routes remain in R8 |
| 7 | **Resolved differently** | `TakeoverSession` class declined under C2712; plain-data consolidation landed |
| 8 | **Resolved differently** | backend interface/entry health check declined; live mechanism is reported |
| 9 | **Partial** | compiled CoCI fast path and TreatAs de-dup landed; conditional attach deferred |
| 10 | **Resolved differently** | zero-wait withdrawn after it suppressed the first menu; cached UIA request + bounded COM-modal wait landed |
| 11 | **Partial** | Recycle Bin removal and flicker measurement landed; two transient SPI mutations remain |
| 12 | **Done** | always-on ring + cross-process export |
| 13 | **Done** | breaker and one-shot bypass, including decline/failure separation |
| 14 | **Done, policy gaps open** | provider health, quarantine and Reliability Center landed; R1/P refine admission and reporting |
| 15 | **Done / measured alternative** | accessibility proved through the real MSAA surface; mnemonics, type-ahead and columns landed |
| 16 | **Done, small state bug open** | repeated watcher repoint fixed; dead-thread `watching()` state remains H |
| 17 | **Partial** | model, selection seam and presenter translation-unit split landed; named presenter boundary remains R4 |
| 18 | **Done** | targeted `moveto`, measured 85.5 ms → 20.2 ms |
| 19 | **Done** | stable identity, favorites, provenance and inspector; live `where=` deliberately declined |
| 20 | **Resolved differently** | icon cache, memoization and lazy selection all declined against recorded measurements |

Strict count: **11 implemented, 5 partial, 4 deliberately resolved differently,
0 untouched**. Against the branch's revised outcomes, the work is late-stage;
against the literal original architecture it is roughly **80–85%**, with the
remaining risk concentrated in host replay, first-paint policy, package
ownership and the final presenter boundary rather than spread across the tree.

---

## 2. Consolidated finding register

Severity is *what a user or a maintainer loses*, not effort.

| ID | Severity | Source | Finding |
|---|---|---|---|
| **A** | High | this pass | By-position host replay was never built; item 5 is tallied closed |
| **B** | High | this pass | The first-paint budget is missed ~2.5× in steady state and nothing reports it |
| **C** | High | this pass | Budget exhaustion silently removes *healthy* items, in registration order |
| **D** | Medium | this pass | The report cannot distinguish "slow" from "out of budget" |
| **E** | Medium | external | Package index still lives in the config CACHE and can enumerate on the menu thread |
| **F** | Medium | external | No `Win32MenuPresenter` boundary; the extraction stopped at the file split |
| **G** | Medium | both | Master-plan invariants 1, 2 and 7 describe guarantees the code does not provide |
| **H** | Low | this pass | A dead config watcher still reports itself alive |
| **I** | Low | this pass | `PerfExportWriter::open` claims a size check it does not make |
| **J** | Low | this pass | `-report perf` and the Reliability window disagree about a provider's name |
| **K** | Low | both | Documentation drift: item 5 tally, stale line counts, C-4 |
| **L** | — | external | Item 9 conditional attach; item 7/8 resolved-by-redesign — no action, see §5 |
| **M** | High | independent QA | R1.1's predicted-cost admission still executes in registration order and does not fix the observed tail starvation |
| **N** | High | independent QA | R2 cannot identify zero-ID or duplicate-ID native selections; its new `MNS_NOTIFYBYPOS` read would also fail invariant rule 8 as written |
| **O** | High | independent QA | R3 leaves AppModel/PRI/MrtCache work synchronous while claiming package menu-path I/O is closed |
| **P** | Medium | independent QA | failed `GetState(FALSE)` is treated as usable; `E_PENDING` is never recorded as `Pending` |
| **Q** | Medium | independent QA | a third-party host with paths but no `IShellItemArray` still does one `SHParseDisplayName` per item before first paint |
| **R** | Medium | independent QA | hostprobe accepts missing option operands and zero selected scenarios as a successful run |
| **S** | Medium | independent QA | two production helpers put ~64 KiB buffers on an arbitrary host thread's stack; store wrappers lack direct integration tests |
| **T** | Low | independent QA | `git diff --check` fails; several acceptance checkboxes and invariant-script descriptions are stale |
| **U** | High | independent QA + Detours docs | inline-detour setup commits even when thread enumeration/enlistment failed, contrary to its own safety premise |

### 2.1 The measurement behind B, C and D

`shell.exe -report perf:all` against the Explorer running this branch's deployed
build (pid 19132, 70 menus, 16 held in the ring):

```text
pre-display  p50 37.4 ms   p95 52.2 ms   n=16
```

All sixteen `popup.total_pre_display` readings, sorted:

```text
13.9  36.0 36.9 37.1 37.2 37.2 37.4 37.4 37.7 38.1 38.1 38.6 40.6  46.8 47.2 52.2
```

A representative warm session:

```text
explorer.commands                    36.6 ms  n=37
popup.total_pre_display              37.7 ms
menu.flicker_wait                     8.5 ms  n=1
menu.flicker_wait                     3.3 ms  n=1
provider {7A53B94A-…} 13.9 ms ok  Create with Designer
provider {BFE0E2A4-…}  6.7 ms ok  Edit with Photos
provider {ED215C26-…}  3.4 ms ok  Ask Copilot
…seventeen more, 0.1–1.7 ms each
```

Three things follow, and none of them is a regression in Shell's own code:

1. **Shell's own pre-paint work is ~1.1 ms** (37.7 − 36.6). The first-paint work
   of this branch did what it set out to do.
2. **Third-party provider work is 36.6 ms of a 37.7 ms menu**, and the provider
   records sum to ≈35 ms of that — so the phase is honestly attributed. This is
   the number that misses [§06.4](06-phases-and-tests.md)'s *"pre-display added
   by Shell ≤ 15 ms p95"*.

   **Two corrections to this reading, both from the closure pass.** The ~1.6 ms
   gap between the phase and the records was not all Shell's own work:
   `GetCanonicalName` was called on the shown path *after* each provider's cost
   had been taken, so every provider's record excluded it while the whole-menu
   phase charged it. W3 moves it inside the measured span, which makes "the
   phase is honestly attributed" mean what it says.

   And the provider list quoted above was read from a **truncating** record
   set. `MAX_PROVIDERS` and `PERF_EXPORT_PROVIDERS` are both 32 against 37
   handlers in this menu — and 55 distinct packaged context-menu CLSIDs on
   this machine, counted 2026-08-26 — so the records were a sample, not a
   census, and which providers survived it was decided by write order. W5
   fixes that order; `dropped_providers` is the field that says when it
   bit.
3. **The policy is designed not to intervene here.**
   [ProviderHealth.h:142](../../src/dll/src/Include/ProviderHealth.h#L142):
   *"`MENU_BUDGET_US` is set just above the measured steady-state total with
   reuse (~41 ms), so in normal operation it never bites."* With
   `SLOW_PROVIDER_US = 25000` ([:173](../../src/dll/src/Include/ProviderHealth.h#L173)),
   a single provider may cost **1.7× the entire first-paint budget** before it is
   ever called slow, and `MENU_BUDGET_US = 50000` ([:174](../../src/dll/src/Include/ProviderHealth.h#L174))
   is 3.3× it.

So §06.4 and `ProviderHealth.h` state incompatible targets, and the code follows
the header. **Finding B is that disagreement, not a bug.**

**Finding C is what happens when the budget does bite.** One of the sixteen
sessions — 52.2 ms — deferred six providers:

```text
provider {8F491918-…} 0.0 ms deferred  Resize with Image Resizer     (0.4 ms warm)
provider {1861E28B-…} 0.0 ms deferred  Rename with PowerRename       (1.6 ms warm)
provider {1C6DF0C0-…} 0.0 ms deferred  Open w&ith Code               (1.3 ms warm)
provider {BFE0E2A4-…} 0.0 ms deferred  Edit with Photos              (6.7 ms warm)
provider {7A53B94A-…} 0.0 ms deferred  Create with Designer         (13.9 ms warm)
provider {CA6CC9F1-…} 0.0 ms deferred  Edit in Notepad               (0.3 ms warm)
```

The budget is spent in registration order
([ExplorerCommand.cpp:435](../../src/dll/src/ExplorerCommand.cpp#L435),
[:479](../../src/dll/src/ExplorerCommand.cpp#L479)), so four providers that cost
**0.3–1.6 ms** lost their place in the menu because they sit *after* one that
costs 13.9 ms. Six items vanished from that right-click and returned on the next,
with nothing marking the omission. [§02.2a](02-first-paint-latency.md)'s story is
"a provider that misses its deadline is omitted"; what was observed is the
opposite — providers that miss nothing are omitted for their position.

**Finding D** is why C is nearly undiagnosable from the report.
`ProviderVerdict` distinguishes `DeferSlow` from `DeferBudget`
([ProviderHealth.h:67-72](../../src/dll/src/Include/ProviderHealth.h#L67)), and
[ExplorerCommand.cpp:486](../../src/dll/src/ExplorerCommand.cpp#L486) collapses
both into one `ProviderResult::Deferred`. The two call for opposite actions —
quarantine the handler, versus raise the budget — and the report cannot tell them
apart. This is the same family as the `dropped_providers` counter
[§02.3a](02-first-paint-latency.md) records as having once been carried through
the export and printed by nobody. That older defect is fixed — the current
report prints telemetry overflow — but overflow and budget refusal remain
different facts.

---

## 3. The plan

Nine workstreams. Each states the contract it is answering to, the exact sites,
the tests that must catch its defect, and what "done" means. Ordered by
dependency in §4, not by the numbering here.

---

### R0 — Make the QA gate capable of failing

*Answers finding R. This lands before any product change.*

`hostprobe`'s option parser treats `--verify`, `--record` and `--shell` as an
ordinary substring filter when their operand is missing. It also returns success
when the filter selects zero scenarios. The exact abbreviated commands in the
first version of §0 therefore printed `0 scenario(s), 0 failure(s)` and exited 0.

Fix in `src/tests/hostprobe/main.cpp`:

1. A recognized option missing its operand prints `FAIL` plus usage and exits
   nonzero. Do not let it fall through to `filter`.
2. An unknown `--word` is an option error, not a filter. Plain words remain
   substring filters for the documented `hostprobe question` workflow.
3. `ran == 0` is a failure. A typo that exercises nothing is not a passing
   focused run.
4. Print the fixture directory in the summary when verification is active, and
   keep printing the loaded DLL path in takeover mode.
5. Put the argument parser in a pure helper and unit-test the missing-operand,
   unknown-option, zero-match, native-verify and takeover-verify shapes.

The canonical commands are:

```powershell
src\bin\x64\hostprobe.exe --verify src\tests\hostprobe\fixtures
src\bin\x64\hostprobe.exe --takeover --verify src\tests\hostprobe\fixtures
```

For the second, the `takeover:` line must name the built DLL under test. When
shell-namespace scenarios are required, use the per-user COM override recipe in
[§08.3.7](08-handoff.md); `--shell` alone cannot redirect those activations.

**Acceptance:** both malformed invocations used in the independent pass fail;
the canonical commands run exactly the counts declared by `kNativeScenarios`
and `kTakeoverScenarios` in
[`Scenarios.h`](../../src/tests/hostprobe/Scenarios.h) — natively, that many
scenarios with the remainder skipped; through takeover, the whole table — and
the harness asserts that itself, exiting 120 when an unfiltered run misses,
rather than leaving it to a reader (W6.2); a deliberately mismatched fixture
still identifies its scenario and fails.

---

### R1 — Provider admission, and reconciling the first-paint budget

*Answers findings B, C, D. Largest user-visible item on the list.*

#### The contract

`IExplorerCommand`'s remarks:

> None of the methods of this interface should communicate with network
> resources. **These methods are called on the UI thread**, so communication with
> network resources could cause the UI to stop responding.
> — <https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorercommand>

This is load-bearing in **both** directions and it settles the design question
[§02.2a-i](02-first-paint-latency.md) already decided once:

- Calling these on the menu thread is the documented environment. Moving them to
  a worker remains declined — the divergence cannot be tested here.
- The same sentence makes the handler's speed a *contract obligation*. A handler
  costing 13.9 ms on every menu is not honouring it, and Shell is entitled to
  budget against it.

`GetState`'s `fOkToBeSlow = FALSE` already asks for the fast path
([ExplorerCommand.cpp:157](../../src/dll/src/ExplorerCommand.cpp#L157) onward),
and [§02.2](02-first-paint-latency.md) is unchanged: never `GetState(TRUE)`
before first paint (enforced by `check-invariants` rule 10).

#### R1.1 Separate resolution order from display order (fixes C/M)

Today is one pass in registration order. The first version of this remediation
proposed a cost-ordered *admission* pass and then still performed the calls in
registration order. That does not fix the measured failure: if predicted costs
sum under 50 ms but one provider runs above its prediction, the live backstop
still fires before the same tail and the same cheap items disappear.

Build a two-phase scheduler in `ContextMenu::append_explorer_commands`
([ExplorerCommand.cpp:435](../../src/dll/src/ExplorerCommand.cpp#L435)–[:560](../../src/dll/src/ExplorerCommand.cpp#L560)):

1. **Plan without activation.** Collect every matching, non-duplicate,
   non-quarantined registration as `{registration ordinal, hash, timing,
   verdict}`. A timing lookup must not mutate the re-probe counters; deciding
   that a slow provider's probe is due remains an explicit policy operation.
2. **Resolve known healthy providers cheapest first.** Use a recent conservative
   estimate (at minimum `max(best_us, last_us)`, not `best_us` alone) to order
   calls. Skip a provider whose estimate cannot fit, but continue looking for a
   cheaper one rather than ending the pass. A live budget check remains before
   every call; one call can still overrun because it cannot be interrupted.
3. **Exploration is fair and bounded.** Providers with fewer than
   `MIN_SAMPLES_TO_JUDGE` must still be sampled, but not always from the head of
   registration order. Keep a rotating cursor and reserve at least one
   exploration opportunity while budget remains. Otherwise an expensive cold
   head can starve an unknown tail forever.
4. **Slow re-probes run last.** A 200-menu re-probe must not evict known healthy
   items from the same menu. Preserve the existing rule that exclusion is not a
   life sentence, but charge the probe only after ordinary affordable work.
5. **Buffer results, then publish in registration order.** Resolution order is
   not presentation order. Run canonical-name/title duplicate resolution in
   original ordinal order too, so a cheaper later provider cannot steal the
   first-registration-wins identity from an earlier one. The displayed order and
   duplicate winner stay exactly as today.

This gives the observed 52.2 ms session the guarantee it actually needs: the four
0.3–1.6 ms providers are resolved before a 13.9 ms provider can overrun. It does
**not** promise that exactly two providers are dropped; that claim was not
derivable from the recorded data. The honest hard bound remains “at most one
in-flight provider overrun.”

**Tests:** extract the scheduler as pure data and cover all of these:

- `{20 ms, 1, 1, 1, 1}` under 15 ms resolves the four cheap entries;
- predicted total fits, one early-registration provider spikes at runtime, and
  the cheap tail is already resolved;
- output order and duplicate winner remain registration order;
- unknown providers rotate rather than starving;
- a due slow re-probe cannot displace known healthy work.

#### R1.2 Make every provider outcome truthful (fixes D/P)

Add `ProviderResult::DeferredBudget` beside `Deferred` (rename the existing to
`DeferredSlow` in the same commit for symmetry) and record the verdict the
policy actually returned at
[ExplorerCommand.cpp:486](../../src/dll/src/ExplorerCommand.cpp#L486).

Fix the state-policy gap in the same outcome pipeline:

- `GetState(FALSE) == E_PENDING` keeps the provisional enabled item but returns
  a `Pending` outcome. `ProviderResult::Pending` already exists and is currently
  emitted nowhere.
- any other failed `GetState(FALSE)` omits the item and returns `Failed`, exactly
  as [§02.2](02-first-paint-latency.md) specifies. Today the code continues to
  `GetTitle` and can display it enabled, then records the provider as successful.
- `Hidden` remains a successful provider declining this selection; it must not
  evict the cached COM object.

This is a wire change: bump `PERF_EXPORT_VERSION` 7 → 8
([PerfExport.h:122](../../src/shared/PerfExport.h#L122)). The reader refuses a
version it does not recognise
([PerfExport.h:1119](../../src/shared/PerfExport.h#L1119)) — deliberate, and the
consequence must be stated in the commit: a rebuilt `shell.exe` reads nothing
from an Explorer still running the previous DLL until Explorer restarts. That is
the existing contract, not a new cost.

The Reliability Center must not present `DeferredBudget` as a provider fault or
offer quarantine as its remedy. Aggregate slow deferrals separately from budget
deferrals; the detail text and the list must use the same stripped name and the
same verdict vocabulary.

**Tests:** factor the `GetState` result classification into a pure helper and pin
S_OK enabled/hidden/disabled, E_PENDING and failure. Then carry every outcome
through the ring, shared memory, report and Reliability aggregation distinctly in
`test_perf_export.cpp`; assert the version bump against the semantic layout, the
same way version 2 was. Reintroducing today's failed-state fall-through must fail
its named test.

#### R1.3 Report the breach

Nothing currently summarizes "this menu did not fit its budget". Add one phase,
recorded only when at least one provider was refused for budget —
`explorer.commands.over_budget`, annotated with the count. The per-provider
`DeferredBudget` records remain the detail; the phase is the summary. An always-present
phase would be noise; a phase that appears only on a breach is a signal, and
[§4 of the handoff](08-handoff.md) already records the rule this follows:
*"when adding a cap, print its overflow in the same commit"*.

Do not confuse this with `dropped_providers`: that counter means the fixed-size
telemetry array overflowed, and the current report already prints it. It does not
mean a provider was refused by the menu budget.

#### R1.4 Reconcile the two budgets — **one decision is the user's**

§06.4 says *pre-display added by Shell ≤ 15 ms p95*.
`ProviderHealth.h` is tuned so the cap "never bites" at ~41 ms. Both cannot
stand. Three options, with what each costs on this machine:

| | What it does | Cost here |
|---|---|---|
| (a) Enforce 15 ms literally — `MENU_BUDGET_US ≈ 12000` | The budget bites on **every** menu | The recorded provider set cannot fit; the exact retained count must be re-measured with the R1.1 scheduler rather than inferred |
| (b) **Split the budget and restate it** (recommended) | §06.4 becomes two numbers: *Shell's own* pre-paint work ≤ 15 ms p95 — today **~1.1 ms**, comfortably met — and third-party provider work as a separate, reported budget with its own target | No behaviour change; the document stops claiming something the design deliberately does not do |
| (c) Lower `SLOW_PROVIDER_US` from 25 ms | Condemns the routine outlier rather than only the pathological one | At 10 ms, *Create with Designer* (13.9 ms) is deferred after its second sample → ~23 ms of provider work, ~24 ms pre-display. It also **permanently removes a menu item** the user has today, until the 200-menu re-probe |

(b) is the honest description and should land regardless, but it is not only a
budget edit: it explicitly retires the master plan's stronger claim that no
unbounded third-party call runs before first paint. The UI-thread contract means
one in-flight handler can still block indefinitely, and arbitrary user NSS
expressions can also perform work outside an engine SLO. Rewrite the product
thesis, R1/R1a and the Phase 1 acceptance criteria together; do not leave a hard
guarantee in §00 while weakening only §06's number.

(c) is a product
decision — it trades a real menu entry for ~14 ms — and per this repository's own
rules it must be **measured before landing** and surfaced in the Reliability
Center. The detail report currently names a provider record `deferred`; the
Reliability list itself does not expose a separate deferred tier, so R1.2 must
land before the UI can explain this choice. *This plan does not assume (c); it
needs the maintainer's call.*

#### R1.5 Bound the selection-array fallback (finding Q)

`ensure_selection_array()` still calls `SHParseDisplayName` once per selected
path when a host supplied paths but no retained `IShellItemArray`
([ExplorerCommand.cpp:242-321](../../src/dll/src/ExplorerCommand.cpp#L242)). The
Explorer common path was fixed (645 ms → 30 ms), but a third-party host can still
take the old O(selection count) path before provider admission even begins.

First run the R8 third-party large-selection case. If it reproduces, do not hide
it behind the provider budget—the budget starts later. Either preserve/build the
array in the selection provider that owns the host data, or establish an
explicit count/time cap that omits modern providers for that menu and reports
`selection.rebuild_deferred`. Any worker design must marshal the resulting COM
interface back to the menu apartment; a raw `IShellItemArray*` cannot cross
apartments.

**Test:** a fake selection provider with 200 paths and no array must hit the
chosen bounded outcome without making 200 parser calls; the ordinary retained-
array path must make none.

**Acceptance for R1:** the observed 52.2 ms session's four cheap providers are no
longer dropped; the report distinguishes the two deferral reasons; a budget
breach appears as a phase; failed/pending state is truthful; the fallback has a
measured disposition; §00 and §06 state a target the implementation actually
holds itself to.

---

### R2 — By-position host replay (`MNS_NOTIFYBYPOS`)

*Answers finding A. Closes the half of backlog item 5 that was never built.*

#### The contract

> **MNS_NOTIFYBYPOS** … Menu owner receives a **WM_MENUCOMMAND** message instead
> of a **WM_COMMAND** message when the user makes a selection. **MNS_NOTIFYBYPOS**
> is a menu header style and has no effect when applied to individual sub menus.
> — <https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-menuinfo>

> *wParam* — The zero-based index of the item selected.
> *lParam* — A handle to the menu for the item selected. …
> The **WM_MENUCOMMAND** message is sent instead of **WM_COMMAND** only for menus
> that are defined with the **MNS_NOTIFYBYPOS** flag set in the **dwStyle** member
> of the **MENUINFO** structure.
> — <https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menucommand>

Measured, and already committed as a fixture:
`src/tests/hostprobe/fixtures/question.notifybypos_reports_a_position.trace:9`
records `WM_MENUCOMMAND position=1 menu=menu#1`, posted after the tracking call
returned. The behaviour Shell must reproduce is therefore already pinned.

#### What is wrong today

For a host that (i) does not pass `TPM_RETURNCMD` and (ii) set
`MNS_NOTIFYBYPOS` on the menu it handed Shell:

- `complete_host_contract` can only answer `HostNotification::Command`
  ([HostContract.h:87-91](../../src/dll/src/Include/HostContract.h#L87)), so the
  hook posts `WM_COMMAND` at
  [Main.cpp:1408](../../src/dll/src/Main.cpp#L1408);
- a by-position host has no reason to give its items meaningful identifiers, so
  `unhandled` is most likely 0, and `complete_host_contract` returns with
  `notify = None` — **the selection is dropped entirely**;
- no shipping call requests `MIM_STYLE` from the borrowed root, so the style is
  never read (`MenuItem.h` has a generic `GetMenuInfo` wrapper, but this path
  does not use it).

Merely storing the original position does not solve the first bullet. Shell
tracks its recomposed menu with `TPM_RETURNCMD`, so a native item whose original
`wID` is 0 still returns 0 — indistinguishable from cancel — and duplicate IDs
still identify only the first match. R2 must separate **internal tracking
identity** from **host replay identity**.

#### Design

1. **Record the host identity.** `ContextMenu::enumerate_native_menu_level`
   ([ContextMenu.cpp:3703](../../src/dll/src/ContextMenu.cpp#L3703)) already walks
   the borrowed menu by index; the loop variable `i` *is* the position and is
   simply discarded. Store `{original_wID, position, containing_borrowed_HMENU}`
   on the mirrored item. Both position and menu are needed: `lParam` is the menu
   that owns the item, not the root.
2. **Give by-position items unique internal IDs.** When the borrowed root is
   by-position, every commandable mirrored native item gets an ID from a range
   reserved for tracking. The origin table maps that ID back to the tuple from
   step 1. This is required even when the original ID is nonzero: duplicate IDs
   are valid in a position-addressed menu and must remain distinguishable. These
   internal IDs never leave Shell.
3. **Read the style once**, on the borrowed root, before composition:
   `MENUINFO mi{ sizeof(mi) }; mi.fMask = MIM_STYLE; GetMenuInfo(hMenu, &mi);`
   → `by_position = (mi.dwStyle & MNS_NOTIFYBYPOS) != 0`. Header style only, per
   the MENUINFO quote — one read, not per level. Check the return value; on
   failure record the Win32 error and use the ordinary ID contract rather than
   reading an uninitialized style.
4. **Extend the completion.** Add `HostNotification::MenuCommand` with
   `notify_position` and `notify_menu`. Pass the extra inputs as a small struct
   rather than growing `complete_host_contract` to seven parameters; the function
   stays pure arithmetic over flags, which is what makes
   `test_host_contract.cpp` possible.
5. **Post it**, next to the existing call:
   `PostMessageW(owner, WM_MENUCOMMAND, position, (LPARAM)containing_hmenu)` —
   posted, not sent, for the reason [§01.3](01-takeover-contract.md) reversed and
   the traces confirm.
6. **The existing guards stay.** A synthetic identifier must never reach a host
   (`is_synthetic_id`), and Shell's own composed menu must never carry
   `MNS_NOTIFYBYPOS` — the style plus `TPM_RETURNCMD` loses the selection
   outright (`question.notifybypos_with_returncmd.trace`), and
   `check-invariants` rule 8 already enforces it. R2 changes nothing about
   Shell's own menu; it only replays to the host's.
7. **Change invariant rule 8 in the same commit.** Today it rejects the token
   `MNS_NOTIFYBYPOS` anywhere in `src/dll`; the required read in step 3 would
   therefore fail the build. Replace the lexical ban with a gate that forbids
   setting/applying the style on Shell's composed menu while permitting a
   read/test of the borrowed root. Prove the gate by temporarily adding the
   forbidden `SetMenuInfo` shape.

#### Tests

- `test_host_contract.cpp`: a by-position host gets `MenuCommand` with the right
  position and never `Command`; a `TPM_RETURNCMD` host is unaffected; a
  Shell-owned item still notifies nothing under either style.
- New harness scenario `takeover.a_by_position_host_is_told_which_position`,
  built on `ShellMenu.h` with `SetMenuInfo(MNS_NOTIFYBYPOS)` on the borrowed root
  — asserting exactly one `WM_MENUCOMMAND` at the right index and zero
  `WM_COMMAND`. It must sit with the other `takeover.*` cases, last, for the
  ordering reason [§01.7a](01-takeover-contract.md) records.
- Give that harness menu **zero and duplicate IDs**, and select both the root and
  a nested submenu item. A scenario with unique 500x IDs does not catch the
  missing internal-ID mapping and would let the first version of R2 pass.
- Check each assertion catches its defect, per rule 2: remove the style read and
  the scenario must fail; remove the internal remap and the zero/duplicate case
  must fail; remove the containing submenu handle and only the nested case must
  fail.

**Acceptance:** item 5 can be marked closed for the first time. Until then it is
**partial** in every tally.

---

### R3 — Package catalog unification

*Answers finding E, and makes master-plan invariant 1 true.*

#### What is wrong

`PackagesCache` is a member of the immutable config `CACHE`
([Cache.h:251](../../src/dll/src/Include/Cache.h#L251)) — OS state inside a config
generation, which is the exact defect Audit 1 §13 named and
[§02.1](02-first-paint-latency.md) adopted a fix for. Three consequences, all
live on a stock install:

1. A menu-thread `package.*`/`appx.*` evaluation can enter `ensure_index()` and
   either enumerate (~2 ms) or block on the condition variable waiting for
   another thread's scan ([Packages.cpp](../../src/dll/src/Packages.cpp)).
2. `CACHE::clear()` calls `Packages.clear()`
   ([Cache.h:312](../../src/dll/src/Include/Cache.h#L312)), so **every config
   reload throws the index away** — and since [§03.3a](03-config-safety.md) the
   watcher reloads on every save.
3. Two package mechanisms coexist: this one and `PackageCatalogService`.
4. `PackageCatalogService::invalidate()` has no shipping caller; freshness is
   TTL-only despite the original plan's opportunistic invalidation/miss hint.

And it is not hypothetical: the shipped configuration evaluates
`package.exists("WindowsTerminal")` on every menu
([terminal.nss:8](../../src/bin/imports/terminal.nss#L8)).

#### Design

1. **Publish one scan, not two services beside each other.** The catalog worker
   already enumerates every package full name and calls
   `GetPackagePathByFullName` for each one before reading its manifest
   ([PackageCatalogService.cpp:60-89](../../src/dll/src/PackageCatalogService.cpp#L60)).
   Extend `CatalogSnapshot` with package identities and resolved install paths
   from that same pass. Do not move the current `PackageIndex` beside the
   catalog and let it enumerate independently; that would change ownership but
   fail the “unified” part of the work item.
   `PackagesCache` remains the thin expression-engine bridge it already
   documents itself to be ([Cache.h:20-23](../../src/dll/src/Include/Cache.h#L20)),
   but holds a service reference rather than its own source/index.
2. **`CACHE::clear()` stops touching packages.** This is
   [§02.1 step 4](02-first-paint-latency.md) verbatim: *"`CACHE::clear()` stops
   touching packages entirely (config reload gets cheaper and conceptually
   clean)"*.
3. **Menu-path queries become snapshot reads.** `exists`, `find_identity`,
   `all`, and `path` read the published snapshot and never enumerate or call the
   package API. The path is already known to the catalog scan; throwing it away
   and asking for it again on the menu thread has no benefit.
4. **Do not let a cold snapshot silently change the menu.** If a cold
   `package.exists()` answered *false*, the stock config's Terminal item would
   vanish from the first menu of every process — a worse defect than the one
   being fixed. Reuse the primitive that already exists for this:
   `snapshot_for_menu()`'s bounded first wait, and add a `packages.first_wait`
   phase mirroring `catalog.first_wait` so that if it ever fires, somebody sees
   it. [§02.1 step 3](02-first-paint-latency.md) declined persistence precisely
   because `catalog.first_wait` never fires; the same instrument answers the
   same question here.
5. **Decide `display_name` honestly.** It is not a single-key registry read.
   `RegistryPackageSource::resolve_display_name` can load a PRI through
   `SHLoadIndirectString`, then enumerate MrtCache subkeys and values
   ([Packages.cpp:205-303](../../src/dll/src/Packages.cpp#L205)). Microsoft's
   `SHLoadIndirectString` page says PRI/package forms extract the resource from
   the package's `Resources.pri`:
   <https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-shloadindirectstring>.
   Choose one of two explicit outcomes:
   - queue named display-name resolution on the catalog worker, publish a small
     immutable name cache, and let the first unresolved query return an
     instrumented provisional value; or
   - keep the synchronous compatibility behavior, measure it, name it as an
     exception to R1, and do **not** claim zero package/resource work.
   The first is architecturally consistent; the second may be the better product
   trade for a rarely used expression, but it needs evidence.
6. **Make refresh reachable.** A package-query miss may request one coalesced
   refresh, as §02.1 originally specifies. If an OS notification is used as a
   hint, keep it explicitly non-authoritative because the documented Shell
   change event set has no package-deployment event. TTL remains the backstop.

#### Tests

`test_packages.cpp` already has the fake source and injected clock. Add:

- a menu-path query never causes the fake source to enumerate;
- `CACHE::clear()` leaves the index intact (fails today);
- a cold query waits at most the budget, then answers from whatever exists.
- `path` returns the value already published by the scan and makes no AppModel
  call on the reader thread;
- a query miss coalesces refresh requests;
- whichever `display_name` policy is chosen is pinned, timed and visible.

**Acceptance:** `rg 'cache->Packages'` finds only the bridge;
`rg 'Packages.clear'` finds nothing in `Cache.h`; `exists`/identity/list/path
perform snapshot-only reads. Invariant 1 can be stated strongly only after the
`display_name` decision above is reflected in its wording.

---

### R4 — The `Win32MenuPresenter` boundary

*Answers finding F. The last named architectural item.*

The handoff predicted that after the split, *"whatever `MenuPresenter.cpp`
reaches for is the presenter's surface … a list somebody can read off one file"*.
That list, measured at this HEAD, is short:

| Reached for | Uses |
|---|---|
| `_theme` | 157 |
| `symbol` | 10 |
| `composition` | 8 |
| `hMenu`, `font`, `dpi` | 7 each |
| `current` | 6 |
| `_hbackground` | 5 |
| `msg` | 4 |
| `_tip`, `_level`, `_items`, `_hTheme` | 3 each |
| `ident` | 2 |
| `_screenshot`, `_menus`, `_log` | 1 each |

Eight functions live there: `draw_string`, `draw_rect`, `OnDrawItem`,
`OnMeasureItem`, `screenshot`, `draw_layer`, `UpdateLayered`, `CreateLayer`
([MenuPresenter.cpp:74](../../src/dll/src/MenuPresenter.cpp#L74) onward).

#### Design — two commits, per §04.4's own rule

1. **Introduce the surface, change nothing else.** A `PresenterContext` giving
   exactly the accessors above — dominated by `const Theme &theme()`, so the
   first cut is mostly one accessor. Thread it through the eight functions; the
   bodies must be verifiable as byte-identical modulo the accessor substitution,
   the way steps 5–7 were.
2. **Move the functions into `Win32MenuPresenter`**, constructed with that
   context. `ContextMenu` becomes its client.

#### Gate

The four `render.*` harness scenarios are the only thing in the tree that can see
a paint regression — they read the live menu back through MSAA and assert item
order, layout containment and submenu placement. They must be green before and
after each commit, run through the per-user CLSID override recipe in
[§3.7 of the handoff](08-handoff.md) so the build under test is the one being
measured.

**Risk:** medium and bounded. No behaviour is intended to change; the failure
mode is a menu that draws slightly wrong, which is exactly what the `render.*`
scenarios were built to catch.

---

### R5 — Reconcile the master-plan invariants with the implementation

*Answers finding G. Cheap, and it prevents a future session "fixing" a decision
that was made on evidence.*

[Master plan §4](00-master-plan.md#L173) lists seven invariants. Three do not
describe this tree. Each should be rewritten to state the guarantee actually
provided, with the measurement and the reason — or the code changed. Recommended
disposition:

| # | Today's wording | Reality | Action |
|---|---|---|---|
| 1 | "zero manifest/package reads on the menu thread" | manifests: true. Package enumeration, AppModel path lookup, PRI load and MrtCache enumeration remain reachable | **R3 first**; make identity/list/path snapshot-only, then word the `display_name` result exactly as R3.5 decides |
| 2 | "zero UIA waits on taskbar UI thread" | a bounded 250 ms wait through `CoWaitForMultipleHandles` ([Main.cpp:400](../../src/dll/src/Main.cpp#L400)) — deliberate; the zero-wait design was withdrawn by [§07 A2](02-first-paint-latency.md) because it met its acceptance by not showing the menu | Restate as *"no unbounded UIA wait; the wait is bounded at 250 ms, entered through `CoWaitForMultipleHandles` so the STA keeps pumping, and every outcome is counted (`TaskbarHitStats`)"* |
| 7 | "no transient global setting mutation around popups" | two remain, both opt-in and restored: `SPI_SETMENUSHOWDELAY` (only when `showdelay` is configured) and `SPI_SETSELECTIONFADE` | Restate — see below |

For invariant 7 the documented parameter shapes are worth pinning, because both
call sites are correct today and a "tidy-up" could break them silently:

- **SPI_SETMENUSHOWDELAY** — *"Sets **uiParam** to the time, in milliseconds…"*.
  [ContextMenu.cpp:2799](../../src/dll/src/ContextMenu.cpp#L2799) passes the delay
  in `uiParam` and `nullptr` in `pvParam`. ✅
- **SPI_SETSELECTIONFADE** — *"Set **pvParam** to TRUE to enable the selection
  fade effect or FALSE to disable it."*
  [ContextMenu.cpp:5673](../../src/dll/src/ContextMenu.cpp#L5673) passes the BOOL
  in `pvParam`. ✅
- **fWinIni** — *"specifies whether the user profile is to be updated, and if so,
  whether the WM_SETTINGCHANGE message is to be broadcast to all top-level
  windows"*. All four call sites pass `0`. ✅ and already enforced by
  `check-invariants` rule 9.
  — <https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfow>

Proposed wording: *"No setting mutation may update the user profile or broadcast
`WM_SETTINGCHANGE` (`fWinIni` must be 0 — enforced). Transient, opt-in,
restored-on-close mutations are permitted and are enumerated here:
`SPI_SETMENUSHOWDELAY` when the configuration sets `showdelay`, and
`SPI_SETSELECTIONFADE` around dismissal."*

Deleting the `SELECTIONFADE` toggle instead is two lines, but its benefit is a
dismissal artifact no screenshot can show — the same class as the flicker wait
([§02.4a](02-first-paint-latency.md)), which was kept for exactly that reason.
Documenting is the consistent choice; deleting needs a person at the machine.

Three stronger statements outside the numbered invariant list also need the
same reconciliation:

- [§00.1](00-master-plan.md) says nothing optional or unbounded runs before the
  first pixel and R1a says **every** third-party call runs under a deadline.
  Current UI-thread `IExplorerCommand` calls cannot be interrupted; user-written
  NSS can also call functions whose cost the engine does not bound.
- [§00.3a](00-master-plan.md) says “R1/R1a held.” The branch deliberately accepts
  one provider overrun and still has Q/O, so that sentence is false.
- [§06 Phase 1](06-phases-and-tests.md) still requires a fake provider sleeping
  2 s not to delay first paint. [§02.2a-i](02-first-paint-latency.md) explicitly
  records that this criterion is not met.

Recommended governing wording: *"Shell-controlled engine work before first
paint is locally bounded and measured. Package enumeration/manifest I/O and
`GetState(TRUE)` are forbidden. Third-party UI-thread callbacks and arbitrary
user NSS expressions are reported and policy-limited where compatible, but one
in-flight call is not preemptible."* Then list Q and any retained
`display_name` behavior as explicit exceptions/validation targets rather than
letting “bounded” silently change meaning between documents.

---

### R6 — Small correctness and instrument fixes

**R6.1 (finding H) — a dead watcher reports itself alive.**
`ConfigWatcher::run` returns on `WAIT_FAILED`
([ConfigWatcher.h:322](../../src/dll/src/Include/ConfigWatcher.h#L322),
[:344](../../src/dll/src/Include/ConfigWatcher.h#L344)) and on a failed
`FindNextChangeNotification` ([:338](../../src/dll/src/Include/ConfigWatcher.h#L338))
without clearing `_running`, so `watching()` answers true for a thread that has
gone. That is precisely the shape [§03.3b](03-config-safety.md) was written
about. Only tests read `watching()` today, so nothing user-visible breaks — which
is the argument for fixing it now rather than after something depends on it.
Clear `_running` on every exit from `run()` (the rearm-failure path at
[:380](../../src/dll/src/Include/ConfigWatcher.h#L380) already does). Test: force
a wait failure and assert `watching()` goes false.

**R6.2 (finding I) — a comment names a check that is not there.**
`PerfExportWriter::open` says the pre-existing section's size *"is checked rather
than assumed"*. There is no check. The behaviour is nonetheless correct, because
`MapViewOfFile` refuses an oversized view — *"All bytes must be within the maximum
size specified by CreateFileMapping"*
(<https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile>) —
so a squatted smaller section makes `open()` fail and the export is simply
absent. Fix the comment to name the mechanism that actually enforces it.

**R6.3 (finding J) — the report and the window disagree about a name.**
`shell.exe -report perf` prints the handler's raw title, mnemonic markers
included — `E&dit with Adobe Acrobat`, `Convert to Ado&be PDF`, `&Move to
OneDrive` — at [src/exe/src/Main.cpp:1142](../../src/exe/src/Main.cpp#L1142). The
Reliability window already strips them with `without_mnemonics`
([Reliability.h:124](../../src/exe/src/Reliability.h#L124), applied at
[:147](../../src/exe/src/Reliability.h#L147)) because six CLSIDs rendering alike
and a raw `&` were both found by looking at the real window
([§05.1d](05-capabilities.md)). Move `without_mnemonics` to a shared header and
use it in both, so one machine cannot have two names for one extension.
`test_reliability_rows.cpp` already pins the stripper's rules.

**R6.4 — test the process-lifetime file holders, not only their file formats.**
`FavoritesStore.h` and `ProviderQuarantineStore.h` both expose
`set_path_for_testing`, but no test includes either header. The shared
`Favorites.h`/`ProviderQuarantine.h` parsers are well covered; lazy load,
two-second refresh, concurrent readers and the menu-facing snapshot are not.
Add direct real-file integration tests, including a same-process concurrent
refresh. While there, take `_path_override` under the mutex in
`ProviderQuarantineStore::reload`, matching `FavoritesStore::current_path`.

Both formats currently overwrite with `CREATE_ALWAYS`. The comments accept a
torn file, but favorites now includes explicit user pins and quarantine controls
reliability behavior. Measure/decide an atomic temp+replace write once for both
formats rather than allowing the two copies to drift. This is low severity, but
the decision belongs in one shared helper and one fault-injection test.

**R6.5 (finding S) — remove two ~64 KiB production stack frames.** A full MSVC
`/analyze` pass reports:

```text
LegacyConfigTransfer::hash_and_copy   65,588 bytes
ConfigShadow::digest_of               65,576 bytes
```

Both are literal `unsigned char buffer[64 * 1024]` arrays. Microsoft documents
C6262's user-mode default threshold as 16 KiB and notes that a large frame
increases stack-overflow risk:
<https://learn.microsoft.com/en-us/cpp/code-quality/c6262?view=msvc-170>.
This DLL runs on threads whose stack belongs to an arbitrary host; move each
buffer to a heap-backed `std::vector`/`unique_ptr` (or share one chunked helper)
and retain the existing short-read/write tests. Do not silence or raise the
threshold.

The four C26110/C26117 warnings in
`ProviderQuarantineStore::refresh_if_stale` are inconsistent with the lexical
RAII scopes and appear to be analyzer false positives. Restructure the early
returns into small locked helpers so the analyzer and a human see the same
ownership; do not suppress them without the direct concurrency test from R6.4.

**R6.6 — correct executable comments that state the wrong policy.** The comment
at `ExplorerCommand.cpp:483` says every twentieth menu re-probes a deferred
provider; `REPROBE_AFTER` is 200. The header and tests use 200. Correct the call-
site comment so a future threshold change is made in one place.

**R6.7 (finding U) — fail the inline-detour transaction if thread enlistment
was not established.** `DetourTransaction::enlist()` returns `void` and silently
returns when `CreateToolhelp32Snapshot` fails; it also skips every thread that
`OpenThread` or `DetourUpdateThread` cannot enlist. `begin()` still returns true
and `Main.cpp` commits the CoCreateInstance patch. That contradicts the wrapper's
own documented premise: Detours says threads not enlisted are not updated and
may execute a mixture of old and new code
(<https://github.com/microsoft/Detours/wiki/DetourUpdateThread>).

Make enlistment return a result. Abort on snapshot/enumeration failure and on a
failure to enlist any still-live current-process thread other than the caller;
a thread proved to have exited during the snapshot race can be ignored. Use
`DetourTransactionCommitEx` so a failed attach identifies its target, and keep
the existing fail-open behavior (`forget()`, no policy hook) on any failure.
Inject the enumeration/update functions in a focused test: snapshot failure,
open failure, update failure and commit failure must all leave the detour
reported uninstalled. This is separate from conditional attachment: the hook
the product chooses to install must be installed safely.

---

### R7 — Documentation corrections

*Answers finding K. None of these is cosmetic: [§1 rule 6](08-handoff.md) exists
because a tally that drifts is how a governing decision came to be made by
nobody.*

1. **Keep two tallies and name what they count.** The project's broad outcome
   tally (“built or measured-and-declined”) is useful, but it cannot answer how
   literally the original plan was implemented. Add the strict §1.3 table to
   [00-master-plan.md §3a](00-master-plan.md), [06 §Status](06-phases-and-tests.md)
   and [08 §3.6](08-handoff.md): **11 implemented, 5 partial (2, 5, 9, 11,
   17), 4 resolved differently (7, 8, 10, 20)**. Under the broader outcome
   tally, item 5 is at least partial until R2; item 2 and item 17 remain active
   because this remediation plan contains R3/R4 for them.
2. **Record why it was missed**, in [§01.3](01-takeover-contract.md): the harness
   proved what *Windows* does with `MNS_NOTIFYBYPOS`; nothing asserted what
   *Shell* does. Harness coverage of a contract is not implementation of it —
   the same shape as §7a's non-interference proof and §9c's health check, both of
   which could not have tested what they were scheduled to test.
3. **Stale numbers.** [§3.6](08-handoff.md) says `ContextMenu.cpp` 7,542 → 5,852
   and `MenuPresenter.cpp` 1,606. At this HEAD they are **6,110** and **1,652** —
   the numbers were true at `f465242` and later commits added to both. State the
   commit a measurement was taken at, or the next reader reads drift as error.
4. **C-4**: [§03.5](03-config-safety.md) and [§3.9](08-handoff.md) should say the
   shadow-refusal is unit-tested and that only the end-to-end fresh-process route
   is open.
5. **Record the live measurement of §2.1** in [§02.2a](02-first-paint-latency.md)
   beside the ~41 ms figure, with its date and provider count. The existing
   numbers were taken against 22–25 providers; this machine now has 37 in a file
   menu, and the difference is the whole of finding B.
6. **Fix the harness recipes and add cardinality to the gate.** Every command
   must include the fixture directory, and the expected counts are part of
   acceptance — as the `Scenarios.h` constants, not as a number copied here. Record that `--shell` cannot redirect shell-namespace COM
   scenarios without the per-user override. R0 fixes the executable.
7. **Correct the invariant-script descriptions.** [§06.3](06-phases-and-tests.md)
   says `check-invariants.ps1` strips string literals; the script explicitly
   leaves them in. R2 also requires rule 8 to distinguish reading a borrowed
   `MNS_NOTIFYBYPOS` style from setting it on Shell's menu.
8. **Reconcile stale acceptance boxes.** Several docs still show unchecked work
   that their own status sections call complete, while Phase 1 still requires
   the deliberately unimplemented 2 s fake-provider deadline. A checkbox is a
   gate, not history; mark it met, open, or superseded with the evidence link.
9. **Make `git diff --check main...HEAD` a cheap gate and clean the current
   failures** in a formatting-only commit. The current branch has trailing
   whitespace in `ContextMenu.cpp`/`MenuPresenter.cpp` and a new blank line at
   EOF in `test_expression.cpp`; no behavior belongs in that commit.

---

### R8 — Validation that needs a machine or a person

Unchanged from [§3.9](08-handoff.md); listed so the plan is complete, not because
anything new is known.

| Item | What it needs |
|---|---|
| §03.5 end-to-end shadow refusal, and a fresh machine with a corrupt stock config | a machine state this one is not in |
| MSI upgrade matrix | clean VMs; `AGENTS.md` has the one row verifiable here |
| Whether any shipping third-party host takes the non-`RETURNCMD` path | a person with a lister open. **R2 raises the stakes**: today that host loses custom commands; after R2 it is served correctly, so the survey becomes a validation rather than a discovery |
| A real **file** selection in Total Commander/Directory Opus/Everything, including a large selection | verifies the capture provider in the host class this machine has not exercised. `selection.rebuild_array` must be absent when the host supplied an array; if it appears, record count and cost. This is finding Q, not covered by the desktop/background harness |
| `package.path` and `appx.name` cold/warm timing with an indirect PRI name and an MrtCache fallback | settles R3.5; record whether either operation is acceptable synchronously before weakening or strengthening invariant 1 |
| The visual half of the flicker wait (§02.4a), and that the inspector's tooltip renders (§05.7a) | a person at the machine |

---

## 4. Sequencing and gates

Dependencies are real but shallow. Recommended order:

```text
R0         harden the harness gate       (first; a gate that can exercise zero scenarios gates nothing)
   │
R6.7       detour enlistment fail-open   (shipping process-safety contract; independent)
   │
R7.1–R7.2  correct the two tallies       (do it before new work is called closed)
   │
R2         by-position replay            ← internal-ID mapping + host replay + invariant-gate update
   │
R1.1–R1.3  scheduler, outcomes, breach   ← the user-visible one; pure scheduler first
   │
R1.4–R1.5  SLO decision + selection path ← third-party measurement, then maintainer's policy call
   │
R3         package unification           ← independent of R1/R2; enables R5 invariant 1
   │
R5         invariant rewrite             ← after R3, so invariant 1 is restated once
   │
R4         Win32MenuPresenter            ← last: largest, and the only one whose gate is a paint regression
   │
R6         small fixes/static warnings   ← anywhere; no product-policy dependency
R8         environment validation        ← whenever a machine or a person is available
```

**Gates, applied to every commit** — these are the branch's existing rules, not
new ones:

1. `.\build.ps1 -Platform all` — three platforms, 0 ordinary-build warnings,
   invariants clean. Run `validate-msi-lifecycle.ps1` over all three emitted
   packages when setup/shared lifecycle code changes.
2. Both canonical harness commands from R0: exactly **23** native and **31**
   takeover scenarios, 0 failures. The takeover line must name the DLL built by
   the commit. A harness failure is a finding again, not something to re-run.
3. Every new test checked to catch its defect: re-introduce the bug, rebuild,
   watch *that* test fail, restore.
4. Anything touching the menu path: deploy it, drive it, and read the menu back
   ([§1 rule 8](08-handoff.md)). Both of the last session's most valuable
   findings were defects in work already marked done, and neither had a crash, a
   log line or a failing test.
5. Cite the contract and quote the passage, in the code and in the commit
   message.
6. `git diff --check` is clean.
7. Run x64 MSVC `/analyze` for production projects at each workstream gate;
   triage every new warning. Ordinary compilation's “0 warnings” does not mean
   static analysis ran.

**Effort and risk, honestly:**

| Workstream | Effort | Risk | Blast radius if wrong |
|---|---|---|---|
| R0 | S | none | QA only, but a false success can bless every later regression |
| R7 | XS | none | documentation |
| R6 | S–M | low–high | mostly diagnostics/hardening; R6.7 alone touches Explorer's inline-detour safety |
| R2 | M | medium | native tracking IDs and host replay; zero/duplicate/nested cases make the risk visible |
| R1.1–R1.3 | M–L | medium | call order, duplicate winner and which items appear; pure scheduler + live harness gate |
| R1.4(c) | XS to write | **product** | permanently removes a menu entry until re-probe. Needs a decision, not a patch |
| R3 | M–L | medium | a cold `package.exists()` answering wrong drops the stock Terminal item; PRI-name policy adds a product decision |
| R5 | S | none | documentation, plus possibly two deleted lines |
| R4 | L | medium | menu painting; gated by the four `render.*` scenarios |

---

## 5. Deliberately not doing

Carried forward with their reasons intact, so they are not re-proposed. Each is
already recorded where it was decided; this is the index.

- **Item 9's conditional CoCI attach** — [§01.9a](01-takeover-contract.md). The
  detour is already confined to `if(rt.loader.explorer)`
  ([Main.cpp:1819](../../src/dll/src/Main.cpp#L1819)), so the prize is the
  empty-policy Explorer case; and the policy does not exist at the point the
  bootstrap decision would be made. The Detours maintainer documentation does
  support runtime transactions: `DetourTransactionBegin` brackets the change
  and `DetourUpdateThread` enlists threads whose instruction pointers may need
  translation
  (<https://github.com/microsoft/Detours/wiki/DetourTransactionBegin>,
  <https://github.com/microsoft/Detours/wiki/DetourUpdateThread>). This tree's
  `DetourTransaction` already enumerates and enlists the other process threads.
  Therefore “background thread” is not by itself a contract reason to call the
  work impossible; the honest reason to defer is ROI and residual race/testing
  risk for the empty-policy Explorer case. Revisit if measurements show the
  always-attached fast path matters.
- **`TakeoverSession` as a class** — [§01.6a](01-takeover-contract.md). C2712: an
  SEH function cannot hold an object needing unwinding. The consolidation
  happened in plain-data form and is separately testable, which a single struct
  would not have been.
- **`PopupInterceptionBackend` as an interface** — [§01.9c](01-takeover-contract.md).
  The two mechanisms are not interchangeable; the PE format makes an import table
  per-image, so the fallback is strictly weaker. What was missing was *which one
  is live*, and that is now reported.
- **Taskbar rectangle model** ([§02.5a](02-first-paint-latency.md)), **icon
  cache**, **per-session memoization**, **lazy large-selection metadata**
  ([§04.7](04-code-health.md)), **`MSAAMENUINFO`** ([§05.3](05-capabilities.md)),
  **catalog persistence** ([§02.1 step 3](02-first-paint-latency.md)),
  **`priority` over `TreatAs`** ([§01.9b](01-takeover-contract.md)), **the
  six-state popup lifecycle** ([§01.6a](01-takeover-contract.md)) — all measured
  and declined with their numbers written down.
- **Moving provider calls to a worker thread** — the `IExplorerCommand` remark
  quoted in R1 is the environment handlers are written against. R1 budgets
  against that cost instead of relocating it.
- **The inspector's live `where=` evaluation** — [§05.7a](05-capabilities.md).
  Re-evaluating an expression to display it runs the configuration a second time
  with a different `_this` and could disagree with what the menu did. Capturing
  the verdict during composition remains the shape that would work; nobody has
  costed it.

---

## 6. One-line summary

The branch is late-stage and ordinary-green, not finished: **11 of 20 original
items are literally implemented, 5 are partial, and 4 were deliberately resolved
differently**. The remaining high-value work is to harden the QA gate (R0), make
by-position replay identifiable as well as correctly messaged (R2), replace
registration-order provider starvation with a two-phase scheduler and truthful
outcomes (R1), publish one package snapshot that includes paths and has an
explicit PRI-name policy (R3), and make inline-detour setup fail open when thread
enlistment was not established (R6.7). The presenter boundary (R4), governing-
document reconciliation (R5), remaining static-analysis/small fixes (R6–R7),
and machine-state validation (R8) follow behind those correctness items.
