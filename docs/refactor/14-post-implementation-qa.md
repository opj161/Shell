# 14 — Closing out `12`: QA pass, remaining implementation, and validation

**Assembled 2026-08-26** against `refactor/takeover-master-plan`.

Two things happened here and the document carries both. The first half is a QA
pass over what the implementing session landed before it ran out of quota — what
was verified, what was still open, and what neither `10`, `11` nor `12` had
found. The second half is the closure: the remaining workstreams implemented,
every gate re-run, and **one use-after-free found and fixed** that the unit
suite could not see and that only the takeover harness caught.

[`12-closure-plan.md`](12-closure-plan.md) is the specification;
[`13-implementation-handoff.md`](13-implementation-handoff.md) is the operating
manual. This is the record of finishing it.

---

## 0. Verdict

**All eleven code and gate workstreams (W0–W10) are complete and verified.**
W11 (R8) remains, and remains what it always was: work that needs a machine or a
person this one is not.

The single most important result in this document is §4: `ac662c4` shipped a
double-release into the menu path, the entire unit suite passed on it, and the
takeover harness — the one gate `12` Part E lists that had not been re-run since
`0a66b38` — failed one to five scenarios a run, differing every run. It is
fixed, and the bisection is recorded because the *shape* of that failure is the
thing worth recognising again.

---

## 1. Final measured state

Every row re-measured on this tree, not quoted from a session.

| Gate | `13` baseline | Final | |
|---|---|---|---|
| x64 build + suite | 33,102 checks / 0 fail | **33,741 / 0**, 0 warnings | ok |
| x86 build + suite | 33,102 checks / 0 fail | **33,741 / 0**, 0 warnings | ok |
| arm64 | builds + packages, tests skipped | unchanged | ok |
| invariants | `OK (10 rules, 0 deferred)` | **`OK (11 rules, 0 deferred)`** | ok |
| `/analyze` DLL | **6** warnings | **0**, 23 TUs | ok |
| `/analyze` exe + CA | clean | clean | ok |
| harness, native | 23 / 0 / 9 skipped | **23 / 0 / 10 skipped** | ok |
| harness, takeover | 32 / 0 | **33 / 0**, three consecutive runs | ok |
| MSI lifecycle | ok ×3 (pwsh 7 only) | **ok ×3 under Windows PowerShell 5.1** | ok |
| `git diff --check` | 30 hits on `main...HEAD` | clean both ways | ok |
| commits | 83 ahead of `main`, `09` uncommitted | **103 ahead**, 20 new commits | ok |

---

## 2. What the implementing session landed (QA pass)

Thirteen commits, `95ffd86`..`29bfbc7`, verified against the plan by re-reading
the code and re-running the gates.

| WS | Commit | Verified |
|---|---|---|
| **W0** | `95ffd86`, `9e7bb8a` | One commit as `12 §W0` required; hash in `09 §0a`; `external-audit.md` moved; `.gitignore` updated |
| **W6.4** | `1d5eef9` | `invariants` job, per-platform MSI step, branch trigger; phantom VM job removed from `06` |
| **W6.1** | `620fc5b` | Empty operands rejected with `kUsageExitCode`; 3 parser tests |
| **W6.2** | `e834995` | Counts in `Scenarios.h`; table size *and* per-mode ran/skipped asserted; exit 120 |
| **W1** | `51426d3` | `last_error` routed through the injected table; tri-state `ThreadPresence`; only `ERROR_NO_MORE_FILES` ends a walk; `begin()` aborts on `EnumerationFailed` |
| **W2** | `eab7e3a` | `budget_deferrals` + `BUDGET_REPROBE_AFTER`; **plus a structural invariant the plan did not ask for** — §3 F-J |
| **W4** | `8dec84a` | Scanner reports failure; `abandon_refresh()` finally has a production caller; `refresh_catalog()` extracted as a seam; `PackagesCache` split out with an injection seam |
| **W3 + W5** | `9669cd5` | `GetCanonicalName` moved inside the cost; deferral loop moved after the resolution loop |
| **W6.5/6.6** | `be577e0`, `d393f21` | Rule 8 covers `MENU::set` and `0x08000000`; rule 11 exists; `title_in` two-call |
| **W6.3** | `0a66b38` | Nested by-position scenario, proved by forcing `native_source.menu` to the root |
| **W7** | `29bfbc7` | `StoreFile.h`: tri-state load, coherent stamp from one handle, `FILE_SHARE_DELETE`, `ReplaceFileW`; `/analyze` 6 → 0 |

### 2.1 Two judgement calls worth endorsing

**W1 declined the plan's "clear the last error before the loop", and was right
to.** Measured instead: a 6,386-entry walk ended with `ERROR_NO_MORE_FILES` on
three consecutive runs *even with a succeeding call inside the loop body
stamping `ERROR_SUCCESS`*, and a module snapshot failed `Thread32First` with
`ERROR_NO_MORE_FILES`, matching
[Thread32Next](https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-thread32next).
The reading also falls the safe way: an unexpected error fails the enlistment
and no detour is applied.

**W7 chose `ReplaceFileW` over the plan's first-named `MoveFileExW`, on a
measurement.** With a reader holding `READ|WRITE|DELETE`, `MoveFileEx` returns
`ERROR_ACCESS_DENIED` while `ReplaceFile` succeeds — because `ReplaceFile` opens
the replaced file with sharing `FILE_SHARE_READ | FILE_SHARE_WRITE |
FILE_SHARE_DELETE`
([ReplaceFileW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)).
`MoveFileEx` is kept only for the case `ReplaceFile` cannot serve: nothing to
replace yet.

---

## 3. What the QA pass found, and what was done about each

| # | Finding | Resolution |
|---|---|---|
| **F-A** | The branch had never been through CI — 13 commits unpushed, so W6.4's workflow had never executed | Pushed; §6 |
| **F-B** | `09` stale on cardinality *again* (23/32 vs a 23/33 harness), by the commit that added the scenario | `5215609` — every count in `09` now references the constants |
| **F-C** | W3 shipped with no test; W8's fake could not stand in for one | `a965737` — `ProviderCall.h` + a real `IExplorerCommand` fake |
| **F-D** | W8 written and green but unlanded and ungated | `ac662c4` — **and landing it exposed §4** |
| **F-E** | W9 untouched; W4 had copied D11's contradiction into a new header | `f79a16b` |
| **F-F** | W10 roughly half done — five statements a reader would act on | `5215609` |
| **F-G1** | `ProviderCache.h` comment stated a reason that did not hold | folded into `ac662c4` |
| **F-G2** | 20 branch-touched files carried bare LF against `eol=crlf` | normalised; §7 |
| **F-K** | the same ownership defect, as an outright leak, in the sub-command path | `1bf2acf` |
| **F-J** | W2 stronger than Part E asked for | endorsed, no action |

### F-J — W2 is stronger than the plan required *(no action)*

Besides promoting a starved candidate to `Reprobe`, `plan_providers` now refuses
to plan a `Known` step whose estimate exceeds `MENU_BUDGET_US` at all, deferring
it with `ProviderDeferral::Budget` so the caller charges it through the same
`note_budget_deferral`. `plan_is_admissible()` states the invariant and two tests
hold the planner to it. That makes the starvation class **structurally
unreachable** rather than bounded at 20 menus.

One consequence, recorded so it is not rediscovered as a bug:
`budget_reprobe_due` is computed for every timed candidate, so a cheap provider
genuinely refused 20 consecutive times is promoted to `Reprobe` and thereby
ordered **last**. That is safe — `provider_step_fits` never refuses a non-`Known`
step while any budget remains — and it is the plan's stated intent.

---

## 4. The defect this close-out existed to catch

**`ac662c4` turned a deliberate ownership transfer into a double-release, and
every unit test passed.**

### What was actually true

`acquire_explorer_command` handed out two references — "one for the cache, one
for the caller" — and the resolution loop released the caller's half **only** on
`Hidden` and `Failed`. Both the implementing session and §3 F-D of the first
draft of this document read that as a leak on the succeeding path.

It is not. On the shown path, `fill_menuitem_from_explorer_command` stores the
pointer on the menu item and sets `explorer_command_owned`, and
`menuitem_t::~menuitem_t` releases it
([`ContextMenu.h`](../../src/dll/src/Include/ContextMenu.h)). **The item is the
second owner.** The loop was transferring, not leaking.

### Why the "fix" was worse than the imagined defect

`BorrowedProvider` released unconditionally at scope exit, so every shown
packaged verb was released once too often. That is not a miscount that surfaces
at shutdown:

- the item is asked for `EnumSubCommands` when its submenu opens, and for
  `Invoke` when it is chosen — both **after** composition;
- `ThisMenuOnly` (which is every third-party host, and the harness) releases the
  cache's reference at the end of composition.

So the object could be freed while a standing menu still pointed at it.

### How it was found

The takeover harness — the one gate `12` Part E lists that had not been re-run
since `0a66b38`, and which this document's first draft flagged as not re-run.

```text
takeover harness, same desktop, same COM override, 33 scenarios
  0a66b38  (before W7)   0 failures   0 failures   0 failures
  29bfbc7  (W7)          0 failures   0 failures
  ac662c4  (W8)          5 failures   5 failures     <- differing sets
  0adb5b1  (the fix)     0 failures   0 failures   0 failures
```

**The failing set differed on every run** — by-position replay, borrowed-popup
INIT/UNINIT balance, three of the four `render.*` measurements, `TrackPopupMenu`
itself returning 0 with `ERROR_FILE_NOT_FOUND`. That is what memory corruption
in the host process looks like from outside, and it is why no single scenario
name points at the cause. It reads exactly like desktop flakiness, and the
fixtures README's own "run on a quiet desktop" note makes that reading
attractive — which is the trap. What settled it was building the last known-green
commit in a worktree and running both under identical conditions.

### Why the suite did not find it

`tests.exe` never failed once, on any of those builds. The fake modelled the
cache and the handle; nothing had told it a third owner exists.

> **AGENTS.md** — *"a test that only ever calls a function the way the test
> calls it proves nothing about the way the product calls it."*

This is the fifth entry in that list, and the most expensive so far.

### The fix

`BorrowedProvider::detach()` — gives the reference away instead of dropping it.
The call site keys on **what the item actually took**, not on `FillResult::Shown`,
because the two are not the same: an `ECF_ISSEPARATOR` command returns `Shown`
from an earlier branch that stores no pointer. That path leaked a reference
outright both before and after `ac662c4`, and now releases it with the handle.

Two tests model the transfer. Making `detach()` release again fails them with
`outstanding() == -1` — the defect stated as a number.

### F-K — and the same rule was missing next door

Establishing that rule made the adjacent function readable, and it had the same
defect the other way round. `IEnumExplorerCommand::Next` hands the caller a
reference; the sub-command loop released it when the fill declined and otherwise
let the child item take it — correct, except that *the fill returned Shown* and
*the item stored the pointer* are again not the same condition. A separator
sub-command's reference was held by nobody for the life of the process.
Fixed in `1bf2acf` with the same test: what the child actually took.

---

## 5. The remaining workstreams, implemented

### W8 — provider cache lifetime *(`ac662c4` + `0adb5b1`)*

Cache, move-only `BorrowedProvider`, injected `ProviderComApi`, and a lifetime
rule keyed on `Selected.loader.contextmenuhandler`: `AcrossMenus` for Explorer's
long-lived menu thread, `ThisMenuOnly` for a host whose thread Shell does not
own, released at the end of composition where the apartment is provably live.
Not a thread-local destructor — it can run after that thread's `CoUninitialize`,
and `ProviderCache`'s implicit destructor makes no COM call, so the ban holds by
construction.

### W3 — the cost boundary as a rule *(`a965737`)*

`Include/ProviderCall.h`, in the shape `Include/ExplorerCommandState.h` already
established: the rule separated out so it can be tested without a handler, a
selection or a menu. It deliberately does **not** sample the start of the span —
activation is inside the cost, so the caller samples before it borrows and hands
the reading in. Moving that sample inside would drop `CoCreateInstance` from
every provider's cost: the same defect pointing the other way, and
`activation_stays_inside_the_cost` is the guard.

### W9.1 — `display_name`, measured then memoized *(`f79a16b`)*

R3.5 shipped synchronous, named and **uncached** without the measurement option
(b) required. Measured here against the real `resolve_display_name` over all 289
packages installed on this machine:

| | n | mean |
|---|---|---|
| `DisplayName` is a plain string | 134 | **0.028 ms** |
| `DisplayName` is `@{...}` | 155 | **5.773 ms** |

| pass over all 289 | mean | worst |
|---|---|---|
| first, in-process | 3.119 ms | 16.45 ms |
| second | 3.014 ms | 8.01 ms |

**The second pass costs what the first did.** Nothing underneath is doing the
remembering, so an uncached call pays 5.8 ms on every menu forever rather than
once — which is what makes a memo worth its mutex.
`Include/DisplayNameMemo.h` remembers per catalog generation, which is exactly
the event that makes an answer suspect. The resolver runs outside the lock,
because two menu threads asking for different names must not serialize on a
16 ms call.

The contradiction `12 §W9.1` named is gone. `PackagesCache` said the call "is
timed under its own phase" two screens above the code saying it is "Not wrapped
in a `MenuPerfScope`"; W4's header split had copied both halves across intact.
The surviving explanation is the true one and is a real constraint.

> Incidental finding, not acted on: Windows Terminal's `DisplayName` is the bare
> string `ms-resource:AppStoreName`, not an `@{...}` reference, so
> `resolve_display_name` returns it unresolved. `package.name("WindowsTerminal")`
> answers `"ms-resource:AppStoreName"`. This corroborates `12`'s note that
> reconstructing the indirect form "resolved 2 of 38 packages, so it was not
> worth adding" — but it means the function's answer is wrong rather than
> merely absent for that class. No shipped configuration reaches it.

### W9.2 — the miss-triggered refresh *(`f79a16b`)*

`PackageCatalogService::invalidate()` had no production caller, so a package
installed since the last scan stayed invisible to `package.exists` for the rest
of the five-minute TTL. It needs a gate rather than a plain call, because the
common miss is permanent — a configuration asking about a package this machine
does not have misses on every menu, forever, and un-gated that is a package scan
per menu.

One minute, chosen against what it replaces: `PackageIndex` used a 30 s TTL and
re-scanned immediately on failure, so a miss there caused a re-enumeration at
least twice as often. `MissRefreshGate` takes the clock as a parameter so the
policy is asserted directly rather than by a test that waits a minute, and
`_asked` is a flag rather than `_last != 0` because `GetTickCount64` is small
shortly after boot — comparing `now - 0` against the interval would swallow the
first miss of the session **only near boot**, which is the worst kind of defect
to find.

### W10 — documentation, and one dead range hazard *(`5215609`)*

- **W10.6.** `get_sys()` and `is_native_tracking()` deleted (no callers). The
  hazard was real: `start_sys = 0x5fffffff`, `start_native = 0x60000000`, and
  `get_sys()` returned `sys++` — so its *second* call would have landed inside
  the native tracking range, making a Shell system identifier
  indistinguishable from a mirrored host item's, which
  `complete_host_contract` would then map to a position in a menu it never came
  from. Three `static_assert`s now hold what only a comment held.
- **W10.2.** `HostContract.h` cited the trace harness for send-versus-post.
  The harness cannot show it — `Probe::track` drains its own queue before the
  summary prints. Re-measured directly here (Windows 11 26200 x64, MSVC
  14.44.35207, three runs), recording whether the window procedure ran inside
  `TrackPopupMenu` or only once the caller pumped:

  | | during the tracking call | after it returned |
  |---|---|---|
  | `WM_COMMAND` | 0 | 1 |
  | `WM_MENUCOMMAND` (`MNS_NOTIFYBYPOS`) | 0 | 1 |

  Both posted, `IsMenu(lParam)` true at delivery, despite the page opening
  with "Sent when the user invokes a command from a menu"
  ([WM_MENUCOMMAND](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menucommand)).
  Shell's `PostMessageW` is correct. `fixtures/README.md` now says what the
  traces cannot show.
- **W10.4** orphaned `ProviderResult` comment moved to the function it documents.
- **W10.5** `00-master-plan.md` §3b now names items **9** and **11** as the two
  partials that closed by reclassification rather than by code, with the
  argument for each, so a reader can disagree with the argument instead of the
  arithmetic.
- **W10.7** `09`'s `/analyze` row corrected from "4 lock warnings plus 2 stack
  frames" to the measured **0 across 23 translation units**.
- **W10.8** every cardinality in `09` now references `kNativeScenarios` /
  `kTakeoverScenarios` instead of copying them.

---

## 6. Validation

All re-run on the final tree, `0adb5b1`.

- `build.ps1` on **x64, x86 and arm64** — 33,741 checks / 0 failures / 0
  warnings, `check-invariants: OK (11 rules, 0 deferred)`; arm64 packages and
  skips the suite, as the host is x64.
- **`/analyze`** on `Shell.vcxproj`, `exe.vcxproj` and `ca.vcxproj` — zero
  warnings, nothing suppressed. Down from the audited 6 (which had risen to 6
  from 4 before W7 closed them).
- **hostprobe native** — 23 scenarios, 0 failures, 10 skipped, with the
  `verified against` line present.
- **hostprobe takeover** — 33 scenarios, 0 failures, three consecutive runs,
  with the `takeover:` line naming the build under test. The per-user COM
  override was set and removed by a script with the removal in a `finally`
  block, and its absence asserted afterwards rather than assumed.
- **MSI lifecycle** — ok on all three packages under Windows PowerShell 5.1,
  the edition `build.ps1` and CI use.
- `git diff --check` clean, working tree and `main...HEAD`.
- **CI** — the branch is pushed, so W6.4's workflow runs for the first time.

---

## 7. Two things left as they are, deliberately

**Three files carry a CRLF blob under an `eol=crlf` attribute.**
`docs/menu.json`, `src/tests/test_stringcompare.cpp` and
`src/tools/hash/hash.sln` were committed with CRLF before `.gitattributes`
settled, so git's filter reports them modified whenever the stat cache is
invalidated, while their working bytes are **byte-identical to the committed
blob** (verified). This branch never touched them, and AGENTS.md says never to
normalise files you are not otherwise changing — so they are left alone. They
are not a change waiting to be committed; they are a repository wart worth one
deliberate normalising commit, on its own, when someone decides to.

**W11 / R8 still needs a machine or a person.** Third-party host smoke tests
(Total Commander, Directory Opus, Everything — the `IShellExtInit`/`IContextMenu`
path), the MSI upgrade matrix on clean VMs, and the live-Explorer re-measurement
of R1. W8's lifetime rule makes the third-party host path more load-bearing than
it was, not less: `ThisMenuOnly` costs one re-activation per provider per menu,
~2 ms each, which is stated in `ProviderCache.h` rather than hidden — and §4 is
what happens when that path is only reasoned about.

The takeover harness is the closest proxy available here, and it is now the gate
that has earned the most trust on this branch.
