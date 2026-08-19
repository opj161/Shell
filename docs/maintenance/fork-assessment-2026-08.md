# Nilesoft Shell fork-diff maintenance assessment

## Scope

This assessment is against the project snapshot included in `Shell.zip`, whose tracked base is:

- Remote: `https://github.com/moudey/Shell`
- Base commit: `81ec1a410d1277efa58aff52be912a254f66e5a3`
- Snapshot commit message: merge of PR #791
- Five supplied fork diffs were analyzed and each was verified to apply cleanly to that base.

Diff sizes from the supplied files (including added files):

| Fork diff | Files | Insertions | Deletions | Character |
|---|---:|---:|---:|---|
| `9000000` | 2 | 6 | 3 | personal menu defaults |
| `xiaobsh` | 1 | 3 | 3 | documentation correction |
| `Roze061` | 11 | 680 | 42 | third-party host selection + UI/CI fixes |
| `JENJET` | 13 | 1,084 | 157 | owner-draw/native-icon experiment + personalized config |
| `TCNOco` | 64 | 5,293 | 1,217 | broad maintenance fork: runtime, parser, shell extension, installer, CI/tests, packaging |

The main conclusion is **not** “merge the largest fork.” The useful maintenance work is concentrated in TCNOco, but that fork also contains fork-specific identity changes and two cache implementations that I would not merge. The safest path is selective extraction.

---

## Executive recommendation

| Fork | Assessment | Recommendation |
|---|---|---|
| **xiaobsh** | Small, correct, directly verified against implementation | **Merge now** |
| **TCNOco** | By far the strongest engineering work; contains multiple real correctness fixes and tests, but also fork branding/identity and a few regressions | **Selectively merge**; use as primary maintenance source |
| **Roze061** | Solves a real architectural problem—selection in non-Explorer context-menu hosts—and includes useful monitor/UI fixes; exact COM/placeholder design is fragile | **Keep concept, not implementation**; TCNOco's handler is a better base |
| **JENJET** | Interesting owner-draw compatibility experiment and useful developer tooling, but most config changes are personalized/localized and the rendering patch is heuristic and hot-path heavy | **Do not merge wholesale**; isolate owner-draw work for a separate experiment |
| **9000000** | Purely preference changes to menu ordering/visibility | **Do not upstream**; optionally offer as a preset |

If this were being maintained as an upstream branch, I would land the supplied **core patch first**, then validate and land selected parts of the **full patch** after Windows/Explorer testing.

---

# 1. High-confidence correctness defects found

These are the changes with the strongest evidence. Most were surfaced by TCNOco; one additional encoding defect was exposed by TCNOco's tests but not actually fixed in that fork, so I fixed it in the maintenance candidates.

## 1.1 UTF-16 case-insensitive comparison is incorrect

### Upstream behavior

Several string/lexer paths use `_memicmp` on `wchar_t` buffers. `_memicmp` is a **byte** comparison routine. Passing UTF-16 data to it means each byte of each UTF-16 code unit is case-folded independently.

This creates false equality. For example, the UTF-16 code units:

- U+4E2D → bytes `2D 4E` in little endian
- U+6E2D → bytes `2D 6E`

`0x4E` (`N`) and `0x6E` (`n`) are an ASCII case pair, so a byte-wise case-insensitive comparison can report these two **different CJK characters as equal**.

It also does not provide genuine Unicode case folding; non-ASCII case pairs are mostly not handled at all.

### TCNOco change

Adds `src/shared/System/Text/StringCompare.h` with an ordinal, locale-independent ASCII fold over **UTF-16 code units**, plus scalar/SSE2/ARM64 NEON paths, and replaces `_memicmp` use in the affected string routines.

### Assessment

**Strong merge.** This is a correctness defect, not a style preference. ASCII-only folding also matches the language/parser semantics better than locale-sensitive CRT behavior.

I retained this in both maintenance candidates and ran a portable 16-bit-`wchar_t` compile/run sanity test covering:

- ASCII case folding
- different CJK code units
- a 16-code-unit SIMD-length path
- inequality on a differing final character

That sanity test passed. A real MSVC/Windows build is still required before release.

---

## 1.2 `string::Find` / `Find0` can compare past the end of the buffer

The old search loops allow the start index to reach positions where fewer than `pattern_length` code units remain, but still compare the full pattern. That permits reads beyond the valid source range.

TCNOco bounds the last candidate start correctly and fixes the reverse-search path too.

### Assessment

**Strong merge.** This is a direct bounds/correctness fix and belongs in the core patch.

---

## 1.3 `MenuItemInfo::set_title(string&&)` can leave a stale `dwTypeData`

The base code stores:

- `dwTypeData = title.text`
- `cch = title.length`

and only then calls `normalize()`.

`normalize()` can split accelerator/tab content and reassign the title storage. That means `dwTypeData` can still point at the previous allocation when USER32 later consumes the menu item.

TCNOco normalizes first, then assigns the pointer and length.

### Assessment

**Strong merge.** Small, well-localized lifetime fix. Included in both candidates.

TCNOco also caches `GetMenuItemCount` once in `get_index()` instead of calling it in every loop condition. That is a straightforward micro-optimization and is included too.

---

## 1.4 Relative import handling is inconsistent and cyclic imports are unbounded

### Upstream problems

`Parser::load_import` only roots a relative path when its string length is greater than two. A short relative import such as `a` can therefore escape import-relative resolution and fall back to the process current directory.

The parser also lacks a true active-stack cycle check. A list of previously imported files is not equivalent to detecting a file already active in the current import chain.

### TCNOco change

- Root every non-drive/non-UNC relative import against the importing file's directory.
- Detect a path already present in the active `_imports` stack.
- Add a maximum import depth of 32.
- Remove three unused/broken `Lexer::peek_token` overloads; one could over-read and the overload set was internally inconsistent.

### Assessment

**Strong merge.** These changes make config loading deterministic and prevent pathological recursion inside Explorer. Included in both candidates.

---

## 1.5 BOM-less UTF-8 detection accepts malformed byte sequences

This is the one correctness defect I added beyond the fork patches.

TCNOco added encoding tests that document malformed UTF-8 cases, but its own runtime code still retained the old detector. Inspection of `Encoding::GetType` showed that the previous table loop:

- accepts a lone continuation byte (`0x80`–`0xBF`) as a one-byte character,
- can accept a multibyte lead byte truncated at EOF,
- validates too little of a multi-byte sequence,
- does not reject overlong forms, encoded UTF-16 surrogates, or values above U+10FFFF.

This can cause a malformed/ANSI `.nss` file to be classified as UTF-8 and decoded incorrectly.

### Fix in both candidates

The detector now validates complete 1/2/3/4-byte UTF-8 sequences and rejects:

- lone continuation bytes,
- `C0`/`C1`,
- `F5`–`FF`,
- truncated sequences,
- overlong `E0`/`F0` forms,
- UTF-16 surrogate encodings (`ED A0` and above),
- values above U+10FFFF (`F4 90` and above).

Tests were changed to expect ANSI for malformed byte streams.

### Assessment

**Strong merge.** This closes a concrete gap revealed by the fork's own test effort.

---

# 2. Installer and registry correctness

## 2.1 `JoinPath` discards the separator it just appended

In the MSI custom action, the function creates a local `path`, appends `\` to it when needed, but returns `path1 + path2` instead of `path + path2`.

It happens to work for callers that already supply a trailing separator, but the function itself is wrong.

### Assessment

**Merge.** Included in both candidates.

---

## 2.2 `InstallFolder` misuses the `RegGetValueW` size parameter

The old code initializes the buffer size as `MAX_PATH`, but `RegGetValueW` expects a **byte count**, not a wchar count. It also keeps the terminating null inside the `std::wstring` length, and a failed read can leave a pre-sized string of nulls that later looks non-empty.

The candidate patch:

- passes `capacity * sizeof(wchar_t)`,
- removes the terminator from the resulting string length,
- clears the result on failure.

### Assessment

**Merge.** Included in both candidates.

---

## 2.3 `REG_SZ` TreatAs value is written without its terminating null

The registration path writes only `length * sizeof(wchar_t)` bytes for a `REG_SZ`. Some readers, including code elsewhere in this project, assume the terminator exists.

The candidate writes `(length + 1) * sizeof(wchar_t)`.

### Assessment

**Merge.** Included in both candidates.

---

## 2.4 Unregister can leave or incorrectly remove the Windows 11 `TreatAs` redirect

The project redirects the Windows 11 context-menu class through `TreatAs`. The previous unregister flow did not reliably remove this on a normal unregister; the old commented approach would also have been unsafe because it could delete a redirect owned by something else.

The candidate:

1. reads the current TreatAs value,
2. removes it only if it equals this project's context-menu CLSID,
3. recognizes both correctly terminated new values and legacy values.

### Assessment

**Merge.** It both cleans up the project's own state and avoids destroying another component's redirect.

---

# 3. Localization and default configuration

## 3.1 xiaobsh: `sys.datetime("Y")` / `("y")` docs are reversed upstream

The implementation in `src/shared/System/Text/string.h` explicitly maps:

- `Y` → two-digit year
- `y` → four-digit year

The upstream docs described these the other way around. xiaobsh swaps the documentation to match the implementation.

### Assessment

**Definitely merge.** Included in both candidates.

TCNOco also corrects datetime examples that used `.` where the implementation formats time with `:`; I retained the useful documentation correction.

---

## 3.2 Locale path tests are vulnerable to the host process current directory

The default `shell.nss` first calls `path.exists()` on a relative localization path. That existence check occurs before the import mechanism can resolve a relative import against the config location, so behavior can depend on the host's current working directory.

The candidates make the locale path absolute via `app.dir + '\imports\lang\'`.

### Assessment

**Merge.** This is especially important for shell extensions hosted by other processes.

---

## 3.3 Locale fallback misses bare-language files

The shipped tree contains names such as `ja.nss`, `it.nss`, `ru.nss`, etc. A locale can arrive as a full tag while only the bare-language file exists.

The candidate fallback is:

1. full `sys.lang`,
2. bare `sys.lang.name`,
3. English.

### Assessment

**Merge.** Included in both candidates.

---

## 3.4 Several shipped translations are omitted from the MSI

The source tree contains translations not listed in the WiX language component. Also, Spanish exists upstream as `src/bin/imports/lang/es-ES` without the `.nss` extension expected by the loader pattern.

The candidates:

- rename it to `es-ES.nss`,
- add WiX entries for Spanish, Italian, Japanese, Romanian, Russian, Slovenian, Turkish, Ukrainian, and Traditional Chinese.

I used a new component GUID in the maintenance patch rather than copying TCNOco's fork GUID blindly.

### Assessment

**Merge.** This turns already-shipped source translations into actually installed translations.

---

## 3.5 Default `showdelay=200` overrides a user-wide Windows setting

TCNOco removes the default `showdelay=200`. The setting ultimately modifies `SPI_SETMENUSHOWDELAY`, which is a per-user Windows preference, so a shell extension should not silently impose its own value by default.

The candidates leave the option commented/unset.

### Assessment

**Merge.** Users can still opt into a value explicitly.

---

# 4. Third-party file-manager / shell-extension support

This is the biggest architectural theme shared by Roze061 and TCNOco.

## Why upstream has a gap

The existing selection path is heavily oriented around Explorer and `IShellBrowser`/Explorer-window discovery. A third-party file manager can invoke registered `IContextMenu` handlers correctly without exposing Explorer's window object model. In that case the shell extension already receives the selected `IDataObject`/folder PIDL through `IShellExtInit`, but upstream does not make that data available to the later menu-building interception path.

That is a real design gap.

---

## 4.1 Roze061 solution: good goal, fragile transport

Roze061 adds a conventional COM class factory and an `IShellExtInit` + `IContextMenu` object. It captures selection into a process-global `GlobalSelectionContext`.

To associate that capture with the menu, `QueryContextMenu` inserts a dummy menu command named:

`ShellExtSelectionRetriever Placeholder`

The later menu reconstruction finds/recognizes that placeholder and switches to the captured selection path.

### What is good

- Uses the actual shell extension interfaces intended to receive selection data.
- Handles selected items and background folder clicks.
- Makes third-party host support possible without hard-coding application names.
- Adds useful selection fallback logic and COM lifetime tracking.

### Why I would not merge this exact implementation

1. **Process-global selection state** can be overwritten by another UI thread/menu in the same process.
2. **The placeholder command is a side channel through the host menu.** It consumes a command ID and can leak into the visible menu if the interception path fails or ordering changes.
3. Selection ownership/lifetime is not bound tightly to a particular `HMENU`.
4. Mixed-parent selection falls back to “This PC” as the parent, which is not necessarily a meaningful semantic parent for callers.
5. The fallback marks the host as Explorer (`Window.id = WINDOW_EXPLORER`, `Window.explorer = true`), which can make Explorer-specific config predicates true in a third-party application.
6. A global `contextmenuhandler` loader flag is vulnerable to stale/cross-thread state.

### Verdict

**Keep the architecture idea; replace the transport/state model.**

---

## 4.2 TCNOco solution: materially better design

TCNOco implements `IShellExtInit` + `IContextMenu` without inserting any commands. Instead it stores a short-lived, thread-local capture containing:

- exact `HMENU`,
- `IShellItemArray` with refcount ownership,
- cloned folder PIDL,
- background/foreground state,
- timestamp.

The later `TrackPopupMenu` interception only accepts a capture when the `HMENU` matches and it is fresh. It clears the capture after the menu lifecycle.

### Why this is better

- no visible/hidden placeholder item,
- no command-ID consumption,
- thread-local instead of process-global selection,
- exact menu-handle association,
- explicit COM/PIDL lifetime ownership,
- Explorer can continue using its richer native selection path; the shell-extension capture is a fallback.

### Additional hardening I made in the full candidate

TCNOco still set a process-global `_loader.contextmenuhandler` flag in `DllGetClassObject`, then reset it later. That flag can be stale or cross-thread even though the selection capture itself is thread-local.

The full candidate removes the global flag and derives `window.is_contextmenuhandler` from `ShellExtCapture::match(hMenu)` for the **exact menu being processed**.

### Residual risk

This still needs real Windows host testing. In particular:

- same-thread nested menus could replace a thread-local single-slot capture,
- an unusual host that marshals `IShellExtInit` and `IContextMenu::QueryContextMenu` onto different threads would fail to use the capture (safe failure, but no feature),
- registration behavior must be tested across Windows 10/11 and third-party file managers.

### Verdict

**Best available implementation among the supplied forks; full-candidate material after Windows smoke testing.**

---

# 5. ContextMenu runtime fixes from TCNOco

These are not in the conservative core patch because they touch the highest-risk UI subsystem, but most are good maintenance changes.

## 5.1 Duplicate-removal replacement can target the wrong native item

The upstream duplicate-removal logic initializes `indexof = 0` and iterates items with a range-for, but the index is not advanced while searching. On a match, replacement can therefore write to `menu->items[0]` regardless of which item matched.

TCNOco switches to indexed iteration and defers deletion of a replaced subtree while lookup maps can still contain pointers to it.

### Assessment

**High-value correctness fix.** Included in the full candidate.

---

## 5.2 `unordered_map::operator[]` creates entries during lookups

Some menu/system lookups use `map[key]` even when the intent is “find if present.” On dead or recycled menu handles this silently inserts null/default entries and grows state.

TCNOco switches these paths to `find()`.

### Assessment

**Merge in full candidate.** Low conceptual risk, but part of UI code that should be smoke-tested as a unit.

---

## 5.3 Disabled owner-draw state is incomplete

Native menu items may use `ODS_DISABLED` or `ODS_GRAYED`; handling only one can produce incorrect appearance/state interpretation.

TCNOco handles both.

### Assessment

**Merge in full candidate.**

---

## 5.4 Recycle Bin count query is synchronous on menu construction

The base performs `SHQueryRecycleBinW(nullptr)` while constructing menus. This can be slow and blocks the UI path. TCNOco removes the recount and relies on the native item's disabled state already supplied by Explorer.

### Assessment

**Good performance fix** provided state fidelity remains correct in Windows testing. Included in the full candidate.

---

## 5.5 Multi-monitor coordinate fixes

Roze061 and TCNOco both contain fixes for monitor rectangles whose origin is not `(0,0)`, including monitors left/above the primary display:

- tooltip clamping must use `_rcMonitor.left` / `.top`,
- scroll-centered menu Y must include the monitor top offset,
- lower-bound checks must compare against the absolute monitor bottom.

### Assessment

**Good and concrete.** Included through the full ContextMenu maintenance work. Test specifically with a secondary monitor at negative X and/or Y.

---

## 5.6 DPI/text-scale and dead rendering paths

TCNOco cleans up DPI/font scaling, corrects text scale factor retrieval sizing/result handling, removes an unreachable Direct2D renderer and associated unused D2D/DWrite/WinMM link dependencies, and removes an Alt-held COM timing/debug path that could alter suppression behavior.

### Assessment

Generally positive cleanup, but because rendering behavior is difficult to validate statically, I keep it in the **full**, not **core**, candidate.

---

# 6. COM initialization / loader-lock hardening

Upstream initializes COM-related state from DLL initialization paths. Calling COM APIs while under the Windows loader lock is a known architectural hazard because COM can load modules and perform work that is unsafe during `DllMain` processing.

TCNOco moves COM initialization to an explicit `ensure_com()` path on the menu thread and adds a loader-lock check/test utility.

### Positive

This is directionally much safer than doing COM setup from DLL attach.

### Caveat

The fork intentionally does not balance a successful `CoInitializeEx` with `CoUninitialize` for the menu thread, to avoid tearing down an apartment it does not own during DLL/TLS teardown. That trades loader-lock risk for a retained COM initialization count on long-lived host threads.

### Assessment

**Worth pursuing, but Windows-test before release.** Included only in the full candidate. A longer-term cleanup should make COM apartment ownership explicit rather than relying on a deliberately unbalanced initialization.

---

# 7. Build, CI, and test engineering

TCNOco is strongest here.

## Useful changes

- Adds a C++ regression test project.
- Tests string comparison and the CJK collision.
- Tests import/encoding behavior.
- Tests COM shell-extension interfaces, aggregation rejection, `LockServer`, object count, selection isolation, and PIDL lifetime.
- Adds a loader-check helper.
- Adds x86/x64/ARM64 CI coverage.
- Restores `packages.config`/VC-LTL explicitly.
- Produces symbols/checksums/artifacts.
- Adds PowerShell build tooling.
- Adds VM scripts intended to exercise a real context menu, not only unit tests.

This is exactly the kind of maintenance infrastructure an under-maintained shell extension needs, because the most serious regressions are often host-specific and cannot be inferred from compilation alone.

## VC-LTL silent fallback

`VC-LTL.props` conditionally imports the restored package. If it is missing, the import simply does not happen, so a local build can silently use a different CRT configuration from a restored/CI build.

Both candidates add an MSBuild guard that fails loudly when VC-LTL is missing unless the builder explicitly opts out with `ShellAllowNoVCLTL=true`.

## What I retained

### Core candidate

- test project with string-comparison and encoding regression tests,
- VC-LTL restore guard.

### Full candidate

- the above,
- shell-extension tests,
- the stronger build workflow/build script,
- loader-check helper.

I did **not** copy the fork's release/pages/install/VM suite wholesale into the upstream candidate because several scripts are coupled to TCNOco's fork identity/install layout. They are still useful references for building a proper upstream integration-test harness.

---

# 8. Changes I would *not* merge from TCNOco

## 8.1 Lazy `PackagesCache` using `std::once_flag`

TCNOco changes package enumeration to lazy initialization with `std::call_once`, then retains `clear()` as:

`_list.clear()`

The problem is that `std::once_flag` is not resettable. After the cache has loaded once:

1. config reload calls `clear()`,
2. `_list` becomes empty,
3. the once flag remains “already executed,”
4. every future `all()` returns the permanently empty list.

There is also no synchronization between `clear()` and readers.

### Verdict

**Reject as written.** Redesign around a mutex + resettable `loaded` generation/state if lazy loading is desired.

I restored the eager upstream cache behavior in the full candidate rather than importing this regression.

---

## 8.2 Process-wide SVG `BitmapCache` is not thread-safe

The fork adds an `unordered_map<uint64_t,HBITMAP>` cache. Its own comments acknowledge that concurrent menu builds can race to add the same icon, but the map operations themselves have **no mutex**.

That means concurrent `find`, `emplace`, and `clear` are C++ data races. More importantly, `clear()` deletes raw `HBITMAP` handles while a menu may still be drawing one returned earlier.

### Verdict

**Reject as written.** The performance idea is good, but the lifetime contract needs redesign. A safe version should use synchronized map access plus ownership that keeps a bitmap alive while any menu item is drawing it (for example a refcounted entry/generation), not raw borrowed handles erased on reload.

I removed this cache and its test from the maintenance candidates.

---

## 8.3 Fork identity/branding/CLSID/ProductCode changes

TCNOco intentionally assigns a different COM identity, company/app registry path, installer Product/Upgrade identity, resources, install folder, and fork documentation so it can coexist with upstream.

That is correct **for a fork** and wrong **for upstream maintenance**.

### Verdict

**Do not merge upstream.** Both candidates restore/preserve the original Nilesoft identity and CLSIDs.

---

## 8.4 Removing all selection-time current-directory behavior

TCNOco removes process-wide `SetCurrentDirectory` behavior and starts passing selection directories explicitly in some expression execution paths. Architecturally, eliminating CWD mutation inside Explorer is desirable because current directory is process-global.

However, only some relative-path consumers are migrated. There may be compatibility dependencies elsewhere in the expression/config surface.

### Verdict

**Defer the broad behavioral removal.** The full candidate keeps the safer import-path fixes but restores the existing selection CWD behavior for backward compatibility. A future change should first enumerate every relative-path consumer, pass an explicit base directory to all of them, then remove process CWD mutation in one tested change.

---

# 9. Fork-by-fork detailed assessment

## 9.1 `9000000`

### Changes

- moves native terminal-related items from bottom to top in `modify.nss`,
- hides “Open Git GUI here” and “Open Git Bash here,”
- moves Command Prompt ordering in the Terminal submenu.

### Value

This can be a sensible **personal preset** for someone who prefers Windows Terminal and wants Git context entries suppressed.

### Problems

There is no general correctness rationale for hiding Git commands or changing ordering globally. It removes functionality for users who intentionally installed those handlers.

### Verdict

**Do not upstream.** If the project grows a preset/profile system, this is suitable as an optional example.

---

## 9.2 `xiaobsh`

### Changes

Only documentation:

- corrects the `Y`/`y` year descriptions,
- normalizes the file ending/newline.

### Verification

The source implementation maps uppercase `Y` to two-digit year and lowercase `y` to four-digit year, so the fork is correct.

### Verdict

**Merge.** Zero runtime risk.

---

## 9.3 `Roze061`

### Main changes

- adds a COM `ClassFactory`, `IShellExtInit`, and `IContextMenu` selection retriever,
- supports third-party file-manager selection fallback,
- inserts/finds a placeholder menu item to associate the capture with a context menu,
- adds context-handler state to the loader/window path,
- adds useful multi-monitor coordinate corrections,
- adds Windows 11 visual/DWM adjustments,
- modifies CI action versions/runner configuration and changes workflow triggering.

### Most helpful parts

1. Recognition that third-party hosts should use the standard shell-extension selection data.
2. Multi-monitor coordinate fixes.
3. COM lifetime/reference counting groundwork.

### Parts to reject/rework

- global selection state,
- placeholder command side channel,
- Explorer identity forced onto third-party hosts,
- process-global context-handler flag,
- workflow change that removes normal push/PR CI coverage in favor of manual execution is undesirable for an under-maintained project.

### Verdict

**Important prototype, superseded by TCNOco's cleaner handler design.** Cherry-pick the monitor fixes if not taking the full TCNO ContextMenu work.

---

## 9.4 `JENJET`

### Configuration/default changes

A large part of this fork is clearly user-specific:

- removes the default localization imports from `shell.nss`,
- hardcodes “Pin/Unpin,”
- hardcodes/translates many shipped menu titles to Chinese,
- changes development-menu visibility to Shift,
- removes several author's hardcoded `D:\...` development-tool entries,
- changes `.lnk` handling in the file-management menu,
- changes terminal/menu wording/order,
- modifies taskbar/goto defaults.

The typo correction `publish sinale file` → `publish single file` is worth taking independently. Removing hard-coded private `D:\config\...` developer tools from a default file is also reasonable if those entries are really shipped upstream, but the rest of the language/default rewrite should not be mixed with it.

### `.gitignore`

Adds common build artifacts. Useful maintenance hygiene and low risk.

### `deploy-shell.ps1`

A practical developer script that:

- finds processes that loaded the target DLL,
- stops them,
- retries replacement,
- restarts Explorer and other stopped processes,
- verifies SHA-256 after copy.

The default destination is hard-coded to `D:\Softs\NileSoftShell\shell.dll`, so it is not upstream-ready. Make the destination discoverable from the registered `InprocServer32` or require it as a parameter before adopting.

### Owner-draw/native-icon compatibility patch

This is the technically interesting part. It attempts to preserve icons from third-party owner-draw shell extensions while still rendering Shell's own themed text/background. It:

- tracks top-level/native-owner-draw state,
- probes known private `dwItemData` layouts for `HBITMAP`,
- optionally scans pointer-aligned offsets through `0x300` under SEH,
- renders native owner-draw into an offscreen bitmap,
- samples/infer background pixels,
- finds connected foreground components in an icon zone,
- crops/reconstructs alpha,
- detects pale/monochrome low-contrast icons and recolors them to a readable themed color.

### Why I did not merge it

This is a **heuristic rendering pipeline in `WM_DRAWITEM`**, one of the hottest and most compatibility-sensitive paths in the project. It allocates vectors, copies DIBs, scans pixels, performs connected-component analysis, and probes private extension memory layouts. SEH prevents an access violation from killing the process, but it does not make a false-positive `HBITMAP` interpretation semantically safe.

It may fix real extensions, but it needs to be isolated and tested with captured fixtures/known owner-draw extensions and benchmarked before becoming default behavior.

### Verdict

**Do not merge wholesale.** Split into:

1. typo/default cleanup,
2. generic deploy tooling,
3. a separately gated/benchmarked owner-draw compatibility experiment.

---

## 9.5 `TCNOco`

### Strongest changes

- UTF-16 string compare correctness and tests,
- string-search bounds fixes,
- parser relative-import/cycle/depth fixes,
- menu title pointer lifetime fix,
- third-party shell-extension selection architecture,
- duplicate-menu replacement fix,
- safer map lookups,
- multi-monitor/DPI fixes,
- removal of synchronous Recycle Bin recount,
- loader-lock/COM hardening,
- taskbar UIAutomation short-lived caching,
- installer path/registry/TreatAs fixes,
- locale fallback and MSI language packaging,
- deterministic build/test improvements and VM smoke-test work.

### Mixed/needs review

- broad ContextMenu renderer cleanup: likely good, but visual behavior requires real Windows regression testing,
- COM initialization ownership: better than loader-lock initialization, but deliberately unbalanced,
- CWD removal: architecturally good goal, incomplete migration risk.

### Reject/defer

- `PackagesCache` once-flag lazy reload,
- unsynchronized raw-handle `BitmapCache`,
- fork branding/CLSID/ProductCode/manufacturer/install-path changes,
- upstream package-manager docs modified only to explain that they install upstream rather than the fork,
- fork-specific local install/release/pages plumbing.

### Verdict

**Primary source for maintenance, but only through selective review.** This is why the supplied full candidate is a cleaned upstream-oriented derivative, not the TCNOco diff itself.

---

# 10. Maintenance candidates produced

## A. Conservative core candidate — recommended first merge

**21 files changed: +928 / -188**

Contains:

- corrected UTF-16 comparisons,
- bounded string search,
- parser relative-import/cycle/depth fixes,
- removal of dead/broken `peek_token` overloads,
- menu-title pointer lifetime fix,
- strict BOM-less UTF-8 validation,
- datetime documentation corrections,
- locale path/fallback fix,
- Spanish filename fix + missing MSI locales,
- removal of default system-wide `showdelay` override,
- installer `JoinPath`/registry buffer fixes,
- correct REG_SZ termination and safe TreatAs cleanup,
- VC-LTL restore guard,
- string/encoding regression-test project.

It intentionally **does not** change the shell-extension architecture, DllMain/COM behavior, or broad ContextMenu rendering logic.

Use this if the priority is to stabilize upstream with minimal behavioral surface area.

---

## B. Full maintenance candidate — second-stage merge

**37 files changed: +2,461 / -1,196**

Includes everything in the core candidate, plus selected TCNOco maintenance work:

- zero-item `IShellExtInit` / `IContextMenu` handler,
- thread-local, exact-`HMENU` selection capture,
- my removal of the process-global context-handler flag,
- third-party host enablement via `window.is_contextmenuhandler`,
- broad ContextMenu correctness/performance cleanup,
- duplicate-item replacement fix,
- monitor/DPI fixes,
- Recycle Bin synchronous-query removal,
- taskbar UIAutomation short-lived cache,
- COM initialization moved out of DLL attach path,
- shell-extension unit tests,
- CI/build script and loader-check utility.

It deliberately **does not** include:

- TCNOco fork identity/branding,
- lazy `PackagesCache`,
- `BitmapCache`,
- broad selection-CWD removal,
- fork-specific release/pages/local-install/VM scripts,
- JENJET's owner-draw image reconstruction,
- Roze061's placeholder selection transport,
- 9000000's personal menu policy.

Use this only after the core candidate is understood and the Windows test matrix below passes. **The full patch already contains the core changes; do not apply both patches to the same tree.**

---

# 11. Validation performed

The assessment environment is not Windows, so I could not perform a true MSVC/Windows SDK build or launch Explorer. I therefore separated static/high-confidence findings from changes that require host testing.

Completed validation:

1. All five supplied diffs apply cleanly to the included base commit.
2. Both generated maintenance patches pass `git diff --check`.
3. Both generated patches pass `git apply --check` against the untouched base snapshot.
4. Modified WiX/MSBuild XML parses successfully.
5. Full candidate GitHub Actions YAML parses successfully.
6. A portable C++ compile/run with 16-bit `wchar_t` validates the new ordinal comparator's key behavior, including the CJK collision case and SIMD-size input.
7. Fork-specific TCNO branding/identity strings were removed from the upstream-oriented candidates; original Nilesoft manufacturer/UpgradeCode/COM identity are retained.
8. Encoding-only BOM churn introduced by copied fork files was stripped from existing upstream source files.

What remains unvalidated here:

- MSVC compilation for x86/x64/ARM64,
- Explorer process injection/hooking behavior,
- actual `IContextMenu` registration and invocation,
- Windows 10 vs Windows 11 DWM/menu behavior,
- third-party file-manager interoperability,
- visual fidelity of all menu states,
- installer execution/upgrade/uninstall on a real Windows machine.

---

# 12. Windows validation matrix before shipping the full candidate

At minimum:

### Build

- Visual Studio 2022 / v143
- x86
- x64
- ARM64
- verify VC-LTL is restored and the deliberate opt-out path works
- run `tests.exe`

### Explorer contexts

- Windows 10 foreground selection
- Windows 10 folder background
- Windows 11 foreground selection
- Windows 11 folder background
- desktop background
- taskbar context menu
- single file / multiple files / folders / mixed selection
- namespace/This PC entries

### Third-party host

Use at least one file manager that invokes standard shell context-menu handlers. Test:

- foreground file selection,
- folder selection,
- background menu,
- two windows/threads in the same process if supported,
- opening menus rapidly/nested where possible,
- verify no placeholder/dummy item ever exists (full candidate should insert zero commands),
- verify Explorer-only predicates are not spuriously true.

### Multi-monitor

- primary monitor at `(0,0)`,
- secondary to the left (negative X),
- secondary above (negative Y),
- different DPI scales,
- menu near every monitor edge,
- tooltip near every monitor edge.

### Menu correctness

- duplicate native items where a custom rule replaces a non-first item,
- checked/disabled/grayed items,
- empty and non-empty Recycle Bin,
- native submenus,
- light/dark themes,
- keyboard invocation and mouse invocation.

### Config/parser

- nested relative imports,
- one-character/short import filenames,
- circular imports,
- depth > 32,
- full locale tag and bare-language fallback,
- valid UTF-8 without BOM,
- malformed/truncated UTF-8,
- ANSI config,
- config reload after package-related expressions (especially important if a future lazy package cache is reintroduced).

### Install/uninstall

- clean install,
- upgrade,
- uninstall,
- verify `TreatAs` is removed only when it points at this CLSID,
- verify a third-party-owned `TreatAs` is preserved,
- verify every locale file lands in the installed directory.

---

# 13. Recommended merge order

1. **Core correctness patch** as one or several reviewable commits:
   - string compare/search,
   - parser/imports,
   - encoding,
   - MenuItem lifetime,
   - installer/registry,
   - locale/docs,
   - tests/VC-LTL guard.
2. Run the Windows build/test matrix for core.
3. Add **TCNO-style ShellExt selection capture** as a dedicated change with its tests.
4. Add the exact-`HMENU` `window.is_contextmenuhandler` integration.
5. Add ContextMenu fixes in small groups:
   - duplicate/map correctness,
   - monitor/DPI,
   - Recycle Bin/performance,
   - rendering/dead-code cleanup.
6. Move COM work out of loader-lock paths as a separate commit and inspect apartment ownership carefully.
7. Treat JENJET owner-draw recovery as an experimental follow-up, with performance measurements and extension fixtures.
8. If icon caching is still wanted, redesign it with synchronized/refcounted lifetime before revisiting TCNOco's `BitmapCache`.
9. If lazy package enumeration is wanted, implement resettable state rather than `once_flag` + `clear()`.
10. Only after all relative-path consumers are explicit should process-wide `SetCurrentDirectory` behavior be removed.

This ordering makes regressions bisectable and prevents several unrelated fork behaviors from becoming one hard-to-review “maintenance mega-merge.”

---

# 14. Artifacts and hashes

Generated against the included base snapshot:

- `Shell_recommended_core.patch`  
  SHA-256: `8ae8ff97bbfc60049877198d192a752ff4b00cbaf6935fd2754a9253d77f4b81`
- `Shell_recommended_full.patch`  
  SHA-256: `06eb3124ce28d11ce3fc272d05d4a237c3961942a6864a3e64a205063dd0b275`
- `Shell_recommended_core.zip`  
  SHA-256: `86bc9acd96cbbb5d15a8f4aaee4845c21e09b6735493bd5416a9269283ab033e`
- `Shell_recommended_full.zip`  
  SHA-256: `175a8a1a1c7018dbeb6099e2d114ccbbeca37687c962e5560b91c9fadc3265d5`

The ZIPs are complete source-tree snapshots of the two candidates and exclude Git worktree metadata.

