# 02 — First-paint latency: async services, stall removals, diagnostics

Governing rule R1: *before first pixel, only bounded local work.* Everything in this
doc either removes synchronous work from the menu thread or makes the remaining work
measurable.

---

## 1. `PackageCatalogService` (process-lifetime, async, persistent)

**Verified problems.**

- ExplorerCommand catalog: 30 s TTL; on expiry the *menu thread* enumerates every
  package, resolves install paths, reads up-to-4 MB manifests, parses them
  (`ExplorerCommand.cpp:17,72-99,106-125`).
- A second package index (`PackageIndex`, `Packages.cpp`) exists for NSS `package.*`
  with TTL+generation freshness — better shaped, but still lets whichever caller hits
  expiry perform the scan synchronously (with CV wait for others), and it is embedded
  in the immutable config snapshot (`PackagesCache` member of `CACHE`,
  `Cache.h:241`): OS state inside a config generation (Audit 1 §13).

**Design.** One process-lifetime service outside any CACHE:

```cpp
class PackageCatalogService {           // src/dll/src/Takeover/PackageCatalogService.h
public:
    // O(1), never scans, never blocks:
    std::shared_ptr<const CatalogSnapshot> snapshot() const;
    void warm_async();                   // called from BootstrapOnce tail
    void invalidate();                   // TTL-driven; shell-change events are heuristic only (see 5.)
    // CatalogSnapshot: explorer-command regs (CLSID + types) AND package identities
    //   (full name/family/version/path stubs) — one snapshot type, two consumers.
};
```

Behavior:

1. **Warm on start.** Worker thread (reuse `TaskbarUiaWorker` pattern: MTA, no windows,
   process lifetime, ticketed requests — `Main.cpp:287-528`) builds the first snapshot
   shortly after bootstrap, not on first click.
2. **Stale-while-revalidate.** `snapshot()` always returns the last good snapshot;
   expiry queues a refresh (coalesced); results publish atomically. A stale catalog is
   benign: activation failures are skipped and recorded (A2§14).
3. **Persist across restarts — measured, and declined (2026-08-24).** Steps 1
   and 2 shipped first, as this said, and then cold start was measured. The
   answer is that persistence has nothing left to buy.

   What it would remove is the wait in `snapshot_for_menu()`: the first menu in
   a process blocks for up to 400 ms if no catalog has been published yet. That
   wait is a phase now — `catalog.first_wait`, present only when it happens —
   and on the coldest right-click reachable on this machine **it never
   happened**. Explorer was killed, the desktop view polled for at 50 ms
   intervals, and a menu raised the moment it existed; three rounds, the view
   appearing 2.67–2.83 s after the kill, and no `catalog.first_wait` in any of
   them. `warm_async()` starts the scan at the tail of `BootstrapOnce`, and on a
   `-treat` machine Explorer asks Shell for a class object well before a user
   can reach a right-click.

   What the cold first menu *does* cost is 58–60 ms of pre-display, of which
   `explorer.commands` is 56 ms: activating twenty-odd packaged verb handlers
   for the first time. An on-disk catalog would remove none of that — the
   catalog says *which* CLSIDs to activate, not what activating them costs. The
   only thing that would help is persisting provider *presentations*, and
   §2a-i declined that on staleness grounds: `GetTitle` and `GetState` genuinely
   depend on the selection.

   So the trade the rest of this step describes — a new on-disk format,
   multi-writer swap logic, fail-closed parsing and a new trust boundary —
   would be paid for a wait that does not occur. **Declined.** Revisit if
   `catalog.first_wait` ever appears in a report, which is now the cheap way to
   find out.

   The integrity note below stays, because it is the rule any future on-disk
   cache in this tree has to satisfy, not just this one.

   **Integrity boundary, stated as an invariant rather than a hardening note.**
   The cache is written under `%LocalAppData%` by a medium-integrity process and
   would be read by *every* host process that raises a context menu — including any
   that runs elevated. A file a medium-IL process can write must never steer
   high-IL COM activation. Invariant: **no field read from the cache may reach an
   activation path without first being corroborated against the live package
   repository**; the cache may only ever make lookup *faster*, never make an
   activation *possible* that a live query would not have authorised. Checksums are
   integrity, not authenticity. This needs a test that feeds a tampered cache naming
   an unregistered CLSID and asserts it is never activated.

   Design if and when it ships: `%LocalAppData%\Nilesoft\Shell\cache\catalog.v2`
   containing per-package: full name, version, manifest filetime+size hash, verb
   registrations, CLSIDs. Fresh Explorer starts warm immediately after a header check;
   background refresh corrects drift. Hardening (QA-08): written atomically via
   temp-file + `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` swap with a versioned header;
   multiple processes (Explorer + third-party hosts) may run the service, so writers
   use the move-based swap and last-writer-wins is safe. The file is parsed as
   **untrusted input, fail-closed**: any anomaly discards it and falls back to scan —
   checksums are integrity, not authenticity, so every cached registration is
   corroborated against the live package repository (subkey exists) before its CLSID
   is ever activated.
4. **Consumers migrate.**
   - `ExplorerCommand.cpp catalog_snapshot()` → `service.snapshot()->commands`.
   - `PackagesCache`/`PackageIndex` → service's identity index; `CACHE::clear()` stops
     touching packages entirely (config reload gets cheaper and conceptually clean);
     NSS `appx.*`/`package.*` read-only wrappers stay in `FuncExpression`.
5. **Change notification.** The documented `SHChangeNotify` event set contains **no
   package-deployment event** (association/folder/file/image/drive events only) —
   QA-08. The TTL remains the primary freshness mechanism (30 s → 5 min once the index
   exists); `SHCNE_ASSOCCHANGED` may be consumed as an opportunistic *hint*, labelled
   undocumented-for-package-lifecycle and probe-gated; an `appx.*` query miss may also
   trigger one coalesced refresh.

Tests: extend `test_packages.cpp` fake-source pattern to the service (inject clock +
fake enumerator; assert `snapshot()` never triggers enumeration; stale-while-revalidate;
persistence round-trip).

## 2. Modern command state policy: never `GetState(TRUE)` pre-paint

**Verified.** Retry exists at `ExplorerCommand.cpp:204-206`. The GetState contract
(fetched): `FALSE` = "should not perform … computations that could cause the UI thread
to stop responding. The verb object should return E_PENDING"; `TRUE` permits them.
Windows 11 guidance says menu-construction methods must stay fast.

Policy:

```text
state = cache.get(provider_identity, selection_shape)   // CLSID/canonical-name hash —
                                                        // NOT the cmd pointer: catalog
                                                        // refresh rebuilds registrations
                                                        // (ExplorerCommand.cpp:106-125)
                                                        // and would dangle a pointer key.
if(state) use it
else:
    hr = GetState(FALSE)
    S_OK      → store, use
    E_PENDING → provisional EXPCMDSTATE_ENABLED, mark provider state_pending,
                record timing in ProviderHealth (§05.1); validate for real at invoke
    failure   → omit item this menu; record failure
```

Never call `GetState(TRUE)` before first paint. Optional later phase: a dedicated COM
STA warmer that refreshes pending providers between menus — explicitly *not* in wave 1
(A1§5 warning about apartment teeth).

## 2a. Provider deadline and deferral (added by the §07 audit — R1a)

Removing the `GetState(TRUE)` retry bounds one call out of five. The rest of
`fill_menuitem_from_explorer_command` is unbounded and runs on the menu thread for
every packaged command:

| Call | Site | Why it is unbounded |
|---|---|---|
| `CoCreateInstance(…, CLSCTX_INPROC_SERVER \| CLSCTX_LOCAL_SERVER, IID_IExplorerCommand)` | `ExplorerCommand.cpp:186-189` | `CLSCTX_LOCAL_SERVER` may **launch a surrogate process**; third-party in-proc servers do arbitrary work in `DllGetClassObject` |
| `GetState(selection, FALSE, …)` | `:204` | `FALSE` asks the handler to be quick; it is guidance, not enforcement |
| `GetTitle`, `GetFlags` | `:206-230` | may hit disk, resources, or the network |
| `GetIcon` + `icon_from_resource` | `:242-247` | resource extraction and rasterisation |

Windows 11's own menu solves this by populating asynchronously. Shell tracks a real
`HMENU`, so it has the same option — but the bounded-and-cached form is far smaller
and lands first:

```text
for each candidate command:
    presentation = catalog.presentation(clsid, selection_shape)   // O(1), no COM
    if(presentation.known)      use it                            // 2nd..Nth menu
    else if(budget_remaining)   resolve on the worker under a deadline
                                  hit  → publish into catalog, use it
                                  miss → omit from THIS menu, keep resolving,
                                         record ProviderHealth.deferred++
    else                        omit; queue for background resolution
```

- **Deadline primitive.** The same one the taskbar path uses and that `AGENTS.md`
  records as the documented answer: a worker thread that owns no windows, and
  `CoWaitForMultipleHandles` on the calling STA so the COM modal loop keeps running
  ("enters the COM modal loop on a single-threaded apartment"). Per-provider budget,
  plus a whole-menu budget so N slow providers cannot sum past the first-paint target.
- **Key** `(clsid, selection_shape)` — never the `IExplorerCommand*`, because a
  catalog refresh rebuilds registrations (`ExplorerCommand.cpp:106-125`) and would
  dangle a pointer key. Same rule as §2.
- **Deferral is visible, not silent.** A command omitted for missing its deadline is
  a `ProviderHealth` event (§05.1) and appears in the Reliability Center as
  "deferred — appeared late", which is the honest description and the feature's most
  useful line.
- **Ordering stays stable.** Cache the resolved presentation against the catalog's
  registration order, so an item that was deferred once does not jump position when
  it appears next time.

### 2a-i. As implemented (2026-08-24) — measured first, and the plan resized

The table above named the calls but not their cost. Probed on this machine
(Windows 11 26200.8875 x64), 23 registered handlers, single-file selection,
doing exactly what `fill_menuitem_from_explorer_command` does:

| | ms on the menu thread |
|---|---|
| first menu in a process | **~700** |
| **every menu after that** | **~170** |
| of which `CoCreateInstance` | ~46 (about 2 ms × 23, *warm*) |
| of which icon extraction | ~32 |
| worst single provider | ~62, warm, every time |

170 ms **per right-click**, before anything is drawn — eleven times the 15 ms
first-paint budget, and larger than the 111 ms packaged-verb scan that `7ee2433`
moved off this thread. This was the biggest remaining item and the plan had it
right in principle; it had the *remedy* wrong in two ways.

**Wrong remedy 1: cache the presentation.** The plan proposed caching
title/state/icon keyed by `(clsid, selection_shape)`. But `GetTitle` and
`GetState` genuinely depend on the selection — a title can embed a filename —
so any shape coarser than the exact selection risks showing a wrong one. The
measurement points somewhere better: **`CoCreateInstance` is the largest single
line and is the one call that takes no selection at all.** Keeping activated
providers alive per thread takes a warm menu from ~170 ms to ~41 ms with *no*
staleness risk, because every menu still asks a live object fresh. That is what
landed, in `ExplorerCommand.cpp`.

**Wrong remedy 2: resolve on a worker under a deadline.** The `IExplorerCommand`
page says "These methods are called on the UI thread"
(<https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorercommand>)
— that is the environment handlers are written against, and moving them to a
thread that owns no windows is a divergence whose blast radius cannot be tested
here. So the calls stay on the menu thread, bounded by a whole-menu budget and
by remembering which providers have never once been quick
(`Include/ProviderHealth.h`).

**The consequence, stated rather than hidden:** a single provider that blocks for
seconds still blocks *one* menu. The plan's acceptance criterion — a fake
provider sleeping 2 s does not delay first paint at all — is not met, and cannot
be without taking the call off the UI thread. What is met is that it delays
exactly one menu and is then skipped, with a re-probe every 200 menus so the
exclusion is not permanent.

**One trap worth recording, because the design was one line from having it.**
On the first menu everything is cold and therefore looks pathological. A policy
that condemned a provider on one slow sample would defer whatever the budget let
it sample, two per menu — and within a dozen right-clicks the menu would contain
no packaged verbs at all, permanently, which is far worse than the latency it
set out to fix. Judgement is therefore on a provider's *best* time and never
before its second sample. `test_provider_health.cpp` simulates the whole
sequence; injecting the one-sample rule fails it 24 times.

Acceptance, restated against what was built: a warm menu costs ~41 ms of
provider work rather than ~170 ms; the first menu in a process is bounded by the
budget instead of costing ~700 ms; a provider that has never been quick is
skipped and recorded as `Deferred` in the ring; and no provider's exclusion is
permanent.

### 2a-ii. The icon path — a leak found, and a cache declined

Splitting the ~32 ms of icon work measured above turned up something worth more
than the milliseconds:

| | |
|---|---|
| `GetIcon`, the COM call | ~18 ms — per selection, not cacheable |
| `SHDefExtractIconW` + `GetIconInfo` | ~11 ms — *is* cacheable by resource string |

**A cache buys 11 ms and is not worth its ownership surface yet.** `BitmapCache`
never destructively evicts precisely so borrowers can hold raw handles; adding a
second class of borrowed bitmap to a field that had just been given clear
ownership is a poor trade for 11 ms. Recorded as measured-and-declined rather
than left as an open item.

**The leak was the real find.** `GetIconInfo` "creates bitmaps for the hbmMask
and hbmColor … The calling application must manage these bitmaps and delete them
with `DeleteObject`"
(<https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-geticoninfo>).
The mask was freed; the colour bitmap was returned and freed by nobody — because
the field it lands in, `menuitem_t::image`, is *borrowed* for every other kind of
item (`MenuItemInfo::FindImage` returns the host's own `hbmpItem`), so a
destructor that deleted unconditionally would have destroyed Explorer's bitmaps.
Nothing deleted anything, and the one path that genuinely owned its handle
leaked it.

Measured: 16 of 23 handlers return an icon, and a probe keeping the colour
bitmap took the process from 4 GDI objects to 164 across ten rounds. **16 GDI
objects per right-click in explorer.exe, against a default per-process limit of
10,000** — on the order of six hundred right-clicks before Explorer starts
failing to draw. Fixed by `Include/IconResource.h` (ownership expressed as a
type) plus an `image_owned` flag on `menuitem_t`.

## 3. Selection array reuse

Keep current preference order but add an assertion/log when the
`SHParseDisplayName` fallback path runs (`ExplorerCommand.cpp:271-274`) — it should be
rare after capture-first selection (§04.5). Any occurrence on the menu thread is a
diagnostics event, not silence.

## 4. Point removals from the menu path (each verified)

| Item | Site | Change |
|---|---|---|
| All-drive Recycle Bin query | `ContextMenu.cpp:4579` (`SHQueryRecycleBinW(nullptr)` during native enumeration) | trust native item disabled state (as fork-assessment §5.4 already argued); if verification needed, cached async refresh |
| `fix_ugly_flicker` vblank Sleep | `ContextMenu.cpp:6052`, called `:6211` in WM_NCCALCSIZE | **Done, see §4a.** Gated on `flicker`, measured at 7.0 ms average per menu, and recorded as its own phase - the cost lands *after* `popup.total_pre_display` stops, so it was invisible to every report |
| `SPI_SETMENUSHOWDELAY` set/restore around menus | set `ContextMenu.cpp:3925`, restore `:4977` with `SPIF_SENDCHANGE` | **Amended (§07 A5).** Keep the feature, drop the broadcast: pass `fWinIni = 0`. See §4b |
| `SPI_SETSELECTIONFADE` off/on | `ContextMenu.cpp:6556/6558` (fWinIni=0, no broadcast) | lowest risk of the three; still remove with the flicker experiment — same A/B gate |
| Per-activation expression evaluation | `Main.cpp:685-805` | replaced by compiled policy (§01.9) |
| Manifest scan on menu thread | §02.1 | replaced by service |

### 4a. The flicker wait: measured, gated, and it was landing where nobody was looking

`fix_ugly_flicker` waits for the next vertical blank before the menu window is
sized. This section asked for it to be registry-gated and benchmarked ON/OFF.
Both are done, and the benchmark turned up something the plan had not
anticipated: **the wait sits after the phase everybody was measuring.**

It runs in `WM_NCCALCSIZE`, which happens inside the tracking call, and
`popup.total_pre_display` is stopped *before* Shell starts tracking. So its cost
was latency the user paid on every menu that no phase recorded and no report
showed. It is a phase now — `menu.flicker_wait`, visible in
`shell.exe -report perf` — and gated:

```
HKCU\SOFTWARE\Nilesoft\Shell    flicker    REG_DWORD    0
```

A/B on one build, a real Explorer, ten menus each, Windows 11 26200.8875 x64 at
59 Hz (2026-08-24):

| | `menu.flicker_wait` | `popup.total_pre_display` |
|---|---|---|
| `flicker = 1` (default) | **7.0 ms avg**, 1.9 – 15.1 ms | 15.8 ms avg |
| `flicker = 0` | 0 ms | 16.0 ms avg |

About seven milliseconds on every menu, capped at one refresh period, with
pre-display unchanged either way. Right-click to pixels is roughly **23 ms with
it and 16 ms without** — so a third of the time before the menu appears was
going to a wait no report mentioned.

**Kept, and default on.** What it buys is a visual artefact during window
creation. A screenshot cannot show a transient, and nothing on this machine can
judge whether the flicker returns without it; deleting a feature on latency
alone when the thing it trades against is unmeasurable is the mistake this
repository's rules exist to prevent. So it is measured, it is switchable, and
the visual half is stated as open rather than assumed either way.

The gate's state is recorded as the phase's count — `n=1` waited, `n=0`
switched off — so a report can tell "this build has no wait" from "this machine
turned it off", which is the one thing somebody comparing two reports needs.

Two things about the A/B that are worth knowing before repeating it. The
registry value has to be written from outside the agent's process tree or
Explorer never sees it (`AGENTS.md`), and the deploy has to restart Explorer
*after* the copy or the measurement is of the previous build. Both of those
silently produced "the setting does nothing" before they were found.

### 4b. `SPI_SETMENUSHOWDELAY`: the defect is the fourth argument, not the feature

The original entry above proposed removing the `showdelay` mutation and offering
instead an opt-in that changes the user's real system setting permanently. That is
*more* invasive than what it replaces, and it misreads the code: the mutation is
already opt-in (it runs only when the config sets `showdelay`,
`ContextMenu.cpp:3915-3927`) and is already restored on close (`:4975-4980`).

The real cost is `SPIF_SENDCHANGE`. It is defined as `SPIF_SENDWININICHANGE`
(SDK `WinUser.h:12778-12780`), and `SystemParametersInfo`'s `fWinIni` parameter
"specifies whether the user profile is to be updated, and if so, whether the
[WM_SETTINGCHANGE] message is to be broadcast to all top-level windows to notify
them of the change"
(<https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfow>).
So every menu open and close broadcasts `WM_SETTINGCHANGE` to **every top-level
window on the desktop**, running each one's window procedure — twice per
right-click, from the menu thread.

Fix: pass `fWinIni = 0`, which is transient-only and is exactly what the adjacent
`SPI_SETSELECTIONFADE` calls already do (`:6551/6553`; those correctly pass the
BOOL in `pvParam`, per the UI-effects table on the same page). Guarded by a
deferred rule in `scripts/check-invariants.ps1`.

## 5. Taskbar: zero-wait, then snapshots

> **Stage 1 was withdrawn by the §07 audit. Do not reinstate it.**
>
> The withdrawn proposal was: on a cache miss, enqueue the request and
> `return false` immediately rather than waiting. It was described here as a
> "pure improvement" that "lands behind nothing". It is neither. `return false`
> means *Windows handles this click* — Shell's menu does not appear — so the
> acceptance criterion ("added latency ≤ native baseline ±2 ms") is met by not
> showing the menu at all.
>
> This repository has already made and recorded this decision. `AGENTS.md`,
> under "The plan is not the specification either":
>
> > One proposal — never blocking the taskbar thread on the UIA worker — **would
> > have broken the first right-click of every sequence**; the documentation
> > supplied the correct primitive (`CoWaitForMultipleHandles`, which enters the
> > COM modal loop on a single-threaded apartment) instead.
>
> That primitive is what `TaskbarUiaWorker::query` uses today
> (`Main.cpp:333`, 250 ms budget). The proposed prewarm does not rescue it: the
> suggested trigger, right-button-down on the taskbar (`Main.cpp:1247`), is *the
> same click*, giving a few milliseconds of head start against a first UIA query
> the code itself measures at ~28 ms (`Main.cpp:277-282`). Hover prewarm misses
> keyboard invocation entirely.
>
> Keep the bounded wait. Stage 2 below is the actual fix, and it makes Stage 1
> unnecessary rather than complementary.

Stage 2 (the whole of this item): replace three `get_Current*` calls
(`Main.cpp:486-488`) with a UIA **cached** request building plain-data rectangles:

```cpp
struct TaskbarTarget { RECT bounds; enum Kind { Background, Start, Tray, Clock, Button } kind; };
std::vector<TaskbarTarget> layout;   // owned by worker, published atomically
```

UI thread does rectangle hit-testing only — no COM, no waiting (UI Automation caching
guidance linked in master plan §2). Merge duplicated taskbar message policy
(`TaskbarProc` vs `TaskbarSubclassProc`, `Main.cpp:1080-1300`) into one
`handle_taskbar_message()` while keeping both attachment mechanisms (subclass for the
XAML bridge child, WndProc swap for legacy tray windows) until coverage proves one
sufficient.

Acceptance (restated after the §07 audit — the old wording could be satisfied by
dropping the menu): **the first right-click of a sequence still shows Shell's menu**,
and with a published layout snapshot the UI thread performs rectangle hit-testing
only — no COM call, no wait. The bounded `CoWaitForMultipleHandles` path remains as
the fallback for a cold layout, and the ring records how often it is taken; that
frequency, not the wait itself, is the number to drive to zero.

### 5a. Measured 2026-08-24 — the rectangle model is wrong, and the wait is not the problem

Stage 2 has two halves, and measurement separated them: one is a behaviour
change disguised as an optimisation, the other is real but small. The probe
walked a 1440-point grid covering the whole taskbar (Windows 11 26200.8875 x64,
1920×48 taskbar, 33 automation elements), asking `ElementFromPoint` at each
point and comparing the answer against a rectangle model built from a cached
subtree request — exactly what the sketch above proposes.

**The rectangle model does not reproduce `ElementFromPoint`.** Two natural
formulations were tried and both are wrong:

| Model | Agreement with `ElementFromPoint` |
|---|---|
| innermost element of the whole subtree containing the point | **49.58 %** |
| innermost element of the `TaskbarFrame` subtree only | **84.17 %** |

The first fails because a `Windows.UI.Input.InputSite.WindowClass` element spans
`(0,1032)-(960,1056)` — an invisible input-site host covering the left half of
the taskbar's top 24 px — which `ElementFromPoint` never returns but which wins
any innermost-rectangle test. The second fails in the direction that costs the
user something: `TaskbarFrame`'s rectangle spans the entire taskbar while the
notification icons live under a *separate* HWND subtree (`TrayNotifyWnd`), so
228 points across the system tray are classified as background. That is Shell's
menu appearing when the user right-clicks a tray icon.

Making it correct would need a hand-curated list of container classes to
exclude — undocumented names, matched literally, breaking silently on the next
Windows update — and a refresh policy, because the rectangles go stale every
time a taskbar button is added or removed. **Declined.** `ElementFromPoint`
stays the authority. A future attempt has to beat 84.17 %, not start from it.

**What the wait actually costs**, over the same grid, out of process:

| | p50 | p95 | first (cold) |
|---|---|---|---|
| `ElementFromPoint` + three `get_Current*` (shipping) | 2.78 ms | 4.28 ms | 18.6 ms |
| `ElementFromPointBuildCache` + cached reads | **2.25 ms** | **3.25 ms** | **3.0 ms** |

So the cached request is worth having, but not for the half-millisecond: it
turns four marshalled calls into one, which means a wedged provider has one call
to hang in rather than four inside the same 250 ms budget. The two forms were
checked to agree on the verdict at **all 1440 points**, and neither returns a
NULL `BSTR` for the `TaskbarFrame`'s empty name — which the shipping guard
rejects on — so the swap is behaviour-preserving. Landed in
`TaskbarUiaWorker::evaluate`.

The rectangle hit-test itself, for reference, costs 0.0002 ms per point against
2.25 ms for the COM call. The prize was never in doubt; the correctness was.

**What was missing is the number this section's own acceptance names.** Nothing
recorded how often the bounded wait is taken, and nothing recorded the outcome
that costs a user their menu — the budget elapsing, after which `query` returns
false and Windows handles the click silently. `Include/TaskbarHitStats.h` counts
all four outcomes (cache hit, answered, timed out, worker unavailable) with one
relaxed atomic increment per right-click, always on. It is deliberately not a
`MenuSessionRecord` phase: the hit-test decides whether there will *be* a menu,
so it runs before any session exists and `session_phase()` would drop it.

Revisit the layout snapshot only if that counter shows the wait being taken
often *and* timing out. Until then the ordering is: cache hit is free, the wait
costs ~2.3 ms, and a rectangle model would trade 15.8 % of the taskbar's area
for it.

## 6. Diagnostics ring (always-on, zero-cost)

Today `MenuPerf` is opt-in because Logger opens/appends/closes per line (AGENTS.md
"Measure before optimising"). Keep phases, change the sink:

```cpp
struct PhaseRecord { uint32_t phase; uint32_t microseconds; uint32_t items; };
struct MenuSessionRecord {
    uint64_t tick; uint32_t host_hash; TakeoverDecision decision;
    PhaseRecord phases[8]; uint8_t phase_count;
    ProviderTiming providers[8]; uint8_t provider_count;   // CLSID hash + µs + result
};
```

Concurrency (QA-09): sessions run on several threads simultaneously (Explorer windows,
taskbar threads), so one shared ring is MPSC, not SPSC. Each session accumulates its
record in **thread-local storage** — a single writer per record — and publishes it into
the process-lifetime ring under one flush mutex at menu close. No locks and no
allocation on the measured path; the ring holds the last 50 sessions. Written records
are flushed after the menu closes to the existing log only when `perf` registry value
is set, or exported on demand by the Reliability Center (§05.1). This is the substrate
for provider health, regression budgets (§06.4), and the "why is my menu slow?" feature
— without file I/O on the measured path.

**As implemented (2026-08-24)**, `Include/Diagnostics/DiagnosticsRing.h`, with
three departures from the sketch:

- **`phases[8]` is too small and silence would be the wrong failure.** The menu
  path already names eleven phases and the hook adds three SEH-safe marks; a
  record that kept eight and dropped the rest would lose exactly the slow ones.
  It is 24, and `dropped_phases` counts what still did not fit.
- **Sessions nest rather than stack.** A menu can open while another is up
  (`TPM_RECURSE`), and the hook can re-enter. Inner sessions fold their phases
  into the outer record and only the outermost publishes — losing the inner
  boundary is better than truncating the outer session or allocating.
- **`phase` is a `const wchar_t *`, not a `uint32_t`.** Every phase name in the
  tree is a string literal with static storage, so a pointer needs no enum, no
  mapping table and no copy. That is what makes recording allocation-free.

Overhead measured rather than asserted (Windows 11 26200.8875 x64): the store
alone **2.7 ns**, a full phase with its two `QueryPerformanceCounter` calls
**47.8 ns**, a whole twelve-phase menu including the publish **~0.6 µs**. The
budget below was 1 µs *per phase record*; an entire menu now costs less than
that. Ring storage is 30 KB, allocated once.

`MenuPerf::enabled()` split into `enabled()` (can we time at all) and
`logging()` (should a breaching phase also be written to the log file). The
registry value now gates only the sink, which was the point.

## 7. Acceptance criteria

- [ ] Cold right-click after Explorer start performs **zero** manifest/package reads on
      the menu thread (asserted in trace harness + debug-mode assertion in
      `catalog_snapshot()`).
- [ ] No code path calls `GetState(…, TRUE)` before first paint (debug assertion).
- [ ] `SHQueryRecycleBinW` absent from menu construction (rg gate in CI script).
- [ ] Taskbar stage-1 lands behind nothing (pure improvement); stage-2 ships with
      layout-snapshot unit tests using recorded fixtures.
- [x] Ring buffer overhead measured < 1 µs per phase record; no allocation after session start.
      **47.8 ns per phase, ~0.6 µs per whole menu**, fixed 30 KB of storage — see §6.
