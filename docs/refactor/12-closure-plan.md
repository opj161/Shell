# 12 — Closure plan for `09`

**Assembled 2026-08-26** against `refactor/takeover-master-plan` @ `450985f`
with the §09 remediation present as an uncommitted working tree.

This document merges three inputs into one executable plan:

- [`10-remediation-qa-assessment-revised.md`](10-remediation-qa-assessment-revised.md)
  — an external QA pass (findings **F1–F10**), every one of which is re-verified
  below against the code and the vendor contract before being accepted;
- [`11-qa-deep-pass.md`](11-qa-deep-pass.md) — the local deep pass (**N1–N7**);
- the surviving items of [`10-remediation-qa-assessment.md`](10-remediation-qa-assessment.md).

Part A is the verification verdict on the external pass — what is confirmed,
what is corrected, and what is rejected. Part B is the deduplicated defect
register. Part C is the implementation plan: exact files, exact changes, exact
tests, sequenced by dependency and by risk retired per unit of work.

**Verdict: `09` cannot be closed.** Four defects can produce unsafe code
patching, permanent loss of a menu item, or a wrong package catalog, and one
gate gap means none of the branch's invariants are enforced by anything except a
developer's memory.

---

## Part A — Verification of the external assessment

Every finding was re-checked here on Windows with the real toolchain, which the
external pass could not do (it recorded its environment as Linux, §1.4). Verdicts
are **Confirmed**, **Confirmed with correction**, or **Rejected**.

| # | External finding | Verdict | Evidence established here |
|---|---|---|---|
| **F1** | Detours enlistment treats a mid-walk Toolhelp failure as clean exhaustion | **Confirmed** | `Thread32Next` documents `ERROR_NO_MORE_FILES` as *the* terminal condition ([d](https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-thread32next)); [`DetourEnlistment.h:221`](../../src/shared/DetourEnlistment.h#L221) ends the walk on any `FALSE` with `out.result` already set to `Enlisted`. `InlineDetourApi` has no `GetLastError` seam, so no test can express the case |
| **F2** | Scheduler can permanently starve a provider after one transient spike | **Confirmed — the most serious defect on the branch** | Traced in full below. It is a **regression introduced by R1.1** |
| **F3** | `GetCanonicalName` sits outside the provider's measured cost | **Confirmed, severity corrected High → Medium** | Ordering verified at [`ExplorerCommand.cpp:652-686`](../../src/dll/src/ExplorerCommand.cpp#L652). **Pre-existing, not introduced by R1** — the same ordering is on the `-` side of the diff. Bounded by what `GetCanonicalName` normally costs |
| **F4** | Failed package scans publish empty/partial state | **Confirmed and strengthened** | Independently found here as **N1**. Strengthened: `CatalogStore::abandon_refresh()` already exists for exactly this, with the right rationale written on it, and is called **only from tests** |
| **F5** | hostprobe accepts empty operands; cardinality unenforced | **Confirmed by execution** | `--verify ""` → **23 scenarios, 0 failures, exit 0, and no `verified against` line** (m). Identical for `--record ""`, `--shell ""` |
| **F6** | Nested by-position acceptance case missing | **Confirmed** | Same as §10 F9 / this pass's independent finding |
| **F7** | `FavoritesStore` consistency defect + missing store tests | **Confirmed and strengthened** | `record_use` swaps unsaved entries in *and* stamps `_stamp`, so the divergence is sticky. Share modes verified. A third race found here: content and timestamp are acquired by two separate calls |
| **F8** | `display_name` exception and miss-refresh unresolved | **Confirmed** | Matches §10 F3/F4/F6 |
| **F9** | Invariant rule 8 is lexical, bypassable by indirection | **Confirmed, and sharper here** | The tree never calls `SetMenuInfo` directly — it uses `MENU::set` ([`MenuItem.h:1333`](../../src/dll/src/Include/MenuItem.h#L1333)), so R2's "prove the gate" step exercised an alternative the codebase would never produce |
| **F10** | Thread-local provider cache leaks a COM reference per thread | **Confirmed** | [`ExplorerCommand.cpp:88-124`](../../src/dll/src/ExplorerCommand.cpp#L88): `CachedProvider` holds a raw pointer, the `thread_local` vector has no destructor, and the header comment says so deliberately |

### A.1 The four confirmations that matter, in detail

**F2 — permanent starvation. Confirmed, and it is R1.1's own doing.**

```cpp
// ProviderSchedule.h:137
return c.best_us > c.last_us ? c.best_us : c.last_us;   // estimate = max(best, last)

// ProviderSchedule.h:256
return step.estimate_us <= remaining_us;                // Known steps refused on the estimate

// ExplorerCommand.cpp:572
if(timing.samples >= MIN_SAMPLES_TO_JUDGE && timing.best_us > SLOW_PROVIDER_US)
    candidate.slow = true;                              // "slow" keyed on best_us ALONE
```

Take `best_us = 2 000`, `last_us = 70 000` — two samples, the second a spike:

- `slow` is **false** (`best_us` 2 ms ≤ `SLOW_PROVIDER_US` 25 ms), so the
  candidate is `Known`, never `Reprobe`;
- `estimate_us` = 70 000 > `MENU_BUDGET_US` = 50 000, so `provider_step_fits`
  is **false even against a completely unspent budget**;
- `note_budget_deferral` increments `deferrals` and deliberately **does not**
  touch `since_probe` ([`ProviderHealth.h:302-311`](../../src/dll/src/Include/ProviderHealth.h#L302));
- the provider is therefore never called, `record()` never runs, `last_us` can
  never come down, and `best_us` can never rise into "slow".

The item disappears from every menu for the life of the process. Two samples are
enough to reach it — `MIN_SAMPLES_TO_JUDGE` is 2, and §02.2a records cold
handlers at ~30 ms each and one at 209 ms on a large selection, so a second
sample above 50 ms is ordinary rather than exotic.

This is the same class of harm R1 was built to remove (§09 finding C, *"budget
exhaustion silently removes healthy items"*) in a **permanent** form. The old
`ProviderHealth::consider()` compared `budget_remaining_us < timing->best_us` —
`best_us` alone — so a provider with a fast best time was always affordable.
R1.1's "at minimum `max(best_us, last_us)`, not `best_us` alone" is precisely
what opened the trap, and the plan specified it.

**F4 — strengthened: the correct primitive exists and is unused in production.**

[`PackageCatalogService.h:183-190`](../../src/dll/src/Include/PackageCatalogService.h#L183):

> Releases the slot without publishing, for a scan that failed outright. What is
> published stays published: **an empty catalog would remove every packaged verb
> from the menu, which is worse than an old one.**

`rg abandon_refresh` over `src/dll` and `src/tests` finds the definition and
**two callers, both in `test_package_catalog.cpp`** (c). The worker publishes
unconditionally at [`PackageCatalogService.cpp:281`](../../src/dll/src/PackageCatalogService.cpp#L281).
So the codebase wrote the policy down, built the primitive, tested the primitive,
and did not wire it up. The TTL that then holds the empty snapshot is
`DefaultTtlMs = 5 * 60 * 1000` — **ten times the 30 s the `PackageIndex` it
replaced used**, and `PackageIndex` additionally retried immediately on failure
([`Packages.cpp:387`](../../src/dll/src/Packages.cpp#L387)).

**F7 — strengthened: three distinct defects, not one.**

```cpp
// FavoritesStore.h:113-123
auto entries = Favorites::load(path);          // {} on ANY open failure
if(!Favorites::record_use(entries, identity)) return false;
auto ok = Favorites::save(path, entries);
std::lock_guard<std::mutex> lock(_mutex);
_entries.swap(entries);                        // (1) swapped in even when ok == false
_stamp = write_time(path);                     // (2) separate call, after the read
return ok;
```

1. **Failed save still replaces process state** *and* refreshes `_stamp`, so
   `refresh_if_stale` sees a matching timestamp and never re-reads. The
   divergence is sticky, not transient — worse than the comment's claim.
2. **Data loss, not a lost increment.** `Favorites::load` opens
   `GENERIC_READ, FILE_SHARE_READ` ([`Favorites.h:266`](../../src/shared/Favorites.h#L266));
   `Favorites::save` opens `GENERIC_WRITE, 0, CREATE_ALWAYS`
   ([`Favorites.h:313`](../../src/shared/Favorites.h#L313)). A conflicting open
   fails with `ERROR_SHARING_VIOLATION`
   ([d](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)),
   and `load` maps every failure to `{}`. Interleaving: A truncates and holds →
   B's load fails → B gets `{}` → A finishes → B saves a one-entry list over the
   complete history. [`FavoritesStore.h:37`](../../src/dll/src/Include/FavoritesStore.h#L37)
   claims racing hosts *"lose at worst one increment"*. They can lose everything.
3. **Content/timestamp incoherence** in both `record_use` and `reload`: the file
   is read, then `write_time(path)` re-stats it. A rewrite in between caches old
   content under the new stamp, and nothing will refresh until the *next* write.

`ProviderQuarantine` uses the identical `load(SHARE_READ)` / `save(0,
CREATE_ALWAYS)` pattern ([`ProviderQuarantine.h:326,379`](../../src/shared/ProviderQuarantine.h#L326)),
so it carries the same class.

### A.2 Corrections to the external assessment

| Item | Correction |
|---|---|
| **§4.2 — "`/analyze` status is UNVERIFIED"** | **Superseded by measurement.** `/analyze` was run here on the final working tree: DLL → **6 warnings** (2 × C26110, 4 × C26117), all in `ProviderQuarantineStore.h` lines 84, 98, 144, 150, 159, 162; **0 × C6262**, so R6.5's stack-frame half is genuinely fixed; `exe.vcxproj` and `ca.vcxproj` are **clean** (m). The count is *up from the plan's baseline of 4* — R6.4's `reload()` mutex fix added two in `reload` without the restructuring R6.5 required. This is a finding, not an unknown |
| **F4 point 5 — "`_published` semantics must be separated"** | **Rejected as a required change.** Setting the event after a failed attempt is deliberate and documented ([`PackageCatalogService.cpp:286-291`](../../src/dll/src/PackageCatalogService.cpp#L286)): a waiter should stop waiting for an answer that is not coming. `snapshot_for_menu` already handles a null `current()`. Once F4's main fix lands, "attempt ended" is the correct contract. Downgraded to a one-line comment clarification |
| **F3 severity HIGH** | **Corrected to Medium.** Real, but pre-existing rather than introduced by R1, and `GetCanonicalName` normally returns a stored GUID. Its measurable effect is the ~1.6 ms gap §09 §2.1 reads as Shell's own work |
| **§5.8 — "the previous `git diff --check` finding is stale"** | **Half right.** The working tree is clean (m) — that part is stale. `git diff --check main...HEAD` still reports **30** hits (m), because nothing is committed, and R7.9's mandated formatting-only commit is now impossible without re-splitting the hunks |
| **§6 — "F14–F16 dilute the critical signal"** | **Accepted as prioritisation, rejected as disposition.** They are cheap and ride along in W10; none is a closure blocker |

### A.3 What the external pass missed

Four items from [`11-qa-deep-pass.md`](11-qa-deep-pass.md) do not appear in it,
one of which changes what "the gates are green" is worth:

- **N3 — CI enforces none of the branch's gates.** The largest assurance gap
  found by either pass. Detail in W6.4.
- **N2 — R1 inverted telemetry eviction order** on machines above the 32-record
  cap. Detail in W5.
- **N5 — no trace fixture can distinguish sent from posted**, because
  `Probe::track` drains its own queue first; settled here by direct probe, in
  Shell's favour.
- **N6 — new harness code reintroduces the fixed-buffer `GetMenuItemInfo` trap**
  the project already solved twice.

---

## Part B — Merged defect register

Deduplicated across all three sources. "Origin" names where it was first found.

| ID | Sev | Origin | Defect | Workstream |
|---|---|---|---|---|
| **D1** | **High** | ext F1 | Detours transaction commits after an unproven thread walk | W1 |
| **D2** | **High** | ext F2 | Provider permanently starved after one spike above the menu budget | W2 |
| **D3** | **High** | N1 / ext F4 | Failed package scan published as a valid empty catalog for 5 minutes | W4 |
| **D4** | **High** | N3 | CI runs no invariants, no harness, no MSI validation; branch never in CI | W6.4 |
| **D5** | Med | ext F3 | `GetCanonicalName` outside the provider's measured cost | W3 |
| **D6** | Med | N2 | Telemetry eviction now discards the most expensive providers first | W5 |
| **D7** | Med | ext F5 | hostprobe false-greens on empty operands; cardinality unenforced | W6.1–6.2 |
| **D8** | Med | ext F7 | Favorites: failed save poisons state; sharing violation → history loss; stamp/content incoherent | W7 |
| **D9** | Med | ext F10 | Thread-local provider cache leaks one COM ref per provider per transient thread | W8 |
| **D10** | Med | §10 F9 / ext F6 | Nested by-position replay has no end-to-end assertion | W6.3 |
| **D11** | Med | §10 F3/F4 / ext F8 | `display_name`: memoization lost, comment self-contradicts, policy unmeasured | W9.1 |
| **D12** | Med | §10 F6 / ext F8 | Miss-triggered catalog refresh dropped silently | W9.2 |
| **D13** | Med | §10 F7 | `/analyze` lock warnings 4 → 6; R6.5's restructuring not done | W7.4 |
| **D14** | Med | §10 F5 | `PackagesCache` has no tests and no injection seam | W4.4 |
| **D15** | Med | §10 F10 | R6.4 store tests and atomic-write decision not done | W7 |
| **D16** | Med | §10 F2 | `ProviderHealth::consider()` dead, 30 tests pin the non-shipping policy | W2.4 |
| **D17** | Low–Med | ext F9 / §10 F11 | Invariant rule 8 misses the codebase's own `MENU::set` idiom | W6.5 |
| **D18** | Low–Med | N4 | §06.3 still says "six enforced rules, two deferred" against 10/0 | W10 |
| **D19** | Low–Med | N5 | `HostContract.h` cites evidence the harness cannot produce | W10 |
| **D20** | Low | N6 | `ShellMenu::title_at` fixed-buffer `GetMenuItemInfo`; no gate for the shape | W6.6 |
| **D21** | Low | §10 F12 | Strict tally reclassified 11→15 by the pass that reports it | W10 |
| **D22** | Low | §10 F13 / ext F5 | `09` says 23/31 in four places; measured 23/32 | W6.2, W10 |
| **D23** | Low | §10 F8/F14/F15/F16 | Dead R2 helpers, `strip_code`, orphaned comment, formatting commit | W10 |
| **D24** | — | §10 F1 / ext §4.1 | Remediation not committed | W0 |
| **D25** | — | R8 | Environment/person validation | W11 |

---

## Part C — Implementation plan

Ordered so that each unit retires the most risk for the least work, and so that
nothing is built on a gate that cannot fail.

---

### W0 — Commit the remediation *(prerequisite, D24)*

Nothing below is verifiable until the tree is content-addressed. 44 modified
tracked files (+2,895 / −389) plus 10 untracked source/test/doc files live only
in the working tree, and `09-remediation-plan.md` itself is untracked.

**Commit the audited remediation as one commit, not as a retroactive
workstream split.** The obvious instinct — replay §09 §4's sequence as eight
commits — is not executable here, and the reason is the branch's own rule:

- **Every commit must build and pass on three platforms** (§4 gate 1). Six files
  carry hunks from two or more workstreams — `Main.cpp` (R2 + R6.7),
  `ContextMenu.h` (R2 + R4), `ExplorerCommand.cpp` (R1 + R6.6), `PerfExport.h`
  (R1.2 + R6.2), `exe/Main.cpp` (R1.2 + R6.3), `tests.vcxproj` (R0 + R6.7 + R1 +
  R3). Splitting them produces intermediate commits whose compilability is
  unknown, and proving it costs ~24 builds.
- Interactive `git add -p` is unavailable in this environment. Hunk-level staging
  is possible only via `git diff <file> > p.patch`, hand-editing, and
  `git apply --cached p.patch` — fiddly, and it would still not make the
  intermediates build.
- The remediation was authored as one working tree and **audited green as one
  working tree** (33,102 checks, three platforms, both harness modes, three MSIs).
  A split invents a history that never existed and never built.

So: one commit whose message names every workstream it contains and carries the
contract citations (§4 gate 5), then **one clean commit per workstream from W1
onward**, each of which does satisfy the per-commit gates naturally.

Record the hash; every gate result below is quoted against it.

**Two honest deviations to write into the commit message rather than paper over:**

- **R7.9's formatting-only commit is unsatisfiable retroactively.** The trailing-
  whitespace fixes are interleaved with R2's `ContextMenu.cpp` and R4's
  `MenuPresenter.cpp` changes. `git diff --check` is clean on the working tree;
  `main...HEAD` reports 30 hits only because nothing is committed, and both
  become moot on this commit. State it; do not fake a split.
- Per-commit gate 3 ("re-introduce the bug, watch *that* test fail") was recorded
  by the authoring session for three changes but cannot be re-evidenced without
  the commits. It applies from W1 onward.

**Do not `git add -A`.** `src/bin/x64/` is untracked and only partly ignored, and
the repository root holds six session-transcript `.txt` files plus
`external-audit.md`. Add the 44 tracked modifications and these 10 files
explicitly:

```text
docs/refactor/09-remediation-plan.md            src/dll/src/Include/ExplorerCommandState.h
docs/refactor/10-remediation-qa-assessment.md   src/dll/src/Include/ProviderSchedule.h
docs/refactor/10-…-revised.md                   src/dll/src/Include/Win32MenuPresenter.h
docs/refactor/11-qa-deep-pass.md                src/shared/DetourEnlistment.h
docs/refactor/12-closure-plan.md                src/tests/hostprobe/Arguments.h
docs/refactor/13-implementation-handoff.md      src/tests/test_detour_enlistment.cpp
                                                src/tests/test_explorer_command_state.cpp
                                                src/tests/test_hostprobe_args.cpp
                                                src/tests/test_provider_schedule.cpp
```

`external-audit.md` belongs under `docs/refactor/` or nowhere; decide, don't
leave it at the root.

---

### W1 — Detours enumeration must distinguish exhaustion from failure *(D1)*

**Contract.** `Thread32First`/`Thread32Next` return `FALSE` for the terminal
condition and expose the reason through `GetLastError`: *"The `ERROR_NO_MORE_FILES`
error value is returned by the `GetLastError` function if no threads exist or the
snapshot does not contain thread information"*
([Thread32Next](https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-thread32next)).
Any other last-error is a failed walk. Detours: *"Threads not enlisted in the
transaction are not updated when the transaction commits. As a result, they may
attempt to execute an illegal combination of old and new code"*
([DetourUpdateThread](https://github.com/microsoft/Detours/wiki/DetourUpdateThread)).

**Changes — `src/shared/DetourEnlistment.h`:**

1. Add `DWORD (WINAPI *last_error)();` to `InlineDetourApi` (default
   `&::GetLastError` in `default_inline_detour_api()`). Injecting the seam is what
   makes the four failure shapes expressible; the struct exists for exactly this
   reason and this is the one call it does not route.
2. In `enlist_process_threads`, after the `do…while(api.thread_next(...))` loop:

   ```cpp
   if(api.last_error() != ERROR_NO_MORE_FILES)
   {
       out.result = EnlistmentResult::EnumerationFailed;
       out.error  = api.last_error();
       api.close_handle(snapshot);
       return out;           // handles opened so far are released by the caller
   }
   ```
   Clear the last error before the loop so a stale value cannot be read.
3. Replace `thread_still_present` with a tri-state:

   ```cpp
   enum class ThreadPresence { Present, Gone, Unknown };
   ```
   `Unknown` for a failed snapshot (today's `return true` case), a failed
   `thread_first`, or a walk that ends with `last_error() != ERROR_NO_MORE_FILES`.
   The caller ignores the thread only on `Gone`; `Present` **and** `Unknown` both
   produce `ThreadUnavailable` and abort. *"Could not prove it left"* must never
   read as *"proved gone"* — the existing snapshot-failure branch already applies
   that principle and the other two paths do not.

**Tests — `src/tests/test_detour_enlistment.cpp` (4 new, 11 → 15):**

- main walk fails after ≥1 successful `thread_next` → `EnumerationFailed`,
  `begin()` false, nothing committed;
- liveness `thread_first` fails → `Unknown` → `ThreadUnavailable`, aborted;
- liveness `thread_next` fails before reaching the target → same;
- `ERROR_NO_MORE_FILES` after a partial walk still succeeds (guards against
  over-correction).

**Gate.** Re-introduce each defect and watch its named test fail (§4 gate 3).

---

### W2 — Scheduler liveness *(D2, D16)*

**The rule to restore:** *no non-quarantined provider may be excluded from every
future menu without a bounded path back.* Today that guarantee exists only for
providers judged `slow` (`REPROBE_AFTER` = 200) and not for budget-deferred ones.

**Changes:**

1. **`src/dll/src/Include/ProviderHealth.h`**
   - `ProviderTiming` gains `uint16_t budget_deferrals{};`. A separate counter,
     because `deferrals` is incremented by both kinds and `since_probe` carries
     the slow-provider accounting. `ProviderTiming` is in-process only —
     `PerfExportProvider` is a distinct layout — so **no `PERF_EXPORT_VERSION`
     bump is required**.
   - `note_budget_deferral` increments it; `record()` and `note_reprobe_started`
     reset it to 0.
   - Add `inline constexpr uint16_t BUDGET_REPROBE_AFTER = 20;` beside
     `REPROBE_AFTER`, with the reason written down: 200 is calibrated for a
     provider whose probe *costs the user a slow menu*; a budget-deferred
     provider is believed cheap, so its forced probe is cheap and can be an order
     of magnitude more frequent.

2. **`src/dll/src/ExplorerCommand.cpp`** (planning loop, ~line 565): populate
   `candidate.budget_reprobe_due = timing.budget_deferrals + 1 >= BUDGET_REPROBE_AFTER;`

3. **`src/dll/src/Include/ProviderSchedule.h`**
   - `ProviderCandidate` gains `bool budget_reprobe_due{};`.
   - In `plan_providers`, a non-slow known candidate with `budget_reprobe_due`
     is pushed as **`ProviderCall::Reprobe`** rather than `Known`. This reuses
     machinery that is already correct: `provider_step_fits` never refuses a
     non-`Known` step on a prediction, and re-probes are ordered **last**, so a
     forced retry cannot evict known-healthy work from the same menu.
   - Add a structural invariant with a test: **no plan may contain a `Known` step
     whose `estimate_us > MENU_BUDGET_US`**, because such a step can never be
     admitted by any menu. That assertion is what makes the whole class of
     starvation unreachable rather than merely patched.

4. **Retire the dead policy (D16).** `ProviderHealth::consider()` has no shipping
   caller; `rg '\.consider\(|->consider\('` finds only
   `test_provider_health.cpp`, 30 call sites (c). Delete `consider()` and
   `ProviderVerdict`, and re-point those assertions at
   `classify` + `plan_providers` + `provider_step_fits`. Leaving them is the
   shape AGENTS.md warns about — *a test that only ever calls a function the way
   the test calls it* — and it lets a reader mistake the dead policy for the live
   one.

**Tests — `test_provider_schedule.cpp` (16 → ~20):**

The regression must pin the whole sequence, not a snapshot of it:

```text
fast(2ms) → fast(2ms) → spike(70ms)
  → provider is Known with estimate 70ms and is refused
  → refused exactly BUDGET_REPROBE_AFTER times, never more
  → forced retry is admitted even though 70ms > remaining budget
  → a fast answer restores it and it is scheduled normally again
```

Plus: no `Known` step ever carries an estimate above `MENU_BUDGET_US`; a forced
budget re-probe still runs after all affordable work.

---

### W3 — Provider cost attribution *(D5)*

**Contract.** *"None of the methods of this interface should communicate with
network resources. These methods are called on the UI thread"*
([IExplorerCommand](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorercommand)).
`GetCanonicalName` is one of those methods; there is no basis for treating it as
outside first-paint provider risk.

**Change — `src/dll/src/ExplorerCommand.cpp:652-686`.** Today `cost` is computed,
`health.record` and `session_provider` are called, and only then does the shown
path call `cmd->GetCanonicalName`. Reorder to:

```
acquire → fill → (if Shown) GetCanonicalName → cost = spent - spent_before
        → health.record(cost) → session_provider(cost) → provider_name → branch
```

so one cost covers every synchronous provider call made before that provider's
item is published. The whole-menu `ProviderBudget` already charges this work; the
defect is only that the provider does not.

**Why it is worth doing even though it is small.** §09 §2.1 reads the gap between
the 36.6 ms `explorer.commands` phase and the ~35 ms of per-provider records as
*"Shell's own pre-paint work is ~1.1 ms … the phase is honestly attributed."*
Part of that gap is unattributed `GetCanonicalName`. Closing it makes the
headline number mean what the document says it means.

**Test.** `src/tests` has no fake `IExplorerCommand`. Add a minimal counted one
(`test_explorer_command.cpp`) whose `GetCanonicalName` sleeps, and assert the
provider's recorded cost includes it. That double is reused by W8.

---

### W4 — Package catalog failure semantics *(D3, D14)*

**Contract.** `RegEnumKeyEx` distinguishes `ERROR_SUCCESS`, `ERROR_NO_MORE_ITEMS`
(normal exhaustion) and `ERROR_MORE_DATA` (name buffer too small) from other
system errors
([RegEnumKeyExW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regenumkeyexw)).
[`Packages.cpp:185-197`](../../src/dll/src/Packages.cpp#L185) collapses all of
them: `else if(rc != ERROR_MORE_DATA) break;` then `return true`.

**Changes:**

1. **`src/dll/src/Packages.cpp` — `RegistryPackageSource::enumerate_full_names`.**
   Return success only on `ERROR_NO_MORE_ITEMS`. Any other non-`ERROR_SUCCESS`
   status fails the scan. Treat `ERROR_MORE_DATA` as a failure rather than
   skipping the key: package full names are bounded by
   `PACKAGE_FULL_NAME_MAX_LENGTH` (~127 chars,
   `Windows Kits/10/Include/…/um/minappmodel.h:39`), so a 512-char buffer cannot
   overflow in practice and a hit means something is wrong — silently advancing
   past an indexed key is the one behaviour to remove.
2. **`Include/PackageCatalogService.h` — `CatalogScan` gains `bool ok{}`**
   (or an `enum class ScanResult`). `scan_package_catalog` sets it; the
   early-return path leaves it false.
3. **`PackageCatalogService.cpp:281` — do not publish a failed scan:**

   ```cpp
   auto scanned = scan_package_catalog();
   if(!scanned.ok) { _store.abandon_refresh(); break; }
   if(_store.publish(std::move(scanned.commands), std::move(scanned.packages),
                     ::GetTickCount64(), token))
       break;
   ```
   `abandon_refresh()` already exists and already carries the correct rationale
   — *"an empty catalog would remove every packaged verb from the menu, which is
   worse than an old one"* — and has never had a production caller.
   After this, a failed scan retries on the next kick instead of pinning an
   empty snapshot for the five-minute `DefaultTtlMs`.
4. **Keep `_published` as it is**, and add one clarifying line: the event means
   *an attempt finished*, and `snapshot_for_menu` correctly returns a null
   snapshot when none was published. (Rejecting external F4 point 5 — see A.2.)
5. **Give `PackagesCache` an injection seam (D14).** `catalog()` hard-codes
   `PackageCatalogService::instance()`, which is why the class has no tests. Take
   the store (or a snapshot provider) as an injectable dependency, matching every
   other service on this branch — `ProviderHealth`'s clock, `PackageIndex`'s
   `IPackageSource`, `ConfigWatcher`'s `WaitForObjects`, `InlineDetourApi`.

**Tests — `test_package_catalog.cpp` + a new `test_packages_cache.cpp`:**

- registry-open failure → previous snapshot survives, `current()` unchanged;
- mid-enumeration error → scan fails, nothing published;
- `ERROR_MORE_DATA` → scan fails rather than skipping a key;
- first scan fails, second succeeds → publishes normally;
- **the cold-query case R3 named as the worst outcome**: a cold
  `package.exists("WindowsTerminal")` waits at most the budget and never answers
  a confident `false` because of a failure;
- a menu-path query performs no enumeration.

---

### W5 — Telemetry eviction order *(D6)*

`MAX_PROVIDERS` and `PERF_EXPORT_PROVIDERS` are both 32
([`DiagnosticsRing.h:153`](../../src/dll/src/Include/Diagnostics/DiagnosticsRing.h#L153),
[`PerfExport.h:173`](../../src/shared/PerfExport.h#L173)). This machine has **55
distinct packaged context-menu CLSIDs** (measured by walking every installed
package's `AppxManifest.xml`), and §09 §R7.5 puts a file menu at 37 — both above
the cap.

R1 writes records in the order: quarantined → **the whole slow-deferral batch,
before any provider is called** ([`ExplorerCommand.cpp:593`](../../src/dll/src/ExplorerCommand.cpp#L593))
→ outcomes in cheapest-first resolution order. So the records evicted are the
tail of the resolution order — the **most expensive** known providers, the
unsampled ones and the re-probes — while stable-verdict deferrals are guaranteed
a slot.

**Change.** Move the `plan.deferred` recording loop to **after** the resolution
loop, so outcome records claim slots first. One-line move; no format change.

Also correct [`src/exe/src/Main.cpp:1168`](../../src/exe/src/Main.cpp#L1168) —
*"this should now be rare"* — and note in §09 §2.1 that the quoted provider list
was read from a truncating record set.

---

### W6 — Gate integrity

#### W6.1 — hostprobe empty operands *(D7)*

Measured here: `--verify ""` → **23 scenarios, 0 failures, exit 0**, with no
`verified against` line — verification silently disabled (m). Identical for
`--record ""` and `--shell ""`. In automation an unset variable produces exactly
this.

`src/tests/hostprobe/Arguments.h`, in `take_operand`: reject `operand.empty()`
with `kUsageExitCode`. Three parser tests in `test_hostprobe_args.cpp` (13 → 16).

#### W6.2 — canonical cardinality from one source of truth *(D7, D22)*

`main.cpp` rejects only `ran == 0`. Deleting or accidentally skipping scenarios
still exits 0.

Put the counts in `Scenarios.h` (`kNativeScenarios = 23`, `kTakeoverScenarios = 32`),
assert them in `main.cpp` for an **unfiltered** run, and have the docs cite the
constants rather than repeating numbers. Then fix the four places `09` still says
23/31 (§0 table, §0 harness-warning paragraph, R0 Acceptance, §4 gate 2) —
[`06-phases-and-tests.md`](06-phases-and-tests.md) already has 23/32 correct.

#### W6.3 — nested by-position acceptance *(D10)*

`09` R2 required a by-position harness menu with zero/duplicate IDs selecting
**both root and nested** items, and a defect injection where removing the
containing submenu handle fails the nested case. The live scenario exercises the
root only — the takeover trace shows `WM_MENUCOMMAND position=33 menu=menu#1`,
the root (m). `test_host_contract.cpp:a_nested_selection_names_its_own_submenu`
covers the pure function, which merely copies `sel.containing_menu` and cannot
detect a `ContextMenu` that always reports the root.

Extend `ShellMenu::apply_notify_by_position` to build a real host submenu with
its own zero/duplicate IDs; add `takeover.a_nested_by_position_selection_names_its_submenu`
asserting one `WM_MENUCOMMAND`, `wParam` = position **within that submenu**,
`lParam` = that submenu's `HMENU`, zero `WM_COMMAND`. Prove it by forcing
`native_source.menu` to the root and watching only the nested case fail.

The code path looks correct — `native_source.menu = hMenu` is stored per level
([`ContextMenu.cpp:3770`](../../src/dll/src/ContextMenu.cpp#L3770)) and
`prepare_system_items` runs per materialised popup — so this is expected to be a
green test, not a bug hunt. That is the point: it is the assertion that keeps it
correct.

#### W6.4 — CI enforces the gates *(D4 — the largest assurance gap)*

[`.github/workflows/build.yml`](../../.github/workflows/build.yml) runs `msbuild`
on the solution and then `tests.exe`. It does **not** run `build.ps1`, and
`rg 'check-invariants'` over `*.ps1 *.yml *.vcxproj *.props *.targets` finds only
[`build.ps1:78`](../../build.ps1#L78) and the script itself (c). Therefore:

- **all ten invariant rules are enforced only on a developer's machine** —
  including rule 8 which R2 rewrote and "proved", rule 9 (`SPIF_SENDCHANGE`) and
  rule 10 (`GetState(TRUE)`);
- **hostprobe never runs** — so all four `render.*` scenarios (R4's only
  paint-regression gate) and the by-position scenario (R2's only end-to-end
  gate) have no automated home;
- **`validate-msi-lifecycle.ps1` never runs** (§06 §3 still lists it as a "cheap,
  immediate" addition, *"currently manual only"*);
- the workflow triggers only on `main` and PRs to `main`, so this 83-commit
  branch **has never been through CI at all**.

`06-phases-and-tests.md:490` routes harness execution to *"the scheduled VM job
(below)"*. **There is no such job** — not below in that document, not elsewhere
in `docs/refactor`, not in the repository (c).

Actions:

1. Add to `build.yml`, after the build step:
   `powershell -NoProfile -File scripts\check-invariants.ps1` and
   `powershell -NoProfile -File scripts\validate-msi-lifecycle.ps1 -Path …` for
   the emitted package. Neither needs a desktop.
2. Add the working branch to the trigger list while it is live, or open the PR.
3. Either write the scheduled desktop job the documents assume — a
   `workflow_dispatch` / `schedule` job on a runner with an interactive session
   running both canonical hostprobe commands — **or delete the references** and
   state plainly that harness execution is a manual pre-merge step with a named
   owner. A gate that exists only in prose is worse than an acknowledged manual
   one.

#### W6.5 — invariant rule 8 covers the real idiom *(D17)*

Current regex:
`SetMenuInfo\s*\([^;]*MNS_NOTIFYBYPOS|dwStyle\s*(\|)?=\s*[^;]*MNS_NOTIFYBYPOS`.

The tree never calls `SetMenuInfo` directly — it calls `MENU::set`
([`MenuItem.h:1333`](../../src/dll/src/Include/MenuItem.h#L1333)), which is also
how the required *read* is written (`MENU::get`,
[`ContextMenu.cpp:3935`](../../src/dll/src/ContextMenu.cpp#L3935)). So only the
second alternative can catch a real violation, and it is defeated by an
intermediate variable or the numeric literal `0x08000000`. R2's "prove the gate"
step used the `SetMenuInfo` shape — the one alternative the codebase would never
produce.

Add `MENU::set` to the first alternative and `0x08000000` beside the symbolic
name. Prove the gate with the shape the tree actually uses. Keep the positive
ability to *read* the borrowed root's style.

#### W6.6 — two-call menu text, and a rule for it *(D20)*

`ShellMenu::title_at` ([`ShellMenu.h:213`](../../src/tests/hostprobe/ShellMenu.h#L213))
reads item text into `wchar_t buffer[256]` with a literal `cch`. AGENTS.md names
this exact shape as a trap; the same project already implements the documented
two-call pattern **twice** — [`Probe.h:494`](../../src/tests/hostprobe/Probe.h#L494)
(*"The documented two-call pattern…"*) and
[`MenuReader.h:296`](../../src/tests/hostprobe/MenuReader.h#L296). Production is
clean ([`Initializer.cpp:726`](../../src/dll/src/Initializer.cpp#L726) uses
`read_menu_text`).

Effect is a **weakened assertion**, not a false failure: both `expected_title`
and `replayed_title` truncate, so two different long host titles sharing a
255-character prefix compare equal — and `title_at` reads the host's real shell
menu, which is where long third-party titles come from.

Reuse `Probe.h`'s helper, and add invariant rule 11 banning a `MENUITEMINFOW`
whose `dwTypeData` is a fixed array with a literal `cch`. `release(n-1)` and the
`MB_*`/`WC_*` flags — the other two "silent wrong answer" families AGENTS.md
lists — both already have rules; this one does not.

---

### W7 — Process-lifetime stores *(D8, D13, D15)*

1. **Loading must distinguish outcomes.** Change `Favorites::load` /
   `Quarantine::load` to return a result carrying `{Missing, Loaded, Failed}`
   plus the entries. **Never** mutate-and-save from a list produced by an I/O
   failure.
2. **`FavoritesStore::record_use` must not poison state.** Swap into `_entries`
   and refresh `_stamp` **only when `save` succeeded**; on failure leave the
   cached state and force the next `refresh_if_stale` to re-read.
3. **Serialize the same-process transaction.** Hold the store mutex across
   load-modify-save so two menu threads in one host cannot interleave.
4. **Coherent stamp.** Take the write time from the same open handle
   (`GetFileInformationByHandle`) instead of a second `GetFileAttributesExW`
   after the read, closing the old-content/new-stamp window in both `reload` and
   `record_use`.
5. **One atomicity decision for both formats (R6.4's third part).** Write to a
   temporary in the same directory and replace, via
   [`ReplaceFileW`](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)
   or `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`, in **one
   shared helper** used by Favorites and Quarantine. That removes the
   `CREATE_ALWAYS` truncate window that makes the sharing violation destructive
   in the first place.
6. **Direct real-file tests** for both holders (R6.4's first part — no test file
   includes either header today): lazy load, 2 s refresh, same-process concurrent
   refresh, sharing violation during load, failed save, stale-stamp refresh.
7. **`/analyze` (D13).** Measured: 6 lock warnings, up from 4, the two new ones
   added by R6.4's `reload()` change (m). Restructure `reload` and
   `refresh_if_stale` into small locked helpers as R6.5 specified, so the
   analyzer and a reader see the same ownership, and re-run `/analyze` to zero.
   Do not suppress.

---

### W8 — Provider cache lifetime *(D9)*

`CachedProvider` holds a raw `IExplorerCommand*`; the `thread_local` vector's
destructor destroys the vector without calling `Release`
([`ExplorerCommand.cpp:88-124`](../../src/dll/src/ExplorerCommand.cpp#L88)). The
comment chooses this deliberately — *"a released process is a released process"*
— which holds for Explorer's long-lived menu thread and not for a host that
raises menus on transient threads. Each such thread leaves its providers
referenced until process exit, keeping provider DLLs loaded. R8 explicitly covers
Total Commander / Directory Opus / Everything, so the assumption cannot stay
implicit.

**Do not simply add a TLS destructor that calls `Release`.** Thread-local
destructors can run after the thread's `CoUninitialize`, and releasing an
apartment-bound interface then is worse than the leak.

Recommended shape: record the owning thread id when the cache is first used, and
release the cache from a point where the apartment is provably still live — the
menu teardown path already runs there. Where that cannot be established, do not
cache at all: an uncached provider costs ~2 ms to reactivate, which the branch has
already measured and accepted elsewhere. Whichever is chosen, **document the
supported host/thread pattern** rather than leaking by construction.

**Test.** Reuse W3's counted fake `IExplorerCommand`: create and tear down a menu
thread repeatedly and assert the reference count returns to zero.

---

### W9 — R3 policy closure *(D11, D12)*

1. **`display_name`.** `PackageIndex::display_name` memoized per entry
   (`display_resolved`, [`Packages.cpp:487`](../../src/dll/src/Packages.cpp#L487));
   `PackagesCache::display_name` constructs a fresh `RegistryPackageSource` and
   re-resolves **every call**, where resolution can load the package's
   `Resources.pri` through
   [`SHLoadIndirectString`](https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-shloadindirectstring)
   and walk MrtCache. R3.5 offered two options and shipped a third: synchronous,
   named, **and uncached**, without the measurement option (b) required.
   Do one of: publish a resolved-name cache from the catalog worker (option a),
   or restore memoization and complete the R8 cold/warm measurement (option b as
   written). Either way, fix the contradiction: the class comment says the call
   *"is timed under its own phase"* ([`Cache.h:59`](../../src/dll/src/Include/Cache.h#L59))
   and the implementation says *"Not wrapped in a MenuPerfScope"*
   ([`Cache.h:98`](../../src/dll/src/Include/Cache.h#L98)) — the same defect class
   R6.2 was written to remove.
2. **Miss-triggered refresh.** R3 step 6 required a coalesced refresh on a query
   miss; `PackageCatalogService::invalidate()` still has no production caller (c).
   Implement it in `find_entry`, or amend R3 with the reasoning for TTL-only and
   a test that pins the choice. Silently dropping a design step is what §1 rule 6
   exists to prevent.

---

### W10 — Documentation and evidence *(D18–D23)*

Cheap, and each one is a statement a future reader would act on:

1. **§06.3 rule count (D18)** — *"Six enforced rules, two deferred … `GetState(…,
   TRUE)` with Phase 1, `SPIF_SENDCHANGE` with Phase 3"*
   ([`06-phases-and-tests.md:497`](06-phases-and-tests.md)). Measured: **10
   enforced, 0 deferred**, and both "deferred" rules are now enforced (rules 9
   and 10). R7.7 corrected the sentence below this one and left this.
2. **Send/post evidence (D19)** — `HostContract.h:42` says posting *"is also
   measured"* by the traces. It is not: `Probe::track` drains its own queue
   ([`Probe.h:301`](../../src/tests/hostprobe/Probe.h#L301)) before the summary is
   printed ([`Scenarios.cpp:646`](../../src/tests/hostprobe/Scenarios.cpp#L646)),
   so **no fixture can distinguish sent from posted**. Settled here by direct
   probe (Windows 11 26200 x64, MSVC 14.44.35207): WM_COMMAND **posted**;
   WM_MENUCOMMAND with `MNS_NOTIFYBYPOS` **posted**, `wParam = 1`,
   `IsMenu(lParam)` true at delivery, three runs (m). **Shell's design is
   correct** — Windows posts even the handle-carrying message, despite the page
   saying "Sent". Replace the claim with the probe and its numbers, and note in
   `fixtures/README.md` what the traces cannot show.
3. **`strip_code` → `Get-CodeText`** (`check-invariants.ps1:54`).
4. **Orphaned comment** in `PerfExport.h:746-786` — the block documenting
   `perf_export_result_name` is separated from it by
   `perf_export_header_understood`.
5. **Tally provenance (D21)** — §00.3b publishes 15/5/0 where R7.1 specified
   11/5/4. Three partials were closed by code (2, 5, 17); **two by relabelling**
   (9 deferred→declined, 11 closed by enumerating the SPI mutations). Keep the
   numbers if defended, but say which two were reclassified and on what argument.
6. **Dead R2 helpers (D23)** — `ContextMenu::ID::is_native_tracking()` has no
   caller; `get_sys()` has none either, and its range starts at `0x5fffffff` so a
   second allocation would land inside `[start_native, end_native)`. Delete, or
   add a compile-time disjointness assertion.
7. **`/analyze` status** — record the measured result (6 lock warnings, 0 C6262,
   exe and CA clean) rather than leaving it "unverified".
8. **23/31 → 23/32** in the four places named in W6.2.

---

### W11 — R8, unchanged *(D25)*

Still needs a machine or a person: end-to-end shadow refusal, the MSI upgrade
matrix on clean VMs, whether any shipping third-party host takes the
non-`TPM_RETURNCMD` path, a real large file selection in Total Commander /
Directory Opus / Everything (**gates R1.5, and now W8's cache policy**), and the
cold/warm `package.path` / indirect `appx.name` timing (**gates W9.1**).

Add one item: **deploy and re-measure R1 on a live Explorer.** R1's acceptance is
*"the observed 52.2 ms session's four cheap providers are no longer dropped"*,
and that has never been measured — the scheduler is proved against numbers, not
against the machine. Read `dropped_providers` while doing it; per W5 it is
expected to be non-zero here.

---

## Part D — Sequencing

Dependencies are shallow; the order below is by risk retired per unit of work.

```text
W0   commit                      ← nothing is verifiable until the tree is addressable
 │
W6.4 CI runs the gates           ← do this second: every later fix is otherwise
 │                                  validated by the same single unenforced path
W6.1 hostprobe empty operands    ← a gate that can be asked to verify nothing
W6.2 canonical cardinality
 │
W1   Detours enumeration         ← process safety; independent
W2   scheduler liveness          ← permanent menu-item loss; independent
W4   package scan failure        ← wrong catalog for 5 minutes; independent
 │
W3   provider cost attribution   ← after W2, same call site
W5   telemetry eviction order    ← one-line move, same file as W3
 │
W7   store consistency + /analyze
W8   provider cache lifetime     ← needs W3's fake IExplorerCommand
 │
W6.3 nested by-position          ← after W6.1/6.2 so the harness gate is real
W6.5 invariant rule 8
W6.6 two-call menu text + rule 11
 │
W9   R3 policy closure           ← W9.1 gated on R8's measurement
W10  documentation
W11  R8
```

**Per-commit gates** (the branch's existing rules, unchanged): three platforms
with zero ordinary warnings; `check-invariants` clean; both canonical hostprobe
commands at the enforced counts with the `takeover:` line naming the build under
test; every new test proved to catch its defect by re-introducing it; the
contract quoted in code and in the commit message; `git diff --check` clean;
`/analyze` triaged at each workstream gate.

---

## Part E — Acceptance checklist

### Code

- [ ] **W1** Toolhelp walk accepts only `ERROR_NO_MORE_FILES` as exhaustion, in
      the main walk and the liveness re-check; liveness is tri-state and
      `Unknown` aborts.
- [ ] **W2** No non-quarantined provider can be excluded indefinitely; no `Known`
      step can carry an estimate above `MENU_BUDGET_US`; `consider()` retired.
- [ ] **W3** All synchronous pre-publication `IExplorerCommand` work, including
      `GetCanonicalName`, is inside the provider's measured cost.
- [ ] **W4** The scanner reports failure; a failed refresh calls
      `abandon_refresh()`; `PackagesCache` has an injection seam and tests.
- [ ] **W5** Outcome records claim telemetry slots before stable-verdict
      deferrals.
- [ ] **W7** Failed save cannot poison in-memory state; load distinguishes
      missing from failed; one shared atomic-replace helper; stamp and content
      come from one handle.
- [ ] **W8** Provider-cache COM references have a bounded, documented lifetime.

### Gates

- [ ] **W6.4** CI runs `check-invariants` and `validate-msi-lifecycle`; the
      working branch is in the trigger list; the desktop harness job exists or
      the references to it are removed.
- [ ] **W6.1/6.2** Empty operands rejected; canonical counts asserted from one
      source of truth; all documents agree on 23/32.
- [ ] **W6.3** Nested by-position scenario proves the containing submenu handle
      and fails when it is forced to the root.
- [ ] **W6.5/6.6** Rule 8 covers `MENU::set` and the numeric literal; rule 11
      bans fixed-buffer `MIIM_STRING` reads; `title_at` uses the two-call pattern.

### Reproducibility

- [ ] **W0** Remediation committed; the tested hash recorded in `09` §0a.
- [ ] `build.ps1 -Platform all` green on that commit, zero ordinary warnings.
- [ ] x64 `/analyze` re-run on production projects and **at zero**, not 6.
- [ ] Both hostprobe commands green at the enforced counts.
- [ ] MSI lifecycle green on all three packages.
- [ ] `git diff --check main...HEAD` clean.
- [ ] **W11** R8 complete, including the R1.5 large-selection measurement, the
      R3.5 package-name measurement, and the live R1 re-measurement.

---

## Part F — Explicitly not doing

- **Separating `_published` into "attempt ended" and "snapshot exists"** — the
  current semantics are deliberate and documented, and become correct once W4
  lands. Rejected as a required change (A.2).
- **Moving `IExplorerCommand` calls off the menu thread** — the UI-thread remark
  is the environment handlers are written against; §02.2a-i declined it on those
  grounds and W2/W3 budget against the cost instead.
- **Lowering `SLOW_PROVIDER_US`** (§09 R1.4 option c) — a product decision that
  trades a real menu entry for ~14 ms. Still the maintainer's call; W2 removes the
  liveness defect without needing it.
- **A naive TLS destructor for the provider cache** — it can run after the
  thread's `CoUninitialize`. W8 says what to do instead.
- Everything in §09 §5, carried forward with its reasons intact.
