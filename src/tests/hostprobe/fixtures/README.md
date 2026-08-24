# Recorded baselines

Each `.trace` file is what an untouched `TrackPopupMenu`/`TrackPopupMenuEx` made
a menu's owner window observe, for one scenario in `Scenarios.cpp`.

**Recorded 2026-08-24 on Windows 11 26200.8875 x64**, toolset v143, by
`hostprobe.exe --record`. Re-check with `hostprobe.exe --verify <this dir>`,
which exits non-zero on the first differing line.

## The same fixtures are the takeover gate

```powershell
src\bin\x64\hostprobe.exe --takeover --verify src\tests\hostprobe\fixtures
```

`--takeover` loads Shell into the probe process and runs every scenario through
its hook (see `../Takeover.h`; it needs no injection and no deployed build).
There is deliberately **no second set of fixtures**, because the answer turned
out to be stronger than a second baseline would be: all 23 scenarios are
byte-identical through the hook. Whatever Shell decides to do about a menu, the
host observes what Windows would have shown it.

That is worth stating precisely, because two different properties produce it:

- Shell **declines** these menus. `Selections::QueryShellWindow` recognises
  Explorer's window classes, the taskbar, and any host that reached Shell
  through its `IContextMenu` handler; the probe's own window is none of those,
  so `ContextMenu::Initialize` returns false and the hook falls open. After
  three such menus the circuit breaker opens and the rest are not attempted.
  So what these traces verify is the **fail-open and breaker paths**, exercised
  in a real host for the first time — not the replay.
- Verifying that the *replay* reproduces the baseline needs Shell to actually
  compose a menu, which means a scenario that builds its menu the way a file
  manager does — `IShellFolder::GetUIObjectOf` → `QueryContextMenu` → track.
  Those are the four `takeover.*` scenarios; see `../ShellMenu.h`. They print a
  trace but store none, because their items come from whichever handlers the
  machine has installed, and they run only under `--takeover` **against the
  registered copy** — COM activates Shell by the path in the registry, so
  pointing `--shell` elsewhere would silently test a different binary.

### What the takeover scenarios found

Two defects, both of which only a host that is not Explorer would ever hit.

**The circuit breaker was counting deliberate declines.** Running everything in
one process is what showed it: the twenty-three plain popups above are all
declined, three declines open the breaker, and the shell-namespace scenario that
follows was then handed straight back to the host — while passing on its own. A
file manager raising three of its own internal popups would have lost Shell for
the rest of the session. `ContextMenu::Initialize` now says whether it refused
on purpose, and only real failures count. The scenario ordering here is
load-bearing: the `takeover.*` cases must stay last.

**One baseline scenario did differ, and it was a defect in Shell.**
`question.a_failed_track_sets_a_last_error` came back `lasterror none` where
Windows sets one: everything between the tracking call and the hook's return —
`InvokeCommand`, `complete_host_contract`, `PostMessage`, `WIC::release`,
`session_end`, `CoUninitialize` — can set the thread's last-error value, and the
hook was handing the host whichever one happened to be left. `TrackPopupMenu`
documents the pairing ("If the function fails, the return value is zero. To get
extended error information, call GetLastError"), and `Include/HostContract.h`
consumes exactly that signal — so Shell was destroying, for its own callers, a
contract it depends on. Fixed by restoring the tracking call's error at the end
of the hook's `__finally`.

These are measurements, not specifications. A different Windows build may
legitimately produce a different stream; that is the thing the fixtures exist to
notice. When one changes, record the new build and the diff rather than
overwriting silently — this directory's value is that it says what *this* Windows
did, and the findings below are things no documentation states.

## Reading a trace

One line per observed message, handles replaced by aliases assigned in
first-seen order, and a summary line at the end:

```
= returned 1, lasterror none, ownerdrawn not drawn
```

The summary exists because the message stream alone does not pin the thing that
most often matters. `cancel.returncmd` and `cancel.plain` produce *identical*
streams and return 0 and 1 respectively; without that line a fixture would not
notice if the two swapped.

Two deliberate omissions:

- **`WM_DRAWITEM` is counted, not listed.** It is a paint — how many arrive and
  where they fall among the selections depends on repaints, not on the menu
  contract, and recording it inline made the two owner-draw scenarios fail their
  own baseline on a re-run. What is worth pinning, that it survives
  `TPM_NONOTIFY` at all, is the `ownerdrawn` field.
- **The exact last-error value is reduced to `none`/`set`.** Its value after a
  *successful* call is not something to hold Windows to; that a *failure* sets
  one is the whole distinction `Include/HostContract.h` depends on.

Consecutive identical lines collapse to one.

## What the baselines established

### `TPM_RETURNCMD` alone suppresses `WM_COMMAND`. `TPM_NONOTIFY` does not.

This inverts the assumption the plan started from.

| Flags | Owner receives `WM_COMMAND`? | Owner receives the menu lifecycle? |
| --- | --- | --- |
| neither | yes (`select.plain.*`) | yes |
| `TPM_RETURNCMD` | **no** (`select.returncmd.*`) | yes |
| `TPM_NONOTIFY` | **yes** (`select.nonotify.*`) | **no** |
| both | no | no |

"Lifecycle" is `WM_ENTERMENULOOP`, `WM_INITMENU`, `WM_INITMENUPOPUP`,
`WM_UNINITMENUPOPUP` and `WM_EXITMENULOOP` — every one of them is gone under
`TPM_NONOTIFY`, and `WM_MENUSELECT`, `WM_MEASUREITEM` and `WM_DRAWITEM` all
survive it.

The documentation says only that the function "does not send notification
messages when the user clicks a menu item", which reads as a statement about
`WM_COMMAND` and is the opposite of what happens
(<https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenuex>).

Consequences for Shell, both recorded in `docs/refactor/01-takeover-contract.md`:

- Adding `TPM_NONOTIFY` would suppress the host's `WM_INITMENUPOPUP` — the very
  notification `NativeMenuBridge` exists to deliver. It must not be added.
- `TPM_RETURNCMD` alone is sufficient for exactly one component to observe the
  selection, which is what amendment A4 concluded — though for this reason
  rather than the one it gave.

### The low word of a `WM_MENUCHAR` reply is a zero-based index

`question.menuchar_low_word_is_an_index`: replying `MAKELRESULT(2, MNC_EXECUTE)`
to a menu whose items carry identifiers 5001…5005 selects **5003**, the item at
index 2. `question.menuchar_low_word_is_not_an_identifier`: replying
`MAKELRESULT(5003, MNC_EXECUTE)` selects **nothing** and the menu closes — an
out-of-range index is refused rather than reinterpreted.

Matches *Using Menus* ("the zero-based index of the menu item to be selected");
the `WM_MENUCHAR` page itself says only "the item specified in the low-order
word". Gates mnemonics, `docs/refactor/05-capabilities.md` §4.

One incidental detail worth knowing: the `WM_MENUSELECT` that follows an
`MNC_EXECUTE` reply carries `MF_MOUSESELECT`, even though nothing was clicked.

### `MNS_NOTIFYBYPOS` and `TPM_RETURNCMD` do not compose

`question.notifybypos_reports_a_position`: the owner gets
`WM_MENUCOMMAND position=1`, and the call returns 1.

`question.notifybypos_with_returncmd`: the call still returns 1 — **not** the
identifier — and **no `WM_MENUCOMMAND` is sent at all**. The selection is simply
lost. So a by-position menu cannot be tracked with `TPM_RETURNCMD`; Shell must
keep its own composed menu free of `MNS_NOTIFYBYPOS` and replay
`WM_MENUCOMMAND` itself for hosts that set it.

### The notification arrives *after* the tracking call returns

Both `WM_COMMAND` and `WM_MENUCOMMAND` were caught by a `PeekMessage` drain that
runs after `TrackPopupMenu` returns, so Windows **posts** them; and both appear
after `WM_EXITMENULOOP`. That is consistent with "The window does not receive a
`WM_COMMAND` message from the menu until the function returns", and it means a
replay that sends the message synchronously from inside the hook is *less*
faithful than one that posts it — the opposite of what `docs/refactor/01`
§3 assumed under QA-03.

### A failed track is distinguishable from a cancelled one, but only by the last error

`question.a_failed_track_sets_a_last_error` tracks a handle that is not a menu:
the call returns 0 and sets `ERROR_INVALID_MENU_HANDLE` (1401), while
`cancel.returncmd` returns the same 0 and leaves the last error at 0.

The documentation says the two are indistinguishable — "If the user cancels the
menu without making a selection, or if an error occurs, the return value is
zero" — and once Shell always sets `TPM_RETURNCMD` they really do collapse to
the same value, so a notifying host would be told the menu succeeded when it
never appeared. `Include/HostContract.h` uses the last-error code to separate
them. That is **undocumented behaviour**, isolated to one boolean at one call
site and pinned by this scenario.

### Submenu teardown is inside-out

`submenu.returncmd`: `WM_INITMENUPOPUP` for the child carries
`position=3`, the child's `WM_UNINITMENUPOPUP` arrives before the root's, and
both precede the closing `WM_MENUSELECT`/`WM_EXITMENULOOP` pair. That is the
ordering `docs/refactor/01-takeover-contract.md` §5 rule 4 has to match.

## The rendering scenarios have no fixture, and cannot have one

`render.*` are four scenarios that read the menu **off the screen** rather than
off the message stream, through `AccessibleObjectFromWindow` on the live
`#32768` window while the owner thread is still blocked inside its tracking
call. `../MenuReader.h` has the design.

They exist because of what seam steps 6 and 7 of `docs/refactor/04-code-health.md`
are about to move: `MenuModel` and then the presenter, out of a 7,559-line
`ContextMenu.cpp` that the unit test project does not link. Those two seams
break things that no message stream shows — an item in the wrong order, a
submenu against the wrong edge, a measure pass that silently changed. Every
other scenario in this directory would stay green through all of it.

Like the `takeover.*` cases they store no trace: the items come from whichever
handlers the machine has installed, so they assert properties.

| Scenario | Property |
| --- | --- |
| `render.every_composed_item_is_readable` | every non-separator item reports a name; every separator reports `ROLE_SYSTEM_SEPARATOR` and none |
| `render.the_composed_order_reaches_the_screen` | MSAA child *i* is the composed HMENU's position *i-1*, for separator-ness, submenu-ness, disabled-ness and title |
| `render.the_popup_contains_the_items_it_measured` | every item rectangle lies inside the popup's, and items advance down a column without overlapping |
| `render.a_submenu_opens_against_its_parent` | the child popup is adjacent to one of its parent's vertical edges and overlaps the row that opened it |

### What had to be measured first

Four things the documentation does not state, all on Windows 11 26200.8875 x64,
2026-08-25. Two of them would each have produced a test that passed for the
wrong reason.

- **`get_accName` strips the mnemonic marker and keeps the accelerator.**
  `&Alpha` reads back as `Alpha`; `&Bravo\tCtrl+B` as `Bravo\tCtrl+B`, tab
  intact. The access key moves to `get_accKeyboardShortcut` (`a`). So comparing
  an MSAA name with an HMENU title means stripping `&` from the title and
  touching nothing else.
- **A separator answers `get_accName` with `S_FALSE` and a null BSTR** — not
  `S_OK` and an empty string. `S_FALSE` is a success code, so a reader that
  tests only `FAILED(hr)` records an empty name for a separator and cannot tell
  it from an item that has none.
- **A menu item's `get_accChild` gives the submenu popup, but with no
  geometry.** The Menu Item page says it "Retrieves the IDispatch interface to
  the pop-up menu object for this item", and it does — role `MENUPOPUP`, right
  child count. Its `accLocation` is `(0,0 0x0)` whether or not the submenu is
  open. A placement assertion built on that documented descent would have
  compared a rectangle at the origin against its parent and reported whichever
  way the comparison happened to be written. Geometry comes from each popup's
  own `#32768` window instead, and parent and child are told apart by which
  window was not there a moment ago.
- **A popup's `accLocation` is exactly the union of its items'**, with no
  border: measured `(503,303)` `217x107` against six items summing to 107. That
  is what makes "every item is inside the popup" a real constraint rather than
  one satisfied by a generous frame.

Two of the assertions rest on documented contracts rather than measurement, and
they are the load-bearing ones. Child IDs "are numbered sequentially from top to
bottom starting with one" and the count "is the number of menu items in the
menu, including menu separators"
(<https://learn.microsoft.com/windows/win32/winauto/pop-up-menu>) — which is
what makes MSAA child *i* equal HMENU position *i-1* with nothing left to
interpret. And `accLocation` returns an origin plus a size: "right = left +
width, and bottom = top + height"
(<https://learn.microsoft.com/windows/win32/api/oleacc/nf-oleacc-iaccessible-acclocation>).

### What they deliberately do not pin

The multi-level property behind `PopupStack::parent_of_top()` — that a
*third*-level popup is placed against its own parent rather than against the
root. With a single level the parent is the root and the two are
indistinguishable, and a composed menu on an arbitrary machine is not guaranteed
to have a three-level cascade. That stays where `docs/refactor/01-takeover-contract.md`
§6a left it: verified by hand once, and structural in `PopupStack` rather than
tested for.

The placement rule is also deliberately loose about *which* edge. Windows flips
a submenu to the left of its parent when there is no room on the right, and this
harness places its popup in whichever screen corner is furthest from the cursor
— so "adjacent to one of the parent's vertical edges" is the strongest rule that
does not depend on where the menu happened to open.

### One trap, because it cost a run

The driver must wait for the popup **window**, not for the harness's own
"menu is up" event. That event is set by the earliest of several messages, and
under takeover the earliest is a `WM_INITMENUPOPUP` that Shell *synthesises* for
the borrowed host menu — sent before Shell has composed anything, let alone
shown it. Reading then finds no `#32768` at all, which reads as "Shell drew
nothing" rather than as "we looked too early". `Probe::wait_for_popups` polls
until two consecutive readings agree, which is the same settling rule
`AGENTS.md` states for reading a menu after an Explorer restart.
