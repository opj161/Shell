# 11 — QA deep pass: what the first assessment missed

**Second, independent verification pass, 2026-08-26**, against
`refactor/takeover-master-plan` with HEAD at `450985f` and the §09 remediation
still present as uncommitted working-tree changes.

[`10-remediation-qa-assessment.md`](10-remediation-qa-assessment.md) audited the
remediation *diff*. This pass deliberately did not re-read it. It works in three
directions the first pass did not:

- **documentation → code** — take a documented edge condition and go looking for
  the places that fail to implement it, which `AGENTS.md` says is the direction
  that finds the defect nobody suspected;
- **the evidence, not the conclusion** — ask of each cited measurement whether
  the instrument could have produced it;
- **the gate surface itself** — not "did the gates pass" but "what do the gates
  actually run, and where".

Seven findings, of which **three are code-level defects** and **two are gate
gaps that invalidate a large part of the branch's claimed assurance**. One
question the first pass left as a hypothesis is now settled by direct
measurement, in Shell's favour.

Nothing here duplicates §10. Evidence markers as before: **(m)** measured here,
**(c)** checked in the tree with file and line, **(d)** quoted from the vendor's
page.

---

## Summary

| ID | Severity | Area | Finding |
|---|---|---|---|
| **N1** | **High** | R3 | A transient registry failure publishes an *empty* package catalog and serves it for **5 minutes**; the code R3 replaced explicitly refused to do this |
| **N2** | **Medium** | R1 | R1 reordered telemetry writes so that, on a machine above the 32-record cap, the evicted records are now the **most expensive** providers |
| **N3** | **High** | gates | CI runs **none** of the branch's gates — not `check-invariants`, not `hostprobe`, not the MSI validation — and has never run on this branch at all |
| **N4** | Low–Med | docs | §06.3's invariant-script description is still wrong in the sentence R7.7 did not touch: "six enforced rules, two deferred" against **10 enforced, 0 deferred** |
| **N5** | Low–Med | evidence | No trace fixture can distinguish *sent* from *posted*; the harness drains its own queue first. The conclusion R2 drew is nonetheless **correct** — now measured directly |
| **N6** | Low | R2 harness | New harness code reintroduces the fixed-buffer `GetMenuItemInfo` trap that this project already solved twice, ten and thirty lines away |
| **N7** | — | — | Six hypotheses checked and **cleared**, recorded so they are not re-investigated |

---

## N1 — High: a transient registry failure caches "no packages installed" for five minutes

R3 moved `package.exists` / `package.path` / identity / list off `PackageIndex`
and onto the catalog snapshot. It carried the freshness policy across without
carrying the **failure** policy, and the two are not the same thing.

### The mechanism

`scan_package_catalog()` returns an empty result when the enumeration fails
([`PackageCatalogService.cpp:83`](../../src/dll/src/PackageCatalogService.cpp#L83)):

```cpp
RegistryPackageSource source;
std::vector<std::wstring> names;
if(!source.enumerate_full_names(names))
    return out;                       // out.commands and out.packages both empty
```

The worker publishes that result unconditionally
([`PackageCatalogService.cpp:281`](../../src/dll/src/PackageCatalogService.cpp#L281)):

```cpp
auto scanned = scan_package_catalog();
if(_store.publish(std::move(scanned.commands), std::move(scanned.packages),
                  ::GetTickCount64(), token))
    break;
```

`publish` stamps `built_at = GetTickCount64()`, and `stale_locked()` then answers
*fresh* until `_ttl_ms` elapses. That TTL is **five minutes**
(`DefaultTtlMs = 5 * 60 * 1000`,
[`PackageCatalogService.h:109`](../../src/dll/src/Include/PackageCatalogService.h#L109)) (c).

So one failed `RegOpenKeyExW` on `HKCU\…\Package Repository\Packages` produces
five minutes in which:

- `package.exists("WindowsTerminal")` answers **false**, and the stock
  configuration's Terminal item silently disappears from **every** menu
  ([`terminal.nss:8`](../../src/bin/imports/terminal.nss#L8));
- `package.path(…)` answers empty, so its image resolves to nothing;
- every packaged verb is absent too, because `commands` is empty by the same
  return.

### The code R3 replaced refused to do this, in writing

`PackageIndex::ensure_index()` has an explicit branch for it, with the reason
recorded ([`Packages.cpp:387-390`](../../src/dll/src/Packages.cpp#L387) — true at that HEAD; `PackageIndex` was deleted by audit candidate D-01, so this anchor is past end-of-file and the git history of that file is now the record):

```cpp
else
{
    // A transient failure must not be cached forever - packages
    // come and go, and the next menu should try again.
    _state = State::Empty;
}
```

`State::Empty` means the *next* query re-enumerates. And its TTL was **30 s**
([`Packages.h:143`](../../src/dll/src/Include/Packages.h#L143) — true at that HEAD; `PackageIndex` was deleted by audit candidate D-01, so this anchor is past end-of-file and the git history of that file is now the record), not five
minutes. R3 therefore moved package queries from a source that retried
immediately on failure onto one that caches the failure for **ten times longer**
than the old success TTL.

### This is the risk R3 itself named, guarded on the wrong axis

R3 design step 4 says exactly what is at stake:

> If a cold `package.exists()` answered *false*, the stock config's Terminal item
> would vanish from the first menu of every process — a worse defect than the one
> being fixed, and a silent one.

The guard built for it — `snapshot_for_menu()`'s bounded first wait — covers the
**cold** case (nothing published yet). It does not cover the **failed** case,
where something *has* been published and it is empty. The distinction is
invisible to `PackagesCache`, which cannot tell "the machine has no packages"
from "the scan could not read the machine".

### A second, quieter half: partial results publish as authoritative

`enumerate_full_names` returns `true` after breaking out of the enumeration on
**any** status other than `ERROR_MORE_DATA`
([`Packages.cpp:185-197`](../../src/dll/src/Packages.cpp#L185)):

```cpp
auto rc = ::RegEnumKeyExW(key, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
if(rc == ERROR_SUCCESS)
    out.emplace_back(name, cch);
else if(rc != ERROR_MORE_DATA)
    break;
...
return true;
```

`ERROR_NO_MORE_ITEMS` (normal completion), `ERROR_ACCESS_DENIED` and
`ERROR_KEY_DELETED` are indistinguishable at the call site. A failure at index 50
of 200 publishes a **partial** package set that reports success — and, since R3,
that partial set is authoritative for five minutes rather than thirty seconds.

This half is pre-existing; what R3 changed is its blast radius and its duration.

### Why no test catches it

Because nothing tests `PackagesCache` at all, and it cannot be tested as written
(§10 F5): `catalog()` hard-codes `PackageCatalogService::instance()`. The
failure-publish path is reachable in a unit test through `CatalogStore` — the
store is injectable and already has three tests — but no test asserts anything
about what an *empty publish* means to a caller.

### Recommended

Do not publish a scan that failed. `scan_package_catalog()` should report failure
distinctly from "no packages", and `run()` should leave the previous snapshot in
place and re-kick, exactly as `State::Empty` did. If there is no previous
snapshot, publishing empty is correct — that is the cold case the bounded wait
already covers. Add the two `CatalogStore` tests that pin it.

---

## N2 — Medium: R1 made the fixed-size telemetry array evict the *expensive* providers

Both the in-process ring and the cross-process export cap a menu's provider
records at 32 (`MAX_PROVIDERS = 32`,
[`DiagnosticsRing.h:153`](../../src/dll/src/Include/Diagnostics/DiagnosticsRing.h#L153);
`PERF_EXPORT_PROVIDERS = 32`,
[`PerfExport.h:173`](../../src/shared/PerfExport.h#L173)). Overflow increments
`dropped_providers` and the record is discarded (c).

**This machine is above the cap.** Measured by walking every installed package's
`AppxManifest.xml` for a `fileExplorerContextMenus` extension and counting
distinct `Clsid` attributes: **15 packages, 55 distinct context-menu CLSIDs** (m).
The remediation plan's own §R7.5 puts it at "37 in a file menu". Either number
exceeds 32, so on this machine records are dropped from every full menu.

### What changed

Before R1, `session_provider` was called **inline, once per registration, in
registration order** — one call site for the deferral and one for the outcome,
both inside the single walk over `regs`.

After R1 the write order is (c):

1. **Quarantined** — during the planning walk, registration order
   ([`ExplorerCommand.cpp:549`](../../src/dll/src/ExplorerCommand.cpp#L549));
2. **every slow deferral, as a batch, before any provider is called**
   ([`ExplorerCommand.cpp:593`](../../src/dll/src/ExplorerCommand.cpp#L593));
3. budget refusals and `Ok`/`Pending`/`Failed`, during resolution — and R1 makes
   resolution order **cheapest-known-first**, then one exploration, then the
   remaining unknowns, then slow re-probes last.

So when the 32 slots fill, the records that are guaranteed a place are the
quarantined and slow-deferred ones — whose verdict is stable menu to menu and
therefore carries the least new information per menu — and the records evicted
are the **tail of the resolution order**: the most expensive known providers,
the not-yet-sampled ones, and the re-probes.

That is precisely inverted. The reason a user reads this report is to find out
which handler is costing them the menu, and R1 made the cap discard those first.
It also degrades `ProviderHealth`'s own feedback loop, since an unsampled
provider that never gets a record never becomes schedulable.

### The report does not expect this

The overflow *is* printed — that defect was fixed earlier and well
([`src/exe/src/Main.cpp:1172`](../../src/exe/src/Main.cpp#L1172)) — but its
comment reads ([`Main.cpp:1168`](../../src/exe/src/Main.cpp#L1168)):

> The caps were raised at the same time, so this should now be rare — but a
> silent cap is what made it invisible, not the cap itself.

On the reference machine it is not rare; it is every file menu. And the plan's
§2.1 measurement — the entire evidence base for findings B, C and D and for R1 —
was read off a record set that was truncating, without that being noted.

### Recommended

Cheap fix: move the slow-deferral batch to **after** the resolution loop, so
outcome records claim slots first and the stable-verdict records take whatever is
left. Better: reserve a slot budget per outcome class. Either way, note the
truncation when quoting a provider list as a measurement.

---

## N3 — High: CI enforces none of the branch's gates, and has never run on this branch

[`.github/workflows/build.yml`](../../.github/workflows/build.yml) is the only
automation in the repository. It does this (c):

```yaml
- name: Build
  run: msbuild /m /p:Configuration=... /p:Platform=... src/Shell.sln
- name: Run unit tests
  if: matrix.platform == 'x64' || matrix.platform == 'x86'
  run: ${{env.BIN_PATH}}\tests.exe
```

Four consequences, none of them recorded anywhere in `docs/refactor`:

1. **`scripts/check-invariants.ps1` never runs in CI.** It is invoked from
   `build.ps1` and from nowhere else — `rg 'check-invariants'` over `*.ps1`,
   `*.yml`, `*.vcxproj`, `*.props`, `*.targets` finds only
   [`build.ps1:78`](../../build.ps1#L78) and the script itself (c). CI calls
   `msbuild` directly, so it never passes through `build.ps1`. **All ten
   invariant rules are enforced only on a developer's machine** — including
   rule 8, which R2 rewrote and whose gate R2 says it "proved"; rule 9, the
   `SPIF_SENDCHANGE` broadcast ban; and rule 10, `GetState(TRUE)`.
   §06.3 states the opposite: *"landed and wired into `build.ps1`, which runs it
   after every successful platform build and fails the build on a violation"* —
   true locally, and not true of any automated check.
2. **`hostprobe` never runs in CI.** `rg 'hostprobe' build.ps1` and the workflow
   both find nothing (c). So none of the 32 scenarios run automatically —
   including all four `render.*` scenarios, which §09 R4 names as *the only thing
   in the tree that can see a paint regression*, and
   `takeover.a_by_position_host_is_told_which_position`, which is R2's only
   end-to-end assertion.
3. **`validate-msi-lifecycle.ps1` never runs in CI.** §06 §3 lists this as a
   "cheap, immediate" CI addition and marks it *"currently manual only"* — still
   accurate, and still not done.
4. **The workflow triggers only on `main` and pull requests to `main`.** This
   branch is 83 commits ahead of `main` with no PR, so **none of its work has
   ever passed through CI**. Combined with §10 F1 — nothing is committed — every
   gate result claimed for the remediation comes from one developer's local runs
   on one machine, on one Windows build, unreproduced.

### The second tier the documents promise does not exist

[`06-phases-and-tests.md:490`](06-phases-and-tests.md) defers harness execution:

> CI role: builds the probe on all platforms but *execution* requires an
> interactive desktop — run in the scheduled VM job (below), not PR CI.

There is **no scheduled VM job** — not below in that document, not elsewhere in
`docs/refactor`, and not in the repository (c). The gate that the branch's most
delicate work is deferred to has never been written. Every reference to it — and
there are several — reads as though it exists.

### Recommended

Two changes buy most of it back, and neither needs a VM:

- CI should call `build.ps1` (or add an explicit
  `powershell -NoProfile -File scripts\check-invariants.ps1` step plus the MSI
  validation). That makes ten rules and the installer invariants enforceable by
  something other than memory.
- Add the branch to the workflow's trigger list while it is live, or open the PR.

The harness genuinely does need an interactive desktop, so either write the
scheduled job the documents assume, or delete the references and say plainly that
harness execution is a manual pre-merge step with a named owner.

---

## N4 — Low–Medium: the invariant-script description is still wrong where R7.7 did not look

R7.7's mandate was *"Correct the invariant-script description**s**"*. One
sentence was corrected — the string-literal claim. Two paragraphs above it, in
the same section, [`06-phases-and-tests.md:496-499`](06-phases-and-tests.md)
still says:

> **`scripts/check-invariants.ps1` — landed and wired into `build.ps1`** … Six
> enforced rules, two deferred (warn-only) that turn on with their phase:
> `GetState(…, TRUE)` with Phase 1, `SPIF_SENDCHANGE` with Phase 3.

Measured: **`check-invariants: OK (10 rules, 0 deferred)`** (m), and
`$deferredRules = @()` is empty in the script (c). Both rules named as "deferred"
are now enforced — they are rules 9 and 10. So a reader is told that the two
strongest rules on the first-paint path are warnings, when they fail the build.

The corrected sentence also names a function that does not exist (`strip_code`;
it is `Get-CodeText`) — already recorded as §10 F14.

---

## N5 — Low–Medium: no trace fixture can support the send-vs-post claim, though the claim is right

### The instrument cannot make the measurement

[`HostContract.h:42`](../../src/dll/src/Include/HostContract.h#L42) states:

> Posted, not sent, and that is also measured. … the traces show Windows posting
> both WM_COMMAND and WM_MENUCOMMAND after WM_EXITMENULOOP. Sending
> synchronously from inside the hook would put the notification in front of the
> host's own tracking call returning, which is a sequence untouched Windows never
> produces.

`Probe::track` drains its own message queue **after** `TrackPopupMenu` returns
and **before** the harness prints the `= returned` line
([`Probe.h:301`](../../src/tests/hostprobe/Probe.h#L301);
the summary is printed at
[`Scenarios.cpp:646`](../../src/tests/hostprobe/Scenarios.cpp#L646)) (c):

```cpp
MSG msg;
for(int i = 0; i < 200 && ::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); i++)
{ ::TranslateMessage(&msg); ::DispatchMessageW(&msg); }
return result;
```

A **sent** message and a **posted** one therefore land in exactly the same place
in every recorded trace: after `WM_EXITMENULOOP`, before `= returned`. **No
fixture in the suite distinguishes them** — not
`question.notifybypos_reports_a_position`, and not the seven `select.*` traces.
The drain is correct and the comment above it explains why it is needed; the
problem is that a conclusion was drawn from an ordering the drain produces.

This matters more than usual because the tree is diverging from the documented
word: [WM_MENUCOMMAND](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menucommand)
says "**Sent** when the user invokes a command from a menu" (d). `AGENTS.md`
requires that a divergence justified by measurement record "the Windows build,
architecture, toolchain, exact probe and observed result".

### Settled directly, and Shell is right

Throwaway probe, scratchpad `menucmd.cpp`, Windows 11 Pro for Workstations
10.0.26200 x64, MSVC 14.44.35207, `cl /std:c++20 /EHsc /O2 … user32.lib`. A flag
is raised **only after** `TrackPopupMenu` returns; the owner's `WndProc` records
whether it was already up when the notification arrived (m):

```text
[WM_COMMAND, ordinary menu]
  VERDICT: POSTED - arrived only after TrackPopupMenu returned
  wParam = 5002
[WM_MENUCOMMAND, MNS_NOTIFYBYPOS]
  VERDICT: POSTED - arrived only after TrackPopupMenu returned
  wParam = 1        IsMenu(lParam) at delivery = 1
[WM_MENUCOMMAND, MNS_NOTIFYBYPOS - repeat]
  VERDICT: POSTED - arrived only after TrackPopupMenu returned
  wParam = 1        IsMenu(lParam) at delivery = 1
```

So Windows posts `WM_MENUCOMMAND` as well, despite the page saying "Sent", and
Shell's `PostMessageW` at
[`Main.cpp:1439`](../../src/dll/src/Main.cpp#L1439) reproduces Windows exactly —
**including for the message that carries an `HMENU`**, which was the case worth
worrying about. R2's design decision is correct.

The consequence a by-position host must live with is Windows', not Shell's: since
delivery is after the return, a host that destroys its menu on the line after
`TrackPopupMenu` gets a stale `lParam` from *untouched* Windows too. Shell does
not widen that window.

### Recommended

Replace the "that is also measured" sentence with this probe and its numbers, and
add a line to the harness's `fixtures/README.md` saying what the traces cannot
show — the drain makes every post-return delivery look identical, and a future
reader will otherwise draw the same unsupported inference.

---

## N6 — Low: new harness code reintroduces the fixed-buffer `GetMenuItemInfo` trap

`ShellMenu::title_at`, added by R2, reads item text into a fixed buffer
([`ShellMenu.h:213-221`](../../src/tests/hostprobe/ShellMenu.h#L213)):

```cpp
wchar_t buffer[256]{};
MENUITEMINFOW mii{};
mii.fMask = MIIM_STRING;
mii.dwTypeData = buffer;
mii.cch = ARRAYSIZE(buffer) - 1;
if(!::GetMenuItemInfoW(_menu, position, TRUE, &mii))
```

`AGENTS.md` names this exact shape as a live trap — *"`GetMenuItemInfo` truncates
silently to whatever `cch` you passed. Use the documented two-call pattern …
A fixed `MAX_PATH` buffer loses everything past 259 characters, and third-party
extensions cross that line routinely."*

The same project already implements the two-call pattern **twice**, in sibling
headers: [`Probe.h:494`](../../src/tests/hostprobe/Probe.h#L494), whose comment
literally reads *"The documented two-call pattern: ask with dwTypeData null to
learn…"*, and
[`MenuReader.h:296`](../../src/tests/hostprobe/MenuReader.h#L296) (c). Production
code is clean — [`Initializer.cpp:726`](../../src/dll/src/Initializer.cpp#L726)
correctly goes through `read_menu_text`.

**The effect is a weakened assertion rather than a false failure.** `title_at`
supplies both `expected_title` and `replayed_title`, so both truncate at 255 and
still compare equal — meaning two *different* long host titles that share a
255-character prefix would satisfy the scenario. `title_at` reads the **host's
real shell menu**, which is where long third-party titles come from.

No `check-invariants` rule covers this shape, which is why nothing caught it.
Given that `AGENTS.md` documents it as one of the two "silent wrong answer"
families alongside `release(n - 1)` and the `MB_*`/`WC_*` flags — both of which
*do* have rules (rules 6 and 7) — a rule banning `dwTypeData = <array>` alongside
a literal `cch` would be consistent and cheap.

---

## N7 — Checked and cleared

Recorded so the next pass does not spend time on them again. Each was a plausible
defect; each is correct in the tree.

| Hypothesis | Outcome |
|---|---|
| The `release(n - 1)` family has returned | **Clear.** Both surviving sites are guarded before subtracting, each with the citation that explains why — [`Environment.h:71`](../../src/shared/System/Environment.h#L71) (`ExpandEnvironmentStringsW` returns 0 on failure), [`Windows.h:271`](../../src/shared/System/Windows/Windows.h#L271) (`GetUserNameW`'s count includes the terminator, `ComputerName`'s does not, and the two are handled differently and correctly) (c) |
| `ProviderBudget::remaining_us()` underflows once the budget is spent | **Clear.** `return spent >= total_us ? 0u : total_us - spent;` ([`ProviderHealth.h:455`](../../src/dll/src/Include/ProviderHealth.h#L455)) (c) |
| `PackagesCache::Found` hands out a pointer into a snapshot the caller does not hold | **Clear.** `Found` carries the `shared_ptr` alongside the pointer, and all four call sites keep the `Found` alive for the whole use — including `exists()`, where the temporary survives to the end of the full expression (c) |
| A tracking identifier is assigned before `_host_by_position` is known | **Clear.** `build_system_menuitems` reads `MIM_STYLE` at [`ContextMenu.cpp:4329`](../../src/dll/src/ContextMenu.cpp#L4329), inside the `native.root_scan` phase; `prepare_system_item` — which assigns tracking IDs — runs later, per level, from [`ContextMenu.cpp:1082`](../../src/dll/src/ContextMenu.cpp#L1082) (c) |
| R3 narrowed the package set, because the new scan gates on `parse_package_full_name` | **Clear.** The old `PackageIndex` gated on the same parse ([`Packages.cpp:376-380`](../../src/dll/src/Packages.cpp#L376) — true at that HEAD; `PackageIndex` was deleted by audit candidate D-01, so this anchor is past end-of-file and the git history of that file is now the record); the sets are identical (c) |
| Nested native items carry the root handle rather than their own submenu | **Clear in code.** `native_source.menu = hMenu` is stored per level inside `enumerate_native_menu_level` ([`ContextMenu.cpp:3770`](../../src/dll/src/ContextMenu.cpp#L3770)), which is called once per materialised popup. The gap is the missing end-to-end assertion (§10 F9), not the implementation (c) |

---

## How this changes the picture from §10

§10's verdict was *"the engineering is sound and the gates are genuinely green;
the bookkeeping around it is not."* The first half survives — N5 and N7 both
strengthen it, and the by-position replay in particular now has a real
measurement behind its most doubtful decision.

The second half needs correcting in one direction and extending in another:

- **N1 is a real user-visible defect**, not bookkeeping. It is the first thing
  found in this remediation that can silently remove items from a user's menu on
  a machine where nothing is wrong with Shell, and it was introduced by the
  workstream §10 already identified as the weakest.
- **N3 changes what "the gates are green" is worth.** They are green on one
  machine, run by hand, on work that is not committed and has never been through
  any automated check. `check-invariants` — ten rules that exist precisely
  because this codebase keeps rediscovering the same contract violations — has
  no automated enforcement at all, and the second CI tier the documents route
  the harness to was never built.

### Suggested order, merged with §10's

1. §10 F1 — commit the work.
2. **N1** — stop publishing a failed scan; add the two `CatalogStore` tests.
3. **N3** — wire `check-invariants` and `validate-msi-lifecycle` into CI; either
   write the scheduled harness job or delete the references to it.
4. §10 F17 — deploy and re-measure the provider set on a real Explorer. **Read
   `dropped_providers` while doing it** — per N2 it is expected to be non-zero on
   this machine, and the numbers in §09 §2.1 should be re-quoted with that noted.
5. **N2** — move the slow-deferral batch after the resolution loop.
6. §10 F3/F4/F5/F6 — the rest of the R3 cluster.
7. **N5**, **N4**, **N6**, then §10's remaining documentation items.
