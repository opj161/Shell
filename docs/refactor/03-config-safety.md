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

## 1a. The in-memory snapshot does not cover the failure above (§07 A3)

`Initializer` is **per-process** and `_snapshot` lives only in that process's memory.
Tracing the two cases separately changes the design:

| Case | What happens today | Does "serve the stale `_snapshot`" help? |
|---|---|---|
| Config saved with a typo while Explorer is **already running** | Nothing re-parses. `config_has_changed()` is dead code (§1), so `init()` is not called, `Status.Error` is never set, and menus keep working from the loaded snapshot. | Nothing to fix here |
| A **new process** raises a context menu, or Explorer restarts | `init()` runs, `parser.Load()` fails, `Status.Error = true`; `query()` refuses (`Initializer.cpp:154-155`) and `DllGetClassObject` returns `CLASS_E_CLASSNOTAVAILABLE` (`Main.cpp:1455`). **This is the failure.** | **No** — a fresh process has no previous snapshot in memory |

So the fix as originally written applies to the case that is not broken, and misses
the case that is. §2 below is still worth having, but it is the second half.

## 1b. Persisted last-known-good (the actual fix)

1. **Shadow the config set on every successful parse.** After `init()` publishes a
   new generation, write the resolved input set — `shell.nss` plus every file in the
   parser's `_imports` stack, each carrying its real path (`imp->path`, the same
   field cycle detection uses at `Parser.cpp:1165-1172`) — to
   `%LocalAppData%\Nilesoft\Shell\lkg\`, content-addressed, with a manifest recording
   the root path and each import's original location. Write via temp file +
   `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` so a torn write is never observable
   (<https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw>).
2. **Fall back to it on a failed parse at startup.** `init()` failure re-runs the
   parse against the shadow manifest and publishes *that* as the active generation,
   with `Status.Stale = true` and `LastError` carrying the real file's parser
   message and location.
3. **Integrity, same rule as §02.1.** The shadow is data this product wrote, read
   back by processes at potentially higher integrity. It is `.nss` source that gets
   *parsed*, not code that gets executed, and the parser is the same one that reads
   user files — so the exposure is the parser's own robustness, not a new class of
   trust. Still: never fall back to a shadow whose manifest does not verify, and
   never write the shadow from a process that did not itself parse the real file
   successfully.
4. **`shell.exe -check [file]`** — parse and report, publish nothing, exit non-zero
   on error. The cheapest possible prevention and the thing a user editing `.nss`
   will actually run. ~~Scope: XS; it is `Parser` plus the new path-taking
   constructor that already exists for tests.~~ **Not XS:** `shell.exe` is a
   single `main.cpp` that does not link the parser, and it is a
   Windows-subsystem binary with no console. It needs an export from `shell.dll`
   plus `AttachConsole(ATTACH_PARENT_PROCESS)` in the manager. Both are small,
   but the original estimate was wrong about what stood in the way.

   **Landed 2026-08-24.** `src/shared/ConfigCheck.h` is the boundary — a
   fixed-size POD with `cbSize` at offset 0, deliberately not a required-size
   buffer protocol — `Initializer::check()` is the parse, `ShellCheckConfig` is
   the export, and `CheckConfig()` in the manager prints the line. Output goes
   to an inherited standard handle first (so `-check > log.txt` lands where the
   user asked), then to the parent's console via `AttachConsole` and `CONOUT$`,
   then to a message box. Three things the sketch above did not anticipate:

   - **A missing file parsed as a success.** `Parser::Load()` returns `true`
     when the root file could not be read, deliberately: a machine with no
     `shell.nss` must still get a working shell. For a validator that is the
     worst possible answer — mistype the path and you are told your
     configuration is fine — and the first end-to-end run did exactly that.
     `LoadedFiles()` is empty in precisely that case, because `open_root` only
     records the root once `load_File` has succeeded, so that is the
     discriminator. Pinned by `parser_imports.a_file_that_cannot_be_read_records_no_loaded_file`
     and its positive counterpart, because nothing else in the tree would
     notice if the parser started recording files it failed to open.
   - **`TotalMenuCount` counts entries, not menus** — every menu, item and
     separator (`Parser.cpp:481`, `:1312`). The report says "entries".
   - **The exit code is correct but neither `cmd` nor PowerShell waits for a
     Windows-subsystem process**, so `%errorlevel%` and `$LASTEXITCODE` are not
     populated by a plain invocation. Verified: `Start-Process -Wait -PassThru`
     gives 0 for a good file and 1 for a bad or unreadable one, and redirection
     to a file works through the inherited handle. Curing the bare-invocation
     case needs a second, console-subsystem binary; that is a real cost for a
     cosmetic gain and is not being paid yet.

**As implemented (2026-08-24).** Steps 1–3 landed as `src/shared/ConfigShadow.h`
plus `Parser::LoadedFiles()`, with two departures from the sketch above:

- **A directory mirror, not a content-addressed store.** Relative imports are
  rooted against the importing file's own directory (`Parser.cpp`, the
  `if(!rooted) path = Path::Combine(l->location, path)` branch), so preserving
  the layout makes the shadow parse exactly as the original did — no redirect
  table, and no parser change beyond recording which files were read. Content
  addressing would have required the parser to consult a mapping on every
  import.
- **FNV-1a, not a cryptographic hash.** The manifest sits in the same directory
  as the files it names, so anyone able to rewrite a copy can rewrite its digest:
  a cryptographic hash buys nothing against tampering here, and would mean
  linking bcrypt into the DLL — a system DLL added to the import table of every
  process that raises a context menu, which is the cost §04.2 has just removed
  for d2d1 and dwrite. What is caught is truncation, corruption and a
  half-replaced set, which is what the guarantee actually needs to be.

One thing the sketch did not anticipate: skipping the write when the manifest is
unchanged is not sufficient on its own. A matching manifest says the *input* has
not changed, not that the store is intact — so a shadow whose copies were deleted
would be skipped over for exactly as long as the user left their configuration
alone, which is exactly as long as there would be nothing to recover from. The
skip re-verifies the store before taking it.

Also: `init()` now returns whether it *served*, not whether the real file
parsed. Returning false after recovering from the shadow cost one menu —
`query()` passes the result straight back, so only the next attempt would have
found the snapshot — and the parse error is restored afterwards, because a
successful shadow parse otherwise reports itself as a healthy load and erases
the error the user needs to see.

Acceptance (replaces the old §5 first bullet, which passed for the wrong reason):
save an invalid config, **restart Explorer**, and menus still work from the shadow
with a single notification; fix the file and the next parse takes over.

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
      (Note: this passes today for the wrong reason — nothing re-parses. The test
      that matters is the next line.)
- [ ] Save invalid config, **restart Explorer**, right-click → menus still work, served
      from the persisted shadow (§1b), `Status.Stale` set, one notification.
- [x] `shell.exe -check` on a good file exits 0 and on a bad one prints file, line,
      column and message, and exits non-zero. Verified end to end against real
      files; see the caveat in §1b about which shells wait for the exit code.
- [ ] A shadow whose manifest fails verification is refused, and the process falls
      back to the never-loaded refusal rather than parsing it.
- [ ] Fix config → new generation active without Explorer restart (watcher); before the
      watcher lands, recovery is the manual Shift+Ctrl+right-click reload — no poll
      fallback is assumed (none exists today, §1).
- [ ] Fresh machine with corrupt stock config → clean "never loaded" refusal + log
      (no half-menus).
- [ ] Suite pins all three states; watcher covered by a real-file integration test with
      injected short debounce.
