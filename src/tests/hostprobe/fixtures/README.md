# Recorded baselines

Each `.trace` file is what an untouched `TrackPopupMenu`/`TrackPopupMenuEx` made
a menu's owner window observe, for one scenario in `Scenarios.cpp`.

**Recorded 2026-08-24 on Windows 11 26200.8875 x64**, toolset v143, by
`hostprobe.exe --record`. Re-check with `hostprobe.exe --verify <this dir>`,
which exits non-zero on the first differing line.

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
