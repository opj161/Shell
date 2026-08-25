# 10 - Remediation QA assessment

**Revised 2026-08-25 against the updated remediation working tree based on
`refactor/takeover-master-plan` @ `450985f`.**

This document is the independent QA assessment of
[`09-remediation-plan.md`](09-remediation-plan.md). It supersedes the previous
version of this file. The purpose is not to count patches or repeat the latest
agent's completion claims. It is to decide whether the remediation can be
certified as **fully and correctly implemented**, using the current code as the
primary evidence and vendor documentation as the contract where Windows or
Detours behavior matters.

The answer is **not yet**.

The remediation is substantial and most of its architecture is sound, but four
implementation defects remain closure blockers, and several acceptance/gate
requirements in `09` are still incomplete. R8 is also explicitly open.

---

## 0. Executive verdict

### Certification status: **NOT READY TO CLOSE `09`**

The current tree should **not** be described as “all remaining issues/gaps fully
fixed” yet.

The strongest remaining findings are:

1. **HIGH - R6.7:** the Detours thread-enlistment helper still treats a
   mid-enumeration Toolhelp failure as normal exhaustion, so an inline-detour
   transaction can commit without establishing that every still-live process
   thread was enlisted.
2. **HIGH - R1.1:** the provider scheduler has a reachable permanent-starvation
   state after one transient timing spike.
3. **HIGH - R1.1/R1.3:** provider timing is recorded before
   `IExplorerCommand::GetCanonicalName`, so one synchronous provider call on the
   first-paint path is outside the provider's learned cost and attribution.
4. **HIGH - R3:** a package-registry scan failure is converted to an empty or
   partial `CatalogScan` and then published as valid, despite the catalog store
   already having an `abandon_refresh()` path specifically intended to preserve
   stale-good data.

Significant QA/acceptance gaps remain around the hostprobe false-green gate,
nested `MNS_NOTIFYBYPOS` end-to-end coverage, process-lifetime file-store
consistency, the unresolved package `display_name`/refresh policy, and invariant
rule 8's lexical coverage. The full-codebase pass also found a separate
COM-lifetime leak in the thread-local Explorer-command cache that should be
resolved before third-party-host support is treated as robust.

### Workstream status after this re-audit

| Workstream | `09` status | QA status now | Reason |
|---|---|---|---|
| **R0** harness gate | done | **Partial** | Missing operands are fixed, but empty-string operands still silently disable verification/recording/path selection; canonical scenario cardinality is not enforced by the executable. |
| **R1.1** provider scheduler | done | **Not closed** | Permanent-starvation state plus incomplete provider-cost boundary around `GetCanonicalName`. |
| **R1.2** truthful outcomes | done | **Verified at source level** | `Pending`, `DeferredSlow`, `DeferredBudget` are distinct; `GetState(..., FALSE, ...)`/`E_PENDING` handling matches Microsoft guidance. |
| **R1.3** breach reporting | done | **Mostly verified** | Aggregate breach reporting exists, but a slow canonical-name call can be charged to later budget loss without being charged to the responsible provider. |
| **R1.4** budget wording | done, option (b) | **Verified** | Documentation now distinguishes the whole-menu admission budget from the non-interruptible in-flight overrun. |
| **R1.5** selection fallback | blocked on R8 | **Open by design** | Correctly blocked on the large-selection third-party-host measurement. |
| **R2** by-position replay | done | **Implementation verified; acceptance partial** | Core `{position, containing HMENU}` replay matches `WM_MENUCOMMAND`; nested live takeover acceptance requested by `09` is still absent. |
| **R3** package unification | done | **Not closed** | Scan failures publish invalid empty/partial state; `display_name` policy remains unresolved by measurement; miss-triggered refresh is absent. |
| **R4** `Win32MenuPresenter` | done | **Verified at source level** | The class and presenter context now exist and the paint methods are actually presenter methods. |
| **R5** invariant rewrite | done | **Mostly verified** | Main wording is aligned with the revised design; rule 8 can still be bypassed by indirection. |
| **R6** small fixes | done | **Not closed** | R6.7 has a process-safety defect; R6.4's direct-store consistency/testing work is incomplete. |
| **R7** documentation/gates | done | **Partial** | `09` still contradicts itself on 23/31 vs 23/32 and the `/analyze` completion gate is not independently established. |
| **R8** environment validation | open | **Open** | Correctly remains open. |

No evidence found in this re-audit justifies reopening R4 or the core R2 replay
model. Conversely, the previous assessment's “mostly complete” conclusion was
too generous because it did not see the four blockers above.

---

## 1. Scope, method and evidence quality

### 1.1 Repository state examined

The uploaded archive was inspected directly. Its Git base is:

```text
branch: refactor/takeover-master-plan
HEAD:   450985f
```

The remediation is still a **working-tree change set**, not a commit. At the
point of this audit Git reports 47 modified tracked files plus untracked
remediation source/tests/docs, including:

- `docs/refactor/09-remediation-plan.md`
- `docs/refactor/10-remediation-qa-assessment.md`
- `src/dll/src/Include/ExplorerCommandState.h`
- `src/dll/src/Include/ProviderSchedule.h`
- `src/dll/src/Include/Win32MenuPresenter.h`
- `src/shared/DetourEnlistment.h`
- `src/tests/hostprobe/Arguments.h`
- `src/tests/test_detour_enlistment.cpp`
- `src/tests/test_explorer_command_state.cpp`
- `src/tests/test_hostprobe_args.cpp`
- `src/tests/test_provider_schedule.cpp`

`git diff --check` was independently run against this working tree and returns
success. It prints only line-ending conversion warnings, not whitespace errors.

### 1.2 Evidence hierarchy used

Claims in this assessment use this order of authority:

1. **Current source and tests in the uploaded archive.**
2. **Official Microsoft Learn documentation** for Win32/Shell contracts.
3. **Microsoft Detours documentation** from the Microsoft Detours project.
4. **The latest recorded agent session**
   (`2026-08-25-214334-this-session-is-being-continued-from-a-previous-c.txt`)
   as secondary evidence for Windows-only runs I cannot reproduce here.
5. Existing refactor documents, used as requirements/history but not accepted as
   proof that the code satisfies them.

### 1.3 Official contracts re-checked

The following contracts materially affect the findings:

- `Thread32First` / `Thread32Next`: `FALSE` is not by itself proof of clean
  completion. Microsoft documents `GetLastError`, with `ERROR_NO_MORE_FILES` as
  the no-more-thread/snapshot-without-thread-information condition.
  <https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-thread32first>
  <https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-thread32next>
- Microsoft Detours `DetourUpdateThread`: enlisted threads are updated at
  commit; threads not enlisted are not updated. Passing a non-pseudo real handle
  for the current thread is unsupported and may hang.
  <https://github.com/microsoft/Detours/wiki/DetourUpdateThread>
- `IExplorerCommand`: the interface's methods are called on the UI thread and
  should not perform work such as network access that can stop the UI
  responding. `GetCanonicalName` is one of those methods.
  <https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorercommand>
  <https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getcanonicalname>
- `IExplorerCommand::GetState`: with `fOkToBeSlow == FALSE`, the provider should
  avoid expensive work and may return `E_PENDING`.
  <https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getstate>
- `RegEnumKeyExW`: success is `ERROR_SUCCESS`, normal enumeration exhaustion is
  `ERROR_NO_MORE_ITEMS`, and `ERROR_MORE_DATA` means the name buffer was too
  small. Other system error codes are failures.
  <https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regenumkeyexw>
- `WM_MENUCOMMAND`: `wParam` is the zero-based item position and `lParam` is the
  handle to the menu containing the selected item; it replaces `WM_COMMAND` for
  menus whose `MENUINFO.dwStyle` contains `MNS_NOTIFYBYPOS`.
  <https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menucommand>
- `CreateFile`: a share mode of zero means the file cannot be reopened with a
  conflicting request until the handle is closed; a conflicting open fails with
  `ERROR_SHARING_VIOLATION`.
  <https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew>
- `ReplaceFileW`: Windows provides a dedicated API to replace one file with
  another while preserving the replacement semantics in one operation.
  <https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew>
- `SHLoadIndirectString`: package-name resource forms can load the package's
  `Resources.pri`, which is why `package.name` / `appx.name` is not equivalent to
  a cheap immutable snapshot lookup.
  <https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-shloadindirectstring>
- COM reference counting: a copied/stabilized interface pointer held with
  `AddRef` must eventually be balanced by `Release`.
  <https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nf-unknwn-iunknown-addref>
  <https://learn.microsoft.com/en-us/windows/win32/com/memory-management-rules>

### 1.4 What was and was not independently executable here

This audit environment is Linux. It does **not** expose a Windows desktop,
Explorer, MSVC/MSBuild, PowerShell, Wine, or computer-use control. I therefore
cannot honestly claim to have independently rerun:

- the x64/x86/ARM64 Visual Studio builds;
- Explorer/hostprobe takeover integration;
- MSI lifecycle/upgrade validation;
- MSVC `/analyze`;
- live `shell.exe -report perf` measurements;
- R8's third-party-host or visual tests.

I did independently compile and execute the platform-neutral hostprobe argument
parser using its actual header. The empty-string cases described in F5 reproduce.

The latest agent transcript records 33,102 checks on x64/x86, ARM64 packaging,
23 native / 32 takeover scenarios and zero ordinary build warnings. Those are
useful **secondary** records, not results independently rerun by this assessment.

---

## 2. Closure blockers

### F1 - HIGH - R6.7 can still commit a Detours transaction after a partial thread walk

**Affected code:**

- `src/shared/DetourEnlistment.h:49-62`
- `src/shared/DetourEnlistment.h:108-136`
- `src/shared/DetourEnlistment.h:150-225`
- `src/tests/test_detour_enlistment.cpp`

#### What the code does

The new helper correctly fails when `CreateToolhelp32Snapshot` fails and when the
initial `Thread32First` call fails in `enlist_process_threads()`.

The main walk, however, ends with:

```cpp
while(api.thread_next(snapshot, &entry));
```

and returns the current `Enlistment` as `Enlisted`. There is no examination of
the reason `thread_next` returned `FALSE`.

The liveness re-check has the same ambiguity:

```cpp
if(api.thread_first(snapshot, &entry))
{
    do { ... }
    while(api.thread_next(snapshot, &entry));
}
return present;
```

If `Thread32First` fails in the fresh liveness snapshot, or `Thread32Next` fails
before the target thread is reached, `thread_still_present()` can return `false`
and treat “could not establish presence” as “proved gone”.

`InlineDetourApi` has no injected `GetLastError`/enumeration-status seam, so the
focused tests cannot represent “the walk failed after one successful entry”
separately from ordinary exhaustion.

#### Why this violates the remediation requirement

`09` R6.7 explicitly requires aborting on snapshot/enumeration failure and on a
failure to enlist any still-live process thread except the caller.

Microsoft documents that `Thread32First`/`Thread32Next` return `FALSE` for the
terminal condition and expose the terminal reason through `GetLastError`. The
code currently collapses all `FALSE` results after the first entry into the
terminal condition.

Microsoft Detours documents that threads not enlisted in the transaction are not
updated at commit. That is exactly why R6.7 was added.

#### Impact

Low-frequency Toolhelp failure becomes a high-impact process-safety condition:
the transaction can patch code while a process thread was never proven updated.
That is worse than the intended fail-open behavior of leaving the detour
uninstalled.

#### Required correction

1. Give the enumeration abstraction an explicit status or inject
   `GetLastError`.
2. Accept only the documented clean end-of-enumeration condition.
3. Make the liveness check tri-state: **present / gone / unknown**. `unknown`
   must not be treated as proof the thread exited.
4. Abort the transaction on any unknown/failed walk.
5. Add deterministic tests for:
   - main walk fails after at least one successful `Thread32Next`;
   - liveness snapshot `Thread32First` fails;
   - liveness `Thread32Next` fails before finding the target;
   - each path prevents attach/commit and reports failure.

**Closure effect:** R6.7 and therefore R6 remain open.

---

### F2 - HIGH - R1.1 can permanently starve a provider after one transient spike

**Affected code:**

- `src/dll/src/Include/ProviderSchedule.h:137-140, 153-257`
- `src/dll/src/Include/ProviderHealth.h:174-177, 268-337`
- `src/dll/src/ExplorerCommand.cpp:560-615`
- `src/tests/test_provider_schedule.cpp`

#### Reachable state

The scheduler estimates a known provider as:

```cpp
max(best_us, last_us)
```

A provider is classified as “slow” only when it has at least two samples and:

```cpp
best_us > SLOW_PROVIDER_US   // 25 ms
```

Only slow deferrals advance `since_probe`. Budget deferrals deliberately do not.

That produces this liveness failure:

```text
sample 1:  2 ms
sample 2:  2 ms
sample 3: 70 ms transient spike

best_us = 2 ms
last_us = 70 ms
estimate = 70 ms
menu budget = 50 ms
```

On every later menu:

- it is **not slow**, because `best_us` is still 2 ms;
- it is **known**, so the 70 ms estimate is enforced;
- it cannot fit even in a completely unused 50 ms menu budget;
- it is deferred as `DeferredBudget`;
- budget deferral does not advance the slow-provider re-probe counter;
- it never runs again, so `last_us` can never recover.

The provider can therefore disappear for the lifetime of the host process after
one transient spike.

#### Why existing tests do not close it

The scheduler tests verify conservative `max(best,last)` estimation and the
explicit slow-provider re-probe path, but do not test the state where a provider
has a historically fast `best_us` and an unaffordable `last_us`.

`ProviderHealth::consider()` has many tests, but production planning no longer
uses that mutating decision API. Production uses `classify()`,
`plan_providers()`, and the three `note_*` methods. Testing the older policy API
is not a substitute for a liveness test of the actual production workflow.

#### Contract context

Microsoft documents `IExplorerCommand` methods as UI-thread calls. A bounded
admission policy is therefore justified. The defect is not that the scheduler
refuses a predicted 70 ms call. The defect is that the refusal has no bounded
recovery mechanism.

#### Required correction

Introduce bounded rehabilitation for **budget-deferred known providers** as well
as explicitly slow providers. Valid designs include a bounded deferral counter
that forces a probe, or a documented decay/recovery policy for the recent-cost
estimate.

The regression test must pin the exact state:

```text
fast -> fast -> >50 ms spike -> bounded deferrals -> forced retry -> fast -> eligible again
```

Also move scheduler-policy coverage away from dead `consider()` behavior unless
production is changed to use it again.

**Closure effect:** R1.1 remains open.

---

### F3 - HIGH - the provider timing boundary excludes `GetCanonicalName`

**Affected code:**

- `src/dll/src/ExplorerCommand.cpp:627-687`

#### What the code does

The timing starts before activation:

```cpp
auto spent_before = budget.spent_us();
```

Activation and `fill_menuitem_from_explorer_command()` run, then provider health
and diagnostics are recorded:

```cpp
auto cost = budget.spent_us() - spent_before;
health.record(..., cost, ...);
Diagnostics::session_provider(..., cost, ...);
```

Only **after that** does the shown-item path call:

```cpp
cmd->GetCanonicalName(&slot.canonical)
```

#### Why this is significant

The whole-menu `ProviderBudget` clock continues to advance, so a slow canonical
call can consume the remaining admission budget and make later providers become
`DeferredBudget`.

But the provider that caused the cost is still recorded with the earlier,
smaller number. It can remain classified cheap and be scheduled early again on
the next menu. The cost is also absent from the provider-level diagnostic line,
which weakens the Reliability Center/quarantine evidence.

Microsoft's `IExplorerCommand` documentation says the interface methods execute
on the UI thread. `GetCanonicalName` is an `IExplorerCommand` method. There is no
contractual basis for treating it as outside the first-paint provider-call risk.

#### Required correction

Include `GetCanonicalName` in the measured provider interval, or create a
separate provider-attributed phase that is included in scheduling/health cost.
The health record must represent all synchronous provider work performed before
publication of that provider's item.

Add a deterministic accounting test or seam where canonical-name retrieval is
artificially expensive and verify that:

- that provider receives the cost;
- later providers may be deferred by the budget;
- the expensive provider is learned/reported appropriately on the next menu.

**Closure effect:** R1.1 and the provider-attribution part of R1.3 remain open.

---

### F4 - HIGH - failed package scans publish empty/partial state instead of preserving stale-good data

**Affected code:**

- `src/dll/src/Packages.cpp:176-198`
- `src/dll/src/PackageCatalogService.cpp:78-119, 253-293`
- `src/dll/src/Include/PackageCatalogService.h:150-190, 248-299`
- `src/tests/test_package_catalog.cpp`

#### Production scan failure is not represented

`RegistryPackageSource::enumerate_full_names()` currently does:

```cpp
if (RegOpenKeyExW(...) != ERROR_SUCCESS)
    return false;

for (...) {
    auto rc = RegEnumKeyExW(...);
    if (rc == ERROR_SUCCESS)
        out.emplace_back(...);
    else if (rc != ERROR_MORE_DATA)
        break;
}
return true;
```

This conflates distinct Win32 outcomes:

- `ERROR_NO_MORE_ITEMS`: normal completion;
- `ERROR_MORE_DATA`: the supplied name buffer is too small;
- any other system error: enumeration failure.

On a mid-walk error other than `ERROR_MORE_DATA`, the function breaks and
returns `true`, turning a partial list into a successful scan. `ERROR_MORE_DATA`
is silently skipped by advancing the numeric index.

If opening the registry key fails, `scan_package_catalog()` returns a default
empty `CatalogScan` with no failure flag.

#### The worker then publishes the failed result

`PackageCatalogService::run()` unconditionally feeds the scan result to
`CatalogStore::publish()`.

This is inconsistent with `CatalogStore::abandon_refresh()`, whose comment says
exactly why failed refreshes must retain the old snapshot:

> an empty catalog would remove every packaged verb from the menu, which is
> worse than an old one.

The worker never uses that failure path.

#### Additional first-publish event problem

`_published` is documented as “manual-reset: a snapshot exists at last”, but the
worker sets the event after the bounded attempt loop whether or not a publish
succeeded. In the current code an outright registry-open failure is incorrectly
published as an empty snapshot, so the pointer is non-null but semantically
wrong. Separately, if first-ever scans are repeatedly invalidated so all four
`publish()` calls reject their tokens, `_published` can be signaled while
`_store.current()` is still null. Once scanner failures are correctly routed to
`abandon_refresh()`, the same distinction matters for a failed first scan unless
the event semantics are changed. “An attempt ended” and “a usable snapshot
exists” need separate state.

#### Official contract

Microsoft's `RegEnumKeyExW` documentation explicitly distinguishes
`ERROR_NO_MORE_ITEMS`, `ERROR_MORE_DATA`, and other failure codes. The production
loop does not.

#### Impact

A transient registry-open/enumeration problem can replace a valid package
catalog with an empty or partial one. Consequences include packaged Explorer
verbs disappearing and `package.*` / `appx.*` conditions producing wrong
answers until a later refresh succeeds.

#### Required correction

1. Give `CatalogScan` or the scanner an explicit success/error result.
2. Treat only documented normal exhaustion as successful completion.
3. Handle `ERROR_MORE_DATA` by resizing/retrying, or fail the whole scan. Do not
   silently skip an indexed key.
4. On failed refresh call `CatalogStore::abandon_refresh()` and keep stale-good
   state.
5. Separate “a scan attempt completed” from “a usable first snapshot exists”, or
   otherwise fix `_published` semantics so a failed first scan cannot masquerade
   as a published snapshot.
6. Add production-path tests with an injectable package-enumeration source for:
   - registry-open failure;
   - mid-enumeration error;
   - insufficient buffer;
   - stale-good preservation;
   - first scan failure followed by successful retry;
   - first-publish wait/event behavior.

The existing test that directly calls `CatalogStore::abandon_refresh()` proves
the store primitive, not that the production worker takes that path.

**Closure effect:** R3 remains open.

---

## 3. Significant QA and acceptance gaps

### F5 - MEDIUM - hostprobe can still false-green empty operands and does not enforce canonical cardinality

**Affected code:**

- `src/tests/hostprobe/Arguments.h:85-106`
- `src/tests/hostprobe/main.cpp:530-545`
- `src/tests/test_hostprobe_args.cpp`
- `docs/refactor/09-remediation-plan.md` R0/R7/gates

The parser correctly rejects a missing operand and an option token being consumed
as another option's operand. It does **not** reject an empty string.

Using the actual pure parser header, independently compiled in this environment:

```text
--verify ""             -> failed=0, exit=0, verify_dir=""
--record ""             -> failed=0, exit=0, record_dir=""
--takeover --shell ""   -> failed=0, exit=0, shell_dll="", takeover=1
--verify                 -> failed=1, exit=122
```

That matters for automation because an unset/empty variable can still supply an
argument syntactically while disabling the requested verification or explicit
DLL path semantically.

Separately, `hostprobe/main.cpp` rejects only `ran == 0`. It does not itself
assert the canonical unfiltered scenario counts. Accidental scenario deletion or
skipping can therefore still exit zero as long as at least one selected scenario
ran and all that remain passed.

`09` also contradicts itself: its execution status records **23 native / 32
takeover**, while older R0/gate text still says **23/31**.

#### Required correction

- Reject empty operands for `--verify`, `--record`, and `--shell`.
- Add parser tests for all three empty cases.
- Put canonical scenario counts in one source of truth and make the canonical
  unfiltered QA gate verify them explicitly, either in hostprobe or its wrapper.
- Update all stale 23/31 text to the current canonical value, currently recorded
  as 23/32.

**Closure effect:** R0/R7 remain partial until the false-green claim is true.

---

### F6 - MEDIUM - R2 core behavior is sound, but the nested live acceptance case requested by `09` is missing

**Affected code/tests:**

- `src/dll/src/Include/HostContract.h`
- `src/dll/src/ContextMenu.cpp`
- `src/dll/src/Main.cpp`
- `src/tests/test_host_contract.cpp`
- `src/tests/hostprobe/Scenarios.cpp`

The **implementation model is correct**. The current code preserves a native
origin including the zero-based position and the containing `HMENU`, and replay
posts `WM_MENUCOMMAND` with those values. This matches Microsoft's contract:
`wParam` is the selected position and `lParam` is the menu containing it.

The unit suite also has `a_nested_selection_names_its_own_submenu`, which is good
coverage of the pure host-contract result.

The live hostprobe by-position scenario, however, exercises the root case. `09`
R2 explicitly required a by-position harness menu with zero/duplicate IDs that
selects **both root and nested submenu items**, and called out a defect injection
where removing the containing submenu handle must fail the nested case.

That exact end-to-end composition/origin/replay path is still not present.

#### Required correction

Add a takeover scenario selecting an item from a nested host submenu and assert:

- one `WM_MENUCOMMAND`;
- `wParam` is the selected position within that submenu;
- `lParam` is that submenu's `HMENU`, not the root;
- no substitute `WM_COMMAND` is used;
- deliberately replacing the captured containing menu with the root makes the
  scenario fail.

**Closure effect:** R2 implementation may remain “done”, but its stated
acceptance gate is not yet complete.

---

### F7 - MEDIUM - R6.4 still has a concrete FavoritesStore consistency bug and lacks the direct store tests it required

**Affected code:**

- `src/dll/src/Include/FavoritesStore.h:95-124`
- `src/shared/Favorites.h:261-325`
- `src/shared/ProviderQuarantine.h:354-392`
- `src/dll/src/Include/ProviderQuarantineStore.h`

No test under `src/tests` includes `FavoritesStore` or
`ProviderQuarantineStore`. The parser/file-format layers are tested, but the
process-lifetime holders requested by R6.4 are not.

There is also a concrete consistency defect in `FavoritesStore::record_use()`:

```cpp
auto entries = Favorites::load(path);
Favorites::record_use(entries, identity);
auto ok = Favorites::save(path, entries);

std::lock_guard<std::mutex> lock(_mutex);
_entries.swap(entries);
...
return ok;
```

Even if `Favorites::save()` fails, the unsaved local entries replace the
process's in-memory state. This contradicts the function's own comment that the
in-memory copy is refreshed from “what was actually written”.

The race is worse than a lost increment. `Favorites::load()` opens for read with
`FILE_SHARE_READ`, while `Favorites::save()` opens the same file for write with
share mode `0`. Microsoft documents that an incompatible reopen fails with
`ERROR_SHARING_VIOLATION`. `Favorites::load()` maps every open/read failure to an
empty vector, without distinguishing “file does not exist” from “file exists but
is temporarily unavailable”. A concurrent update can therefore produce this
sequence:

```text
writer A opens/truncates the file with share mode 0
writer B's load fails with sharing violation and becomes {}
writer A closes after writing the full list
writer B adds one item to {} and successfully saves afterward
=> B can replace the complete favorites history with one entry
```

The quarantine helpers use the same basic load/`CREATE_ALWAYS` pattern, so a
concurrent CLI/read-modify-write operation has the same class of ambiguity.
This directly contradicts the Favorites comment that races lose “at worst one
increment”.

There is a second stale-cache race in both process-lifetime holders: `reload()`
reads file contents, then later samples the path's write time. If the file is
replaced between those operations, old contents can be paired with the new
stamp. The cache then has no reason to refresh again until another write occurs.

Both Favorites and Quarantine continue to overwrite via `CREATE_ALWAYS`. R6.4
explicitly required a deliberate decision and fault-injection coverage for
atomic temp+replace behavior, rather than silently leaving two independent
implementations.

#### Required correction

- Give file loading a result that distinguishes missing, success, and transient/
  hard failure. Never mutate-and-save from an empty list produced by an I/O
  failure.
- Serialize the same-process Favorites load-modify-save transaction.
- Update in-memory Favorites state only after a successful durable write, or
  reload the actual file after failure.
- Make reload stamp/content acquisition coherent, for example by taking metadata
  from the opened file or retrying if the path changed while it was read.
- Add direct real-file tests for both process-lifetime stores, including sharing
  violations, stale refresh and same-process concurrency.
- Make and document one consistency decision for Favorites and Quarantine. If
  atomic replace is required, use a shared implementation around a temporary
  file plus an appropriate Windows replacement API and fault-injection tests.

**Closure effect:** R6.4 and therefore R6 remain open independently of F1.

---

### F8 - MEDIUM - R3's `display_name` exception and miss-triggered refresh are still not closed on their own acceptance terms

**Affected code:**

- `src/dll/src/Include/Cache.h:50-113, 127-176`
- `src/dll/src/PackageCatalogService.cpp`
- `docs/refactor/09-remediation-plan.md` R3.5/R3.6/R8

The important package unification **did** land for existence, identity, list and
path queries. Those now read the published catalog snapshot instead of owning a
second package index. `CACHE::clear()` no longer throws that package state away.

Two requested decisions remain:

#### `display_name`

`PackagesCache::display_name()` still constructs a `RegistryPackageSource` and
calls `resolve_display_name()` synchronously. Microsoft documents package forms
of `SHLoadIndirectString` as extracting from the package's `Resources.pri`.

`09` explicitly allowed this only as a **measured, named compatibility
exception** or asked for asynchronous name caching. R8 still says the cold/warm
PRI/MrtCache measurement is open. Therefore R3.5 cannot simultaneously be “done”
on its stated acceptance basis.

There is also a local documentation contradiction: the top comment says the
operation is timed under “its own phase”, while the implementation comment says
there is deliberately no `MenuPerfScope` and the cost is attributed to the
surrounding expression phase.

#### Miss-triggered refresh

`09` R3.6 says a package-query miss may request one coalesced refresh with TTL as
the backstop. `find_entry()` currently reads the snapshot and returns. No query
miss calls `PackageCatalogService::invalidate()`/kick, and the search of
production call sites finds no package-query invalidation path.

TTL therefore provides eventual freshness, but the requested miss-triggered
refresh was not implemented or explicitly superseded by evidence.

#### Required correction

- Complete the R8 cold/warm `package.path` / indirect `appx.name` measurement and
  make the `display_name` decision explicit.
- Make the documentation/instrumentation describe the actual timing phase.
- Implement coalesced miss-triggered refresh as specified, or amend R3 with
  measured reasoning for choosing TTL-only behavior and tests that pin it.

**Closure effect:** R3 remains open even after F4 is repaired until its own
exception/refresh acceptance is resolved.

---

### F9 - LOW–MEDIUM - invariant rule 8 is still lexical, not semantic

**Affected code:**

- `scripts/check-invariants.ps1:108-117`

The revised rule correctly stopped banning every occurrence of
`MNS_NOTIFYBYPOS`, because reading that flag from the host's borrowed menu is
required.

Its current regex catches direct shapes such as:

```text
SetMenuInfo(... MNS_NOTIFYBYPOS ...)
dwStyle |= MNS_NOTIFYBYPOS
```

but can be bypassed by ordinary indirection, for example assigning the flag to a
local variable and later assigning that variable to `dwStyle` before
`SetMenuInfo`.

This is **not** evidence that the current R2 implementation is wrong. It means
the anti-regression gate is weaker than the document's wording implies.

#### Required correction

Either strengthen the invariant check to the codebase's actual mutation idioms,
or move this contract into a focused unit/source test whose failure is less
regex-shape-dependent. Keep the current positive ability to read
`MNS_NOTIFYBYPOS` from a borrowed host menu.

---


### F10 - MEDIUM - the thread-local Explorer-command cache leaks one COM reference per cached provider when a menu thread exits

**Affected code:**

- `src/dll/src/ExplorerCommand.cpp:75-137`

The provider-object cache is intentionally `thread_local`, which is the correct
shape for avoiding cross-apartment reuse of `IExplorerCommand` pointers. The
lifetime handling is not correct for a thread that ends while the host process
continues.

On first activation the cache takes an extra reference:

```cpp
auto cmd = activate_explorer_command(clsid);  // caller reference
cmd->AddRef();                                // cache reference
cache.push_back({ clsid, cmd });
```

`CachedProvider` stores a raw pointer and has no destructor. The thread-local
`std::vector` itself is destroyed at thread exit, but destroying raw pointer
elements does not call `IUnknown::Release`. The comment explicitly chooses not
to release the cached references and assumes “a released process is a released
process”. That assumption holds for Explorer's long-lived menu/UI thread, but
not for an arbitrary host that creates shell menus on transient worker/UI
threads. Every such thread can leave a set of provider COM objects permanently
referenced and unreachable until process exit.

Microsoft's COM reference-counting rules say a stabilized/copied interface
pointer retained with `AddRef` must be released when that reference is no longer
needed. The current cache intentionally does not balance its cache `AddRef` on
thread exit.

#### Impact

In Explorer this is effectively process-lifetime retention and may be an
acceptable scoped trade. In third-party hosts with repeated transient menu
threads it becomes an unbounded per-thread resource/object leak, potentially
retaining provider DLLs and provider-owned resources. R8 explicitly includes
third-party file managers, so the assumption cannot be left implicit.

#### Required correction

Choose and document a host/thread lifetime policy rather than leaking by
construction. Practical options include disabling the persistent provider cache
outside known long-lived menu threads, or introducing an explicit cache owner
whose COM references are released while the creating apartment is still valid.
Do not simply add a TLS destructor that calls arbitrary apartment-bound COM code
without establishing where that destructor runs relative to the host's
`CoUninitialize`.

Add a Windows test/probe that creates and tears down a menu thread repeatedly
against a fake counted `IExplorerCommand` and proves cached references do not
accumulate after the supported thread/apartment lifetime ends.

**Closure effect:** this is a full-codebase robustness finding adjacent to R1.1.
It is not the reason R1.1 is already open, but it should be resolved or explicitly
scoped before third-party-host support is certified.

---

## 4. Release/process gates that are still open

### 4.1 The remediation is not committed

All remediation examined here is still layered on committed HEAD `450985f` as
modified/untracked working-tree state.

That is not a runtime defect, but it is a reproducibility blocker:

- no immutable commit identifies the exact source claimed to have passed the
  Windows gates;
- `main...HEAD` does not contain the remediation;
- untracked new source/test files can be omitted accidentally;
- a later build can no longer prove it exercised the exact state described by a
  transcript unless that state is committed or otherwise content-addressed.

Before certification, commit the remediation and rerun the gates against that
commit hash.

### 4.2 MSVC `/analyze` status is **unverified**, not proven clean or proven dirty

The previous version of this QA file asserted outstanding `/analyze` warnings.
The later implementation session records “0 warnings”, but the same remediation
plan explicitly warns that ordinary compilation's zero warnings does **not**
prove `/analyze` ran.

This environment cannot run MSVC `/analyze`, and no retained final analyzer log
was available that resolves the conflict.

Correct QA status:

> **UNVERIFIED.** Run x64 MSVC `/analyze` on the final committed production
> projects and retain/record the actual result. Do not inherit either the old
> warning count or the later ordinary-build zero without evidence.

### 4.3 R8 is still open

`09` itself leaves these environment/person-dependent checks open:

- end-to-end shadow refusal in the required fresh/corrupt machine states;
- MSI upgrade matrix on clean VMs;
- whether shipping third-party hosts use the non-`TPM_RETURNCMD` path;
- real file selections, including large selections, in Total Commander,
  Directory Opus and Everything-class hosts;
- cold/warm package path and indirect package-name/PRI/MrtCache timing;
- visual flicker-workaround and inspector-tooltip verification.

R1.5 explicitly depends on the large-selection measurement. R3.5 explicitly
depends on the package-name measurement. Those are not optional bookkeeping if
`09` is to be marked completely closed.

### 4.4 Canonical scenario count documentation is inconsistent

The current status block in `09` says 23 native / 32 takeover. Older acceptance
text in the same document still says 23/31. The latest session also records
23/32.

One canonical count must be defined centrally and enforced by the gate. Until
then, documentation cardinality cannot itself prove a complete harness run.

---

## 5. Areas re-verified with no critical/significant defect found

This section matters because the purpose is not to reopen working code merely
because it was changed.

### 5.1 R1.2 command-state handling

Source inspection confirms the remediation uses `GetState(..., FALSE, ...)` and
has explicit provisional handling for `E_PENDING` rather than retrying with
`TRUE`. That matches Microsoft's contract for avoiding expensive UI-thread work.
`Pending`, slow deferral and budget deferral are distinct diagnostic outcomes.

No significant defect was found in that classification model.

### 5.2 R2 core by-position replay

The current code reads the host's `MNS_NOTIFYBYPOS` style, maps internal tracking
identifiers back to native origins, preserves the containing menu, and posts
`WM_MENUCOMMAND` using `{position, menu}`.

That matches the official `WM_MENUCOMMAND` contract. The remaining F6 is an
end-to-end nested acceptance gap, not a finding that the current model is
incorrect.

### 5.3 R4 presenter boundary

`src/dll/src/Include/Win32MenuPresenter.h` now defines `PresenterContext` and
`Win32MenuPresenter`; the extracted draw/measure/layer methods in
`MenuPresenter.cpp` are actual `Win32MenuPresenter::` methods.

The previous architectural finding “there is no Win32MenuPresenter class” is
stale and must not be repeated.

### 5.4 ConfigWatcher follow-up fix

The previously found “watcher dies after one callback-driven repoint” problem has
source/test remediation in the current tree. No new closure-level defect was
found in that path during this pass.

### 5.5 Large production stack frames

The two ~64 KiB fixed buffers identified in the remediation plan were moved off
the stack. The remaining requirement is the final `/analyze` gate, not evidence
that those two original stack-frame defects remain.

### 5.6 Diagnostics wire version/outcome split

The changed provider-result semantics are accompanied by a perf-export version
bump and exact-version handling. Slow and budget deferrals are no longer
collapsed into one word. No significant serialization/versioning defect was
found in this pass.

### 5.7 Quarantine path override race

`ProviderQuarantineStore::reload()` now reads `_path_override` while holding the
mutex. The specific R6.4 path-override race called out in `09` is fixed. F7 is
about the broader direct-store tests/consistency contract, not this already fixed
race.

### 5.8 Formatting gate

`git diff --check` is clean in this archive apart from Git's line-ending
conversion warnings. The previous QA finding that the remediation still failed
that gate is stale.

---

## 6. Corrections to the previous `10-remediation-qa-assessment.md`

The old assessment contained useful observations but should not remain the
project's current QA record.

| Previous finding | Revised disposition |
|---|---|
| F1 uncommitted remediation | **Keep**, but classify as release/reproducibility gate rather than product runtime defect. |
| F2 dead `ProviderHealth::consider()` | **Fold into F2.** The important issue is that many tests exercise a non-production policy path while the production scheduler has an uncovered liveness defect. Dead-code cleanup by itself is not a closure blocker. |
| F3 display-name memoization | **Fold into F8.** The material issue is unresolved synchronous PRI/resource policy and missing acceptance evidence, not memoization as an end in itself. |
| F4 contradictory package comment | **Fold into F8.** Correct it, but do not present prose drift as equal to runtime correctness defects. |
| F5 `PackagesCache` testability | **Fold into F4/F8.** Production scanner failure injection and policy acceptance are the consequential gaps. |
| F6 miss refresh dropped | **Keep as F8.** Still supported by source search. |
| F7 `/analyze` warning count | **Replace.** Current evidence conflicts. Correct status is **unverified pending retained final `/analyze` output or a fresh run**. |
| F8 dead R2 helpers | **Omit from significant findings.** Cleanup only unless a live caller/behavior risk is established. |
| F9 nested by-position E2E | **Keep as F6.** Core implementation remains correctly scoped as verified. |
| F10 R6.4 partial | **Strengthen as F7.** There is a concrete failed-save/in-memory consistency defect, not only missing tests. |
| F11 weak invariant rule 8 | **Keep, downgraded to low–medium.** Gate weakness, not current functional failure. |
| F12/F13 tally/count drift | **Consolidate into R0/R7 release-gate finding.** Current canonical evidence is 23/32, while `09` still contains 23/31. |
| F14–F16 cleanup/doc issues | **Omit from closure-level findings.** They dilute the critical signal unless they become behaviorally relevant. |
| F17 live R1 measurement | **Move to R8/release validation.** It remains necessary, but it is not the only reason R1.1 is open; F2/F3 are code defects. |

### Newly added findings not present in the previous assessment

The previous QA missed the most important remaining runtime defects:

- **F1:** mid-walk Toolhelp failure can fail open in Detours enlistment;
- **F2:** scheduler permanent starvation after a transient expensive sample;
- **F3:** `GetCanonicalName` sits outside provider health/diagnostic timing;
- **F4:** failed package scans publish empty/partial snapshots rather than using
  stale-good preservation.

Those change the final certification decision.

---

## 7. Required remediation order

Fix in this order because it reduces process-safety and first-paint correctness
risk before polishing the acceptance surface:

1. **F1 Detours enumeration failure semantics.** Add tri-state/last-error-aware
   enumeration and prove transaction abort on every unknown/partial walk.
2. **F2 scheduler liveness.** Guarantee bounded retry for budget-deferred known
   providers and add the transient-spike regression.
3. **F3 provider accounting boundary.** Include canonical-name work in provider
   cost and diagnostics.
4. **F4 package scan failure semantics.** Publish only complete successful scans;
   preserve stale-good data; fix first-publish event semantics and production
   failure tests.
5. **F5 hostprobe gate.** Reject empty operands and enforce one canonical
   unfiltered cardinality.
6. **F6 nested R2 hostprobe case.** Add real nested containing-menu replay proof.
7. **F7 store consistency/tests.** Serialize same-process Favorites update and
   settle the shared write-atomicity policy with tests.
8. **F8 package exception/refresh.** Complete the R8 measurement, make the name
   policy explicit, and either implement or formally supersede miss-triggered
   refresh.
9. **F9 invariant hardening.** Strengthen the regression gate without banning
   the required borrowed-menu style read.
10. **F10 COM-cache lifetime.** Define a supported thread/apartment lifetime and
    stop losing the cache references when transient host threads end.
11. **Commit the remediation**, then rerun every Windows gate against that exact
    commit, including `/analyze` and R8 where the required environment exists.

---

## 8. Acceptance checklist before `09` may be marked fully complete

### Code-level blockers

- [ ] Toolhelp thread walk distinguishes clean exhaustion from failure throughout
      the main walk and liveness re-check.
- [ ] Every non-quarantined provider has bounded scheduler liveness after budget
      deferral, including the fast-fast-spike recovery case.
- [ ] All synchronous pre-publication `IExplorerCommand` work, including
      `GetCanonicalName`, is included in provider cost/attribution.
- [ ] Package scanner has an explicit success/failure result and never publishes
      a failed/partial refresh as a valid catalog.
- [ ] First-package-snapshot signaling remains truthful after first-scan failure.

### QA/gate completeness

- [ ] Hostprobe rejects empty path/directory operands.
- [ ] Canonical unfiltered hostprobe cardinality is enforced from one source of
      truth and all docs agree on it.
- [ ] Nested by-position takeover scenario proves the containing submenu handle.
- [ ] FavoritesStore and ProviderQuarantineStore have direct real-file holder
      tests; Favorites failed-save state remains consistent with disk.
- [ ] Package display-name policy is settled by the R8 measurement and its actual
      timing phase is documented.
- [ ] Package miss-refresh behavior is either implemented/tested or explicitly
      superseded with evidence.
- [ ] Invariant rule 8 has an anti-regression test strong enough for the mutation
      forms the codebase permits.
- [ ] Provider-cache COM references have a bounded, documented lifetime for
      supported third-party-host thread/apartment patterns.

### Final reproducibility/environment gates

- [ ] Remediation is committed and the tested commit hash is recorded.
- [ ] `build.ps1 -Platform all` succeeds on the final commit with zero ordinary
      build warnings.
- [ ] x64 MSVC `/analyze` is rerun and its actual final output is retained/triaged.
- [ ] Invariant checker is green.
- [ ] Native and takeover hostprobe runs are green at the canonical enforced
      counts and identify the DLL under test.
- [ ] MSI lifecycle/upgrade gates required by the touched scope are green.
- [ ] R8 environment/person checks are completed, including the R1.5 large-file
      selection measurement and R3.5 package-name measurement.
- [ ] `git diff --check` is clean on the final commit.

---

## 9. Final assessment

The remediation session fixed a large fraction of the original gaps correctly.
In particular, the presenter boundary now exists, command-state handling is much
more faithful, by-position replay has the right data model, package identity/path
queries were unified onto a published catalog, diagnostics became more truthful,
and several real previous defects received focused tests.

That does **not** justify closing `09` yet.

At this snapshot there are four direct remediation blockers that can produce
unsafe patching, permanent menu-item loss, misleading provider admission
learning, or wrong package-catalog state. The wider pass also found persistent
store-race/data-loss risk and an unbounded COM-reference lifetime on transient
host threads. Those are not documentation nits and are not satisfied by the
recorded 33k-unit-check headline. The QA harness also retains false-green shapes,
and R8/R1.5/R3.5 remain explicitly open.

**Certification decision: `09-remediation-plan.md` is substantially implemented,
but not fully remediated and not ready to be declared complete.** Close it only
after F1–F4 are fixed with the named regressions, F5–F8's acceptance work is
completed or explicitly superseded by evidence, and the final committed state is
re-run through the Windows-specific gates.
