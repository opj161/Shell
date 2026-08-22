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
3. **Persist across restarts.** `%LocalAppData%\Nilesoft\Shell\cache\catalog.v2`
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

## 3. Selection array reuse

Keep current preference order but add an assertion/log when the
`SHParseDisplayName` fallback path runs (`ExplorerCommand.cpp:271-274`) — it should be
rare after capture-first selection (§04.5). Any occurrence on the menu thread is a
diagnostics event, not silence.

## 4. Point removals from the menu path (each verified)

| Item | Site | Change |
|---|---|---|
| All-drive Recycle Bin query | `ContextMenu.cpp:4579` (`SHQueryRecycleBinW(nullptr)` during native enumeration) | trust native item disabled state (as fork-assessment §5.4 already argued); if verification needed, cached async refresh |
| `fix_ugly_flicker` vblank Sleep | `ContextMenu.cpp:6052`, called `:6211` in WM_NCCALCSIZE | registry-gated diagnostic flag (default ON initially); benchmark ON/OFF on Win10 22H2 + Win11 current; delete or capability-gate per build. Rationale: UI-thread sleep + timer-resolution side effects (A2§11, canonical link in master plan) |
| `SPI_SETMENUSHOWDELAY` set/restore around menus | set `ContextMenu.cpp:3925`, restore `:4982` with `SPIF_SENDCHANGE` | stop mutating; obey user setting by default; expose explicit opt-in that changes the real user setting transparently (documented), never transient toggling |
| `SPI_SETSELECTIONFADE` off/on | `ContextMenu.cpp:6556/6558` (fWinIni=0, no broadcast) | lowest risk of the three; still remove with the flicker experiment — same A/B gate |
| Per-activation expression evaluation | `Main.cpp:685-805` | replaced by compiled policy (§01.9) |
| Manifest scan on menu thread | §02.1 | replaced by service |

## 5. Taskbar: zero-wait, then snapshots

Stage 1 (small diff, immediate): in `TaskbarUiaWorker::query`
(`Main.cpp:306-357`) replace the bounded wait:

```text
cache hit            → answer now (unchanged)
miss                 → enqueue request, return false immediately
                       (Windows handles this click; answer lands in cache)
```

Prewarm triggers (worker-side): pointer hover over taskbar rects (throttled),
right-button-down on taskbar (existing WH_MOUSE path already sees it, `Main.cpp:1247`),
taskbar recreation/WM_DISPLAYCHANGE (invalidation sites exist `:1152-1166`).

Stage 2: replace three `get_Current*` calls (`Main.cpp:486-488`) with a UIA **cached**
request building plain-data rectangles:

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

Acceptance: zero synchronous UIA waits on any UI thread; p99 taskbar right-click added
latency ≤ native Windows baseline ±2 ms (measured via §02.6 records).

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

## 7. Acceptance criteria

- [ ] Cold right-click after Explorer start performs **zero** manifest/package reads on
      the menu thread (asserted in trace harness + debug-mode assertion in
      `catalog_snapshot()`).
- [ ] No code path calls `GetState(…, TRUE)` before first paint (debug assertion).
- [ ] `SHQueryRecycleBinW` absent from menu construction (rg gate in CI script).
- [ ] Taskbar stage-1 lands behind nothing (pure improvement); stage-2 ships with
      layout-snapshot unit tests using recorded fixtures.
- [ ] Ring buffer overhead measured < 1 µs per phase record; no allocation after session start.
