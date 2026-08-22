Yes. If takeover mode is treated as the product's defining capability rather than something to be phased out, I would change the optimization target substantially.

The goal should become:

> **Make Shell behave like a highly faithful Win32/Shell context-menu host internally, while retaining the ability to replace, reorganize, restyle and augment the resulting menu.**

That leads to a different architecture than the “maximum native compliance” approach. The current takeover machinery is fundamentally viable, but it needs a stronger compatibility boundary around Windows and third-party code. Once that exists, several surprisingly inexpensive additions can make the product considerably more capable.

I re-scanned the takeover path specifically, including `Main.cpp`, `ContextMenu.cpp`, `NativeMenuLazy.h`, `ExplorerCommand.cpp`, `Selections.cpp`, `Initializer.cpp`, `MenuItem.h`, the expression runtime, bitmap/package caches, performance instrumentation, and the relevant tests. I also cross-checked the important message/menu contracts against current Microsoft documentation.

## Bottom line: highest-value direction

I would prioritize the following, roughly in this order:

| Priority | Change                                                                                                          |                       User benefit |        Scope |
| -------- | --------------------------------------------------------------------------------------------------------------- | ---------------------------------: | -----------: |
| **1**    | Introduce a proper **TakeoverSession + host contract/message bridge**                                           |                        Exceptional |       Medium |
| **2**    | Make first paint strictly non-blocking where Shell controls it, plus **provider health diagnostics/quarantine** |                        Exceptional |       Medium |
| **3**    | Complete native submenu lifecycle fidelity, especially `WM_UNINITMENUPOPUP`                                     |                          Very high | Small-medium |
| **4**    | Add **last-known-good configuration**, automatic recovery and one-shot native bypass                            |                          Very high | Small-medium |
| **5**    | Remove global Windows-setting mutations and gate/remove the vblank `Sleep()` hack                               |                               High |        Small |
| **6**    | Replace `moveto => eager whole tree` with **targeted native path discovery**                                    | Very high for power configurations |       Medium |
| **7**    | Proper accessibility + mnemonic keyboard handling + type-ahead                                                  |                               High | Small-medium |
| **8**    | Stale-while-revalidate Windows 11 command catalog + broader icon caching                                        |                               High |       Medium |
| **9**    | Smart automatic multi-column overflow                                                                           |                        Medium-high |        Small |
| **10**   | Explicit host compatibility profiles and takeover circuit breakers                                              |                               High |       Medium |
| Later    | Lazy large-selection metadata + expression memoization                                                          |                   Potentially high | Medium-large |

The first six matter more than adding another dozen NSS functions. They strengthen the platform on which future functionality sits.

---

# 1. The key architectural change: make takeover a faithful transaction

I would **not** start by rewriting `ContextMenu.cpp`.

Instead, introduce a relatively small object around the existing implementation:

```text
Host calls TrackPopupMenu/Ex
             │
             ▼
      TakeoverSession
             │
     ┌───────┼────────┐
     │       │        │
 Call     Menu      Selection
contract  lifecycle  context
     │       │        │
     └───────┼────────┘
             ▼
       Existing Shell
       ContextMenu logic
             │
             ▼
       CommandRouter
             │
             ▼
     Complete original
       host contract
```

A session should explicitly own/record:

```text
Original:
    HMENU
    HWND owner
    TrackPopupMenu vs TrackPopupMenuEx
    original flags
    TPMPARAMS
    original notification/return semantics

Shell:
    synthetic HMENU
    config generation
    selection snapshot
    synthetic command IDs
    popup HWNDs
    menu nesting/reentrancy

Native bridge:
    original popup handles
    parent position
    INIT sent?
    UNINIT sent?
    materialized?
    rules applied?

Command routing:
    displayed ID
       -> Shell custom command
       -> original native command
       -> IExplorerCommand

Diagnostics:
    phase timings
    provider timings
    compatibility deviations
    fallback reason
```

This sounds larger than it is. Most of these concepts already exist, but they are scattered between `ContextMenu`, global/thread state, `NativePopupState`, hook flags, ID ranges and window properties.

The value is that takeover becomes an explicit **translation layer between two contracts**, rather than “replace the menu and hope the important semantics survive”.

That is the strongest architectural improvement I see if takeover itself remains non-negotiable.

---

# 2. There is a real TrackPopupMenu compatibility problem to solve

The current `NtUserTrackPopupMenu` implementation changes the incoming flags:

```cpp
Flag<uint32_t> flag(uFlags);

flag.remove(TPM_NONOTIFY);
...
flag.remove(TPM_HORIZONTAL);
flag.add(TPM_VERTICAL);
```

It then displays the synthetic menu and calls:

```cpp
result = ctx->InvokeCommand(result);
```

There is a more fundamental issue here.

Microsoft defines two distinct `TrackPopupMenu` contracts:

* with `TPM_RETURNCMD`, the return value is the selected command ID;
* without it, the return value is merely success/failure and the owner receives the command notification;
* with `TPM_NONOTIFY`, command notification is suppressed. ([Microsoft Learn][1])

Shell's own commands live in the high synthetic ID range beginning at `0x0fffffff`. `ContextMenu::InvokeCommand()` needs the actual selected ID to identify them.

For a caller that **did not request `TPM_RETURNCMD`**, the real menu call is allowed to return `TRUE`, effectively integer `1`, rather than the selected Shell ID.

The code records `selectid` during `MN_BUTTONUP`, but `InvokeCommand()` does not use it, and keyboard activation does not follow that mouse path anyway.

So takeover is presently much safer for `TPM_RETURNCMD` callers than for the full Win32 API contract.

### What I would do

Internally, Shell should have one consistent selection protocol regardless of how the host called it.

Conceptually:

```text
Host contract
    ↓
normalize internally to SelectedCommand
    ↓
Shell menu runs
    ↓
CommandOrigin lookup
    ↓
translate back to original host contract
```

Do not just force some flags and call it done. The correct behaviour needs to cover:

* `TrackPopupMenu` and `TrackPopupMenuEx`
* `TPM_RETURNCMD` on/off
* `TPM_NONOTIFY` on/off
* cancel
* mouse selection
* keyboard selection
* original native item
* Shell item
* reconstructed `IExplorerCommand`
* `MNS_NOTIFYBYPOS`.

That last one matters because Microsoft defines `MNS_NOTIFYBYPOS` as changing command delivery from `WM_COMMAND` to `WM_MENUCOMMAND`. The synthetic menu currently does not deliberately preserve that original style. ([Microsoft Learn][2])

I would not decide the exact send/post ordering from documentation alone. Build a Windows probe and compare traces against untouched `TrackPopupMenu`. More on that below.

This single change would make takeover considerably more credible in third-party hosts.

---

# 3. Treat Shell as an `IContextMenu3`-quality host internally

Because Shell captures and reconstructs other people's context menus, it has implicitly taken on some responsibilities of a real context-menu host.

Microsoft specifically calls out messages such as:

* `WM_INITMENUPOPUP`
* `WM_DRAWITEM`
* `WM_MEASUREITEM`
* `WM_MENUCHAR`

as messages an `IContextMenu3` provider may depend upon. ([Microsoft Learn][3])

This gives a good design principle:

> When dealing with an original/native provider, emulate the Windows host contract. When dealing with the synthetic Shell menu, expose only Shell's synthetic contract.

Currently those worlds are somewhat mixed.

For example, `ContextMenu::OnInitMenuPopup()` starts with:

```cpp
LRESULT ret = msg.invoke();
```

before constructing the synthetic popup.

That means the host can receive a `WM_INITMENUPOPUP` pertaining to Shell's synthetic `HMENU`, not necessarily one it originally created.

This is not proof of a bug, because some of the forwarding is necessary for owner-drawn provider compatibility. But it is exactly the kind of implicit behaviour that should become explicit.

I would create a small `NativeMenuMessageBridge` that owns decisions such as:

```text
Synthetic HMENU message?
    -> Shell handles it

Original borrowed HMENU message?
    -> forward with original HMENU / position / item data

Message required by native IContextMenu2/3 provider?
    -> preserve provider's expected parameters

Synthetic command?
    -> never leak synthetic ID to host
```

That will pay off repeatedly.

---

# 4. Fix the asymmetric `WM_INITMENUPOPUP` lifecycle

The repository has already made a good improvement here.

`NativePopupState` tracks:

* borrowed host `HMENU`,
* parent position,
* initialized,
* initializing,
* materialized,
* rules applied.

`initialize_native_popup()` correctly sends the original owner a documented `WM_INITMENUPOPUP` with the actual submenu position. Descendants are initialized lazily.

That is good takeover architecture.

The gap is the other end of the lifecycle.

I found no corresponding bridge that sends the **original borrowed native popup** a matching `WM_UNINITMENUPOPUP`.

Shell handles `WM_UNINITMENUPOPUP` for the synthetic menu and destroys its synthetic `HMENU`, but that is a different menu.

Microsoft's contract is explicit:

> If an application receives `WM_INITMENUPOPUP`, it will receive `WM_UNINITMENUPOPUP`. ([Microsoft Learn][4])

That matters because third-party menu handlers can allocate temporary menu state when their submenu opens.

### Small but important change

Extend `NativePopupState`:

```cpp
bool init_sent;
bool uninit_sent;
```

Then make the lifecycle exactly-once:

```text
first native materialization
    -> WM_INITMENUPOPUP(original HMENU)

synthetic counterpart closes/session terminates
    -> WM_UNINITMENUPOPUP(original HMENU)
```

This needs to happen even in the `LegacyEager` case, where descendants can have been initialized without ever being visibly opened.

It also needs its own bridge guard, analogous to `_native_notify`, so Shell does not interpret the original-menu cleanup notification as a synthetic popup that it owns and should destroy.

This is one of the highest-confidence correctness fixes in the entire takeover path.

---

# 5. First paint should become a hard architectural boundary

The current code already has `MenuPerf`, and it records useful phases such as:

* selection acquisition,
* native popup initialization,
* native materialization,
* native root scan,
* Explorer commands,
* modify rules,
* total pre-display.

That is a strong starting point.

But takeover should go further and adopt this invariant:

> **Nothing optional gets permission to stall the Explorer/menu UI thread before the first menu appears.**

Microsoft's own Shell guidance says not to perform resource-intensive operations or I/O on the UI thread, and explicitly calls out menu initialization/querying as paths that must remain conservative. ([Microsoft Learn][5])

There are currently several violations or near-violations.

---

## 5.1 Stop retrying `IExplorerCommand::GetState(TRUE)`

Current code:

```cpp
auto hr_state = cmd->GetState(selection, FALSE, &state);

if(hr_state == E_PENDING)
    hr_state = cmd->GetState(selection, TRUE, &state);
```

The first call is correct.

The second effectively says:

> “You told me this may make the UI unresponsive. Please do it synchronously anyway.”

Microsoft defines `FALSE` specifically as telling the provider not to perform intensive computation capable of stopping the UI thread; `E_PENDING` is the expected response. `TRUE` permits that expensive work. ([Microsoft Learn][6])

### Better takeover policy

Never issue that second synchronous `TRUE` call before first paint.

For `E_PENDING`, use a policy such as:

1. recent cached primitive state if available;
2. otherwise provisional enabled/visible state;
3. mark the provider as `state_pending`;
4. collect it in provider diagnostics.

I would rather occasionally expose a command whose state cannot be determined immediately than let one extension freeze every right-click.

The provider still gets a chance to validate during invocation.

---

# 6. Add provider health as a first-class takeover feature

This is probably the **highest-value genuinely new capability**.

Takeover has one unusual advantage over native Explorer:

**Shell sees and mediates the ecosystem.**

Use that.

There is already an embryonic version in `CoCreateInstanceHook`: holding Alt times relevant COM activations and logs the CLSID.

Turn that into a proper `ProviderHealth` subsystem.

For reconstructed `IExplorerCommand`s, collect separately:

```text
CLSID
activation time
GetState(FALSE) time/result
GetTitle time/result
GetFlags time/result
GetIcon time/result
EnumSubCommands time/result
failures
E_PENDING count
```

For native popup levels:

```text
borrowed HMENU
WM_INITMENUPOPUP duration
number of generated items
path/title of parent submenu
```

Classic `IContextMenu` attribution will not always be exact because by the time Shell captures the final `HMENU`, multiple handlers may have contributed to it. Be honest about that limitation. Modern `IExplorerCommand` attribution can be exact.

### Then use the information

Do not kill provider threads or attempt unsafe COM cancellation.

Instead use gradual degradation:

```text
Healthy provider
    -> full metadata

Repeatedly slow metadata provider
    -> no forced slow GetState
    -> skip optional icon work if necessary
    -> reuse cached metadata

Repeatedly failing provider
    -> omit it for current process/session
    -> report failure clearly
```

And expose diagnostics to the user:

```text
Context menu diagnostics

Adobe Acrobat
    Activation       2 ms
    State            1 ms

ExampleProvider
    Activation      18 ms
    State          >150 ms, E_PENDING
    Icon             4 ms

SomeExtension
    Init submenu   420 ms
```

An advanced option could generate a suppression rule for a problematic CLSID. The existing `CoCreateInstanceHook` CLSID filtering machinery already supplies much of the enforcement mechanism.

That is a powerful user-facing feature and very aligned with the product.

Instead of merely replacing Windows' context menu, Shell becomes capable of telling users **why their context menu is bad**.

---

# 7. Make diagnostics zero-cost before first paint

Current `MenuPerf` deliberately stays opt-in because the logger opens/appends/closes a file for each emitted line.

Keep the instrumentation, change the sink.

Have `TakeoverSession` collect tiny fixed-size timing records in memory:

```cpp
struct PhaseTiming {
    Phase id;
    uint32_t microseconds;
    uint32_t item_count;
};
```

Then flush after the menu closes.

That enables always-available lightweight performance data without putting file I/O on the thing being measured.

A ring buffer of the last 20 or 50 menus would be enough.

This creates the basis for provider diagnostics, regression reports and compatibility decisions without introducing a service or database.

---

# 8. Add a takeover circuit breaker and one-shot native bypass

Takeover is inherently more fragile than additive integration because Shell is assuming responsibility for foreign menus.

Therefore users should always have an escape route.

The existing outer hook already has a useful fail-open property: if Shell construction fails, it ultimately invokes the original menu.

Formalize it.

### Per-process circuit breaker

If the takeover layer itself experiences repeated failures for the same:

```text
host executable
Windows build
configuration generation
```

disable takeover for the remainder of that process and fall through to the native menu.

Do **not** persistently disable the application because of one crash or malformed extension.

A new Explorer process or new config generation can try again.

### One-shot bypass

Add a configurable modifier that means:

> Show this one menu completely untouched.

Implementation is trivial at the hook boundary:

```text
if bypass modifier:
    invoke original HMENU
    with original uFlags
    original coordinates
    original TPMPARAMS
    return original result
```

No selection acquisition, no parser evaluation, no reconstruction.

This is extremely useful for:

* incompatibility troubleshooting,
* accessing an extension Shell mishandles,
* diagnosing whether a problem belongs to Windows or Shell,
* recovering from a bad configuration.

It is low scope and dramatically improves the risk profile of takeover mode.

---

# 9. Last-known-good configuration should be mandatory

The snapshot machinery in `Initializer` is already almost set up for this.

`Initializer::init()` creates a new cache, parses into it, and only publishes `_snapshot` after successful parsing. That is good transactional behaviour.

But when parsing fails it sets:

```cpp
Status.Error = true;
```

and subsequent `query()` calls reject normal operation. `DllGetClassObject` also returns `CLASS_E_CLASSNOTAVAILABLE` when `has_error()` is true.

So the valid previous snapshot physically exists, but the runtime chooses not to use it.

That should change.

### Better states

```text
Loaded
    newest configuration is valid

StaleWithError
    previous valid configuration remains active
    newest attempted configuration has parser error

Disabled
    explicitly disabled by user
```

Then:

```text
edit shell.nss
      ↓
parse new snapshot
   ┌───────┴────────┐
 success          error
   │                │
atomic swap      retain old snapshot
   │                │
 new menu         report error
```

A typo should never make the shell disappear.

This is an enormous quality-of-life improvement for exactly the users most likely to exploit Shell's advanced NSS capabilities.

### Next small addition

Use `ReadDirectoryChangesW` or another narrow directory watcher to trigger configuration reloads outside the right-click critical path.

Then editing becomes:

> save file → valid configuration appears automatically

with automatic rollback to last-known-good on syntax errors.

No daemon is required.

---

# 10. Remove transient global Windows-setting mutations

This should happen even when takeover mode remains the goal.

The current menu reads and temporarily modifies:

* `SPI_SETMENUSHOWDELAY`,
* `SPI_SETSELECTIONFADE`.

The submenu delay is changed while a Shell menu is active and restored later, including `SPIF_SENDCHANGE`.

Microsoft documents `SystemParametersInfo` as retrieving or setting **system-wide parameters**, and `SPIF_SENDCHANGE` broadcasts `WM_SETTINGCHANGE` to top-level windows. ([Microsoft Learn][7])

That is the wrong isolation boundary for a context menu.

Two simultaneous applications or two Shell menus can observe/modify the same setting.

### Recommendation

Do not mutate either setting transiently.

For submenu delay, there are two defensible choices:

* obey the user's Windows menu delay;
* expose Shell's delay setting as an explicit system-setting change, clearly described as such.

Do not toggle the system around each popup.

For selection fade, I would let Windows manage its own effect rather than switching a system preference off and back on to permit a replacement animation.

That is a very favourable stability-to-effort trade.

---

# 11. Reconsider `fix_ugly_flicker()`

This one is particularly worth benchmarking.

The function:

* queries multimedia timer capabilities,
* calls `timeBeginPeriod`,
* looks at DWM composition timing,
* calculates proximity to the next refresh,
* calls `Sleep()`,
* calls `timeEndPeriod`.

And it is invoked from `WM_NCCALCSIZE`.

In other words, Shell deliberately sleeps the UI thread during popup window creation.

Microsoft notes that higher timer resolution can reduce system performance and power efficiency. ([Microsoft Learn][8])

Even if the sleep successfully hides an old rendering artefact, this is exactly the kind of historical workaround that should be experimentally revalidated against current Windows 10/11 DWM.

I would add a diagnostic flag and benchmark:

```text
flicker workaround ON
flicker workaround OFF
```

on current supported Windows builds.

If it is no longer demonstrably necessary, delete it.

If it is necessary only on a subset of builds, capability-gate it there.

I would not preserve a synchronous vblank sleep indefinitely merely because it once fixed something ugly.

---

# 12. Fix WinEvent reentrancy explicitly

`WinEventProc` currently reacts directly to popup `EVENT_OBJECT_CREATE` and `EVENT_OBJECT_SHOW`:

```text
CREATE -> OnMenuCreate
SHOW   -> OnMenuShow
```

Those functions do nontrivial window/subclass/theme work.

Microsoft explicitly warns that WinEvent callbacks can reenter while processing an earlier event, causing events to complete out of sequence unless the application guards against it. ([Microsoft Learn][9])

Add an idempotent per-popup state machine:

```text
Unknown
   ↓ CREATE
Created
   ↓ subclass complete
Prepared
   ↓ SHOW
Visible
   ↓ close
Closing
   ↓
Dead
```

Duplicate `CREATE`, duplicate `SHOW`, SHOW-during-CREATE and callbacks for a dead `HWND` become no-ops or carefully deferred transitions.

This naturally belongs inside the proposed `TakeoverSession`.

Small scope, disproportionate stability payoff.

---

# 13. A particularly valuable optimization: targeted `moveto` discovery

This is one of the best takeover-specific improvements I found.

The lazy native-menu code is substantially better than an unconditional recursive scan.

Normally:

```text
root initialized
    ↓
root enumerated

submenu A
    not touched until opened

submenu B
    not touched until opened
```

But `choose_native_tree_policy()` switches to `LegacyEager` if all of these apply:

```text
modify enabled
parent movement enabled
applicable moveto rule
location selector present
```

That is understandable because Shell needs descendant topology to move a deep command before the user opens its original parent.

But the result is severe:

> One deep `moveto` rule can force every native submenu to initialize before first paint.

### Add a third mode

```cpp
enum class NativeTreePolicy
{
    Lazy,
    TargetedDiscovery,
    LegacyEager
};
```

At config compilation time, classify move rules.

For a statically determinable exact path:

```text
location = 'Open with/Some provider'
```

Shell can traverse only:

```text
root
  ↓ initialize "Open with"
Open with
  ↓ initialize only required next ancestor
...
```

All unrelated native submenus stay untouched.

For expressions whose location is dynamic, wildcarded or otherwise impossible to resolve without seeing every node, fall back to `LegacyEager`.

So:

```text
exact deterministic move
    -> targeted discovery

dynamic/broad move
    -> eager compatibility fallback

no descendant movement
    -> normal lazy
```

That preserves the full power of `moveto` while removing one of its largest hidden performance costs.

This is much more valuable than micro-optimizing the rule loop.

---

# 14. Make Windows 11 packaged-command discovery stale-while-revalidate

`ExplorerCommand.cpp` currently has a 30-second process-local cache.

Once stale, `catalog_snapshot()` synchronously calls `scan_catalog()`.

That:

1. enumerates installed package registrations;
2. resolves install roots;
3. opens `AppxManifest.xml`;
4. reads files of up to 4 MB;
5. parses `FileExplorerContextMenus`;
6. builds the catalog.

And this is reached through the `explorer.commands` phase before the menu appears.

For takeover mode, the catalog reconstruction is necessary, but **synchronous freshness is not**.

Use:

```text
last known catalog
       │
       ├── return immediately to menu
       │
       └── refresh worker
              ↓
           publish new
           immutable snapshot
```

Even better, persist the last successful catalog across Explorer restarts.

A stale registration is relatively harmless:

* activation succeeds, use it;
* activation fails, skip it;
* background refresh fixes the snapshot.

That is much preferable to making first right-click after Explorer startup parse the packaged-app ecosystem.

No service is required.

---

# 15. Remove the synchronous Recycle Bin workaround from first paint

There is another smaller example in `ContextMenu.cpp`.

When the native Empty Recycle Bin item is disabled, Shell calls:

```cpp
SHQueryRecycleBinW(nullptr, &sqrbi);
```

before deciding its state.

Microsoft documents that an empty/null root can query Recycle Bins on all drives. ([Microsoft Learn][10])

That is exactly the kind of query I would not place on the right-click path.

Prefer the native item's state.

If a Windows-version workaround genuinely requires verification, maintain a cached asynchronous Recycle Bin state.

Small fix, correct philosophy.

---

# 16. Accessibility is a very high-value feature that is surprisingly cheap here

Shell already has good High Contrast handling.

The gap is owner-drawn menu accessibility.

Almost every final item is converted to owner-drawn:

```cpp
item->add_ownerdraw();
item->set_data(this);
```

I found a historical `MSAAMENUINFO` comment in `MenuItem.h`, but no actual `MSAAMENUINFO` implementation.

Microsoft provides this structure specifically so owner-drawn menus can expose their item names to accessibility clients **without implementing an entire custom `IAccessible` tree**. It must simply be the first member of the structure pointed to by `MENUITEMINFO::dwItemData`. ([Microsoft Learn][11])

So use:

```cpp
struct OwnerDrawItemData
{
    MSAAMENUINFO msaa;  // must be first
    MenuItemInfo* item;
};
```

Existing measurement/drawing code dereferences the second field instead of casting `dwItemData` directly to `MenuItemInfo*`.

That is not a giant accessibility project. It is essentially adapting the existing owner-draw metadata layout.

For a product whose entire interface is owner-drawn, the benefit is substantial.

---

# 17. Finish keyboard semantics, then add type-ahead

This is another good capability-to-scope ratio.

`MenuSubClassProc` currently contains:

```cpp
case WM_CHAR:
case WM_SYSCHAR:
case WM_MENUCHAR:
    break;
```

There is also commented-out `WM_MENUCHAR` handling elsewhere.

Microsoft explicitly defines `WM_MENUCHAR` so owners can select or execute menu items when mnemonic processing requires application help, using `MNC_SELECT` and `MNC_EXECUTE`. ([Microsoft Learn][12])

Shell already parses menu text and appears to retain shortcut/mnemonic information.

So complete it.

### Stage 1: normal mnemonics

Correctly handle:

```text
&Open
&Edit
E&xtract
```

including repeated mnemonic characters cycling between matches.

### Stage 2: type-ahead

If there is no explicit mnemonic match:

```text
user types "pow"
    ↓
select "PowerShell..."
```

Do not start with full live filtering or an embedded search editor. That becomes a much larger UI project inside an `HMENU`.

Simple prefix/type-ahead navigation is cheap, familiar, keyboard-friendly and does not disturb mouse behaviour.

This would be one of my first actual feature additions.

---

# 18. Smart overflow can be added using facilities Shell already uses

The current code sets `MENUINFO::cyMax`, which is good. Windows automatically supplies scrolling when the menu exceeds that height. Microsoft explicitly documents this behaviour. ([Microsoft Learn][2])

Shell also already understands `MFT_MENUBREAK` and `MFT_MENUBARBREAK` through NSS `column`, and Windows supports those flags for additional columns in popup menus. ([Microsoft Learn][13])

So an inexpensive enhancement is:

```text
overflow = scroll
overflow = columns
overflow = smart
```

`smart` could:

1. measure the menu normally;
2. if it fits vertically, do nothing;
3. if it would scroll, test available horizontal work area;
4. insert group-aware column breaks;
5. cap at perhaps 2 or 3 columns;
6. fall back to scroll when width is insufficient.

Prefer breaks after separators rather than arbitrary item counts.

The hard drawing/layout support is largely already present.

This could make huge menus much more usable without introducing a new UI toolkit.

---

# 19. Better asset caching is worthwhile, but secondary

`BitmapCache` is good as far as it goes:

* synchronized,
* bounded,
* ownership carefully managed,
* no destructive eviction while raw borrowers exist.

But it primarily caches rendered inline SVG content.

Other images still incur repeated work:

* image file loading,
* editable SVG files,
* `IExplorerCommand::GetIcon`,
* `SHDefExtractIconW` resource extraction.

Extend the cache key:

```text
resource/path
pixel size
DPI
file modification timestamp if applicable
```

For file-backed assets, the timestamp preserves live editing.

For resource specifications:

```text
C:\...\foo.dll,-123
```

the resource specification plus dimensions is sufficient for a process cache.

This removes optional decoration work from repeated menus and becomes particularly valuable with reconstructed packaged commands.

---

# 20. Use a small per-menu memo cache for expensive pure NSS queries

This is worth doing only after instrumentation identifies offenders.

`FuncExpression.cpp` contains functions that can hit:

* filesystem attributes,
* directory enumeration,
* property stores,
* package metadata,
* registry,
* process/window state.

A complex configuration can call the same pure operation repeatedly while evaluating dozens of menu entries.

`TakeoverSession` can provide a tiny memo store for pure queries:

```text
exists("C:\foo")              -> once
registry("...")               -> once
platform/build                -> once
process snapshot              -> once
package lookup                -> once
```

Do not attempt universal expression memoization.

Explicitly whitelist functions whose output is side-effect-free and semantically stable for the duration of one menu.

That keeps the change simple and predictable.

---

# 21. Large selections are a real performance risk, but I would defer the full fix

`Selections.cpp` eagerly turns selected Shell items into Shell's own item metadata.

For each item it can call:

* `GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING)`,
* `GetAttributes`,
* `GetDisplayName(SIGDN_FILESYSPATH)`,
* other display-name forms.

For a selection of hundreds of objects or remote namespace items, that can become expensive.

Microsoft's menu-handler guidance specifically warns that selections can be extremely large and advises implementations not to inspect every object merely to decide whether a verb should be present. ([Microsoft Learn][5])

A clean solution would be:

```text
SelectionContext
    count
    first item
    IShellItemArray
    lazy item metadata
```

Then materialize the complete selection only if the active NSS expressions actually need it or when invoking a command.

That is architecturally desirable, but it touches the expression runtime broadly enough that I would not put it in the first refactor wave.

Measure `selection.items.metadata` first.

---

# 22. Harden theme integration without abandoning custom rendering

Keeping takeover means custom rendering stays valuable.

I would not remove it.

But I would separate:

```text
required for functioning
vs.
optional fidelity enhancement
```

There are private UxTheme ordinal calls in the code.

Meanwhile several Windows 11 DWM features that used to require magic constants now have documented attributes, including:

* `DWMWA_USE_IMMERSIVE_DARK_MODE`,
* `DWMWA_WINDOW_CORNER_PREFERENCE`,
* `DWMWA_BORDER_COLOR`,
* `DWMWA_SYSTEMBACKDROP_TYPE`. ([Microsoft Learn][14])

Introduce a `WindowsCapabilities` object initialized once per process:

```text
documented DWM dark mode available
corner preference available
system backdrop available
private immersive-color functions available
WinEvent behaviour/version
hook mode in use
...
```

Then private APIs should improve visual fidelity only.

If one stops working after a Windows update:

```text
visual fallback
```

rather than:

```text
menu malfunction
```

That is exactly the kind of controlled private-API use a takeover architecture should aim for.

---

# 23. Host-specific compatibility should be explicit instead of accumulating conditionals

Shell supports more than Explorer, which is one of takeover mode's advantages.

But third-party file managers do not all use menus in precisely the same way.

I would add a small internal capability profile:

```cpp
struct HostProfile
{
    SelectionBackend selection;
    NativeTreePolicy native_policy;

    bool preserve_track_flags;
    bool bridge_native_lifecycle;
    bool allow_owner_subclass;
    bool allow_winevent_customization;
    bool allow_modern_command_merge;
};
```

Not hundreds of per-application hacks.

Have:

* a strict default,
* Explorer profile,
* taskbar profile,
* a handful of known compatibility overrides where testing proves they are needed,
* optional user override.

Crucially, record the selected profile in diagnostics.

That avoids ending up with another decade of:

```cpp
if (explorer) ...
else if (xyplorer) ...
else if (...)
```

scattered across `ContextMenu.cpp`.

---

# 24. Provider quarantine should exploit the existing CLSID suppression machinery

The current `CoCreateInstanceHook` already inspects activations of relevant:

* `IContextMenu`,
* `IContextMenu2`,
* `IContextMenu3`,
* `IExplorerCommand`

objects and can suppress configured CLSIDs with `E_NOINTERFACE`.

That is invasive machinery, but if takeover mode is explicitly the goal, it is also a useful capability.

Compile those suppression rules at configuration load.

Currently the hook can iterate `_cache->statics` and evaluate applicable rules during COM activation.

Instead publish something like:

```text
immutable exact CLSID suppression set
dynamic/context-sensitive rules separately
```

Then the overwhelmingly common case becomes:

```cpp
if (!possibly_relevant_clsid(clsid))
    return original();
```

That reduces the tax imposed on Explorer's process-wide `CoCreateInstance` hook.

And it provides a natural enforcement mechanism for the provider-health feature.

---

# 25. What I would build for testing before going much further

This is probably the most important engineering investment that does not itself ship as a user feature.

The current test suite is useful. It covers:

* lazy native menu state,
* Explorer commands,
* Shell extension capture,
* one-shot COM marshalling,
* taskbar state,
* bitmap cache,
* parser/configuration,
* threading,
* registration logic.

But I found no equivalent coverage for the **actual Win32 popup contract matrix**.

Build a small Windows host/probe test executable.

It should create menus and record exact messages:

```text
WM_INITMENU
WM_INITMENUPOPUP
WM_MENUSELECT
WM_MENUCHAR
WM_MEASUREITEM
WM_DRAWITEM
WM_COMMAND
WM_MENUCOMMAND
WM_UNINITMENUPOPUP
WM_EXITMENULOOP
```

Run each scenario twice:

```text
normal Windows menu
takeover menu
```

Then normalize HMENU/HWND values and compare traces.

Test:

| Dimension  | Cases                                         |
| ---------- | --------------------------------------------- |
| API        | `TrackPopupMenu`, `TrackPopupMenuEx`          |
| Return     | `TPM_RETURNCMD` on/off                        |
| Notify     | `TPM_NONOTIFY` on/off                         |
| Menu style | ordinary, `MNS_NOTIFYBYPOS`                   |
| Action     | mouse, keyboard mnemonic, Enter, cancel       |
| Item       | native, Shell custom, modern command          |
| Popup      | static, lazy submenu, nested lazy submenu     |
| Handler    | ordinary, owner-drawn/IContextMenu3-like      |
| Reentrancy | popup init generates additional menu activity |

For native items the goal should be:

> Aside from Shell's intentional visual/layout transformations, the external host observes equivalent menu semantics.

That harness will catch more takeover regressions than another thousand parser unit tests.

---

# 26. The best user-facing additions once the foundation is fixed

If I had a constrained feature budget, I would add these five.

## A. “Why is my context menu slow?”

Provider timing report with slow/failing extension identification.

This is uniquely suited to takeover and provides obvious everyday value.

## B. “Show original menu once”

Configurable bypass gesture.

Low complexity, huge recovery/debugging value.

## C. Safe live NSS editing

Last-known-good configuration plus automatic reload.

A malformed line no longer disables Shell.

## D. Real keyboard/type-ahead navigation

Mnemonics and prefix selection.

Makes large customized menus much faster to operate.

## E. Smart large-menu layout

Automatically use columns where helpful, scroll when not.

Almost all required rendering machinery already exists.

These five add real capabilities without creating a new application framework.

---

# 27. A second wave with unusually good payoff

After those, I would do:

### Targeted `moveto`

This may produce the largest performance improvement for sophisticated configurations because it eliminates unnecessary eager native-subtree initialization while preserving deep rearrangement.

### Modern-command stale cache

Keep reconstructed Windows 11 applications present without package scans on the immediate menu path.

### Better icon cache

Make repeated menus cheaper without changing semantics.

### Provider quarantine

One command to suppress a known-bad provider, preferably with an easy way to undo it.

### Compatibility profiles

Allow Shell to stay aggressive in Explorer while using a more conservative policy in a problematic third-party host.

---

# 28. Things I would deliberately not add yet

Several tempting ideas would expand scope disproportionately.

### Do not build a custom replacement menu framework from scratch

The current architecture still lets USER32 perform menu modality, hit-testing, submenu tracking, scrolling, keyboard navigation and much of the interaction model.

Keep exploiting that.

Replacing `#32768` entirely with custom windows would turn Shell into a UI framework project.

### Do not broker arbitrary in-process shell extensions out-of-process

It sounds attractive for crash isolation, but many old extensions implicitly expect Explorer/host context. Correctly virtualizing that environment would become an enormous compatibility undertaking.

Detect and quarantine bad providers instead.

### Do not make every NSS expression asynchronous

That contaminates the entire expression/evaluation model with futures, invalidation and dynamic UI updates.

Cache and defer known expensive operations instead.

### Do not rewrite the parser or custom foundation libraries now

Neither is the limiting factor in takeover stability.

### Do not expand taskbar interception to application buttons yet

The current blank-taskbar-area UIA adapter is relatively contained. Taking over button/Jump List interactions creates a much larger Windows-version compatibility surface.

---

# 29. The target takeover architecture I would aim for

Not a completely different product, just a cleaner organization around the current one:

```text
             TrackPopupMenu / Ex hook
                       │
                       ▼
              ┌─────────────────┐
              │ TakeoverSession │
              └────────┬────────┘
                       │
      ┌────────────────┼──────────────────┐
      │                │                  │
      ▼                ▼                  ▼
 HostContract    SelectionContext    Diagnostics
      │                │                  │
      │                ▼                  │
      │          NSS Rule Engine          │
      │                │                  │
      ▼                ▼                  ▼
 NativeMenuBridge -> CommandGraph <- ModernCommandCatalog
      │                │                  │
      │                ▼                  │
      │          Synthetic HMENU          │
      │                │                  │
      │          OwnerDrawRenderer        │
      │                │                  │
      └────────► CommandRouter ◄──────────┘
                       │
                       ▼
             complete host contract
```

And outside that:

```text
ProviderHealth
ConfigSnapshotService
WindowsCapabilities
TaskbarAdapter
```

The important part is not the class names.

It is the boundaries:

* Windows caller semantics are owned by one component.
* Borrowed native menu lifecycle is owned by one component.
* command IDs have explicit provenance.
* diagnostics are attached to a menu session.
* configuration remains transactional.
* foreign providers are treated as potentially slow/failing dependencies.
* renderer tricks cannot alter system-wide user settings.

---

# 30. Recommended implementation sequence

### Phase 1: compatibility foundation

Implement `TakeoverSession` without changing visible behaviour.

Move into it:

* original popup call parameters;
* popup HWND state;
* command-origin mapping;
* native popup lifecycle state;
* diagnostics records.

Add the Windows host-contract integration tests.

### Phase 2: remove known correctness hazards

Fix:

* `TPM_RETURNCMD` / notification translation;
* `MNS_NOTIFYBYPOS`;
* paired native `WM_UNINITMENUPOPUP`;
* WinEvent reentrancy;
* synthetic/native message distinction.

Stop changing global submenu delay and selection-fade settings.

Benchmark and remove/gate `fix_ugly_flicker()`.

### Phase 3: attack first-paint latency

* never retry `GetState(TRUE)`;
* stale-while-revalidate modern command catalog;
* remove synchronous all-drive Recycle Bin query;
* collect provider timings;
* flush diagnostics after the popup closes.

### Phase 4: make configuration practically unbreakable

* last-known-good snapshot;
* parse errors no longer disable the previous valid menu;
* automatic config watching/reload;
* explicit one-shot native bypass;
* takeover fault circuit breaker.

### Phase 5: improve functionality

* `MSAAMENUINFO`;
* proper `WM_MENUCHAR`;
* type-ahead;
* smart automatic columns;
* provider diagnostics/quarantine.

### Phase 6: optimize power-user configurations

* `TargetedDiscovery` for deterministic `moveto`;
* precompiled exact CLSID rules;
* resource/file icon cache;
* measured per-session NSS memoization;
* host compatibility profiles.

Only then would I tackle lazy full-selection metadata.

---

# My strongest recommendations

If only a handful of changes are going to happen, I would choose these:

**First, build the `TakeoverSession`/contract bridge and corresponding Windows trace harness.** The current architecture is powerful enough, but it needs to become much more exact about what Windows/hosts are supposed to observe. The TrackPopup return/notification semantics and missing native `WM_UNINITMENUPOPUP` are concrete reasons for doing this. Microsoft documents both contracts precisely. ([Microsoft Learn][1])

**Second, adopt an absolute “no optional slow work before first pixel” policy.** In particular, remove the `GetState(FALSE) -> E_PENDING -> GetState(TRUE)` retry, make packaged-command discovery stale-while-revalidate, and remove incidental filesystem/system queries. Microsoft's Shell guidance strongly supports this philosophy. ([Microsoft Learn][6])

**Third, turn performance visibility into a feature.** The existing `MenuPerf` and `CoCreateInstance` timing hooks mean the project is already halfway there. Provider health, diagnostics and optional quarantine would exploit takeover mode rather than apologizing for it.

**Fourth, make the config last-known-good and add an untouched-menu bypass.** Those two changes make an aggressive takeover product much safer to actually live with.

**Fifth, implement targeted `moveto` discovery.** This is the most interesting optimization specific to Shell's unusual feature set. It preserves functionality that native extension APIs simply cannot provide while avoiding the current all-tree penalty.

**Sixth, add `MSAAMENUINFO`, proper mnemonic handling and type-ahead.** They are modest changes with disproportionate usability benefits. Microsoft explicitly supports the owner-draw accessibility technique Shell needs. ([Microsoft Learn][11])

The resulting product would still be an aggressive Explorer context-menu takeover. It would simply be a substantially better one: **more faithful to the underlying Win32 contracts, more resistant to bad third-party extensions, easier to recover, faster in sophisticated configurations, accessible, and capable of explaining its own performance problems.**

That direction is, in my assessment, more valuable than merely making the current reconstruction loop faster. It turns takeover itself from a collection of necessary hooks and compatibility workarounds into a coherent platform that can safely support further features.

One evidence limitation remains: this review is grounded in the actual source plus Microsoft's current documentation, but I cannot execute the popup hooks against Explorer in this Linux environment. In particular, the precise message ordering needed for the `TPM_RETURNCMD` translation should be established with the proposed Windows trace harness rather than guessed from static analysis.

[1]: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenu "https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenu"
[2]: https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-menuinfo "https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-menuinfo"
[3]: https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-icontextmenu3-handlemenumsg2 "https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-icontextmenu3-handlemenumsg2"
[4]: https://learn.microsoft.com/en-us/windows/win32/menurc/wm-uninitmenupopup "https://learn.microsoft.com/en-us/windows/win32/menurc/wm-uninitmenupopup"
[5]: https://learn.microsoft.com/en-us/windows/win32/shell/verbs-best-practices "https://learn.microsoft.com/en-us/windows/win32/shell/verbs-best-practices"
[6]: https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getstate "https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getstate"
[7]: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfow "https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfow"
[8]: https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod "https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod"
[9]: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook "https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook"
[10]: https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shqueryrecyclebinw "https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shqueryrecyclebinw"
[11]: https://learn.microsoft.com/en-us/windows/win32/winauto/exposing-owner-drawn-menu-items?utm_source=chatgpt.com "Exposing Owner-Drawn Menu Items - Win32 apps | Microsoft Learn"
[12]: https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menuchar "WM_MENUCHAR message (Winuser.h) - Win32 apps | Microsoft Learn"
[13]: https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-menuiteminfow "https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-menuiteminfow"
[14]: https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute?utm_source=chatgpt.com "DWMWINDOWATTRIBUTE (dwmapi.h) - Win32 apps | Microsoft Learn"
