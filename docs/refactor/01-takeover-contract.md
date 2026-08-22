# 01 — Takeover contract: session, host translation, native bridge, interception

Goal: make takeover an explicit **translation layer between two contracts** (Audit 2 §1)
instead of scattered flags and ad-hoc guards. Nothing here changes what the user sees;
everything changes what Windows/hosts can rely on.

---

## 1. `TakeoverSession` (new, `src/dll/src/Takeover/TakeoverSession.h`)

One instance per intercepted popup call. It *consolidates* state that today lives in
`ContextMenu` fields, hook globals and window properties; it does not replace
`ContextMenu`, which becomes its client (strangler step 1).

```cpp
struct HostContract {
    HMENU   hmenu_original;      // borrowed host menu (nullptr for taskbar-synthetic)
    HWND    owner;
    UINT    msg_kind;            // TrackPopupMenu(0) vs TrackPopupMenuEx(1)
    uint32_t flags_in;           // verbatim uFlags from host
    TPMPARAMS tpmparams{}; bool has_tpmparams{};
    int  x{}, y{};
    bool return_cmd{};           // TPM_RETURNCMD in flags_in
    bool no_notify{};            // TPM_NONOTIFY in flags_in
};

class TakeoverSession {
    HostContract         contract;
    SelectionContext     selection;        // existing Selections + capture result
    NativeMenuBridge     bridge;           // §3 below
    CommandOriginTable   origins;          // displayed wID -> origin record (§4)
    SessionDiagnostics   diag;             // ring-buffer records, §02.6
    PopupLifecycle       lifecycle;        // WinEvent state machine, §5
    // ownership: created in NtUserTrackPopupMenu hook body before any Shell work,
    // destroyed after contract completion in InvokeCommand tail.
};
```

Insertion points: constructed at `Main.cpp:897` where `ContextMenu::CreateAndInitialize`
is called today; threaded through via `ContextMenu` ctor param or a session pointer
stored alongside (`ContextMenu::Prop` already exists). The existing static
`Processes/HookMap/point` state moves into session/process services as seams are cut
(§04.5).

## 2. Fail-open is retained and made explicit

Today's fallback (`Main.cpp:1013-1028`: `__finally { ShellExtCapture::clear(hMenu);
invoke(hMenu, uFlags, {x,y}); ... }`) is the single most valuable safety property.
Formalize:

```cpp
enum class TakeoverDecision { TakeOver, BypassOnce, Degraded, FailOpen };
```

Every exit path of the hook must map to one decision, recorded in
`SessionDiagnostics.fallback_reason`. The circuit breaker (§7) flips future decisions.

## 3. `HostContract` normalization — fixing the `TPM_*` fidelity gap

**Verified problem.** The hook mutates incoming flags: removes `TPM_NONOTIFY`
(`Main.cpp:905`), forces `TPM_VERTICAL` (`:974-975`), never adds `TPM_RETURNCMD`.
Documented contract (TrackPopupMenu page, fetched): with `TPM_RETURNCMD` the return
value *is* the selected ID; without it the function returns success/failure and the
owner receives command notification. Consequences in tree:

- Custom items carry synthetic IDs ≥ `0x0fffffff` (`ContextMenu.h:457-458`).
  For a non-`RETURNCMD` host, tracking succeeds → returns `TRUE`(1) →
  `ctx->InvokeCommand(1)` matches nothing (`ContextMenu.h:466-469`
  `ident.equals` fails) → **the user's chosen custom command silently does not run**,
  while a synthetic `WM_COMMAND(0x0fffffff…)` may be posted to the foreign owner.
- Native mirrored items keep their original IDs, so they happen to survive.
- `selectid` captured during `MN_BUTTONUP` (`ContextMenu.cpp:6538-6539`) is unused
  (call commented out at `:2821`); keyboard activation bypasses that path entirely.

**Design (amended by §07 A4).** Internally track Shell's composed menu with
**`TPM_RETURNCMD` alone**. `TPM_NONOTIFY` is *not* added by default.

Rationale, from the TrackPopupMenuEx page
(<https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenuex>):

- The requirement is only that exactly one component observes the selection.
  `TPM_RETURNCMD` alone satisfies it: "the return value is the menu-item identifier
  of the item that the user selected".
- The page groups both flags as "flags to control discovery of the user selection
  **without having to set up a parent window** for the menu". Shell always has a
  parent window — it is the host's. Nothing documents NONOTIFY as *required*
  alongside RETURNCMD, and nothing enumerates which messages NONOTIFY suppresses
  ("does not send notification messages", unenumerated) — which is precisely why
  the original rule needed a probe to discover its own consequences (QA-02).
- Forcing NONOTIFY destroys the `WM_MENUSELECT` stream that third-party hosts use
  for status-bar hints, and the original design then proposed *synthesising*
  `WM_MENUSELECT` to repair that. That is scope created by the default, not by the
  requirement.

So: add `TPM_RETURNCMD`; leave the host's other notification behaviour alone; and
let the §06.2 probe answer the one open question — whether a duplicate `WM_COMMAND`
still reaches the owner when `TPM_RETURNCMD` is set. Only if it does is
`TPM_NONOTIFY` added, and then per `HostProfile` (§8) rather than globally. This
makes the probe a confirmation rather than a prerequisite, and shrinks the diff.

The replay table below is unchanged; only the flags Shell adds have changed:

```text
track(composed_menu, RETURNCMD|NONOTIFY|alignment-preserving-subset)
selected = returned ID
origin = origins.lookup(selected)        // Native | Custom | ExplorerCommand
complete_host_contract(origin):          // runs BEFORE the hook returns
    Native          → RETURNCMD host: return original_wID
                      else if !contract.no_notify: deliver SYNCHRONOUSLY —
                        SendMessage(owner, WM_COMMAND, MAKEWPARAM(original_wID, 0), 0),
                        or WM_MENUCOMMAND (wParam = position, lParam = borrowed HMENU)
                        when MNS_NOTIFYBYPOS was set.
                      else (NONOTIFY host): deliver nothing; return FALSE/0 as the real API would.
    Custom          → run CommandDispatcher now; RETURNCMD hosts get the synthetic ID
                      back (they asked for IDs), non-RETURNCMD hosts get TRUE/FALSE only;
                      never notify — synthetic IDs must not reach a host under any flag
                      combination.
    ExplorerCommand → invoke (already in-place); return semantics as Custom
    Cancelled(0)    → propagate 0 / FALSE exactly as the real API would
```

Delivery-ordering rule (QA-03): native replay is **synchronous, inside the hook**, for
two documented reasons. First, TrackPopupMenuEx's contract places command delivery
"until the function returns" — posting would reorder the observable sequence. Second,
`WM_MENUCOMMAND` carries `lParam = HMENU`; a posted copy is processed after the host's
call site has typically run `DestroyMenu`, handing it a dangling handle. Posting either
message is forbidden unless the §06.2 harness proves delivery-before-destroy for that
host class.

Rules:

- Preserve alignment/animation/layout flags the host passed (`SM_MENUDROPALIGNMENT`
  handling per TrackPopupMenu remarks); stop force-stripping `TPM_HORIZONTAL`
  unconditionally — make it a `HostProfile` decision (§8).
- ~~Forcing internal `TPM_NONOTIFY`~~ — **superseded by the amendment above (§07 A4):
  NONOTIFY is no longer added by default.** The analysis below stands as the reason
  it must not be, and as the specification for the probe that decides whether any
  host class needs it. Today's code *strips* NONOTIFY (`Main.cpp:905`), so hosts currently receive
  notification traffic during tracking — including `WM_MENUSELECT` highlight updates
  ("Sent to a menu's owner window when the user selects a menu item") carrying original
  wIDs for mirrored natives. The TrackPopupMenuEx page documents only "does not send
  notification messages" without enumerating survivors; whether `WM_MEASUREITEM`/
  `WM_DRAWITEM` are exempt is **undocumented** and must be established by the §06.2
  probe before this rule freezes. Where a suppressed stream is needed, the bridge
  synthesizes it from tracked selection state (forwarded `WM_MENUSELECT` with original
  wIDs for natives; nothing for custom items); otherwise the equivalence criterion in
  §01.10 holds modulo that synthesis.
- `MNS_NOTIFYBYPOS` is a header style (MENUINFO page: "no effect when applied to
  individual sub menus") — detect via `GetMenuInfo(dwStyle)` on the borrowed root;
  replay with `WM_MENUCOMMAND` `(wParam = item position, lParam = HMENU)` instead of
  `WM_COMMAND`.

**Probe gate.** Exact message ordering (post vs send, `WM_MENUSELECT` sequence,
`WM_EXITMENULOOP` timing) must be established by the trace harness against untouched
`TrackPopupMenu` before the replay code freezes (§06.2). Static analysis alone is not
evidence here — matching Audit 2 §2's own caveat.

## 4. Command-origin table

```cpp
struct CommandOrigin {
    enum Kind { Native, Custom, ExplorerCommand } kind;
    uint32_t original_id;        // native wID (identity preserved today) or 0
    uint32_t position;           // for MNS_NOTIFYBYPOS replay
    IExplorerCommand* cmd;       // ref'd, retained past Uninitialize (existing pattern,
                                 // MenuItemInfo::retain_explorer_command, MenuItem.h:216-225)
    CommandProperty* custom;     // NSS command list owner
};
```

`origins` replaces today's three parallel vectors `_items_command/_items_popup/
_main_popup` + linear `get_item` scan (`ContextMenu.cpp:161-174`) as the seam is cut;
initially it can be populated inside existing `prepare_*` paths with zero behavior change.

## 5. `NativeMenuBridge` — borrowed-HMENU lifecycle, made symmetric

**Verified gap.** Shell sends borrowed popups a correct just-in-time
`WM_INITMENUPOPUP` (`NativeMenuLazy.h:108-132`, `MAKELPARAM(position, FALSE)`),
but the close path (`OnUninitMenuPopup`, `ContextMenu.cpp:1581-1607`) destroys only the
synthetic menu; no `WM_UNINITMENUPOPUP` is ever forwarded to the borrowed popup whose
owner initialized state on open. Microsoft: "If an application receives
WM_INITMENUPOPUP, it will receive WM_UNINITMENUPOPUP" (fetched page).

Design:

```cpp
struct NativePopupState {           // extends NativeMenuLazy.h:63-81
    // ...existing handle/parent_position/initialized/initializing/materialized/rules_applied
    bool init_sent{};
    bool uninit_sent{};
    bool opened_by_user{};          // synthetic counterpart was actually shown
};
```

Exactly-once pairing rules:

1. INIT is sent once per popup, at first materialization (unchanged behavior).
2. UNINIT is sent once, when the session ends **or** the synthetic counterpart closes,
   whichever comes first, for every popup whose INIT was sent — including
   `LegacyEager` descendants that were initialized but never shown. **Stated
   divergence (QA-12):** for never-shown eager descendants this delivers an UNINIT the
   host would never have received from untouched Windows (native tracking initializes
   nothing for unopened popups). It is state-cleanup courtesy, not contract emulation;
   it is not default policy (`LegacyEager` is opt-in), and harness scenario
   "UNINIT for a popup the host never really tracked" must pass before shipping it.
3. The bridge guard that today routes Shell's own synthetic notifications
   (`_native_notify`, `ContextMenu.h:786-789`) gains a second token so outgoing UNINIT
   for a borrowed HMENU is not mistaken for one of Shell's popups (mirror of the
   existing pass-through trick).
4. Send order relative to `DestroyMenu(synthetic)`: synthetic teardown first, then
   borrowed UNINIT, then borrowed handles are dropped (never destroyed — host owns
   them, comment already in `NativePopupState`). Ordering confirmed by trace harness.

Tests: extend `test_native_menu_lazy.cpp` (real owner window already drives INIT there)
asserting UNINIT arrives exactly once per INIT across lazy, eager, and
user-never-opens-subtree scenarios.

## 6. WinEvent popup lifecycle state machine

`WinEventProc` reacts directly to CREATE/SHOW today; SetWinEventHook documents that
callbacks can reenter and complete out of order (canonical URL in master plan §2 row
"WinEvent"). Fold into session:

```text
Unknown --CREATE--> Created --subclass done--> Prepared --SHOW--> Visible --close--> Closing --> Dead
```

Duplicate CREATE/SHOW, SHOW-before-prepared, events after Dead become no-ops or one
deferred transition. Implementation: per-HWND state in `TakeoverSession.lifecycle`
replacing the `is_prop(UxSubclass)`/`_map[hWnd]` checks in `OnMenuCreate`
(`ContextMenu.cpp:5487`) and `WinEventProc` (`:6715`). Small, testable pure-logic core
(`PopupLifecycle` transitions) → new unit suite.

## 7. Circuit breaker and one-shot bypass

Per-process breaker keyed on `(host exe, windows build, config generation)`:

```text
consecutive_failures >= 3  → mode = Core (no takeover) for process lifetime
                            → diagnostics entry + log line
success resets counter; never persisted across processes (A1§18 conservatism rule:
crash correlation uses session markers, not "explorer died ⇒ shell did it")
```

Modes (A2§8): Normal / Degraded (no ExplorerCommand enrichment, no taskbar takeover,
reduced effects) / Core (native+NSS only) / Bypass.

One-shot bypass (trivial at hook top, before any Shell work):

```cpp
if(bypass_gesture_active(contract)) {          // configurable modifier, default Ctrl+Alt+right-click
    return original_invoke(contract);          // untouched menu, untouched flags
    return original_invoke(contract);          // untouched menu, untouched flags
}
```

Gesture read via existing `Keyboard` helper; documented in README; also exposed as a
menu item ("Windows menu" placeholder item type) — see §05.2.

**Gesture choice is constrained (QA-04):** the obvious Ctrl+Shift+right-click is
already bound — `Initializer.cpp:840-845` treats Shift+Ctrl as config-reload, and those
combos are evaluated by `Initializer::OnState` inside this very hook body
(`Main.cpp:883`) *before* takeover work runs. The default is therefore
`Ctrl+Alt+right-click`; `OnState` combos keep precedence, and a harness case asserts
bypass and reload gestures can never both fire from one click.

## 8. `HostProfile` compatibility profiles (small, data-driven)

```cpp
struct HostProfile {
    bool preserve_track_flags;     // don't force VERTICAL etc.
    bool allow_owner_subclass;     // WindowSubclassProc attach
    bool allow_winevent_customization;
    bool allow_modern_command_merge;
    NativeTreePolicy native_policy;
};
```

Defaults: strict; Explorer profile enables full set; unknown third-party hosts get
strict until proven. Selected profile recorded in diagnostics. Prevents the next decade
of `if(explorer)…` scattering (A2§23). Storage: static table + optional user override
key under `HKCU\SOFTWARE\Nilesoft\Shell\hosts`.

## 9. Interception backend abstraction (R2)

Wrap today's two mechanisms behind one interface (A1§6):

```cpp
struct PopupInterceptionBackend {
    virtual bool install()  = 0;
    virtual bool healthy() const = 0;      // probe: thunk still points at detour
    virtual void uninstall() = 0;
};
// Win32uIatBackend        — current user32→win32u NtUserTrackPopupMenuEx patch (Main.cpp:1333-1335)
// PerModuleIatFallback    — current enumerate-all-modules patch (Main.cpp:1341-1358), demoted to fallback
// PublicTrackPopupDetour  — EXPERIMENT: Detours on documented TrackPopupMenu(Ex); coverage-compared
```

Runtime selects one primary; health check runs on each hook entry (cheap thunk compare,
pattern already in `IATHook::installed()`, `Hooker.h:270-273`); unhealthy ⇒ fail-open
decision + diagnostics. The public-detour experiment ships behind a registry flag and
is decided by measured coverage (trace harness matrix), not aesthetics — keeping A1§6's
warning that the private route may be load-bearing.

`CoCreateInstanceHook` becomes policy-driven (see §01.6 in master backlog; details):

- At config publish time compile `ComActivationPolicy{ exact_blocked_clsid_set,
  conditional_groups }` from the same `statics` rules currently evaluated per
  activation (`Main.cpp:776-796`);
- fast path: `if(!policy->may_affect(rclsid)) return original(...);`
- attach the detour only when policy is non-empty or the Win11-priority feature is on;
- modern-menu suppression moves under `TakeoverRouter`: TreatAs is authoritative when
  healthy; CoCI override only as fallback, never both by default.

## 10. Acceptance criteria for this doc

- [ ] Every hook exit maps to a logged `TakeoverDecision`.
- [ ] Trace harness shows host-observable message equivalence for native items across
      all four `TPM_*` combinations (§06.2 matrix).
- [ ] INIT/UNINIT pairing asserted exactly-once by unit tests in all three policies.
- [ ] Breaker trips only on repeated takeover failures; bypass gesture works with
      Shell fully disabled otherwise.
- [ ] No behavioral change visible in default Explorer flows except bug fixes above.
