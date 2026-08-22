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

## 2. One-shot native bypass ("Windows menu, this time")

Trivial at hook top (§01.7): modifier-gesture check before any Shell work; plus an
optional placeholder item type `menu(type='native')` rendering one item that, when
chosen, closes Shell's menu and replays the original call untouched. Value: debugging,
rescuing rules-hidden commands, comparison testing; makes aggressive configs safe to
live with (A2§26B). Scope S. Default gesture: `Ctrl+Alt+right-click` — deliberately not
Ctrl+Shift, which is the live config-reload combo (`Initializer.cpp:840-845`, evaluated
in the same hook body; see §01.7); documented in README + Reliability Center.

## 3. Accessibility: expose owner-drawn items via `MSAAMENUINFO`

Every rendered item is owner-drawn today; screen readers currently rely on fallbacks.
Microsoft's mechanism (page fetched): put `MSAAMENUINFO` **first** in the structure
pointed to by `dwItemData`; no IAccessible tree needed.

```cpp
struct OwnerDrawItemData {          // MenuItemInfo stays as-is; wrap at insert time
    MSAAMENUINFO msaa;              // must be first member
    MenuItemInfo* item;
};
```

Integration points:
- insertion sites that today do `set_data(this)` (`add_ownerdraw` paths) switch to
  handing out `OwnerDrawItemData*`;
- all internal `dwItemData` consumers switch to the two-field layout (grep set:
  measure/draw paths, `MenuItem.h` helpers);
- precedent for reading foreign MSAA layouts already exists (`MenuItem.h:918-924`),
  so the reverse-direction parsing is proven safe under SEH.
Constraint honored: no virtual functions in the wrapper struct (documented requirement:
"The MSAAMENUINFO structure cannot be a member in a class that contains virtual
functions … create an item data structure that contains MSAAMENUINFO as the first
member", <https://learn.microsoft.com/windows/win32/winauto/exposing-owner-drawn-menu-items>).

Two additions from the §07 audit:

- **Mirrored native items must not be rewrapped.** Shell already *reads* foreign
  `dwItemData` as `AASHELLMENUITEM` (`MenuItem.h:918-924`), and a mirrored native item
  may still carry the host's own layout. Only items Shell owns get the
  `OwnerDrawItemData` wrapper; anything whose `dwItemData` came from the host is
  passed through untouched. Getting this backwards would corrupt Explorer's own
  accessibility data, which is worse than the gap being fixed.
- **Record the documented fallback.** The same guidance offers a second sanctioned
  route: "Provide an option to replace graphic menus with standard text menus when an
  accessibility aid is active … If SystemParametersInfo returns TRUE with its uiAction
  parameter set to SPI_GETSCREENREADER, use standard menus"
  (<https://learn.microsoft.com/windows/win32/tablet/using-owner-drawn-menus>).
  That is the escape hatch if MSAAMENUINFO proves insufficient in the Narrator/NVDA
  pass (§06.5) — not a replacement for it, but the thing to reach for rather than
  inventing an `IAccessible` tree.
Tests: hosted test inserting items into a real popup and reading names back via
`IAccessible`/`AccExplorer`-style lookup is out of scope for CI; instead assert layout
invariants + `MSAA_MENU_SIG` presence at offset 0. Manual Narrator pass listed in §06
machine-matrix. Scope S-M; disproportionate usability win (A2§16).

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
Replaces today's swallow cases (`ContextMenu.cpp:6590-6593`). Position-vs-ID behavior
pinned by a §06.2 probe case before implementation.
Stage 2 — prefix type-ahead: buffer chars for 1 s; select first visible match
(composed items + materialized natives); no lazy-tree violation: unmatched-deep search
stays out; optional "Search deeper…" item defers to a later phase (A2§20 caution).
Scope S each; large-menus usability multiplier.

## 5. Smart multi-column overflow

Machinery exists on both sides: `cyMax` scrolling landed (`a3431df`, MENUINFO page
fetched: scroll bars automatic when items exceed cyMax) and NSS `column` maps to
`MFT_MENUBREAK/MENUBARBREAK`. Add `settings overflow=smart`: measure; if vertical
overflow and work-area width allows, insert group-aware breaks (prefer after
separators; cap 3 columns; else fall back to scroll). Pure presenter logic over the
existing measure pass. Scope S.

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
| MSAA exposure | screen-reader usable menus | S-M | none |
| Mnemonics/type-ahead | keyboard-complete menus | S | none |
| Smart columns | giant menus stay usable | S | none |
| Favorites/recents | personal muscle-memory layer | M | MenuModel |
| Inspector | explainable configuration | M | seams + parser provenance |
