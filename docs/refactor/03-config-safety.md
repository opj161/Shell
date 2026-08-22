# 03 — Configuration safety: last-known-good, watcher reload, error UX

A typo in `shell.nss` must never disable the shell. Today it does.

---

## 1. Verified current behavior (the bug)

- `Initializer::init()` (`Initializer.cpp:62-121`) parses into a fresh `CACHE` and
  publishes `_snapshot` **only on success** (`:101-104`) — the transactional part is
  right, and the previous good snapshot remains in memory.
- But on failure it sets `Status.Error = true` (`:81`), and:
  - `Initializer::query()` refuses menus while `Status.Error` is set
    (`:154-156`, gate `ch < 2`);
  - `ContextMenu::Initialize` bails via `initializer->query()`
    (`ContextMenu.cpp:4783-4790`);
  - `DllGetClassObject` returns `CLASS_E_CLASSNOTAVAILABLE` when `has_error()`
    (`Main.cpp:1468-1469`).

Net effect: the last valid snapshot is *physically present* but deliberately unused;
the context menu disappears system-wide until the user fixes the file **and manually
triggers reload**. There is no automatic recovery today: `has_error` defaults to
`detect_changes = false` (`Initializer.h:42`) and all eight call sites
(`Main.cpp:891,1120,1129,1132,1211,1220,1227,1468`) use that default, so
`config_has_changed()` (`Initializer.cpp:179-207`) is unreachable dead code (QA-05).
The only escape is the Shift+Ctrl+right-click reload combo, whose `query(2)` bypasses
the gate at `:154-156`. Any poll-based recovery in this plan is therefore **new
wiring** (§3), not an existing property being preserved.

## 2. Design: three-state model

```text
Loaded         newest configuration valid          → normal operation
StaleWithError newest attempt failed; previous      → keep serving _snapshot
               generation stays active; surface     + degraded banner hook for UI
               error once per failed parse          + retry on file change (new, §3)
Disabled       explicit user disable                → uninit (existing)
```

Minimal diff:

1. In `init()` failure path: do not clear/replace `_snapshot`; set a new flag pair
   `Status.Error` (kept) + `LastError` (already exists, `:106`) with parser message +
   location. Introduce `Status.Stale == true` when serving a snapshot older than the
   failed attempt.
2. `query()` change: if `Status.Error && _snapshot != nullptr && !Disabled` → serve the
   stale snapshot (return true) instead of refusing. Keep refusing only when no
   snapshot has ever loaded.
3. `DllGetClassObject` gate (`Main.cpp:1468`): allow through when a stale snapshot is
   available; block only when never-loaded or Disabled.
4. Error surfacing without a menu: first failed parse after a previously-good load may
   show one balloon/notification via the existing manager EXE (`shell.exe` already owns
   UX surfaces); subsequent failures are silent until the file changes. Never spam.
5. The existing keyboard reload combos (`Initializer::OnState`, `:786-859`) keep
   working unchanged — they map onto the same `query()/init()` machinery.

Resulting behavior: save-with-typo → previous menu keeps working; fix file → watcher
(or, until §3 lands, the manual reload combo) publishes the new generation;
`Status.Refresh` path unchanged.

Tests: new suite `test_initializer_lkg.cpp` driving `init()` against fixture configs
(valid → invalid → valid), asserting snapshot identity (generation counter) and
`query()` verdicts. Uses real temp files (repo convention: real objects over mocks).

## 3. Config watcher (auto-reload outside the right-click path)

Replace timestamp-polling-on-menu-attempt (`config_has_changed`,
`Initializer.cpp:179-207`) as the *primary* trigger with a directory watcher:

- `ReadDirectoryChangesW` on the config's parent directory, `FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME`,
  watching `shell.nss` + every imported file's directory. Imported paths come from the
  parser's lexer stack: each active `_imports` entry carries its real path
  (`imp->path`, used by cycle detection at `Parser.cpp:1165-1172`) — **not** from
  `m_imports`, which stores only path hashes (`Parser.cpp:1148-1161`, QA-06). The
  watcher collects the distinct parent directories of `_imports` entries at publish
  time; if that proves too broad, extend the parser to record import paths explicitly
  during parse (small, additive).
- Worker thread (same pattern as catalog worker): debounce 250 ms, then run exactly the
  existing `init()` publish path. LKG semantics from §2 make this safe: bad save keeps
  old menu, watcher retries automatically on next save event.
- Watcher is best-effort: on failure fall back to manual reload combos silently
  (timestamp-poll-on-attempt is *not* reinstated as a fallback — it is dead code today,
  §1 — unless separately wired as new work).
- Lifecycle: created lazily at first successful load; stopped in `uninit()`.
  No window ownership (MTA-style worker consistent with §02 service pattern);
  completion via overlapped I/O with an event handle the thread waits on.

Ordering rule (R1 compliance): the watcher only ever *publishes snapshots*; it never
touches live sessions. Open menus hold their generation until closed — unchanged
property of the snapshot design.

## 4. Interplay with other subsystems

| Concern | Resolution |
|---|---|
| Config generation used as circuit-breaker key (§01.7) | StaleWithError keeps old generation number ⇒ breaker state survives bad saves |
| `PackagesCache.clear()` on reload | disappears entirely once packages move to `PackageCatalogService` (§02.1) |
| Fonts/bitmaps caches | unchanged: rebuilt per published generation inside CACHE |
| First-run / never-loaded | unchanged refusal (nothing to serve); installer ships valid stock config |

## 5. Acceptance criteria

- [ ] Save invalid config while menus working → menus continue; single notification.
- [ ] Fix config → new generation active without Explorer restart (watcher); before the
      watcher lands, recovery is the manual Shift+Ctrl+right-click reload — no poll
      fallback is assumed (none exists today, §1).
- [ ] Fresh machine with corrupt stock config → clean "never loaded" refusal + log
      (no half-menus).
- [ ] Suite pins all three states; watcher covered by a real-file integration test with
      injected short debounce.
