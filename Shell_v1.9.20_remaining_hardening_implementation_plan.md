# Nilesoft Shell v1.9.20 — Remaining Production Hardening Implementation Plan

**Target codebase:** attached `Shell(5).zip`  
**Reviewed baseline:** Git commit `74bff70` (`Hardening & release fixes for v1.9.20 with multi-arch build isolation`)  
**Plan date:** 2026-08-20  
**Purpose:** implementation-ready plan for the remaining critical and materially relevant issues after the v1.9.20 hardening round.

> **Important baseline note**
>
> The attached working tree reports eight modified files, but `git diff -w` shows those differences are whitespace/EOL-only. The implementation work below should start from a normalized, clean semantic baseline. Do **not** mix EOL normalization or unrelated formatting with the architectural changes.

---

## 1. Executive implementation order

Implement in this order and keep each workstream in a separate commit/PR-sized change:

1. **P0 — Loader-lock/process-lifetime cleanup**
2. **P0 — Finish immutable configuration snapshots; eliminate raw cache/MUID lifetime leaks**
3. **P0 — Rebuild ShellExt capture ownership for concurrency and COM-apartment correctness**
4. **P0 — Complete MSI architecture/component servicing correctness**
5. **P1 — Make bootstrap transactional and retryable; pin the module before process hooks**
6. **P1 — Finish explicit path semantics and remove process-CWD mutation**
7. **P1 — Fix remaining mutable cache helpers (`PackagesCache`, WIC, runtime variables)**
8. **P1 — Add the missing parser/thread/concurrency/context-menu integration tests**
9. **P1 — Finish ARM64 and MSI architecture verification**
10. **P2 — Move UI Automation off the menu/UI thread**
11. **P2 — Harden developer deployment and experimental owner-draw compatibility paths**
12. **Release gate — full Windows upgrade/runtime matrix**

Do **not** import additional fork features, restyle code, change menu behavior, or modify unrelated UX while these changes are underway.

---

# 2. Behaviors that must not regress

The following changes are already valuable and should be preserved:

- The actual displayed menu must remain Shell's populated replacement handle:

  ```cpp
  invoke(ctx->MenuHandle(), flag, { x, y });
  ```

  Do **not** revert this to the original `hMenu`; that earlier change caused the collapsed/empty menu regression.

- Preserve strict RFC 3629-style UTF-8 validation and UTF-32 BOM ordering.
- Preserve the SIMD ASCII case-insensitive fast path plus `CompareStringOrdinal` fallback for non-ASCII text.
- Preserve fixed string-search bounds.
- Preserve `MenuItemInfo::set_title()` pointer-lifetime fix.
- Preserve duplicate-menu deferred deletion.
- Preserve non-destructive `BitmapCache` capacity behavior.
- Preserve exact-`HMENU` ShellExt binding semantics.
- Preserve `CoInitializeEx`/`CoUninitialize` balancing performed by the menu-hook scope.
- Preserve the GDI `GetDIBits` deselection fix.
- Preserve per-architecture output directories (`src/bin/x64`, `src/bin/x86`, `src/bin/arm64`) and PE architecture validation.
- Preserve v1.9.20 ProductVersion and separate ProductCodes.
- Preserve Restart Manager enablement and updatable stock imports/locales.

---

# 3. Source/documentation baseline

The previous implementation plan correctly identified the major categories, but the latest source still contains incomplete implementations in several of them. In particular:

- `DllMain` itself is smaller, but dynamic C++ global construction/destruction still executes under the CRT's DLL entry-point path.
- `Initializer` publishes a `shared_ptr<CACHE>` but still exposes and consumes naked `CACHE*` pointers.
- the shared `CACHE` still contains mutable runtime variables used by every menu invocation;
- MUID pointers escape their mutex-protected map;
- ShellExt capture storage is RAII-managed but is still one process-global pending slot and stores raw apartment-bound COM pointers;
- architecture-specific ProductCodes exist, but the architecture-specific binary component still has one shared MSI Component GUID;
- parser/thread-stress tests promised by the prior plan were not added;
- `path.*` functions and `currentdirectory(...)` still depend on or mutate process CWD;
- bootstrap still uses an irreversible `std::call_once` despite partial failures being possible.

---

# 4. P0 — Eliminate loader-lock work from the *entire DLL lifecycle*

## 4.1 Why this remains critical

The current user-written `DllMain(DLL_PROCESS_ATTACH)` is mostly minimal, but the DLL still has non-trivial global/static C++ objects and dynamic Win32 initialization. The Microsoft CRT invokes global/static constructors and destructors through the DLL entry-point path, so the same loader-lock restrictions apply to code called from those constructors/destructors.

Current examples include:

### `src/dll/src/Main.cpp`

```cpp
HINSTANCE _hInstance{};
Initializer _initializer;
extern Logger &_log = Logger::Instance();
const Windows::Version *ver = &Windows::Version::Instance();
WindowsHook _taskbar_mouse;
IATHook iathook_NtUserTrackPopupMenuEx;
std::vector<IATHook> iathook_TrackPopupMenu;
Detours<decltype(::DllGetClassObject)> _DllGetClassObject;
Detours<decltype(::CoCreateInstance)> _CoCreateInstance;
std::unordered_map<HWND, Window> _window_taskbar;
```

and:

```cpp
inline static auto wShell_TrayWnd = ::RegisterWindowMessageW(...);
inline static auto wShell_SecondaryTrayWnd = ::RegisterWindowMessageW(...);
```

### `src/dll/src/Initializer.cpp`

```cpp
const Windows::Version *os = &Windows::Version::Instance();
```

### `src/shared/System/Drawing/WICImagingFactory.h`

```cpp
inline static IWICImagingFactory* _factory = nullptr;
inline static const uint32_t _width = ::GetSystemMetrics(SM_CXSMICON);
inline static const uint32_t _height = ::GetSystemMetrics(SM_CYSMICON);
```

### `src/shared/System/Drawing/Icon.h`

```cpp
inline static const auto cx = ::GetSystemMetrics(SM_CXSMICON);
```

The current global `Initializer` destructor also runs substantial cleanup:

```cpp
Initializer::~Initializer()
{
    uninit();
    WIC::release();
}
```

which can cascade into cache destruction, `DeleteObject`, font/GDI teardown, COM `Release`, and other nontrivial operations.

Current `DLL_PROCESS_DETACH` additionally calls registry-dependent `is_registered()`, unhooks windows/taskbar, executes Detours/IAT teardown, unregisters the window class, and closes the logger while loader lock is held.

### Official requirements

- Microsoft states that `DllMain` runs under loader lock, should do minimal work, and must not call registry, COM, User32/GDI, or synchronize with other threads:  
  <https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices>
- Microsoft explicitly states that the same restrictions apply to global/static C++ constructors and destructors invoked by the CRT:  
  <https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain>
- Microsoft recommends effectively empty process-detach behavior when the process is terminating:  
  <https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices>

## 4.2 Target architecture

Create one lazily allocated, intentionally process-lifetime runtime object and stop relying on nontrivial namespace-scope destructors.

Suggested shape:

```cpp
struct RuntimeState
{
    HINSTANCE module{};
    Initializer initializer;
    LoaderState loader;

    WindowsHook taskbar_mouse;
    IATHook ntuser_popup_hook;
    std::vector<IATHook> popup_hooks;
    Detours<decltype(::DllGetClassObject)> dll_get_class_object_hook;
    Detours<decltype(::CoCreateInstance)> co_create_instance_hook;

    std::unordered_map<HWND, Window> taskbar_windows;
    std::mutex taskbar_mutex;

    std::atomic<BootstrapState> bootstrap_state{BootstrapState::NotStarted};
    std::atomic<bool> hooks_installed{false};
    std::atomic<bool> module_pinned{false};
};

RuntimeState& Runtime()
{
    // Intentionally process-lifetime. Do not register a C++ destructor.
    static RuntimeState* state = new RuntimeState();
    return *state;
}
```

The `Runtime()` function itself must not be called from `DllMain`; allocate the object only after normal execution enters a safe bootstrap/export path.

### Required file changes

#### `src/dll/src/Main.cpp`

1. Keep only trivial globals that require no constructor/destructor:

   ```cpp
   HINSTANCE g_hInstance = nullptr;
   ```

2. Remove/migrate the current globals listed above into `RuntimeState`.
3. Remove global `ver = &Windows::Version::Instance()`; query the version after bootstrap or via a safe lazy helper called only outside loader lock.
4. Replace `taskbar_t` dynamic static message registration with lazy fields initialized during normal runtime initialization.
5. Change signature to preserve the reserved argument:

   ```cpp
   BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD reason, LPVOID reserved)
   ```

6. `DLL_PROCESS_ATTACH` should do only:

   ```cpp
   g_hInstance = hInstance;
   DisableThreadLibraryCalls(hInstance); // check/log later if desired; no logger here
   return TRUE;
   ```

   Move `_CrtSetDbgFlag` out of `DllMain` too.

7. `DLL_PROCESS_DETACH`:
   - if `reserved != nullptr` (process termination), return immediately;
   - preferably also do nothing for explicit unload because the module should be pinned once process hooks exist;
   - do not call registry, User32/GDI, COM, logger, Detours, cache destruction, or window teardown here.

#### `src/dll/src/Initializer.cpp` / `Include/Initializer.h`

- Remove destructor-driven `uninit()`/`WIC::release()` behavior.
- Make `Initializer` an owned member of the process-lifetime runtime state.
- Provide an explicit normal-thread `ResetConfiguration()` if needed for reload; do not use destructor as runtime shutdown.
- Remove stale comments saying `init(HINSTANCE)` runs from `DllMain` if it no longer does.

#### `src/shared/System/Drawing/WICImagingFactory.h`

- Remove dynamic `GetSystemMetrics` statics.
- Remove global process COM factory teardown from a destructor.
- See Workstream 8 for WIC ownership.

#### `src/shared/System/Drawing/Icon.h`, `Theme.h`, version helpers

- Replace dynamic Win32-derived class statics with functions evaluated after bootstrap or passed DPI/metrics from menu context.
- Ensure `Windows::Version::Instance()` is first touched after loader lock.

#### Debug-only globals

Any debug-only global `std::unordered_map` or other dynamic container should be changed to a compile-time table/array or lazy object created outside loader lock. Debug builds should be capable of running Application Verifier's loader-lock checks too.

## 4.3 Pin the module before installing process hooks

Immediately before installing the first IAT/Detours/window-procedure hook, pin the module:

```cpp
HMODULE pinned = nullptr;
if (!GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&BootstrapRuntime),
        &pinned))
{
    return BootstrapResult::RetryableFailure;
}
```

Microsoft documents that `GET_MODULE_HANDLE_EX_FLAG_PIN` keeps the module loaded until the process terminates:  
<https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandleexw>

This is stronger than depending only on COM honoring `DllCanUnloadNow`; it protects IAT/Detours function pointers even if a host directly manipulates module lifetime.

Once pinned, do not attempt complex process-hook teardown during DLL detach.

## 4.4 Acceptance criteria

- Application Verifier loader-lock check produces no Shell-origin violation on Explorer load/unload.
- No registry/User32/GDI/COM call is reachable from namespace/global constructors or destructors.
- `DLL_PROCESS_DETACH` with non-null `reserved` is effectively empty.
- Process hooks are installed only after successful module pinning.
- Explorer/menu behavior remains unchanged.

---

# 5. P0 — Finish immutable configuration snapshots and remove all raw shared-cache lifetimes

## 5.1 Current defects

`Initializer` currently contains both the new owner and the old raw alias:

```cpp
std::shared_ptr<CACHE> _cache_snapshot;
CACHE* cache{};

std::shared_ptr<const CACHE> get_cache();
std::shared_ptr<CACHE> get_mutable_cache();
CACHE* get_raw_cache();
```

Raw global cache access remains in:

- `src/dll/src/ContextMenu.cpp`
  - `_context.Cache = initializer->cache;`
- `src/dll/src/Expression/FuncExpression.cpp`
  - `cache = Initializer::instance->cache;`
- `src/dll/src/Expression/Context.cpp`
  - `auto cache = Initializer::instance->cache;`
- `src/dll/src/Parser/Parser.cpp`
  - `get_raw_cache()`

A thread can obtain one of these pointers, another thread can publish a replacement snapshot, and the old object may be destroyed while the first thread still dereferences it.

There is also a semantic race: one `ContextMenu` can own configuration generation A while an expression fetches the process-global raw pointer and silently reads generation B.

## 5.2 The current CACHE is not truly immutable

`CACHE` contains:

```cpp
Scope global;
Scope runtime;
Scope loc;
```

and runtime evaluation explicitly mutates `variables.runtime`:

```cpp
variables.runtime->set(...);
```

Every current `ContextMenu` points its expression context at:

```cpp
_context.variables.runtime = &_cache->variables.runtime;
```

Therefore concurrent menus share mutable runtime variables. This creates both a data race and cross-menu state leakage even if all pointer ownership were fixed.

## 5.3 MUID pointer lifetime is also unsafe

Current code:

```cpp
static MUID* get_muid(uint32_t hash)
{
    std::lock_guard<std::mutex> lock(MUTEX_MUID);
    ...
    return &it.second;
}
```

The returned `MUID*` outlives the lock. Another reload can `clear()`/rehash `MAP_MUID`, invalidating the pointer. Menu items then retain that pointer in `menuitem_t::ui`.

## 5.4 Target data model

Split parsed immutable configuration from per-menu/runtime state.

Recommended model:

```cpp
struct ConfigSnapshot
{
    uint64_t generation{};
    Settings settings;

    Scope global_variables;      // parsed, read-only after publish
    Scope localization_variables; // parsed/read-only after publish

    std::vector<std::unique_ptr<NativeMenu>> statics;
    NativeMenu dynamic;
    std::vector<ImageCache> images;
    GlyphSettings glyph;

    std::unordered_map<uint32_t, MUID> muid;

    // Thread-safe memoization/resources may be mutable internally,
    // but their APIs must provide their own synchronization.
    FontCache fonts;
    PackagesCache packages;
    BitmapCache bitmaps;
};
```

And per-menu:

```cpp
class ContextMenu
{
    std::shared_ptr<const ConfigSnapshot> _config;
    Scope _runtime_variables; // one per invocation/menu
    Context _context;
};
```

If making resource caches members of a `const ConfigSnapshot` is awkward, use a two-object model:

```cpp
struct ParsedConfig;     // fully immutable
struct RuntimeResources; // synchronized memoization/cache
struct ConfigSnapshot {
    std::shared_ptr<const ParsedConfig> parsed;
    std::shared_ptr<RuntimeResources> resources;
};
```

The key property is: **no configuration reload may mutate or destroy state still referenced by an active menu.**

## 5.5 Required `Initializer` changes

### `src/dll/src/Include/Initializer.h`

Replace:

```cpp
std::mutex _cache_mutex;
std::shared_ptr<CACHE> _cache_snapshot;
CACHE* cache;
get_mutable_cache();
get_raw_cache();
```

with:

```cpp
std::mutex _reload_mutex;   // serializes entire build/reload operation
std::mutex _snapshot_mutex; // protects only publication/acquisition
std::shared_ptr<const ConfigSnapshot> _snapshot;
std::atomic<uint64_t> _generation{0};

std::shared_ptr<const ConfigSnapshot> acquire_snapshot() const;
bool reload_config();
```

Use C++ `atomic_load`/`atomic_store` for `shared_ptr` only if the project/compiler standard is known to support the chosen form; a small mutex is simpler and less error-prone.

Delete the public raw `CACHE*` field.

### `Initializer::init/query/reload`

Serialize the **entire** build phase, not just publication:

```cpp
bool Initializer::reload_config()
{
    std::unique_lock reload_lock(_reload_mutex);

    auto next = std::make_shared<ConfigSnapshot>();
    next->generation = ++_generation;

    // Build entirely into `next`.
    // Parser must receive `next`, not fetch process-global Initializer state.
    if (!BuildConfig(*next))
        return false;

    {
        std::lock_guard publish_lock(_snapshot_mutex);
        _snapshot = std::move(next);
    }

    return true;
}
```

Do not call `uninit()` before building a replacement. A failed reload must leave the last known-good snapshot active.

## 5.6 Move MUID into the snapshot

Delete:

```cpp
Initializer::MAP_MUID
Initializer::MUTEX_MUID
Initializer::get_muid(...)
```

Move localization/menu-resource loading into the `ConfigSnapshot` being constructed.

Provide:

```cpp
const MUID* ConfigSnapshot::find_muid(uint32_t hash) const;
```

This pointer is safe because every menu owning it also owns the containing snapshot.

Update callers in:

- `ContextMenu.cpp`
- `Parser/Verification.cpp`
- `Expression/FuncExpression.cpp`
- `Expression/Context.cpp`

They should use their passed context/config, never `Initializer::instance`.

## 5.7 Move runtime variables out of shared configuration

In `ContextMenu::Initialize()`:

```cpp
_config = initializer->acquire_snapshot();
if (!_config) return false;

_context.Cache = _config.get(); // non-owning only because _config owns it
_context.variables.global = &_config->global_variables;
_context.variables.runtime = &_runtime_variables;
_context.variables.local = nullptr;
```

Initialize/clear `_runtime_variables` when the menu object is created/destroyed. Never store runtime variable writes in the shared snapshot.

## 5.8 DPI/resource behavior

Current `FontCache` is keyed by `size + dpi`, which is compatible with additive multi-DPI caching. Remove process-global `Initializer::dpi` as an ownership/control mechanism. DPI should be an invocation property.

Do not mutate an old snapshot because a menu opens on a monitor with a different DPI.

## 5.9 Thread-safety tests

Add `src/tests/test_threadsafety.cpp` or a dedicated testable helper library with:

1. **Snapshot generation consistency**
   - menu thread acquires generation N;
   - reload publishes N+1;
   - all reads in the existing menu remain generation N.

2. **Reload stress**
   - 8+ reader/menu threads repeatedly acquire snapshots;
   - one writer repeatedly builds/swaps 100+ snapshots;
   - no raw pointer/UAF and no mixed generation.

3. **Runtime variable isolation**
   - two menus assign same runtime variable ID to different values;
   - neither sees the other's value.

4. **MUID lifetime**
   - menu keeps MUID pointer from snapshot N;
   - N+1 is published;
   - pointer remains valid until menu releases N.

5. **Multi-DPI resources**
   - simultaneous 96/144/192 DPI font requests;
   - returned resources remain valid and distinct where required.

Microsoft's general DLL guidance requires synchronization of shared global data for multithreaded hosts:  
<https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-creation>

COM apartment behavior makes this especially important for in-process servers:  
<https://learn.microsoft.com/en-us/windows/win32/com/in-process-server-threading-issues>

---

# 6. P0 — Rebuild ShellExt capture ownership for handler concurrency and COM apartment correctness

## 6.1 Current architecture problems

Current `ShellExtCapture` has one global pending capture:

```cpp
inline static Entry _pending;
inline static std::unordered_map<HMENU, Entry> _bound;
```

`ShellExtHandler::Initialize()` writes `_pending`; `QueryContextMenu()` later moves whatever `_pending` currently contains into a menu entry.

This can cross selections between independent handlers:

```text
STA A: Handler A Initialize(selection A)
STA B: Handler B Initialize(selection B)
STA A: Handler A QueryContextMenu(menu A)
         -> can bind B's global pending state
```

Current `match()` also returns borrowed raw objects:

```cpp
return { entry.items, entry.folder, entry.background };
```

After the mutex is released, another thread can erase/prune/clear the entry, release the interface and free the PIDL while the caller still uses them.

Finally, current popup completion calls the global:

```cpp
ShellExtCapture::clear();
```

so one menu can destroy another menu's bound capture.

## 6.2 COM apartment rule

`IShellItemArray*` is a COM interface pointer. A raw interface pointer obtained in one STA cannot simply be stored in process-global memory and invoked from another apartment. Microsoft requires inter-apartment interface pointers to be marshaled.

Official sources:

- Single-Threaded Apartments:  
  <https://learn.microsoft.com/en-us/windows/win32/com/single-threaded-apartments>
- Accessing Interfaces Across Apartments:  
  <https://learn.microsoft.com/en-us/windows/win32/com/accessing-interfaces-across-apartments>
- `IGlobalInterfaceTable`:  
  <https://learn.microsoft.com/en-us/windows/win32/api/objidl/nn-objidl-iglobalinterfacetable>
- Creating the Global Interface Table:  
  <https://learn.microsoft.com/en-us/windows/win32/com/creating-the-global-interface-table>

The current Microsoft documentation also identifies `IAgileReference`/`RoGetAgileReference` as a modern alternative on Windows 8.1+, but the Global Interface Table is a straightforward fit for this classic COM codebase.

## 6.3 Required ownership model

### Pending capture belongs to the handler instance

Add to `ShellExtHandler`:

```cpp
struct PendingCapture
{
    DWORD git_cookie{};
    unique_pidl folder;
    bool background{};
    uint32_t tick{};
};

PendingCapture m_pending;
```

There must be no process-global `_pending` slot.

### Use GIT cookie, not raw cross-apartment pointer

During `Initialize()`:

1. build `IShellItemArray` from `IDataObject`;
2. register the interface with `IGlobalInterfaceTable::RegisterInterfaceInGlobal`;
3. store the returned cookie in **that handler's** `m_pending`;
4. clone `pidlFolder` into owned storage;
5. if `Initialize()` is called again on the same handler, revoke/dispose the previous pending capture first.

Use a dedicated RAII wrapper:

```cpp
class GitCookie
{
    DWORD _cookie{};
public:
    ~GitCookie() { revoke(); }
    GitCookie(GitCookie&&) noexcept;
    GitCookie& operator=(GitCookie&&) noexcept;
    GitCookie(const GitCookie&) = delete;
};
```

Do not let its destructor depend on DLL global teardown; entries should be removed during normal popup/handler lifecycle, and the module is process-pinned after hooks install.

### Bind the correct handler's pending state

`ShellExtHandler::QueryContextMenu(hmenu, ...)` should call:

```cpp
ShellExtCaptureRegistry::bind(hmenu, std::move(m_pending));
```

not a global pending bind.

### `match()` must return an owning capture

Define:

```cpp
struct CaptureResult
{
    IComPtr<IShellItemArray> items; // apartment-valid pointer resolved here
    unique_pidl folder;             // cloned result
    bool background{};
};
```

`match(hMenu)` should:

1. find/copy enough metadata under `_mutex`;
2. resolve the GIT cookie using `GetInterfaceFromGlobal()` on the **calling apartment**;
3. clone the PIDL for the returned result, or keep the bound registry entry behind `shared_ptr` so its lifetime is explicit;
4. return fully owned objects.

The caller must never receive a borrowed COM pointer or borrowed PIDL whose validity depends on an unlocked map entry.

### Clear only one menu

Change the popup finally block from:

```cpp
ShellExtCapture::clear();
```

to:

```cpp
ShellExtCaptureRegistry::clear(hMenu);
```

Prune TTL entries individually. `clear(hMenuA)` must not touch menu B.

## 6.4 Module lifetime

Before publishing/using any process hook, pin the module with `GetModuleHandleExW(...PIN...)` as described in Workstream 4.

`DllCanUnloadNow()` should still return `S_FALSE` when:

- COM object/factory locks exist;
- active captures exist;
- process hooks have been installed.

Once the module is pinned, that function is defensive COM behavior rather than the only thing preventing an invalid hook pointer.

## 6.5 Tests

Add these exact cases:

1. **Two handlers interleaved on one thread**
   - A.Initialize(A)
   - B.Initialize(B)
   - A.QueryContextMenu(menuA)
   - B.QueryContextMenu(menuB)
   - assert menuA→A, menuB→B.

2. **Two STA threads**
   - initialize COM with `COINIT_APARTMENTTHREADED` on each;
   - create/capture independently;
   - bind menus concurrently;
   - resolve from each popup apartment;
   - assert no raw cross-apartment pointer is used.

3. **Owning match result**
   - `result = match(menu)`;
   - `clear(menu)`;
   - result's COM proxy/PIDL remains valid until `result` dies.

4. **Clear isolation**
   - clear A; B remains active.

5. **TTL prune**
   - expired entry is erased and GIT cookie revoked exactly once.

6. **Handler destruction before QueryContextMenu**
   - unpublished pending capture is revoked/freed.

7. **Unload**
   - `DllCanUnloadNow` is `S_FALSE` with active capture/hooks.

---

# 7. P0 — Complete Windows Installer architecture/component servicing

## 7.1 What is already correct

The source now uses separate ProductCodes for x64/x86/ARM64 and `Version='1.9.20'`. Keep that.

Microsoft explicitly requires distinct ProductCodes for 32-bit and 64-bit application packages:  
<https://learn.microsoft.com/en-us/windows/win32/msi/product-codes>

## 7.2 Remaining Component GUID problem

`APPLICATION` currently has one Component GUID for all architectures:

```xml
<Component Id='APPLICATION'
           Guid='{ED9EFE4B-E83D-42BB-8938-F72B13A2FAD2}'>
```

but contains architecture-specific `shell.exe` and `shell.dll`.

Microsoft explicitly lists recompiling a 32-bit component as 64-bit as a change that requires a new Component code:  
<https://learn.microsoft.com/en-us/windows/win32/msi/changing-the-component-code>

Microsoft also requires 64-bit components to carry the 64-bit Component-table attribute:  
<https://learn.microsoft.com/en-us/windows/win32/msi/64-bit-windows-installer-packages>  
<https://learn.microsoft.com/en-us/windows/win32/msi/component-table>

## 7.3 Do not solve this by blindly changing every GUID

The current major-upgrade sequence is:

```xml
<RemoveExistingProducts After='InstallFinalize' />
```

Late removal relies heavily on correct component identity/reference counting. Changing a component GUID while installing the same resources to the same path can cause the old product's later uninstall to remove resources belonging to the new product.

Microsoft documents the valid scheduling positions for `RemoveExistingProducts`:  
<https://learn.microsoft.com/en-us/windows/win32/msi/removeexistingproducts-action>

Therefore the installer change must be implemented as a coherent servicing model, not as isolated GUID edits.

## 7.4 Recommended servicing policy for v1.9.20

### A. Make packages native-architecture only

Shell is an in-process Explorer extension, so deploying an x86 shell DLL into native x64 Explorer is invalid. Add launch conditions so:

- x86 MSI installs only on a 32-bit OS;
- x64 MSI installs only on x64 Windows;
- ARM64 MSI installs only on ARM64 Windows.

Do not support in-place cross-architecture package switching. Require uninstall/clean install if an unsupported architecture variant somehow exists.

This sharply reduces cross-product component collisions and is consistent with the failure that previously deployed x86 `shell.dll` into x64 Explorer.

### B. Architecture-specific binary component

Define architecture-specific component GUID variables for the binary component, stable for that architecture for future compatible versions:

```wxs
<?if $(var.Platform) = x64 ?>
  <?define ApplicationComponentGuid = '{...X64-STABLE-GUID...}' ?>
<?elseif $(var.Platform) = arm64 ?>
  <?define ApplicationComponentGuid = '{...ARM64-STABLE-GUID...}' ?>
<?else ?>
  <?define ApplicationComponentGuid = '{...X86-STABLE-GUID...}' ?>
<?endif ?>
```

Then:

```xml
<Component Id='APPLICATION'
           Guid='$(var.ApplicationComponentGuid)'
           Bitness='default'>
```

For the x64 variant, preserving the existing v1.9.19/v1.9.20 Component GUID is desirable **only if** it was already the x64 component and its resources/paths remain a compatible version of the same component. Do not churn component codes needlessly.

For x86/ARM64, use architecture-distinct Component GUIDs where the compiled component identity differs.

### C. Audit every component whose resource identity differs by architecture

Specifically inspect:

- `APPLICATION`
- `DisplayIcon` registry resource (ProductCode/formatted target and registry view)
- Start-menu shortcut if target differs by architecture
- any future COM-registration component

Do not change GUIDs for purely architecture-neutral file components unless their target path, key path, or component bitness genuinely differs.

### D. Verify component bitness in the emitted MSI

For x64 and ARM64 packages, verify that each actual 64-bit component has `msidbComponentAttributes64bit` (`0x0100`) set in the MSI Component table.

Also verify SummaryInformation platform:

- x64 → `x64`
- ARM64 → `Arm64`
- ARM64 Installer schema/page count ≥ 500 (current `InstallerVersion='500'` is consistent with this requirement).

### E. Explicitly validate upgrade sequencing before changing Component IDs

Because current `RemoveExistingProducts` is after `InstallFinalize`, retain that late schedule only where component rules remain valid.

If a required Component GUID change means the old and new components cannot safely overlap at the same path, move `RemoveExistingProducts` earlier (preferably after `InstallInitialize`) **and separately preserve user configuration before old-product removal**.

Do not move the action earlier without handling `shell.nss`: the upstream product can delete the existing config before the new package installs.

If an early-removal migration becomes necessary, use one of these deliberate policies:

1. **Preferred:** preserve the existing `CONFIG` component identity/path for same-architecture upgrades so late removal remains safe; or
2. implement a tested upgrade migration that copies/restores `shell.nss` using MSI-managed/custom-action state before old removal.

Do not rely on `NeverOverwrite` to protect a file that the old product has already uninstalled.

### F. Remove broad uninstall wildcard cleanup

Current:

```xml
<RemoveFile Id='PurgeAppFiles' Name='*.*' On='uninstall' ... />
```

can remove user-created files in the install folder and makes configuration-preservation strategies harder. Replace broad wildcard removal with explicit known temporary/obsolete files and allow Windows Installer's component ownership to remove installed resources.

## 7.5 Installer tests

Add a script that opens each built MSI through the Windows Installer API and asserts:

- ProductVersion == 1.9.20;
- ProductCode matches the architecture;
- UpgradeCode is the intended preserved code;
- package platform summary is correct;
- APPLICATION component GUID/bitness matches architecture policy;
- x64/ARM64 binary component has attribute 0x100;
- key paths exist;
- package contains matching-architecture PE binaries.

Then test on clean Windows VMs:

| Scenario | Required result |
|---|---|
| upstream 1.9.19 x64 → 1.9.20 x64 | config preserved; new DLL/EXE active |
| repair 1.9.20 x64 | succeeds; user config preserved |
| uninstall 1.9.20 | clean MSI-owned resources; documented config policy |
| reinstall | works without stale component state |
| x86 MSI on x64 OS | rejected before install |
| x64 MSI on ARM64 native Shell target | rejected unless explicitly supported |
| downgrade 1.9.20 → 1.9.19 | correctly blocked |

---

# 8. P1 — Replace irreversible `std::call_once` bootstrap with transactional/retryable bootstrap

## 8.1 Current problem

Current `BootstrapOnce()` catches every exception *inside* the `call_once` function:

```cpp
std::call_once(flag, [] {
    try {
        ...
    } catch (...) {
        ...
    }
});
```

and has early successful returns such as the third-party-disable branch.

Microsoft's `call_once` contract guarantees one **successful** invocation. If the callable returns normally, the once flag is consumed. Since exceptions are swallowed inside the callable, a partial failure is indistinguishable from successful initialization:  
<https://learn.microsoft.com/en-us/cpp/standard-library/mutex-functions>

Current hook setup also ignores/does not transactionally validate every install result and eventually sets:

```cpp
hooks_installed = true;
```

without a formal commit point.

A failure after installing one hook but before completing the rest can leave the process half-hooked while the bootstrap can never retry.

## 8.2 Required state machine

Replace `once_flag` with:

```cpp
enum class BootstrapState : uint8_t
{
    NotStarted,
    Initializing,
    Ready,
    Disabled,
    RetryableFailure,
    PermanentFailure
};
```

Protect transitions with a mutex/condition variable (or equivalent correctly synchronized state machine).

`EnsureBootstrapped()` should:

- return immediately for `Ready`;
- return a distinct result for `Disabled`;
- wait if another thread is `Initializing`;
- retry after `RetryableFailure`;
- never expose partially committed hooks.

## 8.3 Hook-install transaction

Create a small transaction object that records every successfully installed hook:

```cpp
class HookInstallTransaction
{
public:
    bool install(...);
    void commit();
    ~HookInstallTransaction(); // rollback in reverse order unless committed
};
```

Required sequence:

1. determine host/process;
2. check third-party disable policy;
3. initialize non-hook runtime/config;
4. **pin module**;
5. register custom window layer;
6. install hook 1; verify result;
7. install hook 2; verify result;
8. enumerate modules/install remaining hooks; verify or explicitly classify optional failures;
9. install Explorer-specific hooks/taskbar support;
10. `transaction.commit()`;
11. only now set `hooks_installed=true`, `state=Ready`.

Any failure before commit must rollback every hook successfully installed in that attempt. If rollback itself is not guaranteed, keep the module pinned and classify the process as `PermanentFailure` rather than pretending unload is safe.

## 8.4 Testing via fault injection

Abstract hook operations behind testable wrappers and inject failure at each step:

- failure before pin → no hooks, retry allowed;
- failure after first IAT hook → hook rolled back or module remains pinned + permanent safe state;
- failure during Detours commit → no stale `hooks_installed=false` with live hook;
- registry transient failure → second activation can retry;
- disabled third-party state is `Disabled`, not a fake successful bootstrap.

---

# 9. P1 — Finish explicit path semantics and eliminate DLL process-CWD mutation

## 9.1 Current state

`eval_io()` now has a selection-relative resolver, but `eval_path()` still directly calls:

```cpp
SetCurrentDirectoryW(arg0);
Path::Full(arg0);
Path::IsDirectoryEmpty(arg0);
Path::Exists(arg0);
Path::IsFileExists(arg0);
Path::IsDirectoryExists(arg0);
```

Microsoft explicitly documents that a process has one current directory shared by all threads and says multithreaded applications/shared-library code should avoid both changing the process current directory and relying on relative paths:  
<https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setcurrentdirectory>

## 9.2 Introduce one resolver

Create a helper in an appropriate shared/expression path module:

```cpp
enum class PathBase
{
    SelectionDirectory,
    ConfigDirectory,
    ApplicationDirectory
};

string ResolveContextPath(const Context& ctx,
                          const string& input,
                          PathBase base = PathBase::SelectionDirectory);
```

Rules:

1. empty input → unchanged;
2. normalize separators;
3. absolute drive, UNC, device/namespace path → unchanged/canonicalized as appropriate;
4. runtime filesystem operations → selection directory, then selection parent, then application directory fallback;
5. parser/import assets → importing script's directory, not selection directory;
6. never use process CWD as implicit fallback inside the DLL.

Use existing robust path primitives (`Path::IsAbsolute`, canonicalization) rather than local drive-letter checks where possible.

## 9.3 Apply it everywhere

Replace the local `eval_io()` lambda with this central resolver and use it for:

- `path.full`
- `path.exists`
- `path.empty`
- `path.isfile`
- `path.isdir`
- copy/move/rename source and destination
- file create/write/append
- directory create/delete/existence
- attributes
- icon/file helpers where a user-provided path is runtime-relative

## 9.4 Redesign `currentdirectory(...)`

Do not call `SetCurrentDirectoryW` from the shell extension.

Preferred compatibility model:

- getter: return context-local virtual working directory / selection directory;
- setter: set `Context::WorkingDirectory` (or per-menu equivalent), affecting only later Shell expression path resolution;
- never change host-process CWD.

If compatibility cannot be preserved safely, deprecate the setter and make failure explicit rather than silently mutating Explorer/third-party host state.

## 9.5 Tests

- relative file path against selection directory;
- background click folder;
- no selection → application fallback;
- `C:\...`, `C:/...`, UNC and namespace paths stay rooted;
- one menu changes virtual working directory; another menu unaffected;
- process `GetCurrentDirectoryW()` unchanged before/after expression evaluation.

---

# 10. P1 — Fix `PackagesCache` success/retry and returned-reference semantics

Current code does:

```cpp
if (!_loaded) {
    load();
    _loaded = true;
}
```

so a failed registry load permanently marks the cache loaded/empty until `clear()`.

Change to build-then-publish:

```cpp
bool PackagesCache::ensure_loaded_locked()
{
    if (_loaded) return true;

    std::vector<Package> next;
    if (!load_into(next))
        return false;

    _list.swap(next);
    _loaded = true;
    return true;
}
```

Do not mutate `_list` incrementally on a failed load.

Also avoid returning pointers/references whose safety depends on a mutex that has already been released if `clear()` can run concurrently. Prefer:

```cpp
std::optional<Package> find_copy(...);
std::vector<Package> snapshot() const;
```

or a shared immutable package-list snapshot.

Tests:

- first registry/load attempt fails → `_loaded` remains false;
- second succeeds → data appears;
- concurrent find/clear does not return dangling `Package*`.

---

# 11. P1 — Rework WIC ownership so it is apartment/lifetime safe

## 11.1 Current problem

`WIC` owns one unsynchronized raw process-global COM factory:

```cpp
inline static IWICImagingFactory* _factory = nullptr;
```

Multiple menu threads can race lazy initialization. `Initializer` also calls `WIC::release()` from its global destructor, which reintroduces loader-lock teardown.

The class additionally performs `GetSystemMetrics` during dynamic static initialization.

Official WIC documentation shows creation of `IWICImagingFactory` through COM and discusses MTA/threading support:  
<https://learn.microsoft.com/en-us/windows/win32/wic/-wic-api>  
<https://learn.microsoft.com/en-us/windows/win32/wic/-wic-howwicworks>

## 11.2 Recommended implementation

Simplest/safest option for this DLL:

- remove static raw `_factory`;
- provide an owning factory at menu/context scope or create a local `IComPtr<IWICImagingFactory>` for an image operation;
- ensure the calling thread has already established COM in the popup scope;
- pass the factory into helper operations rather than reading a global;
- remove `WIC::release()` from `Initializer` teardown;
- compute icon metrics using the menu's DPI-aware context, not global `GetSystemMetrics` static fields.

If profiling shows factory creation is material, introduce a documented apartment-safe cache only after correctness is proven.

---

# 12. P2 — Move UI Automation off the menu/UI thread

The global cross-apartment `IUIAutomation*` was removed, which was correct. However, current taskbar hit testing still creates `CUIAutomation` and calls `ElementFromPoint`/property getters synchronously on the caller/menu path.

Microsoft warns that desktop-wide UI Automation calls from a UI thread can become very slow or unresponsive and recommends a separate non-window MTA thread:  
<https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading>

## Recommended design

Add a `TaskbarAutomationWorker`:

- lazily started outside loader lock;
- worker calls `CoInitializeEx(nullptr, COINIT_MULTITHREADED)`;
- worker owns `IUIAutomation`;
- requests contain `(HWND taskbar, Point screenPt)`;
- short bounded wait from menu path (for example 50–100 ms); on timeout, fail conservatively rather than hang the menu;
- retain the current ~250 ms debounce cache, but store results in a synchronized cache owned by the worker;
- no worker join/destruction from `DllMain`; module/process-lifetime policy handles process termination.

If the same decision can be obtained reliably from HWND hit testing without UIA, prefer the simpler native window approach and remove UIA entirely.

---

# 13. P1 — Add the regression suites promised by the previous plan

The previous plan explicitly called for parser and thread-safety tests, but the latest tree contains only:

- `test_stringcompare.cpp`
- `test_encoding.cpp`
- `test_bitmapcache.cpp`
- `test_shellext.cpp`

The next release should not claim parser/thread-snapshot validation until the following exist.

## 13.1 `test_parser.cpp`

Prefer extracting import-path/cycle logic into a small testable helper rather than constructing the entire Explorer DLL environment.

Required cases:

1. self-cycle: `A -> A`;
2. mutual cycle: `A -> B -> A`;
3. three-node cycle;
4. repeated shared dependency that is **not** a recursion cycle;
5. `.` alias canonicalization;
6. `..` alias canonicalization;
7. `C:/folder/file.nss` recognized as absolute;
8. normal `C:\folder\file.nss`;
9. UNC path;
10. extended namespace path if supported;
11. depth 32 accepted;
12. depth 33 rejected;
13. missing import with ignore-failure setting;
14. case-insensitive path equality consistent with Windows path semantics.

## 13.2 `test_threadsafety.cpp`

Cover the snapshot/runtime tests described in Workstream 5 plus:

- concurrent `FontCache` requests at 96/144/192 DPI;
- package-cache first-load failure/retry;
- ShellExt A/B menu capture interleaving;
- snapshot reload while menu expressions are actively evaluated.

## 13.3 End-to-end context-menu smoke test

This is the highest-value missing test because the earlier 23k-check suite stayed green while the primary visible menu was broken.

At minimum create a Windows-only integration harness that verifies:

1. Shell builds a replacement popup from a realistic source `HMENU`;
2. the path reaching the final invocation uses `ContextMenu::MenuHandle()`;
3. the resulting menu contains expected Shell/dynamic items;
4. the original menu remains distinct;
5. selected-file and background cases both work.

If full automated Explorer UI interaction is too fragile, add a release-gating Windows VM smoke script plus test-build instrumentation/ETW/logging that records:

```text
original HMENU
replacement HMENU
replacement item count
host process
selection count
```

and fail the release gate if replacement handle/item count is invalid.

---

# 14. P1 — ARM64 validation must go beyond a configured matrix entry

The build system now isolates ARM64 outputs, but the attached verification evidence covers actual x64/x86 builds and test execution, not ARM64 runtime behavior.

## Required CI additions

For every architecture artifact:

1. inspect `shell.dll`, `shell.exe`, `ca.dll` PE Machine field;
2. assert:
   - x86 = `0x014C`
   - x64 = `0x8664`
   - ARM64 = `0xAA64`
3. inspect MSI SummaryInformation and Component table;
4. ensure MSI packaged binaries match the MSI architecture;
5. upload architecture manifest alongside artifact.

ARM64 build must compile the NEON implementation. If an ARM64 runner is not available, label ARM64 **build-supported, runtime-unverified** until tested on Windows on ARM hardware/VM.

Before marking stable ARM64 support, manually test:

- native ARM64 Explorer;
- file/folder/background menus;
- installer register/unregister;
- config reload;
- at least one non-Explorer ARM64 host if supported.

---

# 15. P1 — Harden deployment/build guardrails

The per-architecture output change correctly fixes the x86/x64 clobbering failure. Make it impossible to regress.

## `.bin/deploy_fix.ps1`

Current script falls back from:

```text
src/bin/<host-arch>/shell.dll
```

to legacy flat:

```text
src/bin/shell.dll
```

Remove that fallback. Missing architecture-specific output must be a hard error.

Also replace the user-specific:

```powershell
$repoRoot = "c:\Users\j_opp\Projects\Shell"
```

with a path based on `$PSScriptRoot`/repository root.

Before stopping Explorer verify **both** `shell.dll` and `shell.exe`:

- exist in the architecture directory;
- have expected PE Machine;
- report expected file/product version;
- originate from the same build directory.

Only after all validation passes may the script stop Explorer.

Keep the existing `finally` restart behavior.

## `build.ps1` / CI

- keep isolated OutDir/IntDir;
- add explicit PE architecture assertions after build;
- run x86 tests under WOW64 on x64 runners;
- cross-compile ARM64 and record that tests were not executed unless a native/emulated supported runner is available;
- fail artifact upload if expected MSI is missing.

---

# 16. P2 — Experimental native owner-draw/private-data compatibility must stay isolated

The earlier fork work introduced heuristics for native owner-draw menu icons and foreign `dwItemData` layouts. The broad private scan is currently disabled at the call site, which should remain the default.

Do not re-enable undocumented memory-layout scanning as part of this hardening work.

For the remaining owner-draw reconstruction path:

- keep the corrected `GetDIBits` DC-selection contract;
- feature-gate additional heuristic reconstruction;
- add compatibility fixtures/manual matrix for 7-Zip, WinRAR, disabled/selected items, dark/light themes and DPI;
- ensure calling a third-party owner's `WM_DRAWITEM` for offscreen extraction cannot recursively re-enter Shell without a guard.

This is compatibility work, not a P0 correctness dependency.

---

# 17. Small COM/API cleanup to include while touching the relevant code

These are low-risk and should be handled in the same focused commits, not as separate feature work:

- For unsupported COM classes, return `CLASS_E_CLASSNOTAVAILABLE` consistently. The existing special `IID_FolderExtensions -> E_NOTIMPL` should be reviewed; retain it only if there is a demonstrated caller contract requiring it.
- Validate `ppv != nullptr` at the top of `DllGetClassObject`; return `E_POINTER` where appropriate rather than proceeding with a null output pointer.
- Keep every successful `CoInitializeEx` call paired with `CoUninitialize` on the same thread.
- Do not add COM initialization back into `DllMain`.

---

# 18. Implementation sequence with mandatory checkpoints

## Commit 0 — Baseline normalization only

- Confirm the eight current working-tree modifications are EOL/whitespace-only.
- Normalize/reset them according to `.gitattributes`.
- `git status` clean.
- No semantic code change.

**Checkpoint:** current x64 build + existing tests still pass and current menu still works.

## Commit 1 — Process lifetime / loader lock

- introduce `RuntimeState`;
- remove dynamic global Win32/COM/registry initialization;
- make DllMain minimal including detach;
- add module pin helper;
- remove destructor-driven runtime cleanup.

**Checkpoint:** x64/x86 compile; Explorer smoke; Application Verifier loader-lock test.

## Commit 2 — Snapshot ownership

- remove raw global `CACHE*` API;
- introduce fully owned snapshot acquisition;
- serialize config build;
- move MUID into snapshot;
- move runtime variables into `ContextMenu` instance;
- remove process-global DPI mutation.

**Checkpoint:** x64/x86 existing suite + new snapshot stress tests.

## Commit 3 — ShellExt apartment-safe registry

- per-handler pending state;
- GIT/agile-marshaled COM reference;
- owning match result;
- per-HMENU cleanup;
- new concurrent/STA tests.

**Checkpoint:** Explorer + third-party smoke; DllCanUnloadNow tests.

## Commit 4 — Bootstrap transaction

- explicit bootstrap state machine;
- pin-before-hook;
- hook transaction/rollback;
- retry/failure injection tests.

**Checkpoint:** simulate failures at every installation stage.

## Commit 5 — MSI servicing

- architecture component identity audit;
- correct binary Component GUID/bitness;
- native-architecture launch policy;
- remove unsafe wildcard cleanup;
- validate upgrade schedule/component strategy;
- add MSI metadata test script.

**Checkpoint:** clean VM upgrade/repair/uninstall tests.

## Commit 6 — Path/package/WIC correctness

- central context path resolver;
- remove `SetCurrentDirectoryW` from DLL runtime;
- PackagesCache retry/build-swap;
- WIC safe ownership.

**Checkpoint:** new parser/path/package tests.

## Commit 7 — QA/ARM64/UIA

- parser suite;
- thread-safety suite;
- menu smoke harness;
- ARM64 artifact verification;
- UIA worker thread if retained.

**Checkpoint:** full release matrix below.

---

# 19. Full release validation matrix

A release candidate should not be tagged until all applicable cells pass.

## 19.1 Build/static checks

- [ ] clean repository
- [ ] x64 Release: 0 errors, 0 warnings
- [ ] x86 Release: 0 errors, 0 warnings
- [ ] ARM64 Release compiles
- [ ] correct PE Machine for every artifact
- [ ] `git diff --check` clean
- [ ] WiX/MSBuild XML parses
- [ ] MSI ICE/validation clean or every remaining warning documented
- [ ] ProductVersion/ProductCode/UpgradeCode verified from MSI database
- [ ] x64/ARM64 Component bitness verified

## 19.2 Unit/component tests

- [ ] string compare
- [ ] encoding
- [ ] BitmapCache
- [ ] PackagesCache fail/retry
- [ ] parser import/canonicalization
- [ ] configuration snapshot stress
- [ ] runtime variable isolation
- [ ] multi-DPI font access
- [ ] ShellExt concurrent handler isolation
- [ ] ShellExt two-STA marshaling
- [ ] DllCanUnloadNow/module-pin behavior
- [ ] bootstrap fault injection/rollback

## 19.3 Explorer runtime tests — Windows 11 x64

- [ ] file right-click
- [ ] multi-file selection
- [ ] folder right-click
- [ ] drive/root
- [ ] folder background
- [ ] desktop
- [ ] taskbar
- [ ] Windows 11 modern path/TreatAs behavior
- [ ] “Show more options”/classic path as applicable
- [ ] dark theme
- [ ] light theme
- [ ] 100/125/150/200% DPI
- [ ] negative-coordinate secondary monitor
- [ ] config edit/reload while another menu is still alive
- [ ] rapid repeated menus
- [ ] two simultaneous UI threads using snapshots

## 19.4 Third-party hosts

At minimum test two real hosts that use classic shell extensions:

- [ ] Total Commander selection
- [ ] Total Commander background
- [ ] Directory Opus selection
- [ ] Directory Opus background
- [ ] file open/save dialog if handler is expected there
- [ ] host exit after Shell hooks installed
- [ ] multiple simultaneous menus/captures where host permits

## 19.5 Installer tests

- [ ] clean x64 install
- [ ] upstream 1.9.19 x64 → 1.9.20 x64
- [ ] custom `shell.nss` survives upgrade
- [ ] stock imports/locales update
- [ ] repair
- [ ] uninstall
- [ ] reinstall
- [ ] newer-version downgrade blocked
- [ ] wrong-architecture MSI rejected
- [ ] Explorer/file-in-use handling through Restart Manager
- [ ] no orphaned TreatAs/COM registrations

## 19.6 Loader/process checks

- [ ] Application Verifier loader-lock check
- [ ] no dynamic global registry/User32/GDI/COM initialization
- [ ] no global destructor doing COM/GDI/registry teardown
- [ ] no hook target can outlive module mapping
- [ ] process termination does not perform complex detach cleanup

---

# 20. Agent-specific implementation constraints

The coding agent should follow these rules for this round:

1. **Do not modify `invoke(ctx->MenuHandle(), ...)` except to add a regression assertion/test.**
2. Do not perform broad formatting/EOL rewrites.
3. Do not merge new visual/fork features.
4. Do not replace concurrency problems with naked atomics around pointers; ownership must be explicit.
5. Never return a pointer/reference to a mutex-protected container element and then release the mutex if another thread can erase/rehash the container.
6. Do not store apartment-bound COM interface pointers in global containers without marshaling/agile indirection.
7. Do not call registry/User32/GDI/COM from `DllMain`, global constructors, or global destructors.
8. Install process hooks only after the module is pinned.
9. Make hook installation transactional; `hooks_installed=true` is a commit marker, not an optimistic flag.
10. Do not call `SetCurrentDirectoryW` from the injected DLL.
11. A configuration reload must be **build new → validate → atomically publish**. Never destroy the old snapshot first.
12. Active menu objects must remain internally consistent with one configuration generation.
13. Runtime variables belong to a menu invocation, not the shared parsed configuration.
14. Never change MSI Component GUIDs independently of the upgrade schedule/component rules.
15. Every architectural fix must add a regression test that would have failed before the fix.
16. Build/test after every commit; do not stack all P0 changes and debug them simultaneously.

---

# 21. Official documentation references

## DLL loader/process lifetime

- Microsoft — **Dynamic-Link Library Best Practices**  
  <https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices>
- Microsoft — **DllMain entry point**  
  <https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain>
- Microsoft — **Dynamic-link library creation / thread-safe global data**  
  <https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-creation>
- Microsoft — **GetModuleHandleExW / GET_MODULE_HANDLE_EX_FLAG_PIN**  
  <https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandleexw>

## COM/apartments

- Microsoft — **In-Process Server Threading Issues**  
  <https://learn.microsoft.com/en-us/windows/win32/com/in-process-server-threading-issues>
- Microsoft — **Single-Threaded Apartments**  
  <https://learn.microsoft.com/en-us/windows/win32/com/single-threaded-apartments>
- Microsoft — **Processes, Threads, and Apartments**  
  <https://learn.microsoft.com/en-us/windows/win32/com/processes--threads--and-apartments>
- Microsoft — **Accessing Interfaces Across Apartments**  
  <https://learn.microsoft.com/en-us/windows/win32/com/accessing-interfaces-across-apartments>
- Microsoft — **IGlobalInterfaceTable**  
  <https://learn.microsoft.com/en-us/windows/win32/api/objidl/nn-objidl-iglobalinterfacetable>
- Microsoft — **Creating the Global Interface Table**  
  <https://learn.microsoft.com/en-us/windows/win32/com/creating-the-global-interface-table>
- Microsoft — **RoGetAgileReference** (modern alternative)  
  <https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-rogetagilereference>
- Microsoft — **DllCanUnloadNow**  
  <https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-dllcanunloadnow>

## Windows Installer

- Microsoft — **Product Codes**  
  <https://learn.microsoft.com/en-us/windows/win32/msi/product-codes>
- Microsoft — **ProductCode property**  
  <https://learn.microsoft.com/en-us/windows/win32/msi/productcode>
- Microsoft — **Changing the Component Code**  
  <https://learn.microsoft.com/en-us/windows/win32/msi/changing-the-component-code>
- Microsoft — **Component Table / 64-bit attribute**  
  <https://learn.microsoft.com/en-us/windows/win32/msi/component-table>
- Microsoft — **64-bit Windows Installer Packages**  
  <https://learn.microsoft.com/en-us/windows/win32/msi/64-bit-windows-installer-packages>
- Microsoft — **RemoveExistingProducts Action**  
  <https://learn.microsoft.com/en-us/windows/win32/msi/removeexistingproducts-action>
- Microsoft — **Applying Major Upgrades by Installing the Product**  
  <https://learn.microsoft.com/en-us/windows/win32/msi/applying-major-upgrades-by-installing-the-product>

## Filesystem/path behavior

- Microsoft — **SetCurrentDirectory**  
  <https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setcurrentdirectory>

## Threading/helpers

- Microsoft C++ — **`call_once` / `<mutex>`**  
  <https://learn.microsoft.com/en-us/cpp/standard-library/mutex-functions>
- Microsoft — **UI Automation: Understanding Threading Issues**  
  <https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading>
- Microsoft — **WIC API Overview**  
  <https://learn.microsoft.com/en-us/windows/win32/wic/-wic-api>
- Microsoft — **How Windows Imaging Component works / MTA support**  
  <https://learn.microsoft.com/en-us/windows/win32/wic/-wic-howwicworks>

---

# 22. Definition of done

The remaining hardening round is complete only when all of the following are true:

- `DllMain` **and CRT global ctor/dtor paths** contain no unsafe registry/User32/GDI/COM/runtime teardown.
- process hooks cannot remain mapped to an unloadable DLL.
- no runtime consumer can obtain a naked process-global cache pointer.
- every menu owns one consistent configuration snapshot for its full lifetime.
- runtime expression variables are menu-local.
- MUID pointers are snapshot-owned and cannot be invalidated by reload.
- configuration construction/reload is serialized and failed reload leaves the old configuration active.
- ShellExt selection state is handler-specific, menu-specific, owning, and apartment-safe.
- one menu cannot clear or overwrite another menu's capture.
- bootstrap failures roll back or enter an explicitly safe state and can retry where appropriate.
- no Shell expression mutates Explorer/host process CWD.
- MSI architecture-specific compiled components have correct Product/Component identity and bitness.
- MSI upgrade behavior has been tested against actual upstream 1.9.19 with user config present.
- parser and thread/concurrency suites exist and pass.
- a context-menu integration test or formal release-gating VM smoke test exists specifically to catch replacement-`HMENU` regressions.
- x64/x86 are fully verified and ARM64 is either actually verified or explicitly labeled runtime-unverified.
- current working Explorer behavior remains unchanged from the known-good `74bff70` baseline.

---

## Final implementation recommendation

Do **not** create another large all-in-one hardening commit. The current code is functionally useful and working on the tested machine; the remaining risks are exactly the kind that are easier to introduce than observe. Treat the work as an ownership/lifetime project rather than a feature project.

The most important design change is to make three lifetimes explicit:

1. **process lifetime** — pinned module + process-lifetime runtime infrastructure;
2. **configuration generation lifetime** — immutable shared snapshot retained by each menu;
3. **menu/COM capture lifetime** — per-handler pending capture → per-HMENU bound capture with apartment-correct interface resolution.

Once those three are structurally correct, most of the remaining race/unload/loader-lock defects disappear instead of being patched individually.
