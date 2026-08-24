# 05 — Capabilities: highest user value per unit of scope

Selection criterion (per charter): features that exploit what only a takeover engine
can see or do, reusing machinery that already exists, without new frameworks. Ranked.

---

## 1. Provider health + quarantine → **Reliability Center** (flagship)

Takeover's unique advantage: Shell mediates the whole menu ecosystem, so it can answer
"why is my context menu slow/broken?" — stock Explorer cannot.

**Substrate already in tree:** Alt-timing of COM activations (`Main.cpp:764-774`);
CLSID suppression via `CoCreateInstanceHook` (`:749-796`); MenuPerf phases; the §02.6
ring adds always-on per-provider timings; modern commands give exact attribution
(native classic menus are honestly labeled approximate — multiple handlers merge into
one HMENU before capture).

**Design.**

```cpp
struct ProviderRecord {                  // keyed by CLSID hash
    uint32_t clsid_hash; wchar_t name[64];
    uint32_t activate_us, state_us, title_us, icon_us;
    uint16_t activations, e_pending, failures;
    enum class Tier { Healthy, Slow, Pending, Quarantined } tier;
};
ProviderHealth g_providers;              // process-lifetime, published atomically
```

Degradation ladder (never kill threads — A1§23/A2§28): Healthy → Slow (skip optional
icon work, reuse cached metadata) → **Deferred** (missed its first-paint deadline;
omitted from this menu, resolved in the background, present next time — §02.2a) →
Quarantined (activation refused via existing E_NOINTERFACE path; visible, reversible).
Thresholds from ring p95s after §06.4 baseline; defaults conservative
(Slow ≥ 50 ms metadata, Quarantine manual-first).

**Deferred is what makes this feature actionable (§07).** Without §02.2a the
Reliability Center can only report that an extension is slow, while the user still
waits for it on every right-click. With it, the report describes something the
product already did on the user's behalf — "Acrobat 2 ms · ExampleExt 186 ms,
deferred 4× today" — and quarantine becomes the user confirming a decision rather
than discovering a problem. Sequence accordingly: telemetry with §02, the deferral
tier with §02.2a, the UI last.

**UI:** `shell.exe -reliability` window (manager EXE already owns UX; MSI already ships it):

```text
Takeover      TreatAs ✓  Interception: win32u-IAT ✓  Handler ✓  Taskbar ✓
Last menus    pre-display p50/p95, composition split (from ring)
Providers     Acrobat 2 ms · ExampleExt 186 ms [quarantine] · …
Actions       Repair takeover · Show Windows menu once · Export report
```

Quarantine persists as a config-level rule (`modify`-independent blocklist file under
ProgramData, compiled into `ComActivationPolicy`, §01.9) so it survives reloads and is
user-editable like any NSS-adjacent artifact. Undo = delete line / click.

Scope: M-L but staged — telemetry-only first (lands with §02), UI second.

**Both prerequisites now exist.** The telemetry landed with the ring (`1f17d00`),
and the export it needed in order to leave the process landed as
`shell.exe -report perf` (§06.4). That command already prints most of the "Last
menus" and "Providers" rows above, in text, for every host on the desktop:

```text
Explorer.EXE  pid 41272  x64  -  8 menus, 8 held
    pre-display  p50 13.2 ms   p95 58.2 ms   n=8
    decisions    8 takeover
    host flags   RETURNCMD|RIGHTBUTTON
    slowest      58.2 ms to display  takeover  6.7s ago
                   explorer.commands          57.0 ms  n=23
                   popup.total_pre_display    58.2 ms
                   menu.flicker_wait           4.6 ms  n=1
                   provider e345019d  6.4 ms  ok
```

### 1a. Provider names — landed 2026-08-24, beside the records rather than in them

This section used to end by saying names were missing and that the Reliability
Center "will have to resolve them from the catalog at display time". That turned
out to be the wrong place, for a reason worth recording: **nothing outside a host
that has built a menu can resolve a CLSID to a name.** The name comes from
`IExplorerCommand::GetTitle`, which takes the selection and is answered by the
handler; the catalog holds registrations, not titles. Resolving in `shell.exe`
would have meant duplicating the manifest scan there and still getting a
different answer.

So names live **beside** the records instead of in them, which keeps the property
the hash was chosen for. `PerfExportBlock` gained a 32-entry directory of
`(clsid_hash, name)`, written once the first time a host activates a provider;
records still carry hashes only, and the measured path still copies no string
except on that first sighting. The reader copies the directory out — the view is
unmapped before `perf_export_read` returns, so a pointer into the block would
dangle exactly when it gets printed.

Measured in a real Explorer the same day:

```text
                 provider e345019d  11.4 ms  ok        NanaZip
                 provider 9b0df3d3   0.0 ms  deferred
                 provider 7c2bc7ba   0.0 ms  deferred  Unlock with File Locksmith
```

The third line is the one this feature exists for, and it is the sentence §02.2a
made possible: a named extension that Shell **already dropped from this menu on
the user's behalf**, rather than a hash the user can do nothing with. A provider
with no name is one this host has not successfully activated yet — a deferral on
the first menu in a process, or a handler that declined the selection — which is
a true statement rather than a gap.

The hash stays on the line. It is the stable identity; a title is not, because a
handler may title itself differently for a different selection, and a quarantine
entry has to be written against something that does not move.

**One defect fixed on the way, and it was not in the new code.** `export_session`
runs *outside* the diagnostics ring's mutex, so two menu threads publishing at
once both drove the block's sequence counter. That counter is a seqlock: it lets
a *reader* notice it copied a half-written record, and it offers nothing at all
against concurrent writers — two writers each bump it twice, so it can read even
while a write is in flight, and `header.next` is a plain read-modify-write that
would put two records in one slot. A reader would then print a record that is
half of two, with nothing to say so, which is worse than a lost record because it
looks like a measurement. One host genuinely does raise menus on several threads:
every Explorer window has its own and the taskbar has another. The writer is
serialised now.

`PerfExport.h`'s "exactly one writer for its whole life" was always about one
*process* per block — which is what makes the security descriptor the right
boundary — and was never a claim about threads.

### 1b. Quarantine — landed 2026-08-24, and it skips rather than refuses

The treatment for what §1a diagnoses. `shell.exe -quarantine:add {clsid}` stops
Shell asking a handler when it builds a menu; `:remove` puts it back, `:list`
(the default) shows what is quarantined. The list is a per-user file,
`%LocalAppData%\Nilesoft\Shell\quarantine.txt`, re-read by every host within a
couple of seconds — so the answer to "when does this take effect" is "the next
menu", with no restart.

Three departures from §1's sketch, each because the measurement said so.

**Skipped in `append_explorer_commands`, not refused at `CoCreateInstance`.**
§1 said "activation refused via the existing E_NOINTERFACE path", reusing the
CLSID blocklist `CoCreateInstanceHook` already compiles. That is the wrong
instrument twice over. It is far too blunt — the detour sees *every* activation
in the host, so refusing a CLSID there breaks that extension for everything the
process does, and a user who quarantined a slow context-menu handler has not
asked for its DLL to start failing elsewhere. And it does not save the thing
worth saving: the measured cost is activation *plus*
`GetState`/`GetTitle`/`GetIcon` (§02.2a-i), and skipping the provider outright
pays none of it while refusing the activation still walks the catalog and still
enters the hook. Measured on a real Explorer: a quarantined provider costs
**0.0 ms** and is reported as such.

**`%LocalAppData%`, not ProgramData.** ProgramData needs elevation, which would
make quarantining an administrative act for a per-user complaint. The integrity
rule §02.1 states — a file a medium-integrity process can write must never make
an activation *possible* that a live query would not have authorised — is
satisfied in the strong direction: this file only ever *removes* providers, so
the worst a tampered one can do is hide a menu item the user can see and undo.

**`Quarantined` is its own word in the ring, not a kind of `Deferred`.** "It has
never once been quick" is Shell's judgement and gets re-probed every 200 menus;
"you told me to stop asking" is the user's and never does. Reporting them as one
thing would make the feature's own effect look like a heuristic.

Verified end to end on a real Explorer, 2026-08-24, reading the composed menu
back through MSAA:

| | menu items | the report |
|---|---|---|
| NanaZip quarantined | **29**, no NanaZip | `provider {CAE3F1D4-…} 0.0 ms quarantined NanaZip` |
| released again | **30**, NanaZip back at 29 | `provider {CAE3F1D4-…} 7.2 ms ok NanaZip` |

One thing this forced, and it is the kind of gap that only shows up when both
halves exist: the report printed a *hash* and `-quarantine:add` wanted a
**CLSID**, with nothing bridging them. The export directory carries the CLSID
now and the report prints that instead, so the identifier a user reads is
exactly the one the next command takes. Export version 4 → 5.

What is still missing for the window: the window itself, and the "Repair
takeover" row.

The `Takeover` line's inputs also all exist now: `RegistryConfig::ModernMenuRedirectedToUs()`
answers the TreatAs half (§01.9b), `IATHook::installed()` the interception half,
and `TaskbarHitStats` the taskbar half.

## 2. One-shot native bypass ("Windows menu, this time")

Trivial at hook top (§01.7): modifier-gesture check before any Shell work; plus an
optional placeholder item type `menu(type='native')` rendering one item that, when
chosen, closes Shell's menu and replays the original call untouched. Value: debugging,
rescuing rules-hidden commands, comparison testing; makes aggressive configs safe to
live with (A2§26B). Scope S. Default gesture: `Ctrl+Alt+right-click` — deliberately not
Ctrl+Shift, which is the live config-reload combo (`Initializer.cpp:840-845`, evaluated
in the same hook body; see §01.7); documented in README + Reliability Center.

## 3. Accessibility: expose owner-drawn items

> **Closed 2026-08-24: this already works, and `MSAAMENUINFO` must not be
> adopted.** The section's premise was wrong. What follows is the evidence, in
> the order it was gathered; the design it replaced has been deleted.
>
> The claim was that owner-drawn items leave screen readers with nothing. A probe
> built four menus and asked each what a reader is told — from another thread,
> while the owner was blocked inside `TrackPopupMenu`, via
> `AccessibleObjectFromWindow(menu, OBJID_CLIENT, IID_IAccessible)` and
> `get_accName` per child. Windows 11 26200.8875 x64:
>
> | Menu shape | Names reported |
> |---|---|
> | plain `MFT_STRING` items (control) | Alpha / Bravo / Charlie |
> | owner-drawn, nothing else | `<no name>` ×3 |
> | owner-drawn + `MSAAMENUINFO` in `dwItemData` | Alpha / Bravo / Charlie |
> | **owner-drawn + `MIIM_STRING`** | **Alpha / Bravo / Charlie** |
>
> `MIIM_STRING` and `MFT_OWNERDRAW` are not exclusive in the modern
> `MENUITEMINFO`: the text lives in `dwTypeData` and the owner data in
> `dwItemData`, and they are separate mask bits. Windows exposes the string to
> accessibility and still sends `WM_MEASUREITEM`/`WM_DRAWITEM`.
>
> **And Shell already does that.** `ContextMenu.cpp:846` sets `MIIM_STRING`
> whenever an item has a title, `MenuItemInfo::set_title` fills `dwTypeData` and
> `cch`, `add_ownerdraw()` only ORs `MFT_OWNERDRAW` into `fType`, and nothing
> anywhere clears `MIIM_STRING` (`rg MIIM_STRING` over `src/dll/src` finds no
> clearing site). The same object is what `InsertMenuItemW` receives.
>
> So the fourth row is the configuration Shell ships, and the work below would be
> spent on a solved problem — while carrying real risk, because `MSAAMENUINFO`
> claims the first four bytes of `dwItemData` and that is exactly where
> `MenuItemInfo::Signed()` looks for `cbSize` to recognise Shell's *own* item
> data, in the draw and teardown paths. `MenuItem.h:918` separately reads
> *foreign* `dwItemData` by checking that same offset for `MSAA_MENU_SIG`, so
> adopting it would make Shell's items indistinguishable from a host's by the
> discriminator the code already relies on.
>
> **Confirmed against a deployed Shell in a real `explorer.exe`, 2026-08-24.**
> The remaining doubt was that the probe proved the *mechanism* on a menu built
> the same way, not that Shell's own composed menu reaches a screen reader. A
> second probe asked that question directly: raise Shell's menu in Explorer by
> posting `WM_CONTEXTMENU` to `SHELLDLL_DefView`, then from a separate process
> — while Explorer's thread is blocked inside its tracking call, which is the
> situation a screen reader is in — walk the `#32768` window with
> `AccessibleObjectFromWindow` and `get_accName`.
>
> All 28 children reported correctly: 22 named menu items and 6 separators
> (unnamed, `role=separator`, which is right). `STATE_SYSTEM_HASPOPUP` on every
> submenu and `STATE_SYSTEM_UNAVAILABLE` on the greyed Paste. Names came through
> for all three item origins at once — mirrored natives (`View`, `Sort by`,
> `Properties`), NSS customs (`Terminal`, `File manage`, `Develop`, `Go To`) and
> packaged verbs (`Search Everything 1.5a...`, `Rename with PowerRename`).
>
> **§05.3 is closed as already satisfied.** Narrator reads menus through this
> same MSAA surface, so a separate Narrator pass would be confirming the
> transport rather than the product; the §06.5 matrix row is satisfied by the
> reading above. The `MSAAMENUINFO` design that used to occupy the rest of this
> section is deleted rather than deferred — it would spend real risk on a solved
> problem, for the `dwItemData` reason two paragraphs up.
>
> The documented fallback is worth keeping on record even so, because it is the
> sanctioned escape hatch if some future host does defeat this: "Provide an
> option to replace graphic menus with standard text menus when an accessibility
> aid is active ... If SystemParametersInfo returns TRUE with its uiAction
> parameter set to SPI_GETSCREENREADER, use standard menus"
> (<https://learn.microsoft.com/windows/win32/tablet/using-owner-drawn-menus>).
> Reach for that before inventing an `IAccessible` tree.

## 4. Keyboard completeness: mnemonics then type-ahead

Stage 1 — implement `WM_MENUCHAR` per the two documentation pages **together** (QA-01,
P0): the WM_MENUCHAR page defines the high word of the return as exactly *one* of
`MNC_IGNORE/MNC_CLOSE/MNC_EXECUTE/MNC_SELECT` — they are values, not combinable flags
— and *Using Menus* is decisive about the low word: for `MNC_EXECUTE`, "the low-order
word of the return value contains the **zero-based index** of the menu item". So return
`MAKELRESULT(position, MNC_*)` where position is the matched item's index in the
current popup's composed order — never its command ID (synthetic IDs ≥ 0x0FFFFFFF would
make Windows execute unrelated items). Match against stored mnemonics (`title.normalize`
already parsed, `MenuItem.h:198`); duplicate mnemonics cycle per press, Windows-native.
Replaces today's swallow cases (`ContextMenu.cpp:6590-6593`).

**Position-vs-ID is now measured, and the probe is committed.** Against a menu
whose items carry identifiers 5001…5005, replying `MAKELRESULT(2, MNC_EXECUTE)`
selects **5003** — the item at index 2 — and replying
`MAKELRESULT(5003, MNC_EXECUTE)` selects **nothing**: an out-of-range index is
refused, not reinterpreted as an identifier
(`src/tests/hostprobe/fixtures/question.menuchar_low_word_is_*`, Windows 11
26200.8875 x64, 2026-08-24). So the stated rule holds and the failure mode it
guards against is real: a synthetic ID at or above `0x0FFFFFFF` in the low word
would simply lose the keystroke. Stage 1 is unblocked.

One incidental behaviour to expect when implementing: the `WM_MENUSELECT` that
follows an `MNC_EXECUTE` reply carries `MF_MOUSESELECT`, although nothing was
clicked. Anything keying off that flag to distinguish mouse from keyboard will
be wrong for mnemonics.

**Stage 1 landed (2026-08-24)** as `Include/Mnemonics.h` plus
`ContextMenu::OnMenuChar`, with three further harness scenarios recorded because
the unit tests could only pin the decision, not the environment it runs in:

| Scenario | What it establishes |
|---|---|
| `question.menuchar_executes_an_ownerdrawn_item` | An index reply is honoured for an **owner-drawn** item — the only kind Shell renders, and one Windows cannot itself read. Returns 5003 for position 2, `WM_MENUSELECT` shows `HILITE\|OWNERDRAW` |
| `question.menuchar_select_moves_the_highlight` | `MNC_SELECT` moves the highlight and leaves the menu open, which is what a duplicated mnemonic needs on its first press |
| `question.menuchar_sees_the_current_highlight` | `MFS_HILITE` reads back correctly at `WM_MENUCHAR` time (`hilite=1` after navigating to position 1) — the cycling rule depends on it and nothing else records that fact |

Two decisions worth stating because they are not forced by the documentation:

- **An unmatched key returns `MNC_IGNORE`, not `MNC_CLOSE`.** A mistyped letter
  should not dismiss the menu the user was reading. This also makes handling the
  message free when there is nothing to match — the reply is byte-identical to
  what `DefWindowProc` returned before.
- **A disabled item does not swallow the keystroke.** It still draws its
  underline, but choosing it would do nothing, so an enabled duplicate below it
  gets the key instead of the press appearing to be ignored.

Stage 2 — prefix type-ahead: buffer chars for 1 s; select first visible match
(composed items + materialized natives); no lazy-tree violation: unmatched-deep search
stays out; optional "Search deeper…" item defers to a later phase (A2§20 caution).
Scope S each; large-menus usability multiplier.

**Stage 2 landed (2026-08-24)** as `Include/TypeAhead.h`, on the `WM_MENUCHAR`
path Stage 1 opened. It is the half that matters for a real menu: Stage 1 made
"E&dit with Adobe Acrobat" reachable by pressing D, which covers the items whose
titles happen to declare a mnemonic, and in a menu built from packaged verbs and
NSS rules most declare none.

Three decisions, none forced by documentation, each pinned by a test that fails
if it is reversed:

- **A prefix selects; it does not execute.** A unique *mnemonic* executes,
  because that is Windows' own rule and what makes a mnemonic a shortcut. A
  unique prefix must not: menus contain Delete, and somebody typing "de" looking
  for "Deselect" would have executed it before they saw it. Type-ahead moves the
  highlight; Enter chooses.
- **Mnemonics keep precedence on the first character.** A single keypress is
  tried as a mnemonic first and falls through to a prefix only when no mnemonic
  matched, so nothing Stage 1 shipped changes behaviour.
- **A character that matches nothing is not added to the buffer.** Otherwise one
  typo poisons the word for a whole second and every keystroke after it matches
  nothing either. Rejecting it leaves the last good prefix in place.

Two smaller things the sketch did not mention. The prefix is matched against the
title as stored rather than against a label built from it — mnemonic markers and
the accelerator column are skipped as the comparison walks — so a keystroke
allocates nothing. And the buffer clears when the *popup* changes as well as on
its timeout: a submenu is a new list, and carrying "de" into it would select
something the user never typed towards.

The lazy-tree rule is respected for free: the candidates are the items in the
`HMENU` that is open, which is what has already been materialised.

## 5. Smart multi-column overflow

Machinery exists on both sides: `cyMax` scrolling landed (`a3431df`, MENUINFO page
fetched: scroll bars automatic when items exceed cyMax) and NSS `column` maps to
`MFT_MENUBREAK/MENUBARBREAK`. Add `settings overflow=smart`: measure; if vertical
overflow and work-area width allows, insert group-aware breaks (prefer after
separators; cap 3 columns; else fall back to scroll). Pure presenter logic over the
existing measure pass. Scope S.

### 5a. As implemented (2026-08-24) — a number, not a mode, and two bugs the tests missed

Landed as `Include/MenuColumns.h` (pure, tested) plus
`ContextMenu::apply_smart_columns` (measure and `SetMenuItemInfo`).

**`settings { columns = 3 }`, not `overflow = smart`.** A cap says the thing the
user actually cares about — how wide they will tolerate the menu getting — and
it needs no new constant in the expression engine, which `smart` would have.
Unset, 0 and 1 all mean today's behaviour, so no existing configuration changes.

**Reading the rows back off the finished `HMENU` rather than planning during
insertion.** `Menu::insert` can add separators of its own on either side of an
item, so the position an item ends at is not the index the insert loop used —
and the plan needs *every* row, separators included, because they take vertical
space and are where the good breaks are. One `GetMenuItemInfo` per row, no
string retrieval.

**Two defects the unit suite did not catch, both found by looking at a real
menu.** A 100-item configuration in `hostprobe`'s shell-namespace scenario, on a
1920×1080 display:

| | menu window |
|---|---|
| before | 239 × 1031, scrolling |
| after | **938 × 990, four columns, no scrollbar** |

Getting there took two rounds:

1. **Group boundaries piled up in the last column.** Every break pulled back to
   a separator leaves height behind, and with a fixed per-column target three
   such pulls hand all of it to the final column — which came out at 1148 px
   against 1040 of screen, so the whole plan was thrown away and the menu kept
   scrolling. The target is now recomputed over what is left after every break.
2. **A break may not be so early that the rest cannot fit in the columns that
   remain.** The same shape, one round later: a boundary a few rows back looks
   free because it only shortens the column being closed. It is now rejected
   unless `prefix[at] >= total - columns_remaining × available_height`.

Both are pinned by `test_menu_columns.cpp`, including a sweep over separator
spacings that would have caught either.

**`ColumnPlan::refused` names the rule that declined.** Refusing is the common
answer and a legitimate one, so "nothing happened" needed to be
distinguishable from "this menu fits". It is what made the first defect
findable at all — the log said `column-too-tall`, which is the whole diagnosis
in one word.

## 6. Favorites & recents

Requires origin-stable identity — deliberately scheduled *after* `MenuModel`
(§04.4 step 6). Persist identities, never session wIDs: ExplorerCommand canonical GUID;
NSS rule id (file + rule ordinal hash); native items matched by normalized
parent-path+title signature (the same normalization used by modify-rule matching
today). Rendered as a pinned section with usage counters in ProgramData JSON. Privacy:
local only. Scope M. Unlocks "recently used", "pinned actions" — genuine productivity
stock Windows lacks.

## 7. Rule/context inspector

Modifier-hover or context submenu on any item ("Why is this here?") dumping, from data
the session already holds: source kind, matched modify/moveto rule locations (file:line
— parser retains import paths + rule ordinals after §04 changes), evaluation of the
rule's `where=` against current selection, construction timing from ring. Implementation
is mostly formatting of existing state; the long pole is threading rule-provenance
(file+line) through the parser — add during the §04 seam work while touching those very
files. Converts NSS opacity into approachability (A1§21). Scope M.

## 8. Explicitly deferred (consensus)

WinUI renderer; taskbar button/Jump-List takeover; universal async expressions;
out-of-process extension broker; per-user TreatAs as default (experiment flag only);
full live-filtering editor inside HMENU. Each rejected for scope-vs-value in A1§23,
A2§28, master plan §2.

## 9. ROI summary

| Feature | New capability for users | Effort | Depends on |
|---|---|---|---|
| Reliability Center | diagnose/slow-quarantine extensions | M-L | §02.6, §01.9 |
| Bypass gesture | instant native escape hatch | S | none |
| MSAA exposure | screen-reader usable menus | — | **already satisfied**, see §3 |
| Mnemonics/type-ahead | keyboard-complete menus | S | none — **both stages landed** |
| Smart columns | giant menus stay usable | — | **landed**, see §5a |
| Favorites/recents | personal muscle-memory layer | M | MenuModel |
| Inspector | explainable configuration | M | seams + parser provenance |
