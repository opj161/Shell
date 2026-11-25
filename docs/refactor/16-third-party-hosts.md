# 16 — Third-party hosts: which ones Shell can serve, and why Opus is not one

**Assembled 2026-08-28**, from a user report that the menu was wrong in
Directory Opus and "completely broken" in Everything while Explorer was fine.
Two screenshots, two different causes, and neither of them the one the report
implied. Everything measured on this machine (Windows 11 Pro for Workstations
26200, x64, one 3840×2160 display at 200%).

---

## 0. Verdict

| | Reported | Actually |
|---|---|---|
| **Directory Opus** | shows the classic Windows menu | correct behaviour, and unreachable by design. Opus builds its own menu; Shell's handler adds no items, so it contributes nothing. Not a regression, not fixable from this side |
| **Everything** | "fucked up completely" | Shell composes correctly. The process had been running since before the deploy and had the **previous** `shell.dll` mapped. Restarting it fixed it, confirmed by the user |

Neither was a defect in the branch. What the investigation did turn up is
recorded in §4 and §5: a race in the hostprobe harness that has been mimicking
memory corruption, and an icon-overlay registration that is claimed but not
implemented.

---

## 1. The host taxonomy, which is the thing to know first

Shell can only compose a menu in a host that falls into one of two classes, and
the distinction is not about the host's quality — it is about one mechanical
question.

**Class 1 — Explorer.** `Selections::QueryShellWindow` recognises the window
class (`SHELLDLL_DefView`, `SysListView32`, the taskbar classes, and the rest).

**Class 2 — a host that displays the `HMENU` it handed to the handlers.**
`ShellExtHandler::QueryContextMenu` binds that handle into `ShellExtCapture` and
**inserts zero items** (`src/dll/src/ShellExt.cpp`), returning
`MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0)`. The `TrackPopupMenu` hook
later recognises the handle, `loader.contextmenuhandler` becomes true, and
`QueryShellWindow` takes the `goto ui` path. **Everything is in this class** and
is now confirmed working by hand.

**Everything else cannot be served, and no change here changes that.** A host
that enumerates the registry itself and builds its own menu never displays the
handle it gave the handlers, so the capture never matches — and a handler that
adds no items is invisible to it.

**Class 2 has one exception: a host that repaints its own menu windows.**
Added 2026-11-25. WizTree uses Delphi VCL Styles, whose `TSysPopupStyleHook`
replaces the procedure of every `#32768`. Shell's owner-drawn items were never
drawn, so the menu came up as a blank rectangle that only closing WizTree
removed. Shell now detects the replacement (`Include/HostMenuPainter.h`) and
hands such a host its own menu. `AGENTS.md` has the full account.

The zero-item design is not an oversight. It is what lets Shell *replace* a menu
rather than append to one, and replacing is the entire product: the stock
configuration's `modify.nss` removes `id.restore_previous_versions`, moves
`id.copy_as_path` into a `file manage` submenu, and folds `id.send_to`,
`id.share`, `id.include_in_library` and eight others into `More options`. None
of that is expressible through `QueryContextMenu`, where a handler can only add.

---

## 2. Directory Opus

**Out of scope by the vendor's design. Do not try to take it over.**

Opus does not ask the shell for a menu:

> When you right-click a file or folder to display its context menu, Opus
> searches the system registry and its own file type settings for the commands
> to display on the menu.
>
> — [Context Menu, Directory Opus 12 manual](https://www.gpsoft.com.au/help/opus12/documents/Context_Menu.htm)

and does it deliberately, for the same reason Shell exists:

> Opus does file/folder context menus itself, largely in order to provide the
> same sort of functionality Nilesoft Shell does, by letting you add/remove/move
> context menu items.
>
> — GPSoftware, [Directory Opus Resource Centre](https://resource.dopus.com/t/how-can-i-make-the-directory-opus-use-my-windows-context-menu/53508)

So Opus instantiates the handlers it finds under `shellex\ContextMenuHandlers`,
takes their items, and composes its own menu. Shell's handler yields nothing,
and Shell is absent from the result. That is the whole explanation for the
reported screenshot.

Two observations from that screenshot that are **not** Shell's doing:

- The duplicated *Unlock with File Locksmith* and *Rename with PowerRename* are
  Opus finding the same PowerToys handler under more than one registry root and
  merging each occurrence. Opus has its own control for this
  (**Preferences → Windows Integration → Hide Windows items on file context
  menus**, shift overrides) and its file-type context menu editor.
- `Open in new Folder Tab`, `Show Contents Summary`, `Copy File Names`,
  `Copy Full Pathnames`, `Set Label` and `Set Status` are Opus's own commands.

**Why forcing it was rejected.** Inserting real items in `QueryContextMenu`
would only ever *add* — the native items all remain, so the result is a longer
menu with more duplicates, and `modify.remove.duplicate=1` cannot fire. Hooking
Opus's own menu and rewriting it fails on identity: the configuration's rules
key on Explorer's canonical verbs (`id.copy_as_path`, `id.send_to`), and Opus's
items carry Opus's internal command IDs, so the rules would match nothing while
Shell rewrote another vendor's UI with no contract and no test. That is the
failure class this branch has already paid for once, in the provider-cache
double-release.

`docs/refactor/09` R8 previously listed "Total Commander/Directory Opus/
Everything" as one host class to validate. That grouping was wrong: Everything
is Class 2, Opus is neither class, and **Total Commander is still unknown** —
the only question worth asking there is whether it tracks the `HMENU` it handed
the handlers.

---

## 3. Everything — and the trap that made it look broken

Shell composes correctly in Everything. The reported artefact came from a
process running a binary two deploys old.

`Everything.exe` had been up since 05:30; the deploys were at 15:50 and 16:47.
`GetModuleFileName` reported `C:\Program Files\Nilesoft Shell\shell.dll` — the
canonical path — because a mapped image keeps the name it had at load and the
deploy script rotates the old file aside rather than overwriting it. The mapped
image size is what gives it away:

| | `SizeOfImage` |
|---|---|
| `shell.dll` (current, 16:43) | 2,072,576 |
| `shell.dll.old.20260827155006_8407` (03:39) | **2,068,480** |
| `Everything.exe` pid 19848 | **2,068,480** |
| `explorer.exe`, `dopus.exe` | 2,072,576 |

Restarted into the current build, the menu is correct — confirmed visually by
the user. The 03:39 binary corresponds to no committed state: it was built from
a working tree between `df04eb5` and `c3d6079`. All three DLLs import the same
14 modules with `msvcrt.dll`, so VC-LTL is not the difference.

AGENTS.md now carries the diagnostic, because the module listing actively lies
here and it has cost a session.

### What was ruled out along the way

Recorded so the next reader does not re-derive them:

- **`disable\3rdparty`** — not set; `HKCU\SOFTWARE\Nilesoft\Shell` exists and is
  empty, so `BootstrapOnce` does not take the early return.
- **DPI cached at bootstrap** — `Initializer::init` reads
  `GetDpiForWindow(GetForegroundWindow())` once, but `dpi.val` is re-derived per
  menu from `Theme::GetDpi(pt, hwnd.owner)`, so the cached value does not drive
  menu geometry.
- **Monitor-versus-window DPI** — handled correctly and already documented in
  `Include/Theme.h`: the window is the authority, and the monitor is only asked
  when the window is per-monitor aware. This mattered because the machine runs
  at 200% while Explorer is `PER_MONITOR_AWARE` and both Everything and Opus are
  `SYSTEM_AWARE`.

---

## 4. The hostprobe harness has a race, and it has been mimicking corruption

**Fixed in this pass.** `Probe::track`'s post-tracking drain stopped at the
first empty `PeekMessage`. `TrackPopupMenuEx` documents only that the owner
"does not receive a WM_COMMAND message from the menu until the function returns"
([TrackPopupMenuEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenuex)) —
a floor, not a delivery guarantee, and `WM_COMMAND` is *posted*. So whenever the
poster was descheduled the drain ended early and the trace came up one line
short of its baseline.

Measured before the fix:

| mode | runs | failures |
|---|---|---|
| native (Shell never loaded) | 21 | 3 |
| takeover | 17 | 3 |

Always one scenario, always a different one, always `select.*`, always the
missing trailing `WM_COMMAND`, and clustering under load — the failures came in
a run of three immediately after a build and then twelve consecutive clean runs
on an idle machine. **It reproduces in native mode, where Shell is not in the
process at all**, which is what settles it as harness rather than product.

`Probe.h` already recorded the same rate ("fourteen runs of twenty-three
produced two"; this pass measured exactly two in fourteen) and the same scenario
names, and mitigated it with `settle(120)` between scenarios. That reduced it
without removing it, because 120 ms is a clock rather than a condition.

The drain now waits for the queue to stay quiet, bounded by a deadline. AGENTS.md
gains the note that an erratic `select.*` failure is this and not the
memory-corruption signature described two paragraphs above it — that guidance,
followed literally, sends the next reader to build a known-green worktree over a
harness bug.

**A single green hostprobe run is not a gate result.** Report it as *N of M*.

---

## 5. The icon overlay is registered and not implemented

Found while asking why `shell.dll` was loaded in `dopus.exe` at all.

`shell.exe -register` registers `{BAE3934B-…BAF2}` as
`HKLM\…\Explorer\ShellIconOverlayIdentifiers\ @nilesoft.shell`
(`src/shared/RegistryConfig.h`, `reg.ICONOVERLAY`). But
`IShellIconOverlayIdentifier` **is implemented nowhere in the tree** — `rg` for
the interface, `IsMemberOf`, `GetOverlayInfo` and `GetPriority` returns nothing —
and `DllGetClassObject` accepts the CLSID in its triage only to fall through to
`return CLASS_E_CLASSNOTAVAILABLE`.

Three consequences:

1. Windows loads a bounded number of overlay identifiers, sorted by name. On
   this machine the list is OneDrive ×7, Google Drive ×4, AccExtIco ×3, then
   ` @nilesoft.shell` — **the fifteenth and last slot**, taken from a real
   overlay handler for a feature that does not exist.
2. Every process that queries overlays loads `shell.dll`, and
   `BootstrapOnce()` runs at [`Main.cpp:1825`](../../src/dll/src/Main.cpp#L1825)
   **before** the CLSID is examined on the next line — so it pins the module,
   registers the layer, inline-hooks `NtUserTrackPopupMenuEx` and IAT-hooks
   `TrackPopupMenu`/`TrackPopupMenuEx` across every loaded module, and only then
   returns `CLASS_E_CLASSNOTAVAILABLE`. `DllCanUnloadNow` answers `S_FALSE` once
   hooks are installed, so the module never leaves.
3. Because Explorer queries overlay handlers at startup, this is in practice how
   Shell gets into `explorer.exe` early — plausibly before the first taskbar
   right-click, which is a supported menu that no context-menu activation would
   have preceded.

**Deliberately not changed in this pass.** Narrowing the bootstrap to the CLSID
that is actually served, or retiring the registration the way `E-02` retired
`FolderExtensions`, is the obviously correct-looking change and is exactly the
shape of thing this repository keeps writing down as a trap: point 3 is an
implementation accident that a supported feature may be resting on, and the test
for it — right-click the taskbar immediately after a cold boot, before touching
any file — is one nobody has run. It also would not have helped the case that
started this investigation, since Opus reaches Shell through the context-menu
handler as well.

So it is recorded as an open item with a named risk and a named test, not
implemented. The two candidate changes, in order of appetite:

- move `BootstrapOnce()` below the CLSID triage, keeping it unconditional in
  `explorer.exe`; then run the cold-boot taskbar test;
- or retire `reg.ICONOVERLAY` and keep its uninstall, freeing the overlay slot,
  after establishing what actually loads Shell into Explorer first.

---

## 6. Also fixed in this pass

- **`disable_modern`'s uninstall branch had no diagnostics.** The install branch
  got per-step reporting when the borrow was fixed; the removal branch — the one
  that fails outright on a stock machine, and the reason an MSI uninstall could
  fail — kept four bare `return false`s. It now reports which give-up it was.
- **`BorrowedKeyAccess` would restore a null DACL.** `SetSecurityInfo`
  documents that a null DACL "grants full access to the object to everyone", so
  restoring one hands back the machine-wide hole the class exists to remove, and
  widening one first narrows the key on the way there. It also cannot be the
  case worth borrowing for, since a null DACL means the plain write would have
  succeeded. `acquire` now refuses.
- **The custom action's `MSIHANDLE` outlived its action.** `TreatAsApply` and
  `TreatAsRollback` assigned `g_borrow_install` at entry and left it set across
  several return paths, in a DLL the installer keeps loaded. Scoped.

---

## 7. Still open

- **Total Commander** — untested; the question is in §2.
- **The icon overlay** — §5, with its test named.
- **The MSI upgrade matrix** on clean VMs. Unchanged, though the authoring was
  read closely this pass and the `(NOT UPGRADINGPRODUCTCODE)` guard on all four
  TreatAs actions is correct: it is what keeps the redirect alive across a major
  upgrade, where `RemoveExistingProducts` at sequence 1501 runs long before
  `TreatAsApply` at 5002.
- **A cold-boot taskbar right-click**, which §5 now depends on.
