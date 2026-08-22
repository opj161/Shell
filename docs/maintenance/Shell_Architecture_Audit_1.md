## Executive assessment

The highest-value change is **not** a UI rewrite, parser rewrite, or an attempt to make Shell behave like a conventional Microsoft-supported shell extension**.** Given your stated goal, takeover should remain the product.

The architecture I would aim for is:

> **A very small, carefully isolated takeover shim, feeding a fast in-process menu engine from precomputed immutable snapshots. Nothing optional or potentially unbounded should run synchronously before the menu appears.**

That is the main architectural correction.

I inspected the uploaded `Shell-main (4).zip` repository, including the DLL, manager EXE, installer/custom actions, shared Win32 layer, parser/expression engine, default NSS configuration, tests, CI and the existing maintenance assessment. The archive is about 3.3 MB.  The first-party C/C++ code is roughly 70k production lines plus about 6k test lines. The critical implementation is concentrated particularly heavily in `src/dll/src/ContextMenu.cpp`, which alone is 6,974 lines.

My main conclusions are:

1. **The custom renderer and NSS rule engine are not the architectural problem.** They are the product's strongest differentiators and should stay.
2. **The current lazy handling of native HMENU submenus is good architecture.** Preserve it.
3. **The COM selection-capture work is now substantially correct and worth preserving.**
4. **The biggest immediate defect is Windows 11 ExplorerCommand/package discovery on the menu-opening path.** This can turn a right-click into package enumeration, manifest I/O, COM activation and explicitly slow `GetState` calls before first paint.
5. **Takeover uses too many overlapping interception mechanisms.** The private `NtUserTrackPopupMenuEx` IAT interception may remain necessary, but it should be isolated as one replaceable backend rather than mixed with broad per-module popup hooks and a global `CoCreateInstance` policy path.
6. **Taskbar takeover has been improved substantially, but still allows a 250 ms UI-thread wait on UI Automation.** That should become zero-wait.
7. **`ContextMenu` has become the architectural gravity well.** Do not rewrite it. Extract well-defined seams incrementally.
8. **The best user-facing expansion is a Compatibility/Reliability Center, safe-mode/circuit-breaker behavior, a one-shot Windows-menu bypass, interactive menu search, and a rule/context inspector.** All can leverage capabilities Shell already almost has, without turning the project into another Windows shell replacement.

I would describe the desired strategy as:

> **Minimal unsupported shim + bounded synchronous data plane + asynchronous snapshot control plane.**

That is the direction I would put engineering effort into.

---

# 1. What this project actually is

Despite the term "custom shell", this code is **not replacing `explorer.exe` as the Windows shell through Winlogon**. It is a sophisticated context-menu takeover engine operating inside Explorer and compatible third-party hosts.

The flow is approximately:

```text
Explorer / third-party file manager
        │
        ├── registered IContextMenu shell extension
        │      └── captures exact selection / background PIDL
        │
        ├── popup-menu interception
        │      └── catches TrackPopupMenu-family presentation
        │
        └── Windows 11 taskbar interception
               │
               ▼
          ContextMenu session
               │
      ┌────────┼─────────┐
      │        │         │
 Native HMENU  NSS     Win11 packaged
  commands    rules    IExplorerCommand
      │        │         │
      └────────┼─────────┘
               ▼
         custom composed menu
               │
       Win32 owner-draw UI
               │
               ▼
      native / custom command
            invocation
```

There are four principal products/subsystems.

### `shell.dll`

This is the actual runtime. It is:

* a registered in-process shell extension,
* the popup interception layer,
* the menu compositor,
* the NSS runtime/parser consumer,
* the owner-draw renderer,
* the Explorer integration layer,
* the taskbar integration layer.

### `shell.exe`

This is primarily management/registration/lifecycle infrastructure, including the Windows 11 `TreatAs` setup.

### MSI/custom actions

These handle installation, rollback, legacy configuration migration, registry ownership and TreatAs transactionality.

### NSS parser/expression language

This provides the actual product model: dynamically generated items, conditional visibility, matching/removing/reordering native items, commands, variables, selection-dependent expressions, styling, images, SVG, nested menus and so on.

The README accurately describes the system as a File Explorer context-menu manager rather than a full replacement shell (`README.md:6-35`).

---

# 2. The current takeover mechanism, precisely

## DLL bootstrap

`DllMain` is now commendably minimal:

* stores the HINSTANCE,
* calls `DisableThreadLibraryCalls`,
* otherwise does effectively nothing.

See `src/dll/src/Main.cpp:1425-1444`.

The substantial initialization happens later from `DllGetClassObject` through `BootstrapOnce()` (`Main.cpp:1302-1398`, `1453`).

This is exactly the right pattern. Microsoft explicitly warns that `DllMain` runs under the loader lock and recommends deferring almost everything, including COM, registry and User32 work. ([Microsoft Learn][1])

**Do not "clean this up" by moving startup back into `DllMain`.**

Once bootstrapped, the DLL intentionally pins itself for the lifetime of the process. Given the hooks, worker thread and window procedures, that is defensible. Trying aggressively to make the DLL unloadable would actually make this architecture more dangerous.

---

# 3. Selection capture is one of the parts I would keep

Shell registers an Apartment-threaded in-proc context-menu handler in `RegistryConfig.h:97-180`. Microsoft documents shell extension handlers as in-process COM objects and specifically recommends the Apartment threading model. ([Microsoft Learn][2])

The current handler is deliberately minimal:

* `IShellExtInit::Initialize` captures the host's `IDataObject`/folder PIDL.
* It converts the data to an `IShellItemArray`.
* `QueryContextMenu` binds that pending selection to the exact `HMENU`.
* It inserts zero items.

See `src/dll/src/ShellExt.cpp:36-82`.

That is clever because the handler is used primarily as a **selection/context transport into the eventual takeover**, rather than as the final menu implementation.

More importantly, the current tree has fixed one of the nastiest problems in this sort of software: cross-apartment COM objects.

`CapturedSelection` uses:

* `CoMarshalInterThreadInterfaceInStream`
* followed by a one-shot `CoGetInterfaceAndReleaseStream`.

See `src/dll/src/Include/ShellExt.h:142-230`.

Microsoft explicitly requires apartment-bound interface pointers to be marshaled between apartments and identifies these exact functions for doing it. ([Microsoft Learn][3])

The HMENU-indexed capture registry also:

* has a TTL,
* distinguishes individual concurrent handlers,
* moves expired objects outside the mutex before COM cleanup.

That is solid.

### Recommendation

**Preserve this mechanism.**

In fact, I would elevate it architecturally. Make it the canonical `HostSelectionCapture` interface for every host where it is available.

Explorer-specific `IShellBrowser` discovery should become an **enrichment provider**, not the conceptual foundation of selection.

That matters because it gives third-party hosts a clean supported route into Shell without having to pretend they are Explorer.

---

# 4. The native HMENU laziness is also the right solution

This is another area I would explicitly *not* rewrite.

The current system:

1. materializes the root native menu,
2. records handles for native submenus,
3. does **not** recursively initialize them,
4. when the user opens a corresponding custom submenu, sends the host its `WM_INITMENUPOPUP`,
5. enumerates only that newly initialized level,
6. repeats recursively only as the user actually navigates.

See:

* `ContextMenu.cpp:4489-4507`
* `4512-4657`
* `4659-4672`
* `4680-4700`.

This matches the Windows menu contract unusually well for a takeover engine.

Microsoft defines `WM_INITMENUPOPUP` precisely as the notification sent when a drop-down or submenu is about to become active, allowing the application to modify that menu immediately before display rather than constructing everything in advance. ([Microsoft Learn][4])

The current code even correctly acknowledges that sending the message executes arbitrary host/third-party code synchronously and avoids holding Shell locks over that call.

That is good architecture.

There is one exception: `moveto` rules which depend on descendant topology sometimes force eager discovery. The policy is explicitly gated at `ContextMenu.cpp:4894-4908`.

That is a reasonable capability/performance trade.

### Recommendation

Keep:

* root-only initial scan,
* one-level JIT initialization,
* native menu handles as authoritative backing data,
* special eager mode only for rules that mathematically require descendant discovery.

Do **not** attempt to "simplify" this by eagerly cloning the whole HMENU tree. That would make takeover less correct and slower.

---

# 5. P0: Windows 11 ExplorerCommand discovery is the highest-value fix

This is where I see the clearest current architectural mistake.

Before displaying a context menu, `ContextMenu::Initialize()` performs:

```text
selection resolution
config snapshot
native root scan
append_explorer_commands()
modify rules
first display
```

See `ContextMenu.cpp:4744-4928`.

`append_explorer_commands()` is therefore directly on first-paint latency.

## What `append_explorer_commands()` can currently do

The catalog cache has only a 30-second TTL:

`ExplorerCommand.cpp:17`

When expired or uninitialized, `catalog_snapshot()` synchronously calls `scan_catalog()`:

`ExplorerCommand.cpp:106-125`.

That scan:

* enumerates installed package full names,
* resolves every package install directory,
* opens its `AppxManifest.xml`,
* reads up to 4 MB per manifest,
* parses it,
* extracts `FileExplorerContextMenus`.

See `ExplorerCommand.cpp:72-99`.

That entire operation can occur because the user right-clicked a file.

It then activates matching COM classes:

`ExplorerCommand.cpp:177-192`, `419-427`.

Then, for each command:

```cpp
cmd->GetState(selection, FALSE, &state);

if(hr_state == E_PENDING)
    cmd->GetState(selection, TRUE, &state);
```

`ExplorerCommand.cpp:200-206`.

That second call is the key issue.

Microsoft's `IExplorerCommand::GetState` contract says that when `fOkToBeSlow` is FALSE, a command should return `E_PENDING` rather than performing work that could make the UI unresponsive. TRUE explicitly permits the expensive work. ([Microsoft Learn][5])

So the current implementation effectively does:

> "Please don't block."
>
> Extension: "I would have to block."
>
> Shell: "Fine, block."

And it does this before showing the menu.

Microsoft's current Windows 11 guidance is even more explicit: `GetTitle`, `GetIcon`, `GetState` and other menu-construction methods should remain fast, with expensive work performed after `Invoke`. ([Microsoft Learn][6])

This should be the first architectural refactor.

## Recommended design: `ModernVerbCatalogService`

Move package/manifest discovery into a process-lifetime service.

```text
Explorer starts / Shell first loads

        background worker
               │
        enumerate packages
               │
        parse changed manifests
               │
      immutable catalog snapshot
               │
       atomic/shared_ptr publish
               ▼

Right click
    │
    └── read current snapshot
             O(1), no disk
```

### Specifically

**1. Start catalog warming immediately after `BootstrapOnce`, on a worker.**

Never make `catalog_snapshot()` responsible for discovering packages.

It should literally be something close to:

```cpp
std::shared_ptr<const ModernVerbCatalog> snapshot();
```

No scanning. No waiting.

**2. Use stale-while-revalidate.**

After TTL expiration:

* keep returning the last snapshot,
* queue an asynchronous refresh,
* publish a new snapshot when complete.

A stale packaged-command catalog for 30 seconds or even several minutes is vastly preferable to freezing Explorer.

**3. Persist the catalog across Explorer restarts.**

A compact LocalAppData cache could contain:

* package full name/version,
* manifest timestamp/hash,
* registered file Explorer menu types,
* command CLSIDs.

Then a fresh `explorer.exe` can start with a usable snapshot before the package scan completes.

**4. Unify this with `PackageIndex`.**

There are currently effectively two package discovery systems.

`Packages.cpp:306-397` has another package index that synchronously makes whichever caller noticed expiry perform the scan.

Worse, `PackagesCache` lives inside `CACHE`:

`src/dll/src/Include/Cache.h:227-244`.

`CACHE` is an immutable configuration-generation snapshot. Installed Windows packages are **machine/user environment state**, not NSS configuration.

That ownership is conceptually wrong.

Move package discovery completely outside config snapshots.

One process-level `PackageCatalogService` should serve:

* NSS `package.*` functions,
* ExplorerCommand manifest registration discovery,
* diagnostics.

**5. Stop calling `GetState(TRUE)` during menu construction.**

If `GetState(..., FALSE)` returns `E_PENDING`:

* use a previously cached result for the same selection shape when sufficiently fresh, or
* treat that optional modern command as unavailable for this menu invocation,
* record telemetry that the CLSID requested slow state evaluation.

Do not make the entire context menu hostage to an optional Windows 11 verb.

For commands important enough that disappearing for a first cold opening is unacceptable, a later iteration can introduce carefully warmed state through a dedicated COM worker strategy. I would not make that the first implementation, because COM apartment semantics are precisely where apparently simple asynchronous solutions tend to acquire teeth.

**6. Reuse the captured `IShellItemArray`.**

`ensure_selection_array()` falls back to reconstructing selection by calling `SHParseDisplayName` per path (`ExplorerCommand.cpp:256-313`).

That is another operation Microsoft itself recommends avoiding casually on UI-sensitive paths.

The exact host-captured shell item array should be preferred whenever available.

### Payoff

This one refactor improves:

* cold right-click latency,
* warm right-click variance,
* resilience to malformed or slow package manifests,
* resilience to slow packaged COM extensions,
* Explorer responsiveness,
* separation of config from OS state.

It also **adds** functionality because modern packaged verbs can be maintained and refreshed much more aggressively without penalizing every right click.

---

# 6. P0: consolidate takeover interception rather than adding more hooks

The popup takeover currently has three interacting mechanisms.

## Primary path

`BootstrapOnce()` patches `user32.dll`'s import of the private:

`win32u.dll!NtUserTrackPopupMenuEx`

at `Main.cpp:1333-1335`.

## Fallback

If that fails, it enumerates loaded modules and patches imports of:

* `user32!TrackPopupMenu`
* `user32!TrackPopupMenuEx`

`Main.cpp:1337-1359`.

## Separate Explorer-wide inline detour

Explorer gets a Detours hook for `CoCreateInstance`:

`Main.cpp:1361-1377`.

This has accumulated because takeover inherently requires reaching places the normal shell-extension API does not expose.

That fact needs accepting rather than fighting.

Microsoft documents `TrackPopupMenu`/`TrackPopupMenuEx` as the public APIs for displaying/tracking shortcut menus. ([Microsoft Learn][7]) There is no corresponding public Explorer API saying "replace the entire composed Explorer context menu with your own renderer and still receive every third-party/native item."

So a fully supported implementation cannot provide the product you want.

The correct goal is therefore **not zero unsupported techniques**.

It is:

> **exactly one small unsupported compatibility boundary.**

Microsoft Detours itself explicitly notes that Microsoft does not warrant/support code altered through detouring or equivalent mechanisms. ([GitHub][8])

## What I would change

Introduce an explicit abstraction:

```cpp
class PopupInterceptionBackend {
    virtual InstallResult install() = 0;
    virtual BackendHealth health() const = 0;
};
```

Potential backends:

```text
Win32uIatBackend
PublicTrackPopupDetourBackend
LegacyPerModuleIatBackend
```

Only **one primary backend** should be active.

### First experiment

Implement a controlled Detours backend targeting documented:

* `TrackPopupMenu`
* `TrackPopupMenuEx`

rather than enumerating every currently loaded module.

Then test coverage against the private `NtUserTrackPopupMenuEx` backend.

If the public target catches all the menu routes Shell actually needs on supported Windows versions, use it.

If it misses important Explorer internal paths, keep the private win32u route.

The important architectural gain still occurs because the private dependency is now:

* named,
* isolated,
* health checked,
* replaceable,
* measurable.

Rather than leaking into `ContextMenu` and taskbar logic.

### Why I would not blindly replace the private hook today

The existing comments indicate that USER32 itself eventually forwards through the `NtUserTrackPopupMenuEx` import, and the code has been specifically structured to avoid reentrancy through that path (`Main.cpp:848-865`).

So "just hook the documented function" sounds cleaner, but may reduce coverage.

That needs an actual Windows comparison test rather than architectural aesthetics deciding it.

---

# 7. P0: make the global `CoCreateInstance` hook optional, not foundational

`CoCreateInstanceHook()` at `Main.cpp:685-805` is unusually high blast radius.

It runs for COM activations throughout Explorer and:

* specifically suppresses the private Windows 11 File Explorer context-menu CLSID,
* examines `IContextMenu`, `IContextMenu2`, `IContextMenu3`, `IExplorerCommand`,
* evaluates configured static rules,
* can return `E_NOINTERFACE` for selected CLSIDs,
* can profile activation latency when Alt is pressed.

The capability itself is useful.

In particular, it lets Shell **quarantine a problematic extension before it is activated**, which cannot be achieved simply by removing the resulting menu item afterward.

I would retain that capability.

What is suboptimal is making every Explorer process carry a global COM activation detour regardless of whether it is needed.

## Better architecture

Compile relevant NSS extension-blocking rules when config loads into something like:

```text
ComActivationPolicy
    blocked CLSIDs
    optional conditional groups
```

Then:

```cpp
if (!policy->could_affect(rclsid, riid, dwClsContext))
    return OriginalCoCreateInstance(...);
```

The normal route should be virtually trivial.

Even better, only attach the CoCreateInstance detour at all if:

* a CLSID quarantine rule exists, or
* a currently required compatibility feature needs it.

### Remove duplicated Windows 11 suppression

At present the private `{86ca1aa0-...}` Windows 11 menu path is attacked both through:

* the TreatAs routing,
* the global CoCreateInstance detour.

Once TreatAs is proven healthy and authoritative on a given Windows build, I would avoid having two independent mechanisms suppress the same thing.

Duplicate takeover mechanisms make recovery much harder because a "disabled" route can remain disabled from a second hidden layer.

Use:

```text
TakeoverRouter
  Windows11Route = TreatAs
  fallback = CoCI compatibility override
```

rather than both by default.

---

# 8. TreatAs itself is currently better engineered than it looks

I would **not** throw out the current TreatAs code.

The manager distinguishes:

* absent,
* ours,
* foreign,
* inaccessible.

See `src/exe/src/Main.cpp:345-399`.

Creation refuses to overwrite an existing foreign value, and removal deletes only Shell's own redirect (`401-483`).

The installer goes considerably further:

* prepares the state,
* records whether this installation created the key,
* applies later,
* rolls back only what that invocation created,
* commits by deleting its marker,
* refuses mutation where MSI rollback has been disabled.

See `src/setup/ca/TreatAsPlan.h` and `src/setup/ca/dllmain.cpp:737-929`.

That is responsible registry ownership.

`TreatAs` itself is a documented COM redirection mechanism. Microsoft describes it as transparently redirecting activation of one CLSID to another, and explicitly mentions setup-program use. ([Microsoft Learn][9])

The unsupported part is **using TreatAs against Explorer's private `{86ca1aa0-...}` Windows 11 class as a shell takeover mechanism**, not the TreatAs concept itself.

### Recommendation

Retain the hardened machine-wide strategy, but put it under a first-class `TakeoverRouter`/`TakeoverHealth` service.

Expose:

```text
Modern menu routing:
  ✓ TreatAs installed
  ✓ points to Shell
  ✓ Shell COM registration valid
  ✓ popup interception active
```

And one-click:

* Repair takeover
* Disable Shell takeover
* Restore Windows menu
* Re-enable takeover

### Per-user TreatAs?

Interesting experiment, not baseline.

HKCR is a merged view of per-user and machine `Software\Classes`, with specific shadowing behavior. ([Microsoft Learn][10])

That makes an HKCU prototype technically plausible, but the private Explorer class and COM/UAC behavior need real matrix testing. I would not swap the reliable current installer for it based on registry theory alone.

---

# 9. P0: taskbar takeover should become zero-wait

The recent taskbar work has fixed an important architectural defect already.

Windows 11's taskbar UI is XAML-backed, so the code uses UI Automation to determine whether a right-click is on empty taskbar area.

Crucially, UIA now runs on a process-lifetime MTA worker thread, not Explorer's UI thread.

`Main.cpp:252-528`.

That follows Microsoft's recommendation: UI Automation clients interacting with desktop UI including their own process should make UIA calls on a separate thread because UI-thread calls can be very slow or deadlock-like. ([Microsoft Learn][11])

Good.

But the taskbar thread still waits:

```cpp
CoWaitForMultipleHandles(..., BUDGET_MS = 250, ...)
```

at `Main.cpp:287-357`.

Failing open after 250 ms is much better than freezing indefinitely. But 250 ms is still a very noticeable taskbar interaction stall.

## Stage 1, simple and high value

Change taskbar hit testing to:

```text
cache hit:
    answer synchronously

cache miss:
    queue UIA worker request
    immediately let Windows handle this click
```

Zero synchronous UIA wait.

Then prewarm aggressively:

* pointer movement across taskbar,
* right-button-down preceding context-menu presentation,
* taskbar recreation,
* monitor/layout changes.

Most actual clicks should then have local cached answers.

## Stage 2, better architecture

Rather than cache isolated 16-pixel point answers, let the worker maintain a plain-data taskbar layout snapshot.

For example:

```cpp
struct TaskbarTarget {
    RECT bounds;
    TaskbarTargetKind kind;
};
```

The MTA worker can retrieve:

* AutomationId,
* ClassName,
* Name if genuinely required,
* BoundingRectangle,

using a UIA cache request.

Microsoft specifically recommends bulk property caching because individual UIA property getters can produce expensive cross-process calls. ([Microsoft Learn][12])

Currently `evaluate()` does:

```cpp
get_CurrentAutomationId
get_CurrentClassName
get_CurrentName
```

as three calls (`Main.cpp:486-488`).

A cached UIA snapshot is both cleaner and faster.

Then the Explorer/taskbar UI thread does nothing more complex than:

```text
point
  ↓
local rectangle hit test
  ↓
background / start / tray / clock / button
```

No COM. No waiting.

### Also deduplicate taskbar message processing

`TaskbarProc` and `TaskbarSubclassProc` duplicate significant control flow (`Main.cpp:1080-1300`).

There are also two hooking methods:

* `SetWindowSubclass` on the composition bridge child,
* direct `GWLP_WNDPROC` replacement on the tray HWND.

`SetWindowSubclass` is the cleaner documented helper where applicable. ([Microsoft Learn][13])

I would consolidate common policy into something like:

```cpp
TaskbarMessageResult handle_taskbar_message(
    TaskbarSurface &,
    HWND,
    UINT,
    WPARAM,
    LPARAM);
```

Keep the two low-level attachment mechanisms only if both are demonstrably necessary for different Windows surfaces.

---

# 10. `ContextMenu` is too large, but a rewrite would be the wrong response

`ContextMenu` currently owns almost everything about a menu invocation.

From `ContextMenu.h:333-738` alone it contains:

* selection state,
* config/runtime context,
* native HMENU tree,
* modern ExplorerCommand objects,
* original/custom menu correspondence,
* moved/replaced items,
* rendering theme,
* fonts,
* SVG/images,
* menu windows,
* hooks,
* WinEvent handling,
* keyboard hooks,
* layered windows,
* screenshots,
* tips,
* command invocation,
* COM marshaling,
* native lazy policy.

This is the clearest structural maintainability problem.

But I would strongly oppose a ground-up `ContextMenu` rewrite.

The class contains countless implicit Windows/menu lifetime relationships which are exactly the sort of thing a clean rewrite accidentally deletes.

## Strangler extraction order

### 1. `ModernVerbCatalogService`

Do this first because it solves a real user-visible problem and immediately removes responsibility.

### 2. `TakeoverRouter`

Own:

* popup interception backend,
* TreatAs status,
* COM quarantine activation,
* health/fallback status.

### 3. `NativeMenuAdapter`

Own:

* original HMENU,
* native item identity,
* `WM_INITMENUPOPUP`,
* JIT native children,
* native command execution correspondence.

### 4. `MenuModel`

Introduce a neutral representation with origin:

```cpp
enum class MenuOrigin {
    Native,
    Custom,
    ExplorerCommand
};
```

Plus stable-ish identity metadata.

This makes the renderer stop needing to understand all source-specific mechanics.

### 5. `CommandDispatcher`

Own:

* invoke native HMENU command,
* invoke IExplorerCommand,
* execute NSS custom command.

### 6. `Win32MenuPresenter`

Only after the previous boundaries are real should drawing/window-event code leave `ContextMenu`.

The eventual `ContextMenu` becomes closer to:

```text
MenuSession
    acquire host context
    acquire config snapshot
    compose source models
    apply rules
    present
    dispatch selection
```

That is a meaningful simplification.

---

# 11. Do not replace the Win32 renderer with WinUI

This would be one of the easiest ways to explode the project scope while solving almost none of the actual problems.

The renderer is deeply coupled to:

* HMENU lifecycle,
* popup HWND creation,
* Windows menu input behavior,
* owner draw,
* submenu timing,
* native menu geometry,
* accessibility/input/event behavior,
* Explorer window ownership.

WinUI/Windows App SDK would add:

* another runtime/lifecycle model,
* more window interop,
* another event model,
* more deployment dependencies,
* a larger gap between the host HMENU and what the user sees.

The result could look more modern while takeover becomes less reliable.

Keep Win32 rendering. Improve its internal model.

---

# 12. Selection resolution should become layered rather than Explorer-centric

`Selections.cpp` contains a large amount of Explorer archaeology.

For example, `GetIShellBrowser(HWND)` relies on an Explorer-specific message mechanism and the rest of the code uses window class/module heuristics for things such as:

* `SHELLDLL_DefView`,
* `SysListView32`,
* `ShellTabWindowClass`,
* `SysTreeView32`,
* `Shell_TrayWnd`,
* `LauncherTipWnd`,
* Explorer frame/shcore behavior.

That is understandable. Explorer has no single convenient public "tell me exactly what this right-click represents" API covering every special surface.

The problem is having host identification and selection acquisition intertwined.

## Better layering

```text
HostContextDetector
    |
    +-- Explorer desktop
    +-- Explorer folder view
    +-- navigation tree
    +-- Home / Quick Access
    +-- taskbar
    +-- third-party context menu host

SelectionProvider
    |
    +-- ShellExtCapturedSelection
    +-- ExplorerShellViewSelection
    +-- BackgroundFolderProvider
    +-- SpecialNamespaceProvider
```

If the registered handler supplied a valid exact selection, use it whenever semantically sufficient.

Use Explorer internals only for enrichment:

* Home,
* Quick Access,
* Libraries,
* navigation-tree targets,
* drop targets,
* special backgrounds,
* other namespace cases not represented fully in normal IDataObject capture.

That reduces dependency on Explorer's volatile window tree without sacrificing the cases the current machinery handles.

---

# 13. Config snapshots are good, package state inside them is not

The current `Initializer` publishes:

```cpp
std::shared_ptr<const CACHE>
```

under a generation model.

See `Initializer.h:13-59`.

That is a strong pattern.

A successful parse produces a new snapshot and active sessions can hold their old immutable generation. Configuration reload therefore does not require mutating live menu sessions under them.

Keep it.

But the `CACHE` includes `PackagesCache`.

That conflates:

### Configuration

Things that change when `.nss` changes:

* parsed rules,
* expressions,
* theme,
* variables,
* menu definitions,
* image definitions.

### External environment state

Things that change independently:

* installed AppX packages,
* ExplorerCommand registrations,
* taskbar layout,
* shell extension performance/health.

Those should be process-lifetime services.

That separation will make config reload faster and substantially easier to reason about.

---

# 14. Command execution should eventually stop creating detached threads indefinitely

For custom dynamic commands:

```cpp
std::thread(&Invoke, this).detach();
```

is used at `ContextMenu.cpp:5063`.

The worker correctly initializes a COM STA and properly consumes a marshaled `IShellBrowser` (`5133-5154`).

Again, the COM correctness has improved.

The remaining issue is lifecycle.

Every invocation gets an unmanaged detached native thread.

That is probably harmless for ordinary use, but it is an unnecessarily weak execution model for a shell component.

## Later refactor

Use a tiny process-level execution service:

```text
CommandExecutor
  STA worker 1
  STA worker 2
  optional ordinary worker pool
```

This gives:

* bounded concurrency,
* explicit apartment lifecycle,
* diagnostics,
* cancellation semantics for Shell-owned work,
* cleaner process shutdown semantics.

I would place this below the takeover/performance work because users are far more likely to notice the previous issues.

---

# 15. Ranked architectural priorities

| Priority | Area | Problem | Recommended change | User benefit | Scope |
|---|---|---|---|---|
| **P0.1** | Modern Explorer commands | Package/manifest scan and potentially slow COM work before display | Async persistent `PackageCatalogService`, stale snapshots, never `GetState(TRUE)` on opening path | Very high | Medium |
| **P0.2** | Popup takeover | Multiple overlapping interception strategies | `PopupInterceptionBackend`, one primary backend, measurable fallback | Very high stability | Medium |
| **P0.3** | CoCreateInstance | Global Explorer-wide hook with duplicate Win11 suppression | Attach only when required, precompiled CLSID policy, TreatAs authoritative | High | Low-medium |
| **P0.4** | Taskbar | UI thread can wait 250 ms on UIA | Zero-wait local snapshots, async UIA cache | High | Medium |
| **P1.1** | `ContextMenu` | 7k-line all-responsibilities session | Incremental service extraction | High long-term | Medium-high |
| **P1.2** | Selection | Too much Explorer-window archaeology in core flow | Provider/context separation | Compatibility | Medium |
| **P1.3** | Reliability UX | Powerful takeover lacks sufficiently visible health model | Compatibility/Reliability Center | Very high | Medium |
| **P2.1** | Invocation | Detached STA per custom command | Bounded executor | Moderate | Low-medium |
| **P2.2** | Per-user takeover | Machine TreatAs requires admin | Experiment with HKCU routing only after test matrix | Moderate | Medium |
| **P3** | Parser/rendering substrate | Large/custom but functional | Leave alone until measured problem | Low relative benefit | Huge if rewritten |

---

# 16. The target architecture I would actually build

```text
                  Explorer / third-party host
                            │
             ┌──────────────┴──────────────┐
             │                             │
      ShellExtCapture              PopupInterception
       selection/PIDL              one active backend
             │                             │
             └──────────────┬──────────────┘
                            ▼
                     TakeoverRouter
                  ┌───────────────────┐
                  │ TreatAs health    │
                  │ hook health       │
                  │ COM quarantine    │
                  │ fail-open policy  │
                  └─────────┬─────────┘
                            ▼
                       MenuSession
                            │
       ┌────────────────────┼────────────────────┐
       │                    │                    │
HostContext /       NativeMenuAdapter        RuleEngine
SelectionProvider   HMENU + lazy init        existing NSS
       │                    │                    │
       └────────────────────┼────────────────────┘
                            │
                   ModernVerb snapshot
                            │
                            ▼
                        MenuModel
                            │
                    Win32MenuPresenter
                            │
                            ▼
                    CommandDispatcher


       process-lifetime, asynchronous services
       ───────────────────────────────────────

       PackageCatalogService
       TaskbarSnapshotService
       CommandExecutor
       TakeoverHealth / Diagnostics
```

The critical architectural invariant should be:

## Before first menu paint, Shell may perform only bounded local work

Allowed:

* inspect already-created HMENU root,
* read current config snapshot,
* evaluate local NSS expressions,
* consume existing selection capture,
* read immutable service snapshots,
* do ordinary local CPU work.

Not allowed:

* package enumeration,
* package manifest disk I/O,
* network I/O,
* unbounded registry traversal,
* UI Automation cross-process calls,
* `GetState(TRUE)`,
* arbitrary recursive submenu initialization,
* waiting for background workers.

Third-party native menu handlers can of course still block Windows while constructing their own normal root menu. Shell cannot make an already in-process third-party extension magically safe.

What Shell *can* guarantee is that **its own optional enrichment never multiplies that risk.**

---

# 17. Highest-value functionality to add after the architecture is stabilized

This is where I would spend feature budget rather than adding more visual effects or configuration primitives.

## 1. Compatibility & Reliability Center

This would have exceptionally high practical value for a takeover product.

Expose something like:

```text
Takeover
  Windows 11 route           Healthy
  TreatAs                    Shell
  Popup interception         win32u backend
  Context handler            Registered
  Taskbar integration        Active

Performance
  Last menu pre-display      38 ms
  Native construction        21 ms
  Shell composition           7 ms
  Modern commands             1 ms

Extensions
  ExampleExt {...}           186 ms
  OtherExt {...}              3 ms

Actions
  Repair takeover
  Open Windows menu once
  Disable extension...
  Restore Windows defaults
  Export diagnostic report
```

The project already has most raw ingredients:

* phase timing in `MenuPerf`,
* CLSID knowledge,
* CoCreateInstance interception,
* registration inspection,
* TreatAs state,
* menu-origin information.

This converts low-level engineering into something directly useful to users.

### Particularly valuable: extension quarantine

Turn the existing CLSID suppression capability into a supported user workflow.

If one context-menu handler repeatedly causes delays:

* identify it by CLSID/name,
* show timing evidence,
* allow user to quarantine it from Shell,
* make undo obvious.

That is a genuine improvement over stock Explorer.

---

# 18. Add a takeover circuit breaker / safe mode

Because this DLL runs inside Explorer, recovery must be a product feature, not just a debugging technique.

Possible hierarchy:

```text
Normal
  everything enabled

Degraded
  no optional ExplorerCommand enrichment
  reduced visual effects
  taskbar takeover disabled

Core
  native + NSS menu only

Bypass
  Windows owns this interaction
```

A crash-loop mechanism should be conservative.

Do not infer "Explorer crashed therefore Shell caused it."

Instead use:

* Shell session/start markers,
* repeated abnormal Explorer termination while Shell was loaded,
* recent takeover initialization status,
* previous successful-session marker.

After repeated suspicious failures, automatically boot into Core mode and prominently tell the user what was disabled.

This makes takeover substantially safer without weakening normal operation.

---

# 19. Add "Open the Windows menu this time"

Every takeover shell needs a reliable escape hatch.

Not:

> disable Shell, restart Explorer, try again.

Instead:

* hold a documented modifier while right-clicking, or
* a Shell menu entry named something like **Windows menu**, or both.

That invocation should bypass custom composition for exactly one menu.

This is useful for:

* compatibility,
* debugging,
* discovering a missing native command,
* comparing behavior,
* rescuing the user when Shell's rules hide something unintentionally.

It also means Shell can become more aggressive with useful customization because users always retain an escape route.

---

# 20. Interactive type-to-filter would be a surprisingly high-value addition

The README says "search and filter", but the current architecture's existing search/filter functionality is primarily rule/config-oriented. The useful addition here is different:

> **While a menu is open, typing filters the composed menu interactively.**

Once `MenuModel` exists, it is comparatively cheap.

It could search:

* custom NSS actions,
* already materialized native items,
* cached modern Explorer commands.

Example:

```text
right click
type "hash"

  Calculate SHA256
  Copy file hash
  Verify checksum
```

For native submenus not yet materialized, do not destroy the lazy architecture by recursively opening everything.

Possible UI:

> Search deeper...

which intentionally materializes additional levels on demand.

That preserves performance while adding a capability the stock Windows context menu badly needs.

---

# 21. Add a rule/context inspector

For advanced Shell users, this may be even more valuable than another NSS feature.

With a modifier held, an item could expose:

```text
Source          Native HMENU
Title           "Open with"
Normalized ID   ...
Command ID      ...
CLSID           ...
Path            ...
Matched rules:
  modify rule line 82
  moved by line 113

Evaluation:
  sel.type == file       true
  sel.ext == ".png"      true

Construction time:
  native popup init      4.2 ms
```

For ExplorerCommand entries:

```text
Source          IExplorerCommand
CLSID           {...}
CanonicalName   {...}
Package         ...
GetState        cached
```

For custom NSS:

```text
Definition      file-manage.nss:47
where           true
visibility      normal
command         ...
```

The NSS language is already powerful enough that opacity is becoming a bigger usability limitation than expressiveness.

A good inspector converts Shell from "configuration sorcery" into something users can reason about.

---

# 22. Command favorites / recent actions would fit the architecture well

Once items have an origin-neutral `MenuModel` identity, Shell can track action usage.

Then support:

* favorite actions,
* recently used commands,
* frequently used actions.

This needs care with native commands.

Do **not** persist raw HMENU `wID` values, because those are session-specific.

Persist an identity such as:

* canonical ExplorerCommand GUID,
* NSS item identity,
* native normalized path/title/source characteristics,

and re-resolve it against the current menu.

This creates genuine shell productivity functionality without another major Windows integration project.

---

# 23. What I would explicitly **not** pursue

### Do not replace takeover with only `IExplorerCommand`

That would make the architecture more Microsoft-conventional but remove the central ability to reorganize and replace the composed Windows menu. It conflicts with your objective.

### Do not chase "maximum native compliance"

There is no supported public contract for the complete product Shell implements.

Use documented APIs wherever they fit, but optimize the unsupported part for containment and resilience.

### Do not eagerly enumerate the native tree

The current lazy mechanism is better.

### Do not add more hooks to fix individual cases

Every newly discovered Windows quirk should first be solved inside an existing interception/provider abstraction.

### Do not build a giant out-of-process menu broker

Live:

* HMENUs,
* HWND lifecycle,
* shell menu callbacks,
* apartment-bound COM objects,

want to remain in the host process.

An external helper could later make sense for **package indexing or diagnostic persistence**, but making the core menu compositor out-of-process would trade one form of fragility for IPC and lifetime complexity.

### Do not kill threads running bad shell extensions

An arbitrary in-process COM extension may own locks and process state.

Timeout-and-kill is not isolation.

The safe options are:

* don't activate it,
* quarantine it next time,
* let Windows handle the click,
* genuinely execute something out-of-process where the API permits that from the beginning.

### Do not rewrite the NSS parser because it is custom

There is substantial specialized behavior and test coverage there. It is not on the principal instability path.

### Do not rewrite the custom drawing/string/shared libraries merely for modernity

Some are idiosyncratic, yes. But replacing 25k lines of substrate is a poor risk/reward move until profiling or defect data says otherwise.

---

# 24. Implementation sequence I would use

## Phase 1: remove avoidable first-paint blocking

Highest ROI.

1. Introduce process-level `PackageCatalogService`.
2. Move both package discovery systems into it.
3. Background warm and stale-while-revalidate.
4. Persist manifest registration index.
5. Make menu catalog reads nonblocking.
6. Remove synchronous `GetState(TRUE)`.
7. Record per-CLSID modern-command latency.

This should land independently before the structural refactor.

## Phase 2: make takeover a formal subsystem

1. Introduce `TakeoverRouter`.
2. Put TreatAs inspection/control behind it.
3. Introduce `PopupInterceptionBackend`.
4. Run private win32u and public TrackPopup detour implementations through the same test interface.
5. Choose one primary backend at runtime.
6. Make CoCreateInstance suppression optional and policy-driven.
7. Add clear fail-open telemetry.

At the end of this phase, you should be able to answer precisely:

> "Why did Shell take this popup?"

and:

> "If it cannot, which Windows path handles it instead?"

## Phase 3: remove taskbar stalls

1. Cache-request UIA properties.
2. Queue requests without waiting.
3. Prewarm based on pointer activity.
4. Move to rectangle/surface snapshots.
5. Merge duplicate taskbar message policy.

## Phase 4: extract `ContextMenu`

Without changing output behavior:

1. `NativeMenuAdapter`
2. `HostContextDetector`
3. `SelectionProvider`
4. `MenuModel`
5. `CommandDispatcher`

Do not combine this with a new UI.

## Phase 5: turn reliability infrastructure into features

Ship:

* Reliability Center,
* one-shot native menu,
* extension quarantine,
* safe/degraded mode,
* diagnostic export.

## Phase 6: productivity features

Then:

* interactive type-to-filter,
* rule/context inspector,
* favorites/recent actions.

At this point new features become easier because they target `MenuModel`, rather than understanding HMENU + NSS + ExplorerCommand separately.

---

# 25. Testing is the largest strategic gap after the runtime changes

The repository has a much healthier unit-test suite than many shell projects:

* parser,
* manifest parsing,
* ExplorerCommand behavior,
* package indexing,
* native-menu laziness,
* shell extension capture,
* one-shot COM marshaling,
* selection resolver,
* taskbar cache/origin,
* TreatAs transaction planning,
* registry/thread safety,
* WIC.

CI builds x64, x86 and ARM64 and runs unit tests on x64/x86 (`.github/workflows/build.yml:16-47`).

That is good.

But this product's hardest failures are **integration failures inside Explorer**, and CI does not currently prove those.

I would build a Windows integration/compatibility harness before a major takeover refactor.

## Matrix

At minimum:

**OS**

* Windows 10 supported target
* current stable Windows 11
* Insider Beta
* Insider Dev/Canary for early warning

**Architecture**

* x64 Explorer
* x86 third-party hosts
* ARM64

**Context types**

* one file
* folder
* mixed selection
* very large selection
* folder background
* desktop
* Home
* Quick Access
* Libraries
* navigation tree
* Recycle Bin
* shell/system popup
* primary taskbar
* secondary taskbar
* multi-monitor
* mixed DPI
* RTL

**Extensions**

* ordinary `IContextMenu`
* lazy submenu handler
* owner-draw handler
* packaged `IExplorerCommand`
* in-proc command
* surrogate/local-server command
* command returning `E_PENDING`
* intentionally slow test command
* failing COM activation

**Takeover lifecycle**

* TreatAs absent
* ours
* foreign
* unreadable
* install
* upgrade
* failed upgrade/rollback
* uninstall
* Explorer restart
* taskbar recreation

**Config**

* valid reload
* malformed reload
* reload while menu open
* rules requiring descendant `moveto`
* third-party host.

### The most useful regression gates

Not arbitrary microbenchmarks, but architectural invariants:

* **zero package manifest reads on the menu-opening thread**
* **zero UIA calls/waits on the taskbar UI thread**
* **zero `GetState(TRUE)` during initial menu composition**
* **no raw apartment COM pointer transferred between threads**
* **optional enrichment never blocks first paint**
* **one primary popup-interception backend**
* **foreign TreatAs state is never modified**
* **every takeover failure has a fail-open Windows path**

Then collect actual timing distributions and set p95/p99 budgets from real hardware rather than inventing numbers in advance.

---

# 26. Important assessment of the current code quality

One thing I would avoid is treating the repository's own `docs/maintenance/fork-assessment-2026-08.md` as a list of present defects.

A meaningful amount of that work has already been incorporated.

The current tree contains explicit hardening for:

* minimal DllMain,
* COM inter-apartment marshaling,
* HMENU-bound third-party selection capture,
* lazy native submenu materialization,
* safe TreatAs ownership,
* installer rollback,
* package index concurrency,
* UIA moving off the Explorer UI thread,
* immutable config snapshots,
* parser/encoding correctness,
* numerous regression tests.

So this is **not** a codebase where the answer is "replace everything with clean modern abstractions."

It is a codebase that has gradually learned many ugly Windows truths, but those truths have accumulated in a few overly central places.

The refactor needs to preserve the knowledge while changing where it lives.

---

# Bottom line

If I owned the next architectural cycle, I would set one overriding engineering rule:

> **Right-click takeover itself must be deterministic and local. Everything that discovers Windows state, probes third-party components, or enriches the menu must feed it through asynchronously prepared snapshots.**

Then I would spend the work in this order:

1. **Eliminate Windows 11 package/ExplorerCommand discovery and slow-state evaluation from first paint.**
2. **Collapse takeover into one explicit routing/interception subsystem.**
3. **Turn the CoCreateInstance detour from an always-on Explorer-wide mechanism into an optional extension-quarantine mechanism.**
4. **Make taskbar interception zero-wait.**
5. **Incrementally decompose `ContextMenu`, preserving behavior.**
6. **Build the Reliability Center, bypass mode and safe-mode circuit breaker.**
7. **Add interactive menu filtering, diagnostics/context inspection and favorites on top of the resulting unified menu model.**

That combination is the strongest path I see to making Shell **more takeover-oriented rather than less**, while simultaneously making it faster, less fragile across Windows changes, easier to debug and materially more capable for users.

[1]: https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices?utm_source=chatgpt.com "Dynamic-Link Library Best Practices - Win32 apps | Microsoft Learn"
[2]: https://learn.microsoft.com/en-us/windows/win32/shell/handlers?utm_source=chatgpt.com "Creating Shell Extension Handlers - Win32 apps | Microsoft Learn"
[3]: https://learn.microsoft.com/en-us/windows/win32/com/single-threaded-apartments?utm_source=chatgpt.com "Single-Threaded Apartments - Win32 apps | Microsoft Learn"
[4]: https://learn.microsoft.com/en-us/windows/win32/menurc/wm-initmenupopup?utm_source=chatgpt.com "WM_INITMENUPOPUP message (Winuser.h) - Win32 apps | Microsoft Learn"
[5]: https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getstate?utm_source=chatgpt.com "IExplorerCommand::GetState (shobjidl_core.h) - Win32 apps | Microsoft Learn"
[6]: https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/integrate-packaged-app-with-file-explorer?utm_source=chatgpt.com "Add a File Explorer context menu command to a packaged desktop app - Windows apps | Microsoft Learn"
[7]: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenu?utm_source=chatgpt.com "TrackPopupMenu function (winuser.h) - Win32 apps | Microsoft Learn"
[8]: https://github.com/microsoft/detours/wiki/Using-Detours?utm_source=chatgpt.com "Using Detours · microsoft/Detours Wiki · GitHub"
[9]: https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cogettreatasclass?utm_source=chatgpt.com "CoGetTreatAsClass function (combaseapi.h) - Win32 apps | Microsoft Learn"
[10]: https://learn.microsoft.com/en-us/windows/win32/sysinfo/merged-view-of-hkey-classes-root?utm_source=chatgpt.com "Merged View of HKEY_CLASSES_ROOT - Win32 apps | Microsoft Learn"
[11]: https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading?utm_source=chatgpt.com "Understanding Threading Issues - Win32 apps | Microsoft Learn"
[12]: https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-cachingforclients?utm_source=chatgpt.com "Caching UI Automation Properties and Control Patterns - Win32 apps | Microsoft Learn"
[13]: https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-setwindowsubclass?utm_source=chatgpt.com "SetWindowSubclass function (commctrl.h) - Win32 apps | Microsoft Learn"
