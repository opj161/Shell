# 17 — Where the right-click latency actually is, and six defects found on the way

**Assembled 2026-09-05/06**, from a user report that packaged verbs were
appearing under *Properties*, on the wrong file types, and inconsistently — and
then that the menu still felt slow with a brief loading cursor. Everything below
was measured on the development machine (Windows 11 Pro for Workstations 26200,
x64, one 3840×2160 display at 200%).

Two separate things came out of it, and they are worth keeping apart:

- **§1–§6** — six defects in the packaged-verb path, all fixed, all verified in
  a deployed Explorer.
- **§7–§9** — the latency assessment, which found that **84% of the delay is not
  Shell's** and had never been measured because no instrument reached it.

---

## 0. What landed

| | Commit | What |
|---|---|---|
| F1 | `98e1312` | a dotted `ItemType` matched every file, not that extension |
| F3 | `98e1312` | the packaged block was appended after the host's last group |
| F4 | `98e1312` | `Hidden` was reported as `Ok`, so the report could not be read |
| F5 | `e4fcd18` | **`ECS_HIDDEN` was read before the handler wrote it** |
| F6 | `321f54b` | handlers that never contribute took 38% of the menu budget |
| F7 | `c779777` | the menu paid in full for verbs it then threw away |
| — | `3ceec73` | every right-click destroyed the process-wide arrow cursor |
| — | `f5551b5` | the report now says whether the menu is slow because of Shell |
| — | `30194d2` | a "run as administrator" shortcut cleared every other flag |

Gates went from 12 rules to 14. Suite 33 585 → 33 783 checks.

---

## 1. F5 — the one that mattered, and the shape to remember

```cpp
classify_command_state(cmd->GetState(selection, FALSE, &state), state)
```

`GetState` writes `state` through its `[out] pCmdState`; the **second argument
reads the same variable**. There is no order to rely on:

> "In C++17 and later, the function expression is evaluated before any argument
> expressions or default arguments. The argument expressions are evaluated in an
> indeterminate sequence."
> — [Function call operator ()](https://learn.microsoft.com/cpp/cpp/function-call-operator-parens)

> "There is no specified order of evaluation among the arguments or the
> function-call expression."
> — [Semantics of expressions](https://learn.microsoft.com/cpp/cpp/semantics-of-expressions)

Not undefined behaviour — *unspecified order*, which is enough. MSVC materialises
arguments right to left, so `state` was copied while it still held its
`ECS_ENABLED` initialiser. `ECS_ENABLED` is 0 and `ECS_HIDDEN` is `0x2`
([EXPCMDSTATE](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-_expcmdstate)),
so the flags word tested was always zero.

**`ECS_HIDDEN` was never honoured — for any handler, on any menu, since the
packaged-verb path landed.** Every verb that declined went on to `GetTitle` and,
if it had one, into the menu. That is the whole of "Resize with Image Resizer on
a .txt", "Edit in Notepad on a folder", "Ask Microsoft 365 Copilot on a .txt".

Proved with a standalone probe (cl 19.44 `/std:c++20`): for `f(writer(&v), v)`
the writer runs first and the callee **still sees the old `v`**.

**Why nothing caught it.** `classify_command_state` is correct and is tested by
being handed both values explicitly — nine assertions, all passing, all about a
function that was never the problem. The defect was in the call. This is the
fourth member of the "silent wrong answer" family AGENTS.md names and the first
that had no gate; it has one now, self-tested by reintroducing the original line.

### The investigative lesson

Six candidate causes were eliminated by measurement before the real one was
found, and each cost real time. Do not re-derive them:

| Ruled out | How |
|---|---|
| `E_PENDING` shown provisionally | both handlers return `S_OK` + `ECS_HIDDEN` |
| stale `ProviderCache` answer | one object alternated `.png`→`.txt`→`.png` correctly |
| null / empty `IShellItemArray` | null gives `E_INVALIDARG` → `Failed` → omitted |
| selection stale across menus | `Selections` is a member of `ContextMenu`, per popup |
| data-object backing, cross-apartment marshal | all four array shapes answered `HIDDEN` identically |
| the array not describing this selection | instrumented in-product: count 1, first item equal, every menu |

The thing that finally located it was comparing *what a probe measured* against
*what the product recorded* for the same input. **When those disagree on the same
input, the difference is in the reading, not in the environment.**

---

## 2. F1 — a dotted ItemType matched every file

`explorer_command_type_matches` parsed the manifest's extension, noticed the
leading dot, and returned `kind == File`. The selection's extension was not a
parameter, so it could not participate.

`Type` "must begin with a period or be the wildcard `*`"
([desktop4:ItemType](https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-desktop4-itemtype)),
and the dotted form names **one** extension.

Invisible for a handler that filters itself, loud for one that does not.
Measured `GetState` on this machine:

| Verb | registered for | folder | .md | .json | .png |
|---|---|---|---|---|---|
| Create with Designer | image extensions only | ENABLED | ENABLED | ENABLED | ENABLED |
| Edit with Photos | image extensions only | ENABLED | ENABLED | ENABLED | ENABLED |
| Edit with Paint | image extensions only | HIDDEN | HIDDEN | HIDDEN | ENABLED |

Photos never self-filters — it relies on Explorer honouring the manifest, which
is the only gate the platform gives it.

The suite **asserted the defect** (`.zip` matched `ExplorerCommandKind::File`
unconditionally), so those assertions were replaced, not extended.

Multi-selection is deliberately permissive: a registration matches when *any*
selected file carries the extension, because the handler's own `GetState` takes
the whole array and is the authority for a mixed selection.

---

## 3. F3 — everything landed after Properties

`append_explorer_commands` ended in `root->items.push_back`, and nothing moved
the items afterwards: they carry `Position::Auto`, and the stock rules key on
canonical ids and name patterns, which a packaged verb has neither of.

Worse than "one item in the wrong place", because *which* verbs landed there
changed on every right-click — a verb with a native twin is dropped by the
identity check, one without it is not, and which get that far is decided by the
budget. Matched against the user's screenshots exactly:

| session | Shell resolved | screenshot showed below Properties |
|---|---|---|
| s0 | PowerRename only | Rename with PowerRename |
| s1 | PowerRename, NanaZip, Open with Code | NanaZip + PowerRename (Code deduplicated) |
| s12 | Image Resizer, Create with Designer | both |

The block now inserts before the last separator.

---

## 4. F7 — the menu paid for verbs it threw away, behind a guard that could not fire

Measured over twelve menus: **three to six providers resolved per menu at
33–46 ms — essentially the whole 50 ms budget — and zero or one items added.**
Everything else was activated, asked for state/title/flags/icon, then discarded
by the identity check because a native item already carried the same title.

A guard for exactly this existed and **could never match**: `accepted` is seeded
from native items with hash and type only, so every entry's `clsid` and
`canonical` are `GUID_NULL`, while all three matching rules need a usable GUID on
both sides or a non-zero hash. `explorer_clsid` is set in one place — on items
that function itself adds. It read as a working optimisation, which is why the
cost behind it went unexamined.

It could not be repaired in place: the colliding identity is the title hash,
which comes from `GetTitle` — one of the calls the guard exists to avoid. So
`ProviderTiming` remembers what the provider last produced and the planning pass
tests that against the native items of the menu being built now.

**Why this skip is safe when skipping a decliner would not be:** it only fires
when the verb is already in *this* menu, so the user sees the same menu either
way. A stale identity cannot hide anything — if it no longer matches, nothing is
skipped. `DUPLICATE_REFRESH_AFTER` bounds it and `record()` clears the counter.

After deploying, over sixteen menus: **6–8 providers skipped per menu at no cost,
and `deferred(budget)` fell to zero** on all but the cold first menu. The menu
stopped dropping verbs at random, which was the user-visible half.

---

## 5. F4 and F6 — the instrument, and what it then showed

`FillResult::Hidden` was reported as `ProviderResult::Ok`, the same word a verb
that reached the menu reports. About half the handlers decline any given
selection, so the common case was unreadable — `prov Resize with Image Resizer
Ok` on a `.json` was equally consistent with shown and with correctly dropped.
**That is why F5 stayed open as long as it did.**

With `Hidden` separated, F6 became visible: across twelve menus,

| | total | per menu | share |
|---|---|---|---|
| verbs that reached the menu | 345.0 ms | 28.7 ms | 61.8% |
| **handlers that declined** | 213.1 ms | 17.8 ms | **38.2%** |

the same six every time — five OneDrive commands and Adobe's notification
client, none of which has produced a title on this machine at all.

`ProviderTiming::shown` counts menus a provider's item actually **reached**
(reported from the composition pass, not from `FillResult::Shown` — those are
different questions), and the planner sorts on it ahead of cost. A sort key,
never a deferral: declining is a statement about *this selection*, and Image
Resizer declines a `.txt` and accepts a `.png` at the same shape.

Wire format moved 8 → 10 across the session: `Hidden` (6), then `Duplicate` (7).

---

## 6. The cursor, and the shortcut flags

`Uninitialize` ran `DestroyCursor` on `LoadCursorW(nullptr, IDC_ARROW)` — once
per menu, in explorer.exe and in every third-party host. Both pages are explicit:
DestroyCursor "Do not use this function to destroy a shared cursor… LoadCursor"
obtains one, and LoadCursor "loads the cursor resource only if it has not been
loaded; otherwise, it retrieves the handle to the existing resource". Gated;
nothing in the tree creates a cursor, so `DestroyCursor` can never be correct
here.

Outside the branch, `Path::CreateLnk` computed `dwFlags |= SLDF_RUNAS_USER` and
then passed the bare literal to `SetFlags`, which is a **whole-set** write — so
creating a "run as administrator" shortcut cleared every other flag on it. The
unchecked `GetFlags` HRESULT made a failed read produce the same destructive
write. Same family as the `Environment::Expand(value).move()` statements
AGENTS.md records: the right answer computed and never handed over.

---

## 7. The latency, and the instrument that was missing

**Every phase Shell records starts at the `TrackPopupMenu` hook.** The user's
stopwatch starts at the right-click, and between the two the host builds the
classic menu. None of that was visible, so a slow menu had only Shell's numbers
to be blamed on — and was, for the whole first half of this investigation.

`host.input_to_hook` closes it. `GetMessageTime` "specifies the elapsed time, in
milliseconds, from the time the system was started to the time the message was
created" for "the last message retrieved by the GetMessage function"
([GetMessageTime](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmessagetime));
`GetTickCount` shares its origin and unit. Inside the hook the thread is still
dispatching the message the right-click produced, so the difference is
input-to-hook. Subtracted in int32 and allowed to wrap, which is the documented
method; a delta outside a plausible window records −1, because a host that calls
`TrackPopupMenu` outside message dispatch leaves `GetMessageTime` describing an
unrelated message and *"we cannot tell"* must not print as *"it was fast"*.

Thirteen real Explorer menus:

| | median | range |
|---|---|---|
| **input → hook (the host's own work)** | **312 ms** | 62–1218 |
| hook → first pixel (Shell) | 61 ms | 9–68 |
| flicker wait (after pre-display) | 8 ms | 1–16 |
| **total right-click → menu** | **370 ms** | 80–1295 |

**Shell is 16% of the latency.** `-report perf` now prints a `host menu` line
beside `pre-display`, so this is answerable on any machine rather than only here.

---

## 8. What the host's 312 ms is — and the control that settles it

Building the same menu from outside Explorer agrees (267–347 ms warm), and it is
nearly independent of file type: an extension with **nothing registered for it**
still cost 276–315 ms, because the `*` and `AllFilesystemObjects` handlers apply
to everything.

The decisive control is `CMF_DEFAULTONLY`, which tells every extension to add
nothing — "A shortcut menu extension or drag-and-drop handler should not add any
menu items if this value is specified"
([QueryContextMenu](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-icontextmenu-querycontextmenu)):

| built with | items | warm |
|---|---|---|
| `CMF_NORMAL` | 34 | 277–344 ms |
| `CMF_DEFAULTONLY` | 14 | **23–26 ms** |

**Windows' own menu machinery is ~23 ms. The registered extensions add 20 items
and ~255 ms.**

### The architectural fact

`-register -treat` makes the **legacy** menu the primary one. Windows 11's
default compact menu is fast precisely because it does not instantiate every
`IContextMenu` handler. Enabling Shell converts every right-click from the fast
path to the slow one. That is the product's inherent cost, not a defect — and it
means **no amount of optimising Shell's 61 ms addresses the 312 ms.**

### A caution against the obvious arithmetic

It is tempting to divide: 255 ms ÷ 20 items ≈ 12.7 ms per item, therefore
removing ten handlers saves ~130 ms. **Do not promise that.** Directory Opus was
uninstalled on 2026-09-06, removing the `OpusZip` handler and one item — and the
next measurement went *up*, 277–344 → 422–472 ms, on a machine that was busy
after an uninstall.

What is stable is the **ratio** (14× between `DEFAULTONLY` and `NORMAL`) and the
conclusion that extensions dominate. The absolute per-item figure drifts 8–21 ms
between runs of a cold probe process. Measure the lever in situ with
`host.input_to_hook`, which reads the real Explorer, rather than with the probe.

---

## 9. Still open

- **The 312 ms.** The only thing that would address it is intercepting *before*
  the host builds the legacy menu — handling `WM_CONTEXTMENU` on the shell view
  and materialising native items lazily, the way `NativeMenuLazy` already does
  for submenus. Since the stock configuration folds most native items into
  *More options*, most menus would never pay for them. Large and risky; not
  started, and not to be started without a decision.
- **Shell's own 61 ms → ~25 ms.** Verify declines off the UI thread: skip the
  eight always-declining handlers during composition and re-verify on a
  background thread with its own apartment, own provider instances and an array
  built from paths (so no marshaling and no UI-thread cost). If an answer
  changes, clear the memo so the next menu includes it. Worst case is losing a
  verb for exactly one menu, and only when the state actually changed.
- **The flicker wait.** Costs 8–16 ms *and* calls `timeBeginPeriod`, raising the
  system timer resolution for all of explorer.exe. Its own comment says nobody
  could judge whether it works, and the user reports flicker **with it enabled**.
  A/B it: `HKCU\SOFTWARE\Nilesoft\Shell` → `flicker` = `0`, written from a
  scheduled task so Explorer sees it. If the flicker is unchanged, delete it.
- **The spinner.** Four `dllhost` COM surrogates were observed launching during
  menu use, each hosting a packaged verb handler (Windows Terminal, PowerToys
  New+, PowerToys PowerRename, VS Code). `CoCreateInstance` on a
  `com:SurrogateServer` handler starts a process on the UI thread — measured at
  60.7 ms cold against ~4 ms warm. `ProviderCache` is thread-local by design, so
  this repeats on each menu thread rather than once per process.
- **`FileConverterExtension`** costs ~8 ms warm and adds **zero** items, which is
  the user's — it is supposed to appear. Worth its own investigation.
- **The icon overlay** (doc 16 §5) and **Total Commander** (doc 16 §2, §7) are
  unchanged.

---

## 10. The probes

All in the session scratchpad; none links repository sources except
`perfread.cpp`, which takes `src/shared`. Built with the BuildTools 2022 x64
environment — note `vswhere` reports nothing on this machine and the only
`vcvars64.bat` is under `C:\Program Files (x86)\…\2022\BuildTools`.

| Probe | Answers |
|---|---|
| `getstate.cpp` | what a packaged handler answers for a selection (`-null` for a null array) |
| `reuse.cpp` | is the handler safe to cache across selections |
| `marshaled.cpp` | direct vs data-object vs cross-apartment array — all four shapes |
| `argorder.cpp` | which argument this toolchain evaluates first |
| `nativemenu.cpp` | the host's own menu cost, per `CMF_` mode, cold and warm |
| `handlercost.cpp` | per classic handler: cold, warm, items added |
| `perfread.cpp` | every host's perf block, **unelevated** |

`shell.exe -report perf` is manifested `requireAdministrator`, so it cannot be
run from an unelevated agent shell at all; reading the block is a
`FILE_MAP_READ` of a `Local\` section and needs no privilege, which is what
`perfread.cpp` does. Worth fixing separately — the elevation requirement is
inherited from the executable's other verbs and makes the instrument unavailable
in the situation it was built for.
