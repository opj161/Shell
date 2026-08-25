## Updated assessment

The implementation has progressed **substantially** since the previous scan.

One correction to my earlier update: the new HEAD is `450985f`, which is **26 commits after the previous `6b26e61` baseline**, not 28. It is **83 commits ahead of `main`**.

### Overall progress

I would now assess it as:

* **Revised refactor backlog resolved:** **19/20 workstreams = 95%**
* **Literal implementation of the original master-plan architecture:** **~85%**
* **Practical correctness/performance/reliability goals:** **~90–95%**
* **Structural refactor / architecture:** **~80–85%**
* **Completely untouched major workstreams:** **none**

That is a major move from the previous ~65% assessment.

The distinction matters because the project now counts several items as “closed” after measurement showed that the originally proposed solution should **not** be built. That is reasonable. It just means 95% backlog closure is not the same thing as 95% literal implementation.

---

# 1. Item-by-item assessment

| #  | Workstream                                      | Current assessment                                   |
| -- | ----------------------------------------------- | ---------------------------------------------------- |
| 1  | Expression/P0 fixes                             | **Done**                                             |
| 2  | Unified async package + ExplorerCommand catalog | **Partial**                                          |
| 3  | Remove `GetState(TRUE)`                         | **Done**                                             |
| 4  | INIT/UNINIT lifecycle pairing                   | **Done**                                             |
| 5  | HostContract / TPM normalization                | **Done**                                             |
| 6  | LKG config / persisted shadow                   | **Done**, final environment validation remains       |
| 7  | `TakeoverSession`                               | **Resolved by redesign**, class not implemented      |
| 8  | `PopupInterceptionBackend`                      | **Resolved/declined**, interface not implemented     |
| 9  | CoCI policy / conditional attach / router       | **Partial**                                          |
| 10 | Taskbar zero-wait                               | **Resolved by revised design**, not literal original |
| 11 | Recycle/flicker/SPI cleanup                     | **Partial against original invariant**               |
| 12 | Diagnostics ring                                | **Done**                                             |
| 13 | Breaker / native bypass                         | **Done**                                             |
| 14 | Provider health + Reliability Center            | **Done**                                             |
| 15 | Accessibility / keyboard / columns              | **Done / measured alternative**                      |
| 16 | Config watcher                                  | **Done**, including important follow-up fix          |
| 17 | MenuModel / dispatcher / presenter seams        | **Nearly done**                                      |
| 18 | Targeted `moveto`                               | **Done**                                             |
| 19 | Favorites / identity / inspector                | **Done**                                             |
| 20 | Icon cache / memoization / lazy selection       | **Measured and declined**                            |

So, under a stricter implementation-oriented classification:

**12 fully implemented, 4 materially partial, 4 deliberately resolved differently, 0 untouched.**

---

# 2. What has materially progressed since the previous assessment

The last scan found the functional work considerably ahead of the architectural refactor. That gap has narrowed dramatically.

### Reliability Center is now real

Previously, provider health/quarantine existed but the user-facing Reliability Center was missing.

It now exists, including:

* provider diagnosis;
* slow-provider aggregation;
* human-readable provider identity;
* quarantine;
* release;
* persistent state;
* Reliability Center UI;
* command-line support.

A particularly useful follow-up fix also landed: the slowest extension could previously become impossible to quarantine because its reported identity did not map correctly back to the provider. That was fixed in `029b9f8`.

**Item 14 is now genuinely complete.**

---

### Menu architecture has been substantially decomposed

This is probably the largest architectural improvement since the previous assessment.

There is now:

* `MenuModel.h`;
* `SelectionRoute.h`;
* dedicated selection providers;
* menu painting extracted into `MenuPresenter.cpp`;
* DWM/layer compositing extracted with it;
* command/origin information represented in the model rather than parallel vectors.

`MenuPresenter.cpp` is now about **1,652 lines**, while `ContextMenu.cpp` is about **6,110 lines**.

That is not merely file shuffling. The model and selection-route changes remove duplicated representations of the same facts and establish actual seams.

One architectural step remains:

> there is still no actual `Win32MenuPresenter` class.

The extracted functions remain `ContextMenu::` methods and therefore still depend directly on `ContextMenu` private state. The handoff itself correctly identifies this as the **last substantial named architecture boundary**.

So item 17 has moved from “mostly untouched” to **~90% complete**.

---

### Selection handling received a significant real-world performance fix

A 200-file selection exposed a major problem:

* total menu cost: approximately **645 ms**
* Shell's own contribution: approximately **616 ms**

That was traced to redundant selection work rather than the extension initially being blamed.

After the refactor:

* total path dropped to roughly **30 ms**
* `explorer.commands` dropped from roughly **643 ms to 28 ms**

This is one of the strongest pieces of evidence that the refactor is doing useful architectural work rather than simply reorganising code.

The selection refactor also uncovered and fixed another correctness problem: a host whose window resembled Explorer could be classified as Explorer and then end up with no usable selection.

---

### Favorites, stable identity and rule inspector have landed

Item 19 was previously open. It is now implemented.

There is now:

* parser file/line provenance;
* stable menu identity;
* favorites persistence;
* favorite ordering/integration;
* rule-inspection support;
* inspector diagnostics;
* tests covering favorites/provenance.

The remaining `where=` inspector enhancement is optional rather than a missing foundation. The difficulty is legitimate: re-evaluating `where` for the inspector could produce a verdict different from the one that actually composed the menu. Capturing the original verdict during composition is the correct future direction.

So I would consider **item 19 complete**, with one enhancement remaining.

---

### Config live reload is now much more trustworthy

This deserves emphasis because it demonstrates why re-verifying “done” work was valuable.

The config watcher had already been marked complete, but it actually died after **exactly one reload** because the callback could repoint the watcher from its own thread and hit the self-join/restart problem.

Commit `de0216f` fixes that.

Tests now cover:

* callback-triggered watcher repointing;
* subsequent file changes after the first reload.

That turns item 16 from nominally complete into **substantively complete**.

---

# 3. The largest remaining technical gap: first-paint boundedness

This is the biggest place where the master-plan language currently overstates the implementation.

The master plan says:

> R1a: no unbounded third-party call before first paint.

That is **not strictly true today**.

`ExplorerCommand.cpp` still synchronously performs provider calls such as:

* `GetState(selection, FALSE, ...)`
* `GetTitle(...)`
* `GetFlags(...)`
* `GetIcon(...)`

on the menu/UI thread.

The new provider-health system is useful:

* it tracks cost;
* remembers slow providers;
* defers providers subsequently;
* periodically reprobes them;
* caches activated provider objects;
* uses selection shape as part of the health decision.

But it cannot interrupt an individual COM call already in progress.

The refactor docs now explicitly acknowledge this:

> a provider blocking for seconds still blocks **one menu**.

So the actual guarantee is:

**A provider may freeze one first paint, after which Shell learns to defer it.**

That is materially better than before, but it is **not R1a as originally stated**.

Moving those calls to a worker was considered and rejected because `IExplorerCommand` is documented around UI-thread use, which is a defensible compatibility decision.

I would therefore consider this an **accepted unresolved product limitation**, not an accidental unfinished implementation.

---

# 4. Package catalog unification is still genuinely incomplete

This is the clearest conventional implementation gap.

The ExplorerCommand side is in good shape:

* `PackageCatalogService` exists;
* it warms asynchronously;
* manifests/command registration scanning was moved away from ordinary menu composition;
* stale-while-revalidate behaviour exists;
* the service is warmed during bootstrap.

However, the original plan explicitly required this same service to replace the independent package identity cache used by NSS `package()/appx()` expressions.

That has **not happened**.

`Cache.h` still contains:

* `PackagesCache Packages`
* `Packages.clear()`

and `PackagesCache` still owns a `PackageIndex`.

`PackageIndex::ensure_index()` can still synchronously enumerate packages when its index is empty or stale.

`FuncExpression.cpp` continues to access:

```text
cache->Packages
```

for package/appx expression evaluation.

Consequences:

1. NSS `package()` / `appx()` expressions can still trigger package discovery from the menu/config evaluation path.
2. Config reload still clears package state.
3. There are still effectively two package/catalog mechanisms.
4. The master invariant:

> zero manifest/package reads on the menu thread

is therefore **not guaranteed**.

Persistence was measured and reasonably declined because `catalog.first_wait` was not occurring in actual measurements.

But **catalog persistence being declined does not explain away the missing package-index unification**. Those are separate concerns.

So I would mark item 2 **~70% complete**, not closed.

This is probably the most concrete remaining implementation task if the original architecture is still authoritative.

---

# 5. CoCreateInstance work remains intentionally partial

The good part is implemented.

`ComActivationPolicy` now:

* compiles relevant CLSIDs at config publication;
* provides a cheap `may_affect()` path;
* avoids constructing contexts/stringifying/evaluating expressions for irrelevant COM activations;
* fixes ordering around Alt timing/block policy;
* has test coverage.

What remains:

### Conditional detour attachment

The detour is still installed whenever:

```cpp
if(rt.loader.explorer)
```

is true.

It is not installed conditionally based on the compiled policy.

The docs explicitly defer this because the policy does not yet exist at the point where the hook would need to be attached, and dynamically installing an inline detour later while Explorer is active has its own risk.

### Router abstraction

There is no named `TakeoverRouter` consolidating this logic.

So item 9 remains the one item that even the project's own current backlog calls **partial**.

I agree with that assessment.

---

# 6. `TakeoverSession` and interception backend were not implemented as designed

These should not be described as unfinished bugs, but neither should they be described as literally implemented.

### `TakeoverSession`

There is no `TakeoverSession` class.

The class design was abandoned because the hook body uses SEH and MSVC's C2712 restrictions make objects requiring C++ unwinding problematic there.

The intended consolidation was instead implemented through:

* plain-data state;
* lifecycle helpers;
* host-contract planning/completion;
* diagnostics session state;
* breaker state.

This achieves most of the purpose without the class.

**Architectural implementation: no.
Design intent: largely achieved.**

### `PopupInterceptionBackend`

Likewise, there is no `PopupInterceptionBackend` class/interface.

The team concluded that the different interception mechanisms are not actually interchangeable backends in a useful OO sense.

Instead, the implementation now reports which interception mechanism is active.

Again:

**original abstraction not built, operational requirement addressed differently.**

I agree with closing these as design decisions, but they should be labelled “resolved by redesign”, not simply “implemented”.

---

# 7. Taskbar work is good, but the “zero wait” invariant is obsolete

The taskbar path now uses:

* a worker;
* a single `ElementFromPointBuildCache` request;
* cached UIA properties.

Measured latency in the docs is roughly:

* **p50 2.25 ms**
* **p95 3.25 ms**

That is good.

But `TaskbarUiaWorker` still has:

```cpp
BUDGET_MS = 250
```

and waits through `CoWaitForMultipleHandles()`.

So master invariant:

> zero UIA waits on taskbar UI thread

is not literally met.

The original queue-and-return design was tested and withdrawn because it prevented the menu from appearing correctly. The rectangle-based replacement was also measured and rejected.

I regard item 10 as **successfully resolved by evidence**, but the master-plan invariant should be rewritten because it describes an architecture the project deliberately decided not to implement.

---

# 8. Global SPI mutation remains

The original plan says:

> no transient global setting mutation around popups.

The current code still does both kinds of mutation.

`ContextMenu.cpp` still contains:

* `SPI_SETMENUSHOWDELAY`
* `SPI_SETSELECTIONFADE`

The dangerous broadcast side effect was removed: `SPIF_SENDCHANGE` is no longer used. That is a meaningful improvement.

The flicker workaround is also gated and measured at approximately:

**7 ms per menu**

and remains enabled because its visual benefit cannot be established reliably through the automated harness.

But the literal invariant is still false.

In particular, the selection-fade value is still temporarily changed and restored.

Therefore I would mark item 11 **partial**, despite the master table calling it closed.

Either:

1. remove the remaining mutation; or
2. explicitly amend invariant 7 to describe what is actually considered acceptable.

Leaving the invariant and implementation disagreeing invites a future refactor to “fix” an intentional choice or, worse, assume protection that does not exist.

---

# 9. Configuration safety is essentially complete

The config work is now strong:

* persisted last-known-good shadow;
* content/integrity verification;
* corrupt/missing manifest refusal;
* bad new config does not replace good current configuration;
* real Explorer restart validation;
* live watcher reload;
* repeated watcher reload after the recent fix.

Two acceptance cases remain because they require a machine in specific state:

* an invalid-manifest shadow being refused through the full actual-process startup/fallback route;
* a genuinely fresh machine with corrupt stock config reaching the never-loaded refusal path.

There is also an MSI upgrade matrix that needs an appropriate installation state.

These are **validation gaps**, not missing core implementation.

I would call config safety **~95% finished**.

---

# 10. Host-contract fidelity is now strong

This area has moved beyond what I considered open previously.

The hostprobe now exercises the real behaviour rather than just synthetic menu semantics.

Relevant coverage includes:

* native borrowed menu lifecycle;
* INIT/UNINIT pairing;
* `TPM_RETURNCMD`;
* `TPM_NONOTIFY`;
* `MNS_NOTIFYBYPOS`;
* owner-draw behaviour;
* mnemonic semantics;
* native identifier replay;
* shell-composed menus;
* live screen/menu reading.

The repository records:

* **23 native scenarios**
* **31 takeover scenarios**
* **26 consecutive clean harness runs** after fixing a transient harness/lifecycle issue.

The only remaining question is whether any **shipping third-party host** actually uses the non-`TPM_RETURNCMD` path.

That is now a field-survey issue rather than an implementation hole.

---

# 11. Test/verification posture

The current tree contains:

* **59** top-level `test_*.cpp` files;
* **678** `TEST(...)` cases;
* current x64 `tests.exe`, `hostprobe.exe`, and `shell.dll` artifacts built together at the latest baseline;
* the handoff records roughly **32,800 unit assertions/checks**;
* hostprobe documentation records repeated real-menu runs.

I cannot execute the Windows binaries in this Linux analysis environment, so I am not independently claiming that HEAD currently passes the complete Windows suite.

What I can establish from the repository is that the validation surface has grown considerably and, importantly, now includes real Explorer/menu behavioural probing instead of relying solely on unit tests.

That distinction matters because two serious bugs were recently found in code already marked “done” despite the large unit suite:

* watcher stopped after one reload;
* menu-selection/string lifetime behaviour that only surfaced in actual deployment.

The repository's own conclusion here is correct: **real-host verification remains necessary.**

---

# 12. Remaining work, ranked by importance

### High priority

**1. Decide whether the hard first-paint invariants are still requirements.**

There are two violations of the literal R1/R1a model:

* synchronous `package()/appx()` package-index discovery;
* one potentially unbounded `IExplorerCommand` provider call.

If strict bounded-first-paint is a genuine product requirement, these remain architectural problems.

If compatibility is more important and the current mitigations are accepted, rewrite the invariants. Do not keep claiming guarantees the implementation intentionally does not provide.

**2. Finish package-catalog unification.**

Move package identity/query state out of `PackagesCache`/`PackageIndex` into the catalog service and make NSS package queries snapshot-only.

This is the cleanest remaining implementation task with both architectural and latency value.

### Medium priority

**3. Finish the `Win32MenuPresenter` boundary.**

The difficult extraction has already happened. What remains is designing the dependency surface and turning the extracted implementation into an actual presenter component.

**4. Resolve the remaining SPI mutation.**

Especially `SPI_SETSELECTIONFADE`, or formally amend the invariant if there is a measured reason to retain it.

**5. Complete config/installer environment acceptance.**

The two fresh-machine/shadow tests and MSI upgrade matrix.

### Low priority / defer unless evidence changes

**6. Conditional CoCreateInstance attachment.**

Current blast radius is already restricted to Explorer. Dynamically attaching later may introduce more risk than it removes.

**7. Inspector `where=` verdict capture.**

Useful enhancement, not a blocker.

**8. Third-party host survey and visual checks.**

Necessary release validation, but not major engineering backlog.

---

# Final judgement

The refactor is no longer in a state where “most of the architecture is still waiting to happen.”

It is now **late-stage**.

My updated assessment is:

| Dimension                           |  Assessment |
| ----------------------------------- | ----------: |
| Correctness / host fidelity         |    **~95%** |
| Reliability / recoverability        |    **~95%** |
| Config safety                       |    **~95%** |
| Performance work                    |    **~90%** |
| Structural refactor                 | **~80–85%** |
| Original master-plan implementation |    **~85%** |
| Revised backlog resolution          |     **95%** |

The largest change since my previous **~65%** assessment is that the previously missing middle and upper layers are now real: Reliability Center, selection boundaries, MenuModel, presenter extraction, favorites/identity/inspector, repeated config reload, and large-selection performance have all landed.

I would **not** call the implementation 95% complete against the original design, despite the docs' 19/20 tally. The remaining divergence is concentrated rather than broad, but it is substantive: **package unification, strict first-paint guarantees, the final presenter boundary, SPI mutation, and conditional CoCI routing.**

A fair single-number assessment is **~85% implemented overall**, with **no major workstream unstarted and most remaining work concentrated in a handful of explicit architectural/guarantee gaps.**
