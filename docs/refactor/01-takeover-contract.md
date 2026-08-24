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

### 3a. What the harness measured — the flags do the opposite of the guess

The reasoning above reached the right rule from the wrong premise. The trace
harness (§06.2, `src/tests/hostprobe/`) was run against untouched Windows on
2026-08-24, Windows 11 26200.8875 x64, and the fixtures are committed under
`src/tests/hostprobe/fixtures/`:

| Flags | Owner gets `WM_COMMAND`? | Owner gets `ENTERMENULOOP`/`INITMENU`/`INITMENUPOPUP`/`UNINITMENUPOPUP`/`EXITMENULOOP`? |
| --- | --- | --- |
| neither | yes | yes |
| `TPM_RETURNCMD` | **no** | yes |
| `TPM_NONOTIFY` | **yes** | **no** |
| both | no | no |

`WM_MENUSELECT`, `WM_MEASUREITEM` and `WM_DRAWITEM` all survive `TPM_NONOTIFY`.

Three corrections follow, and each shrinks the work:

1. **It is `TPM_RETURNCMD`, not `TPM_NONOTIFY`, that stops the duplicate
   `WM_COMMAND`.** The open question this section left for the probe is
   answered: no duplicate. `TPM_NONOTIFY` is not needed by any host class, so
   the `HostProfile` opt-in for it can be dropped rather than built.
2. **`TPM_NONOTIFY` must never be added, and for a stronger reason than
   tidiness.** What it actually suppresses is the menu *lifecycle* — including
   `WM_INITMENUPOPUP`, the notification `NativeMenuBridge` (§5) exists to
   deliver to the host on time. Adding it would break the borrowed-menu
   contract this document is otherwise trying to repair.
3. **The claim that NONOTIFY "destroys the `WM_MENUSELECT` stream" was wrong.**
   `WM_MENUSELECT` survives it. So the proposal to *synthesise* `WM_MENUSELECT`
   was scope invented to solve a problem that does not exist, in either
   direction. It is dropped.

There is a fourth, about today's code rather than the plan. `Main.cpp:910`
**removes** `TPM_NONOTIFY` from the host's flags. Given the table above that
means a host which asked for a quiet menu is given the full lifecycle stream
anyway — a divergence in the opposite direction from the one this document
assumed. It is currently load-bearing (the bridge needs those notifications to
reach the host), so it stays, but it belongs in `HostProfile` (§8) as a stated
decision rather than an unexplained line.

And a fifth, which is a hard constraint on the replay design:
**`MNS_NOTIFYBYPOS` and `TPM_RETURNCMD` do not compose.** With both set the call
returns 1 rather than an identifier *and* no `WM_MENUCOMMAND` is sent — the
selection is simply lost (`question.notifybypos_with_returncmd.trace`). Shell's
own composed menu must therefore never carry `MNS_NOTIFYBYPOS`; it does not
today, and an invariant rule keeps it that way. Replay for a by-position host is
Shell sending `WM_MENUCOMMAND` itself, per the table below.

The replay table, rewritten against what the harness recorded:

```text
track(composed_menu, contract.flags_in | RETURNCMD, minus NONOTIFY)
  // RETURNCMD is added so the selection comes back as an identifier and no
  // WM_COMMAND carrying a synthetic ID is posted to the host (measured).
  // NONOTIFY is removed so the host still receives the menu lifecycle, which
  // is what NativeMenuBridge depends on (measured; today's behaviour).
  // The composed menu never carries MNS_NOTIFYBYPOS - with RETURNCMD the
  // selection would be lost entirely (measured).

selected = returned ID
origin   = origins.lookup(selected)      // Native | Custom | ExplorerCommand

complete_host_contract(origin):          // AFTER Shell's own tracking returns,
                                         // BEFORE the hook returns to the host
    Native          → RETURNCMD host: return original_wID, notify nothing.
                      NONOTIFY host:  return TRUE, notify nothing.
                      otherwise:      return TRUE and POST one notification -
                        PostMessage(owner, WM_COMMAND, MAKEWPARAM(original_wID, 0), 0),
                        or PostMessage(owner, WM_MENUCOMMAND, position, borrowed_HMENU)
                        when the borrowed root had MNS_NOTIFYBYPOS.
                      Posted, not sent: Windows posts it, and the host must not
                      see it before its own tracking call returns.
    Custom          → run CommandDispatcher now; RETURNCMD hosts get the synthetic ID
                      back (they asked for IDs), non-RETURNCMD hosts get TRUE only;
                      never notify — synthetic IDs must not reach a host under any flag
                      combination.
    ExplorerCommand → invoke (already in-place); return semantics as Custom
    Cancelled(0)    → propagate 0 / FALSE exactly as the real API would
```

**The defect this fixes is now measured, not inferred.** Because Shell adds no
`TPM_RETURNCMD` today, a non-`RETURNCMD` host's tracking of Shell's composed
menu behaves exactly like `select.plain.classic.trace`: the call returns 1 and
Windows posts `WM_COMMAND` to the host window carrying **whatever wID the chosen
item had** — for a custom item, a synthetic ID at or above `0x0fffffff`. Shell
then calls `InvokeCommand(1)`, which matches nothing. So the user's command does
not run *and* a meaningless command reaches the host. Adding `TPM_RETURNCMD`
closes both halves at once.

Delivery-ordering rule (QA-03) — **reversed by measurement.** This section
originally required native replay to be *synchronous, inside the hook*, reading
TrackPopupMenuEx's "The window does not receive a `WM_COMMAND` message from the
menu until the function returns" as placing delivery before the return. It says
the opposite: the owner does not receive it *until* the call returns.

The harness confirms the reading. In `select.plain.classic.trace` and
`question.notifybypos_reports_a_position.trace` the `WM_COMMAND` and
`WM_MENUCOMMAND` are both caught by a `PeekMessage` drain that runs *after*
`TrackPopupMenu` returned — so Windows **posts** them — and both appear after
`WM_EXITMENULOOP`.

So the rule is inverted: **replay is posted, not sent.** A synchronous
`SendMessage` from inside the hook would deliver the notification before the
host's own tracking call returned, which is a sequence untouched Windows never
produces. The dangling-`HMENU` concern behind the old rule is real but is
Windows' own behaviour, not something Shell introduces by matching it; a host
that destroys a by-position menu before pumping is already broken against the
real API.

Rules:

- Preserve alignment/animation/layout flags the host passed (`SM_MENUDROPALIGNMENT`
  handling per TrackPopupMenu remarks); stop force-stripping `TPM_HORIZONTAL`
  unconditionally — make it a `HostProfile` decision (§8).
- **`TPM_NONOTIFY` is never added, and the removal of it stays.** The whole
  question is settled by §3a's table; no synthesised `WM_MENUSELECT` is needed,
  because `WM_MENUSELECT` was never the thing NONOTIFY suppressed. Which
  messages it *does* suppress, and that `WM_MEASUREITEM`/`WM_DRAWITEM` are
  exempt, is recorded in `src/tests/hostprobe/fixtures/README.md` and pinned by
  `select.nonotify.*` and `question.nonotify_still_measures_ownerdraw`.
- `MNS_NOTIFYBYPOS` is a header style (MENUINFO page: "no effect when applied to
  individual sub menus") — detect via `GetMenuInfo(dwStyle)` on the borrowed root;
  replay with `WM_MENUCOMMAND` `(wParam = item position, lParam = HMENU)` instead of
  `WM_COMMAND`. Measured payload and timing:
  `question.notifybypos_reports_a_position.trace`.

**Probe gate — met.** The ordering questions this section could not answer from
documentation (post versus send, the `WM_MENUSELECT` sequence, `WM_EXITMENULOOP`
timing, the NONOTIFY suppression set, and whether a duplicate `WM_COMMAND`
survives `TPM_RETURNCMD`) were recorded by the harness on 2026-08-24 and are
committed as fixtures. Phase 2.3 is unblocked; the replay code freezes against
those traces, and `hostprobe.exe --verify` is what says a later Windows changed
its mind.

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

### 7a. As implemented (2026-08-24) — the non-interference proof needed a different shape

Both landed: `Include/TakeoverGesture.h` and `Include/TakeoverBreaker.h`.

QA-04 asks for a *proof* that bypass and reload can never both fire from one
click, and it was scheduled as a harness case. That could not have proved it.
The two gestures were to be evaluated from two independent reads of the live
keyboard, microseconds apart in the same hook body, and no test can establish a
property about two separate reads of global state — a passing harness run would
only have shown the user did not release a key that particular time.

So the rules became a pure function of one snapshot. `OnState`'s condition nest
moved into `classify_gesture(GestureState)`, `OnState` acts on the result, and
the hook classifies **once** and passes the same value to both. Non-interference
is then structural — a function returns one value — and
`test_takeover_gesture.cpp` walks every reachable combination of the four
modifiers at every plausible held-count to assert it. Harness probe 4 is
therefore satisfied without a deployed build, which is the better outcome: it is
now a property of the code rather than an observation about one run.

The bypass itself is four lines at the top of the hook, and reuses machinery
that was already there: `__leave` hands control to the `__finally`, whose
fail-open call tracks the host's own menu with the host's own flags — which is
exactly what a bypass is. The `__finally` no longer overwrites the recorded
decision, so `BypassOnce` and `Degraded` survive into the ring instead of every
non-takeover being reported as `FailOpen`.

The breaker counts a failure only when the click got as far as *trying* to build
Shell's menu. An unregistered process, a disabled shell, a bypass gesture and an
already-open breaker all reach the same fallback deliberately, and counting
those would open the breaker on a machine where nothing is wrong.

**That list was incomplete, and the gap was a real defect (found 2026-08-24).**
It stops at the checks the hook itself makes and misses the ones inside
`ContextMenu::Initialize`, which refuses for several reasons that are equally
deliberate: `QueryShellWindow` does not recognise the window, the configuration
hides the menu in this context, or no generation is currently being served.
All three returned the same `nullptr` as a genuine failure, so all three counted.

Three of them is the threshold. A host that is not Explorer raises plenty of
popups Shell does not handle — its own toolbar and tree menus — so **three of a
file manager's internal popups switched takeover off for the rest of that
process**, including for the file context menus Shell handles perfectly well.
The recoverable cases are worse still: a configuration error that the watcher
would have fixed on the next save had already opened the breaker permanently.

Found by running the trace harness through the hook. Twenty-three plain popups
run first, Shell declines every one of them, and the shell-namespace scenario
that follows was handed straight back to the host — while passing when run on
its own. `Initialize` now records *why* it refused (`init_declined`,
`Include/ContextMenu.h`), the hook counts only real failures, and the ring gets
a `Declined` decision of its own rather than reporting these as `FailOpen`.

The ordering in `Scenarios.cpp` is load-bearing because of this: the takeover
scenarios must stay *after* the declining ones and in the same process, or the
regression walks back in unnoticed.

One implementation trap, and it is the one `AGENTS.md` already names: the first
version logged the breaker opening directly in the `__finally`, and
`Logger::write` is a variadic template whose `string::Argument` temporaries
require unwinding — C2712, in a function that has to stay plain-old-data. The
log call lives in its own function now, the same shape as
`menu_perf_begin`/`menu_perf_end`.

Not verified here: any of it inside a real host. What is verified is the
decision table, the state machine, and that both survive their defects being
re-introduced — a cumulative rather than consecutive count, a `store` instead of
the `exchange` that tells exactly one thread it opened the breaker, and a bypass
combination that collides with reload.

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

### 9a. As implemented (2026-08-24) — the fast path landed; conditional attach did not, and here is why

`Include/ComActivationPolicy.h` gains the compiled policy;
`Initializer::compile_com_policy` builds it from `cache->statics` at every
publish and swaps it into a process-wide `shared_ptr<const>`;
`CoCreateInstanceHook` reads it lock-free and returns to the original
immediately when no rule can name the CLSID.

What that removes is real. The detour sees **every** in-process and
local-server activation in the host, and for each one whose IID was one of the
four it cared about it built a `Context`, called `Guid::to_string` — an
allocation — and walked the rule list evaluating `where` expressions. On the
stock configuration, which names no CLSID at all, every one of those was
wasted. The replacement is sixteen bytes `memcpy`'d and compared against a
vector that is usually empty.

Two rules the tests pin, because the direction of a mistake here is asymmetric.
Answering `may_affect` true for a CLSID no rule names costs the old slow path
and nothing else; answering **false** for one a rule does name silently
disables the blocklist — the same class of defect as §04.9's Alt-held bypass,
which also failed silently and also let a suppressed extension quietly
reappear. And a policy with no CLSIDs is *not* empty if the Win11 priority rule
is on: that rule matches a Windows CLSID rather than a configured one, so it
needs the hook by itself.

The timing probe is deliberately **not** gated by `may_affect`. It exists to
time every activation and find the slow one, so narrowing it to the CLSIDs a
blocklist already names would destroy the diagnostic while looking like an
optimisation.

**Conditional attach is deferred, and the reason is worth recording.** Two
things came out of looking at it properly:

1. **The blast radius is already smaller than this section assumes.** The
   detour is installed inside `if(rt.loader.explorer)` — third-party hosts
   never get it. "Attach only when needed" therefore buys the case of Explorer
   with an empty policy, not the process-wide exposure the section describes.
2. **The policy does not exist yet when the decision would be made.**
   `BootstrapOnce` installs the detour before any configuration has been
   parsed — `Initializer::init(HINSTANCE)` sets up paths; the parse happens
   later, in `DllGetClassObject`. Deciding at bootstrap would mean skipping the
   hook and then needing to install it later, from whichever thread published
   the configuration, which now includes the config watcher's. Installing an
   inline detour from a background thread while menus are open is a real risk
   on a machine that cannot test it, taken in exchange for the item in (1).

Not worth it in that trade. Revisit if the ring ever shows the empty-policy
Explorer case mattering, or alongside the `TakeoverRouter` work, where the
decision has somewhere natural to live.

### 9b. The router de-dup, and `priority` on a `-treat` machine - settled by measurement (2026-08-24)

§9's last bullet - "TreatAs is authoritative when healthy; CoCI override only as
fallback, never both by default" - is now answered, and the answer is not the
one the bullet implies.

**Both mechanisms are live on a `-treat` machine, and they reach the same
outcome by different routes.**

- `CoCreateInstanceHook` returns `E_NOINTERFACE` for
  `IID_FileExplorerContextMenu` whenever `settings.priority` evaluates truthy.
  Explorer cannot create the modern menu and falls back to the classic one,
  which Shell then takes over.
- The `TreatAs` redirect on `{86ca1aa0-...}` makes COM substitute Shell's own
  CLSID. `DllGetClassObject` accepts it - `rclsid` *is* `IID_ContextMenu` - and
  hands back a class factory whose object does not implement whatever interface
  the modern menu asked for, so the activation fails there instead. Same
  outcome, one COM round trip and a DLL load later.

#### What the four experiments showed

Run on a real machine by editing the installed `shell.nss` under elevation,
restarting Explorer, and reading back which window class the desktop
right-click produced. Windows 11 26200.8875 x64:

| TreatAs | `priority` | menu |
| --- | --- | --- |
| ours | 1 | classic - `#32768`, Shell's |
| ours | 0 | **classic - Shell's.** The setting is inert |
| absent | 0 | modern - `Microsoft.UI.Content.PopupWindowSiteBridge` |
| absent | 1 | classic - Shell's |

So `priority` does exactly what it says when there is no redirect, and nothing
at all when there is one.

#### Why it cannot be fixed by making the setting authoritative

Three things close that door, and it is worth recording them so the next reader
does not re-derive them:

1. **COM does not fall back to the original class when a `TreatAs` substitute
   fails.** Refusing the redirected activation from `DllGetClassObject` lands on
   the classic menu too - the same outcome, for a different reason.
2. **There is no per-call opt-out of `TreatAs`.** `CoCreateInstance` honours the
   registry; `CoTreatAsClass` would have to *write* to it.
3. **Removing the redirect is machine-wide HKLM state and an elevated act**, not
   something a configuration file read by every host process gets to do.

On a machine registered with `-treat` the modern menu is gone until somebody
runs `shell.exe -unregister -treat`. That is the honest answer rather than a
bug in the setting, and making `TreatAs` "authoritative when healthy" would be
the same behaviour with a different name.

#### What was changed

**The waste.** When the redirect is ours the answer is already decided, so
`CoCreateInstanceHook` no longer builds a `Context` and evaluates an expression
on every `IID_FileExplorerContextMenu` activation to reach it. That is the
"router de-dup" this section was about, and it is behaviour-preserving by the
table above. `RegistryConfig::ModernMenuRedirectedToUsCached()` reads HKCR -
the merged view COM itself resolves against, so a per-user redirect shadowing
the machine one is seen - on the same two-second terms as `IsRegisteredCached`.

**The silence.** `shell.exe -check` now says when `priority = 0` will do
nothing, which is where somebody who has just written it will look. A note
rather than an error, and the exit code stays 0: the configuration is valid.
`ConfigCheckResult` gained one field at the end for it, which is what its
`cbSize` at offset 0 has always been for.

`priority` is an *expression*, so a validator that runs on a file can only
speak for the cases where it is a constant; anything else is reported as
dynamic rather than guessed at.

#### One trap found on the way

`Object` declares a non-template `explicit operator bool` meaning *not null*,
and separately a template conversion for numeric types meaning *not zero*.
Being non-template, the explicit operator wins `static_cast<bool>` - so a
numeric zero casts to **true**, and `priority = 0` read that way reports as
switched on. The hook next door is correct by accident, because it assigns to a
`bool`, where the explicit operator is not a candidate. Use `to_bool()`;
`test_expression.an_objects_truth_is_to_bool_not_a_cast` fails if that is ever
"simplified" back into a cast.

## 10. Acceptance criteria for this doc

- [ ] Every hook exit maps to a logged `TakeoverDecision`.
- [ ] Trace harness shows host-observable message equivalence for native items across
      all four `TPM_*` combinations (§06.2 matrix).
- [ ] INIT/UNINIT pairing asserted exactly-once by unit tests in all three policies.
- [ ] Breaker trips only on repeated takeover failures; bypass gesture works with
      Shell fully disabled otherwise.
- [ ] No behavioral change visible in default Explorer flows except bug fixes above.
