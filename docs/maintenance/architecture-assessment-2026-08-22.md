# Nilesoft Shell fork — full architecture scan and strategic assessment

**Date:** 2026-08-22
**Tree:** `main` @ `a3431df` ("fix(menu): let Windows scroll overflowing popups via cyMax")
**Method:** full source scan of the five first-party components (`src/dll`, `src/shared`,
`src/exe`, `src/setup`, `src/tests`), grounded against Microsoft Learn reference pages
fetched this session (IContextMenu, IShellExtInit::Initialize, TrackPopupMenu,
WM_INITMENUPOPUP, Using Menus), the local official WiX mirror `.bin/wix-docs`
(`schema/wxs/customaction.mdx`, `registryvalue.mdx`, `component.mdx`), and the
probe-established facts recorded in `AGENTS.md`. Every contract claim carries its
citation; conclusions that rest on undocumented behaviour are labelled as such.

---

## 1. What this codebase is

Nilesoft Shell is a per-machine COM in-process extension that replaces the
right-click context menu of Windows. It is loaded into `explorer.exe` and any
third-party file manager that honours `ContextMenuHandlers`, then intercepts every
popup-menu display in the process, rebuilds the menu from a user script
(`shell.nss`), owner-draws it, and dispatches the chosen command itself. The fork
carries the 1.9.20 latency-and-hardening programme described in
`docs/maintenance/fork-assessment-2026-08.md` and `shell-agent-work-audit-2026-08-21.md`.

First-party size (measured, excl. `3rdparty`):

| Component | Files | Lines | Role |
|---|---:|---:|---|
| `src/dll` | 63 | 35,027 | `shell.dll` — hooks, menu engine, NSS interpreter |
| `src/shared` | 54 | 20,812 | custom runtime library (string/collections/drawing/registry) |
| `src/exe` | 2 | 1,697 | `shell.exe` — registration/TreatAs tool |
| `src/setup` | 2 (+wxs) | 1,032 | WiX MSI + deferred custom actions |
| `src/tests` | 30 | 5,100 | dependency-free self-registering suite |

Largest translation units: `ContextMenu.cpp` 191 KB / ~6,970 lines;
`FuncExpression.cpp` 152 KB / ~5,600 lines; `string.h` 80 KB / ~3,200 lines;
`Verification.cpp` 47 KB; `Main.cpp` 47 KB; `Context.cpp` 41 KB.

---

## 2. Runtime architecture

### 2.1 Activation

Registration is declarative MSI: one registry-only component writes
`HKCR\CLSID\{BAE3934B-…}`, `InprocServer32` = `[$REGISTRATION]shell.dll`
(ThreadingModel Apartment), `Approved`, eight `ContextMenuHandlers` rows
(`*`, Directory, Drive, Folder, Directory\Background, DesktopBackground,
LibraryFolder±Background), overlay identifier, and the `.nss` ProgID
(`setup.wxs:120–193`; asserted byte-exact by `scripts/validate-msi-lifecycle.ps1:25–83`).
On Windows 11 the menu becomes primary via the `{86ca1aa0-…}\InprocServer32\TreatAs`
redirect, applied/removed by deferred custom actions with a marker-file ownership
protocol, or standalone by `shell.exe -register -treat`.

The host loads the DLL through normal shell-extension activation
(`DllGetClassObject`, `Main.cpp:1448`). `DllMain` only disables thread calls
(`Main.cpp:1425–1444`) — all real work moved out of loader lock. First activation
runs `BootstrapOnce` (`Main.cpp:1302`): detect explorer vs third-party host, pin the
module for process life (`GetModuleHandleExW … PIN`, `Main.cpp:233–250`), register the
layer-window class, install hooks. `DllCanUnloadNow` answers honestly from
`com_object_count` + live captures + hook state (`Main.cpp:1487–1502`).

### 2.2 Hook layer

| # | Hook | Mechanism | Scope | Purpose |
|---|---|---|---|---|
| 1 | `NtUserTrackPopupMenuEx` | IAT patch of `user32.dll`'s import of `win32u.dll` (`IATHook`, `Main.cpp:1333–1335`) | whole process | master interception point for **every** popup menu |
| 2 | `TrackPopupMenu`/`TrackPopupMenuEx` | per-module IAT patches across all loaded modules (`Main.cpp:1341–1358`) | fallback | used only when hook 1 cannot install |
| 3 | `CoCreateInstance` | inline detour (Microsoft Detours), explorer only, single transaction (`Main.cpp:1366–1377`) | explorer | suppress Win11 modern menu CLSID when `settings.priority`; drop configured handlers by CLSID before activation |
| 4 | Taskbar windows | `GWLP_WNDPROC` swap on `Shell_TrayWnd`/secondary (`taskbar_t::hook_all`) or `SetWindowSubclass` on the XAML bridge child (`Main.cpp:551–609`) | taskbar | route taskbar right-clicks to Shell's menu |
| 5 | Transient `WH_MOUSE` | thread hook while a taskbar button-down is being tracked (`Main.cpp:1247–1263`) | taskbar thread | catch button-up on empty taskbar |
| 6 | `SetWinEventHook` EVENT_OBJECT_CREATE…SHOW | installed per menu lifetime (`ContextMenu.cpp:4866–4871`) | process id/thread id | notice native menu windows being created → subclass them (`OnMenuCreate`) |
| 7 | `WH_KEYBOARD` | during menu lifetime (`ContextMenu.cpp:4875`) | menu thread | screenshot hotkey feature |

Hooks 1–2 funnel into one body: `NtUserTrackPopupMenu` (`Main.cpp:822–1029`),
an SEH function that initialises an STA apartment (`CoInitializeEx(APARTMENTTHREADED
| COINIT_DISABLE_OLE1DDE)` — the documented recommended form, see
https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex ),
builds Shell's replacement menu, tracks it with the *saved original* track function,
then dispatches the selection. The re-entrancy trap (calling the real
`TrackPopupMenu` re-enters through the same patched import slot) is documented and
avoided by calling the saved original directly (`Main.cpp:846–859`).

### 2.3 Menu replacement pipeline

For each intercepted popup:

1. `ContextMenu::CreateAndInitialize` (`ContextMenu.h:859–872`) builds a fresh
   `ContextMenu` instance; everything is per-popup, torn down after invocation.
2. Selection resolution: Explorer path via `WM_GETISHELLBROWSER` → `IShellBrowser`
   → view selection (`Selections::QuerySelected`); third-party-host path via the
   captured `IShellExtInit` data (§2.5); taskbar/desktop special cases.
3. Config snapshot: `Initializer` owns an immutable-ish `CACHE` generation
   (shared_ptr, swapped under mutex, ref-counted by each open menu —
   `Initializer.cpp:62–121`). A stale snapshot is rebuilt on timestamp change or F5.
4. `init_cfg()` (`ContextMenu.cpp:2837–4231`, ~1,400 lines) evaluates theme/font/
   effect/item-policy settings through the expression evaluator.
5. Native mirroring: the host's original HMENU is mirrored into a `menuitem_t`
   tree (`build_system_menuitems`, root scan at `ContextMenu.cpp:4910–4915`;
   lazy policy §2.4). Packaged `IExplorerCommand` verbs are composed onto the tree
   (`append_explorer_commands`, catalog TTL-cached in `ExplorerCommand.cpp:101–125`).
   Config `static` rules — verb overrides and CLSID-filtered handler suppression —
   are matched against the mirrored items (`build_main_system_menuitems`,
   `ContextMenu.cpp:4285–4488`; also consumed by the `CoCreateInstanceHook`
   filter), and `modify()` rules apply (`apply_system_modify_rules`).
6. Shell builds its own new HMENU: config-defined items from the parsed dynamic
   `menu()` trees (`CACHE->dynamic` via `prepare_new_items`), mirrored native
   items, positioned Top/Middle/Bottom/Auto
   (`OnInitMenuPopup`, `ContextMenu.cpp:1071–1580`). Each rendered item is a
   heap `MenuItemInfo : MENUITEMINFOW` (`MenuItem.h:174`) registered in a
   `GC<MenuItemInfo>` arena and reachable by command ID.
7. Display: the *host* window is subclassed (`WindowSubclassProc`), the resulting
   native menu windows (`#32768` class) are subclassed (`MenuSubClassProc`,
   ~45 message cases including private `MN_*` messages), restyled, given DWM accent
   layers (`OnMenuCreate`, `ContextMenu.cpp:5487–5562`; layered background windows
   `CreateLayer/draw_layer/UpdateLayered`), and every item is `MFT_OWNERDRAW`.
   `OnDrawItem` (~1,050 lines) paints text/icons/checks/separators with GDI +
   DrawThemeTextEx; foreign owner-draw content is composited through a memory bitmap
   with a GetDIBits/SetDIBits alpha pass (`ContextMenu.cpp:1771–1808`).
8. Selection: `TrackPopupMenuEx` returns the ID; `InvokeCommand` (`ContextMenu.cpp:5023`)
   dispatches: packaged `IExplorerCommand` → invoke in place; dynamic item → detach a
   worker STA thread with the `IShellBrowser` one-shot marshaled across
   (`_browser_marshal`, `OneShotMarshal.h`), running each `command=` expression
   (`InvokeCommands`, `ContextMenu.cpp:5156–5218`); otherwise native behaviour.
9. Teardown: `Uninitialize` destroys menu/hooks/layers; `delete this` self-destructs;
   capture cleared for exactly this HMENU (`Main.cpp:1013–1028`).

### 2.4 Native mirroring and lazy popups

The pre-1.9.20 design walked and initialised the entire host menu tree before first
paint. The current design follows the documented just-in-time model —
"Sent when a drop-down menu or submenu is about to become active"
(https://learn.microsoft.com/windows/win32/menurc/wm-initmenupopup ): the root level is
mirrored eagerly, each submenu materialises when opened
(`NativeMenuLazy.h`: `NativePopupState{handle, parent_position, initialized,
initializing, materialized}`; `native_popup_contents_known_empty` distinguishes
"pending" from "known empty", which fixed the NanaZip cascade regression in commit
`870fb43`). Synthetic `WM_INITMENUPOPUP` carries the correct
`MAKELPARAM(position, FALSE)` established by probe (AGENTS.md table). Only configs
whose `moveto` rules use a location selector forfeit laziness
(`choose_native_tree_policy`, `NativeMenuLazy.h:43–51`) — measured cost model pinned
by `test_native_menu_lazy.cpp`.

### 2.5 Third-party hosts (zero-item handler)

`ShellExtHandler : IShellExtInit, IContextMenu` (`ShellExt.h:427–486`) implements the
standard contract — "Shell extension handlers that export this interface must also
export IShellExtInit" (https://learn.microsoft.com/windows/win32/api/shobjidl_core/nn-shobjidl_core-icontextmenu )
— inserting **zero** items. It captures `pdtobj`/`pidlFolder` exactly as documented:
"For shortcut menu extensions, pdtobj identifies the selected file objects … pidlFolder
is either NULL … or specifies the folder for which the shortcut menu is being requested"
(https://learn.microsoft.com/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellextinit-initialize ).
Captures bind to the exact HMENU in a mutex-guarded registry with 30 s TTL, cross
apartments via move-only one-shot marshaling
(`CoMarshalInterThreadInterfaceInStream` / `CoGetInterfaceAndReleaseStream`,
documented single-unmarshal recommendation), and destruction happens outside the lock.
`window.is_contextmenuhandler` exposes the capability to scripts; the stock
`exclude.where` accepts any host that invoked the handler (`src/bin/shell.nss:6–16`).

### 2.6 Taskbar path

Windows 11 taskbar hit-testing ("did the click land on empty taskbar?") requires UI
Automation. Per Microsoft's threading rule, a client inspecting its own UI from the UI
thread risks stalls (https://learn.microsoft.com/windows/win32/winauto/uiauto-threading ),
so a process-lifetime MTA worker thread owns `IUIAutomation` and every element; only a
bool crosses back. The taskbar thread waits through `CoWaitForMultipleHandles` with a
250 ms budget — it enters the COM modal loop instead of freezing the very STA the
worker must talk to (https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-cowaitformultiplehandles ).
Answers cache per 16 px cell with TTL and invalidation on WM_SETTINGCHANGE/DISPLAYCHANGE
(`TaskbarHitCache.h`, `Main.cpp:287–528`). Measured: ~28 ms first query, ~2–3 ms after.

### 2.7 Configuration engine (NSS)

Pipeline: file → flat in-memory `wchar_t` buffer → **scannerless recursive-descent**
parse (the "Lexer" is a character cursor; there is no token stream) → AST owned by a
fresh heap `CACHE`, published atomically as `std::shared_ptr<const CACHE>` per
generation (`Initializer.cpp:62–121`) → tree-walk evaluation **per menu open, per
item, on the UI thread**.

- One abstract `Expression` base embeds a `Scope` map and virtual `Eval`; node kinds
  are literals, unary/binary/ternary operators, statement blocks/interpolated
  strings, arrays, variables/assignments/`for`, and `FuncExpression` — the universal
  callee for every built-in (`sel.path`, `if()`, `loc.x`, …).
- Dotted names flatten into an `Ident` of up to 50 precomputed DJB2 hashes
  (`Parser/Ident.h:10–16`; ~1,400 constants in `IdentHash.h`), so keyword/member
  comparison is integer equality and identifiers never exist as strings after lexing.
- Identifier classification happens during parsing: `verify_ident`
  (`Verification.cpp`, ~2,049 lines) decides variable vs function vs compile-time
  constant vs `"str.method"` sugar — a parse-time whitelist that encodes the same
  namespace/arity tree the runtime dispatch encodes again.
- Menu definitions become `NativeMenu` trees whose every property is an unevaluated
  expression (`Menu.h:243–337`). Imports resolve relative to the importing file,
  evaluate their path expressions at parse time, detect active-chain cycles, cap
  depth at 32; diamond imports are re-loaded (§4.5).
- Built-in dispatch is one monolithic nested switch over identifier hashes —
  `FuncExpression::Eval` (~2,075 lines) delegating to per-domain members (`eval_sel`,
  `eval_path`, `eval_io`, `eval_str`, `eval_reg`, …; 739 case/if branches across
  ~5,600 lines). Variables live in chained `Scope`s (global/runtime/local).
- The package index behind `appx.*` is deliberately cheap: one registry subkey
  enumeration with TTL + generation counter, per-package path/display-name resolution
  on demand (`Packages.h`).
- Nothing derived from selections is cached between opens, and within one open there
  is no memoization; the `eval`/`_result` members intended for it are inert
  (`FuncExpression.cpp:112–118`).

### 2.8 Rendering stack

GDI owner-draw for items; `DrawThemeTextEx`/theme data for styled text; layered
windows (`UpdateLayeredWindow`) plus undocumented DWM accent policy for blur/acrylic
backgrounds; PlutoSVG (static lib) rasterises SVG icons through WIC; WIC handles all
bitmap conversion to premultiplied BGRA (`WICImagingFactory.h`, factory is
thread-local and released before the owning `CoUninitialize`); a synchronized,
capacity-bounded `BitmapCache` keeps rasterised icons alive until config reload
(`BitmapCache.h` — redesigned after the unsigned-cache rejection in the fork audit).

### 2.9 Data models and ownership

Four coexisting item models:

| Model | Lifetime | Owner |
|---|---|---|
| `NativeMenu` (config AST node + property expressions) | config generation | `CACHE->statics/dynamic`, recursive raw `new/delete` |
| `menuitem_t` (mirror of host menu) | one popup | `__system_menu_tree`, recursive raw delete |
| `MenuItemInfo` (rendered item, IS a `MENUITEMINFOW`) | one popup | `GC<MenuItemInfo>` linked-list arena (`System.h:1280`) |
| `MUID` (localised title/id record) | config generation | `CACHE->muid` map |

Cross-links are manual back-pointers (`owner`, `owner_static`, `owner_dynamic`,
`system_source`, `native_source`, `ui`) with hand-written teardown order
(`Uninitialize`, `ContextMenu.cpp:4941–5021`). The fork's own history (duplicate-item
replacement writing to the wrong node, title pointer staleness, replaced-subtree
deletion while lookup maps still held pointers — fork-assessment §5.1–5.2) shows this
is where defects concentrate.

The support library under `src/shared` is itself an archaeology layer: a pre-C++11
personal framework in .NET clothing (`System.Text/Drawing/Registry` naming), of which
roughly **half by line count is commented out or unreferenced** — all four
`Collections/*` headers are fully commented out, and `StringBuffer.h`, `TString.h`,
`Text::Buffer.h`, `Int.h`, `MemoryManager.h` have zero external users
(grepped). Live code uses the STL directly (`std::vector`, `std::deque`,
`std::make_unique<Object[]>`) plus two survivors: `Text::string` (~686 `.move()`
call sites tree-wide) and `IComPtr` (~60 live instantiations, unit-tested
semantics in `test_comptr.cpp`). Every DLL translation unit sees all of it through
`pch.h:74–88` blanket includes and using-directives.

### 2.10 Installer and registration

Fully covered in the companion analysis; summary: declarative MSI registration +
transactional `RemoveExistingProducts` after `InstallInitialize`; immediate planners /
deferred SYSTEM mutators / rollback twins for TreatAs and legacy-config rescue;
hidden CustomActionData; marker-file proof for rollback; permanent user config;
post-install `SHChangeNotify(SHCNE_ASSOCCHANGED)`; validator script asserts emitted
tables byte-exact. `shell.exe` is no longer part of install; it remains the
developer/deployment registration tool with an ACL-borrowing fallback for the
TrustedInstaller-owned TreatAs key.

### 2.11 Tests and CI

Dependency-free self-registering harness (`test.h`), 28 suites, run automatically on
x64 builds (`tests.vcxproj:120–124`), gated per host architecture in `build.ps1:57–72`.
Strong coverage of string/encoding/registry/path/WIC/shell-ext-capture/marshaling/
lazy-menu/package-index/TreatAs-plan/config-transfer invariants, mostly with real OS
objects rather than mocks. Not covered: menu building/drawing pipeline, expression
evaluation end-to-end, hooker internals, theme painting, ExplorerCommand hosting
runtime, and anything needing a real injected host. CI builds x64/x86/arm64 and runs
tests on x64/x86 only; `validate-msi-lifecycle.ps1` does not run in CI.

---

## 3. Contract grounding and the undocumented surface

### 3.1 Documented contracts the design correctly relies on

- `WM_INITMENUPOPUP` just-in-time semantics — lazy popups follow the page verbatim
  (https://learn.microsoft.com/windows/win32/menurc/wm-initmenupopup ).
- `TrackPopupMenu` return-value/flag semantics incl. `TPM_RETURNCMD` and the
  notification-icon foreground quirk (https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-trackpopupmenu );
  the `PostMessage(WM_NULL)` fix in `ShowTaskbarContextMenu` matches the documented
  workaround.
- `IShellExtInit`/`IContextMenu` pairing and parameter meaning (links above).
- One-shot COM stream marshaling for the single-consumer cross-apartment handoffs
  (selection capture; `IShellBrowser` to the invoke worker)
  (https://learn.microsoft.com/windows/win32/com/single-threaded-apartments ,
  https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-comarshalinterthreadinterfaceinstream ).
- UIA on a dedicated MTA that owns no windows; `CoWaitForMultipleHandles` modal-loop
  wait on the STA caller (links in §2.6).
- MSI custom-action context rules: deferred actions read only `CustomActionData`,
  rollback sequenced before the deferred action it reverses
  (https://learn.microsoft.com/windows/win32/msi/deferred-execution-custom-actions ,
  https://learn.microsoft.com/windows/win32/msi/rollback-custom-actions ).

### 3.2 Undocumented compatibility surface (risk inventory)

These have **no vendor documentation**; each is load-bearing and pinned only by
observation:

| Surface | Where | Risk if changed by Windows |
|---|---|---|
| Private menu messages `MN_*` (0x01E0–0x01FF): `MN_GETHMENU`, `MN_SELECTITEM`, `MN_SIZEWINDOW`, `MN_OPENHIERARCHY`, `MN_BUTTONDOWN/UP`, `MN_FINDMENUWINDOWFROMPOINT`, `MN_ENDMENU`, … | `Window.h:35–86`, handled in `MenuSubClassProc` | menu interaction breaks wholesale |
| `WM_UAH*` menu-drawing messages (0x0090–0x0096) | `Window.h:25–31` | themed-frame drawing differences |
| uxtheme ordinals 94–98 (`GetImmersiveColorFromColorSetEx` etc.) | `Main.cpp:1628–1664` | dark-mode colour detection fails |
| `SetWindowCompositionAttribute` + `AccentPolicy` struct | `Window.h:220–280` | acrylic/blur effects silently stop |
| `win32u!NtUserTrackPopupMenuEx` being the internal target of `user32!TrackPopupMenu(Ex)` | `Main.cpp:1333–1335` (probe-established) | master hook stops firing; fallback 2 engages (slower, broader) |
| Hardcoded resource IDs/titles of ~20 Windows modules (`shell32` menu 208/210/211/215/217/223/225/…, `twinui.pcshell.dll` 10944, …) | `Initializer::load_mui` (`Initializer.cpp:218–781`) | localised titles fall back to English strings compiled in |

This inventory is the price of the product's promise ("replace the menu everywhere,
themed"), not an accident — but it must be isolated, probed, and tested, never spread.

---

## 4. Strategic assessment

### 4.1 Strengths worth protecting

1. **Correct integration primitives.** Zero-item `IContextMenu` capture, one-shot
   marshaling, lazy popups honouring `WM_INITMENUPOPUP`, MTA UIA worker, loader-lock
   hygiene, honest `DllCanUnloadNow`, module pinning with rename-based upgrades.
   These were each earned the hard way (see AGENTS.md table); do not regress them.
2. **Snapshot-isolated configuration.** Immutable-generation `CACHE` shared by
   pointer means reload races are structural-free; menus hold a stable snapshot.
3. **Test culture.** Real-object invariant tests with measured-cost models; the
   suites encode *why*, not just *what*.
4. **Declarative installer core.** Registration is data; only irreducibly
   conditional state (TreatAs) is procedural, with rollback proofs.

### 4.2 Complexity hotspots (ranked)

1. **`ContextMenu.cpp` is six subsystems in one file (~6,970 lines).** Config
   application (`init_cfg` ≈1,400 lines), native mirroring, popup lifecycle,
   painting (~1,050-line `OnDrawItem`), window/layer management (~600 lines),
   tooltips/screenshots/command invocation, plus two window procedures. Every
   concern shares the `ContextMenu` object's ~90 fields, so nothing can be tested
   or reasoned about in isolation. *Evidence:* function map at lines 275–6751.
2. **Three-and-a-half coexisting item models with hand-managed lifetimes**
   (§2.9). Raw-pointer trees, back-pointer webs, an arena misnamed `GC`, and
   deletion-order sensitivity. The fork's worst historical bugs lived here.
3. **Monolithic built-in dispatcher.** `FuncExpression::Eval` + friends: 739
   branches, mixed concerns (string math next to registry reads next to icon
   rasterisation), all on the menu thread. Adding a function means editing the
   giant switch; testing one means loading the world.
4. **Undocumented-surface entanglement.** `MN_*` handling, accent policy, and
   immersive-colour ordinals are interleaved with ordinary logic in the same
   procedures instead of behind named adapter boundaries (§3.2).
5. **`init_cfg` imperative settings application.** ~1,400 lines of hand-rolled
   parse-eval-assign per setting, duplicated shape for colours/fonts/effects.
6. **Installer residue.** Four duplicated condition literals; `shell.exe`
   reimplements MSI-owned registration rows with a live drift bug (space-prefix);
   dead `ValidatePath` export; legacy-config bridge (~450 lines) serving sources
   older than 1.9.20 indefinitely.
7. **Macro-singleton runtime.** `#define _initializer/_loader/…` global accessors,
   `inline static` mutable state on `ContextMenu` (`Processes`, `HookMap`,
   `point`, `FontNotFound`), two parallel `hooks_installed` flags
   (`Main.cpp:209` vs `ShellExt.h:73`).

### 4.3 Inefficiency hotspots (ranked)

1. **Per-open, per-item expression evaluation on the UI thread.** Every property of
   every visible item is re-walked each menu build, allocating `Object`s and
   copying `string`s (custom value type, heap buffer per copy, `string.h:332–451`)
   — for a typical desktop there are hundreds of evaluations before first paint.
   The opt-in timers exist precisely because this dominates.
2. **`CoCreateInstanceHook` work per handler activation.** During menu build,
   Explorer activates every registered handler; the hook evaluates
   `settings.priority` (for the Win11 CLSID) and walks `cache->statics` evaluating
   each `where=` expression synchronously in every activation
   (`Main.cpp:685–805`). These are pure-per-generation results recomputed per call.
3. **ExplorerCommand catalog scan.** On TTL expiry the first menu pays an
   enumeration of every installed package plus reading/parsing each
   `AppxManifest.xml` (≤4 MB each) on the UI thread (`ExplorerCommand.cpp:72–99`);
   unlike `PackageIndex` there is no cheap-index/stale-while-revalidate split.
4. **Alpha-fix pixel pass for foreign owner-draw items.** GetDIBits/SetDIBits round
   trip per draw (`ContextMenu.cpp:1783–1802`).
5. **String churn.** The custom `string` copies on every by-value pass; growth
   1.5× is fine, but pervasive `.move()` calls signal manual transfer everywhere;
   several audited bugs (`release(n-1)`, unterminated API buffers) were of this
   class.
6. **Registry/file I/O re-executed inside evaluation.** `reg.get/open` opens keys on
   every evaluation (`FuncExpression.cpp:5273–5288`); `color.dwm/accent` opens the
   DWM registry key per lookup (`:5805–5808`); `ini.get` hits disk per call
   (`:746–761`) — all per menu open, per item, with no per-open cache.
7. **Allocation churn in the evaluator.** `BinaryExpression::Eval` builds up to two
   string temporaries per operand comparison (`OperatorExpression.cpp:69–134`);
   every array result heap-allocates `new Object[n+1]`; regex objects are
   constructed per call; `ForStatement::Eval` deep-copies the variable's tree every
   iteration (`Expression.cpp:142`). Structural hotspots: `CACHE::find_muid`
   falls back to a full-map scan on miss (`Cache.h:261–272`); the ~150-entry colour
   table is searched linearly per lookup; every `Expression` embeds an
   `unordered_map` `Scope` whether or not it declares variables
   (`Expression.h:35`).
8. **Dead weight shipped:** `DllGetClassObjectHook` + its Detours member never
   installed (`Main.cpp:203, 807–814`); `OnDrawItem_D2D` declared, never defined
   (`ContextMenu.h:762`) while `d2d1.lib`, `dwrite.lib`, `Winmm.lib` remain forced
   imports (`Main.cpp:50–52`) — three system DLLs mapped into every host process
   for nothing; `InlineHook` commented block (`Hooker.h:444–603`); CA `ValidatePath`
   export wired nowhere (`ca/dllmain.cpp:941–1006`). In `src/shared`: all
   commented-out collections plus `StringBuffer`/`TString`/`Buffer`/`Int.h`/
   `MemoryManager` with zero users; **five overlapping UTF-8 validators** coexist
   (`Encoding.h:159-204, 760-801, 803-856, 858-880, 882-962`) alongside a
   hand-rolled `Utf16ToUtf8` with real defects (`short w = src[i]` sign-breaks
   surrogates/astral planes and its `w <= 0x10ffff` branch is unreachable,
   `Encoding.h:625–649`). In the evaluator: pasted-in tutorial `WindowProcedure`
   (`FuncExpression.cpp:75–87`), ~150 lines of commented COM experiments
   (:1885–2030), unused `TokenId`/`TokenType`, self-false `Ident::equals`
   (`Parser/Ident.h:168–180`).
9. **Latent correctness debt in the support library** (dead-but-armed or
   narrow-trigger): shallow-copy `assign(const string&)` that steals the source
   pointer without nulling it — a double free one call away (`string.h:651–653`);
   explicit-destructor-then-reuse inside live methods
   (`CommandLine.h:187–188`, `PlutoVGWrap.h:475/482`); missing return in non-void
   `PlutoVG::clear()` (`PlutoVGWrap.h:428–439`); pixel-index arithmetic adding
   `x` bytes instead of pixels (`PlutoVGWrap.h:210`); copyable resource owners
   (`auto_handle` copyable by default `auto_ptr.h:267`; `File` copies a closing
   `FILE*`, `File.h:52–71`; `RegistryKey` pseudo-refcount increments a fresh
   member, `Registry.cpp:165–173`); unchecked `operator[]` and a catch-all
   template conversion operator on the string class (`string.h:1931–1935,
   1963–1964`).

### 4.4 Verified live defects found in this scan (expression engine)

Each was re-verified first-hand in this tree before inclusion; all are small,
testable fixes.

| Defect | Location | Effect |
|---|---|---|
| Numeric `<` inverted | `FuncExpression.cpp:527–535` — `case IDENT_LESS:` numeric branch computes `arg0 > arg1` while the string branch correctly uses `<`; no operand swap exists anywhere else (grepped) | every numeric `a < b` comparison in user configs is wrong for numbers |
| `TernaryExpression::Copy` checks the fresh object, not `this` | `Expression.h:140–148` — ctor leaves `True/False` null, so both branches are silently dropped on copy | any config path that clones a ternary (e.g. `for` bodies via `ForStatement::Eval`) loses its branches |
| `FuncExpression::Copy` drops/mis-handles `Array` | `IdentExpression.h:174–189` — copies `Array` only inside the `Child` branch and dereferences it unguarded there | cloned `foreach(x in arr)` loses or crashes on its source |
| Duplicate-import check falls through | `Parser.cpp:1150–1162` — logs "already imported", `break` exits only the scan loop, file is pushed and loaded again | diamond imports re-execute (side effects: variables re-assigned, parse time doubled) |
| `msg(right)` maps to `IDNO` | `Constants.h:21` `{ IDENT_MSG_RIGHT, IDNO }` | right-click answer reported as "No" |

Related fragility noted but not separately verified here: control flow by magic
numbers (`break`/`continue` signaled by evaluating to the identifier's own hash,
`Expression.cpp:160–172`); self-modifying `eval()` that replaces its argument node
mid-evaluation (`FuncExpression.cpp:118–133`); `Array2Expression::Copy` returning a
`NumberExpression` (`LiteralExpression.h:81–84`).

---

### 4.5 Risk register

- **Windows evolution vs undocumented surface** (§3.2): the single largest
  existential risk; a Win11 menu refactor can break Shell regardless of code quality.
- **Uninstall hard-fail on unreadable TreatAs key** (`ca/dllmain.cpp:753–761`) —
  support trap requiring out-of-band repair script.
- **ACL-borrow fallback persistence failure** leaves widened DACL, log-only
  (`exe/src/Main.cpp:149–159, 327–335`).
- **Third-party-host smoke gap**: Total Commander/DOpus/Everything paths untested
  on this machine (AGENTS.md); nested/same-thread menu sequences vs single-slot
  assumptions.
- **ARM64 tests never execute** in CI or locally (x64 runner/host).

---

## 5. Improvement roadmap

Ranked by value ÷ risk. "Simplest fully functional solution" is stated for each.

### P0 — delete dead weight, break nothing (hours each)

1. Remove `d2d1`/`dwrite`/`Winmm` pragma links + the orphan `OnDrawItem_D2D`
   declaration; remove never-installed `DllGetClassObjectHook` machinery; delete
   commented-out blocks (`Hooker.h`, `VnPatchIAT`, all commented-out
   `Collections/*`, `Int.h`, `auto_ptr` block). Shrinks imports of every host
   process; zero behavioural risk.
2. Delete zero-user support-library files outright: `StringBuffer.h`,
   `TString.h`, `Text::Buffer.h`, `MemoryManager.h`; replace `GC<MenuItemInfo>`
   with `std::vector<std::unique_ptr<MenuItemInfo>>`. Consolidate encoding:
   keep the strict validator (`Encoding.h:159–204`) + `Unicode::From/ToUTF8`,
   delete the four duplicate validators and the buggy `Utf16ToUtf8`.
3. Fix the dead-but-armed defects: `string::assign(const string&)` shallow-copy
   (`string.h:651–653`), `PlutoVGWrap.h` missing return (:428) and byte-vs-pixel
   indexing (:210), explicit `this->~T()` reuse in `CommandLine::Parse`/
   `PlutoVGWrap` re-create paths, make `auto_handle`/`File` non-copyable,
   remove the fake `RegistryKey` refcount.
4. **Expression-engine correctness fixes** (§4.4 table): numeric `<`
   (`FuncExpression.cpp:532`), `TernaryExpression::Copy` condition
   (`Expression.h:143–146`), `FuncExpression::Copy` Array handling
   (`IdentExpression.h:174–189`), duplicate-import fall-through
   (`Parser.cpp:1150–1162`), `IDENT_MSG_RIGHT` constant (`Constants.h:21`) —
   each a one-to-five-line diff plus a pinning test in `src/tests`.
5. Delete CA `ValidatePath`; fix `ARPNOREMOVEY` typo (`ui.wxi:324`); collapse the
   four duplicated TreatAs condition literals into one scheduled property
   (`setup.wxs:382–394`).
6. Run `validate-msi-lifecycle.ps1` in CI after build (free regression gate).
7. Share registration row constants between `RegistryConfig.h` and `setup.wxs`
   generation (or assert equality in the validator) and fix the space-prefix drift
   in `shell.exe check()`.

### P1 — de-risk and de-fang the hot path (days each, high payoff)

8. **Extract subsystem seams in `ContextMenu.cpp`** without changing behaviour:
   `MenuPainter` (OnDrawItem/measure/glyphs), `NativeMirror` (menuitem_t tree +
   lazy popups), `LayerChrome` (WND/layers/accent), `ConfigBinder` (init_cfg),
   `CommandDispatcher` (invoke). Keep the public `ContextMenu` façade; move the
   undocumented-message handling into `NativeMirror`/`MenuHostAdapter` with one
   comment block per `MN_*` contract (probe citation, per AGENTS.md policy).
   Enables the missing test targets identified in the test inventory.
9. **Single source of truth for function metadata.** The namespace/arity rules are
   hand-encoded twice — parse-time whitelist (`Verification.cpp::verify_ident`,
   ~2,049 lines) and runtime dispatch (`FuncExpression.cpp`) — plus string-method
   rules a third and fourth time (`Verification.cpp:388–433`, `:1995–2047`);
   adding one built-in touches ≥2 files and they can drift silently. Replace with
   one declarative table `{root, member, min/max args, handler}` consumed by both
   sides; simplest interim step is generating the arity checks from the same macro
   list that names the `eval_*` cases.
10. **Cache per-generation evaluation results.** Memoise `settings.priority` and the
    static-handler `where=` verdicts once per `CACHE` generation so
    `CoCreateInstanceHook` becomes a hash lookup; add per-menu-open memoisation of
    side-effect-free expressions (literals, `sys.*`, `theme.*`, pure string ops —
    exclude `io/reg/clipboard/input/cmd`), which also removes the per-evaluation
    DWM/registry reads of §4.3 item 6.
11. **Make the ExplorerCommand catalog cheap-first:** registry/manifest index like
    `PackageIndex` (cheap identity list + TTL + generation), full manifest parse
    off-thread with stale-while-revalidate publish; menu opens never block on it.
12. **Data-drive `init_cfg`.** Replace the imperative wall with a table of
    {setting key, kind, apply lambda}; cuts ~1,000 lines and makes settings
    testable individually.
13. **TreatAs uninstall resilience:** downgrade `inaccessible` during REMOVE=ALL
    from hard-fail to warn-and-continue with explicit user guidance (the ACL
    repair script already exists), or attempt the documented ACL-borrow path in the
    deferred action. Preserves safety (never deletes what it cannot read) without
    bricking uninstalls.
14. **Persist ACL-restoration failure visibly** in `shell.exe -register` (event log
    entry + non-zero exit facet) instead of destructor-only retries.
15. **Evaluator hygiene:** replace sentinel-hash `break`/`continue`
    (`Expression.cpp:160–172`) with the existing `Context::Break/Continue` flags;
    compare operands as views instead of building `to_string()` temporaries in
    `BinaryExpression::Eval`; stop `eval()` from mutating its own AST.

### P2 — structural modernisation (weeks; schedule behind P1 wins)

16. **Consolidate item models around explicit ownership.** Introduce
    `std::unique_ptr` ownership for `NativeMenu`/`menuitem_t` trees (children own
    children), replace `GC<MenuItemInfo>` with a vector-of-nodes arena, and make
    cross-links weak/index-based where they are not ownership. This removes the
    deletion-order bug class permanently; do it incrementally per subsystem after
    seam extraction (P1 8).
17. **Retire the custom `string` gradually.** Freeze new features on it; the
    lowest-risk structural step is swapping its interior to a `std::wstring`
    member while preserving the `buffer(n)`/`release(len)` Win32 fill-pattern API
    (its real raison d'être), then fixing semantics (`assign` shallow copy,
    bounds-checked `operator[]`, constrained conversion operator, `c_str()`
    returning non-null for empty). ~686 `.move()` sites and ~50 fill-pattern
    sites keep working unchanged; behaviour is already pinned by
    `test_string_search/test_path_bounds/test_loadstring`. Wholesale replacement
    is not realistic; boundary `std::wstring_view` APIs capture most remaining
    value.
18. **Adopt `wil` for COM/HANDLE RAII at touched sites.** `IComPtr`'s documented
    ownership semantics map 1:1 onto `wil::com_ptr`; migration is mechanical
    per-file (~60 instantiations) and deletes hand-written Release discipline
    (≈15 manual releases in `WICImagingFactory.h` alone). Do it opportunistically
    in files already open for P1 work rather than as its own sweep.
19. **Split `FuncExpression` dispatch per domain** (sel/path/io/reg/sys/str/icon)
    into separate translation units behind the P1 9 metadata table, enabling unit
    tests per domain (test gap #1) and shrinking the monolith without behavioural
    change.
20. **Expire the legacy-config bridge** once the supported upgrade floor passes
    1.9.20 (removes ~450 lines + one Upgrade row + four CAs; the permanent-config
    + file-versioning rule already covers modern sources).
21. **Capability additions unlocked by the above:**
    - *Async freshness*: run PackageIndex/catalog refreshes on a background
      thread triggered by `SHChangeNotify`-observed events instead of TTL-on-click.
    - *Accessibility audit*: verify screen-reader exposure of owner-drawn items and
      label gaps; menus are a11y-critical surface. *(Unverified in this scan —
      needs a Narrator/UIA-inspector pass on a real machine.)*
    - *Per-monitor DPI v2*: context-aware DPI for layer windows (the DPI request
      already asks the window, commit `b9a3caa`).
    - *Config editor story*: `.nss` already registers notepad; a schema + LSP-style
      validator reusing `Parser`/`Verification.cpp` in `shell.exe` would be a
      low-cost, high-user-value addition.

### Explicitly rejected alternatives (assessed, not viable)

- **MSIX packaging**: cannot provide parity — no host injection, no icon overlays,
  Packaged COM lives in a private registry location the shell handlers don't read
  (https://learn.microsoft.com/windows/msix/desktop/desktop-to-uwp-prepare ;
  https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-desktop9-fileexplorerclassiccontextmenuhandler
  covers only the classic-handler slice on Win11 21H2+).
- **Full custom-drawn menu window replacing native menus**: would shed the `MN_*`
  surface but lose keyboard navigation, IME, accessibility, and drag semantics that
  native menus provide for free; the cost/benefit remains negative versus adapter
  isolation (roadmap P1 8).
- **Global Interface Table for the cross-apartment handoffs**: superseded by the
  documented one-shot stream primitive; single-consumer invariant proved in R5.

---

## 6. Verification status

- All file:line references verified in this working tree at `a3431df`.
- The §4.4 expression-engine defects were each re-read and confirmed first-hand in
  this tree (including a grep proving no operand swap for `<` exists elsewhere);
  the remaining "related fragility" items were reported with evidence but not
  independently re-proven here.
- Microsoft Learn pages fetched this session: IContextMenu,
  IShellExtInit::Initialize, TrackPopupMenu, WM_INITMENUPOPUP, Using Menus.
  Pages quoted from prior verified audits and code comments (UIA threading,
  CoWaitForMultipleHandles, marshaling, MSI custom-action context) carry their
  canonical links inline.
- WiX claims cite `.bin/wix-docs/schema/wxs/{customaction,registryvalue,component}.mdx`.
- `MN_*`, `WM_UAH*`, uxtheme ordinals, `AccentPolicy`, and `win32u` forwarding are
  **undocumented**: treated as probe-established compatibility facts, labelled as such.
- Not verified here (needs machines/hosts): real upgrade matrix, third-party hosts,
  ARM64 test execution, accessibility behaviour, visual fidelity across themes.
