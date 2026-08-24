# 04 — Code health: verified fixes, deletions, seams, targeted moveto

Everything here is behavior-preserving except the listed bug fixes. This doc absorbs
the P0/P2 items from `architecture-assessment-2026-08-22.md` §5 that are not covered by
01–03, merged with both audits' "leave alone / extract only" guidance.

---

## 1. Expression-engine correctness fixes (verified defects; fix + pin each)

| # | Defect | Fix | Test |
|---|---|---|---|
| 1 | Numeric `<` inverted: `case IDENT_LESS` computes `arg0 > arg1`; string branch correct (`FuncExpression.cpp:527-535`); no operand swap anywhere (grepped) | `:532` → `arg0 < arg1` | `test_expression.less_numeric` |
| 2 | `TernaryExpression::Copy` tests fresh object's null members → branches dropped (`Expression.h:140-148`) | copy `this->True/False` unconditionally | `test_expression.ternary_copy` (clone + eval both arms) |
| 3 | `FuncExpression::Copy` drops `Array` unless `Child`, and derefs it unguarded inside that branch (`IdentExpression.h:174-189`) | copy `Array` when non-null, independent of `Child`; guard deref | `test_expression.func_copy_array` via `foreach` clone |
| 4 | Duplicate import logs then falls through and re-loads (`Parser.cpp:1150-1162`: `break` exits scan loop only) | `continue` outer load on hash match | extend `test_parser.cpp` diamond-import case with counter |
| 5 | `IDENT_MSG_RIGHT → IDNO` (`Constants.h:21`) | map to `MB_RIGHT` | `test_expression.msg_right_const`. Evidence: the repo's own reference defines `msg.right` as "The text is right-justified" (`docs/functions/msg.html`) — i.e. `MB_RIGHT`; `MSG_FLAGS` values feed `MessageBoxW`'s uType directly (`FuncExpression.cpp:5150`, `:5165-5168`), so today `msg(msg.yesno \| msg.right)` produces `MB_YESNO\|7`: garbage flags, never right-justification |

Also adopt after a shipped-config grep for reliance: sentinel-hash `break/continue`
(`Expression.cpp:160-172`) → real `Context::Break/Continue` flags; self-mutating
`eval()` argument replacement (`FuncExpression.cpp:118-133`) → evaluate into local,
replace node only when safe; `Array2Expression::Copy` wrong type
(`LiteralExpression.h:81-84`). Each is small but touches evaluation semantics — land
with the trace harness green.

## 2. Dead-weight deletion (zero-risk list, all verified)

- Link pragmas `d2d1/dwrite/Winmm.lib` + orphan `OnDrawItem_D2D` declaration
  (`Main.cpp:50-52`, `ContextMenu.h:762` — never defined): removes three system DLLs
  from every host process import table.
- Never-installed `DllGetClassObjectHook` + Detours member (`Main.cpp:203,807-814`).
- Commented blocks: `Hooker.h` VnPatch*/InlineHook; evaluator tutorial `WindowProcedure`
  (`FuncExpression.cpp:75-87`), COM experiments `:1885-2030`; unused
  `TokenId/TokenType`; self-false `Ident::equals` (`Parser/Ident.h:168-180`);
  `peek_for` no-op (`Lexer.h:559-573`).
- Zero-user shared files: `StringBuffer.h`, `TString.h`, `Text/Buffer.h`,
  `Int.h` body, `MemoryManager.h`; commented-out Collections/* and auto_ptr block;
  `GC<T>` single user replaced by `std::vector<std::unique_ptr<MenuItemInfo>>`.
- Encoding consolidation: keep strict validator (`Encoding.h:159-204`) +
  `Unicode::From/ToUTF8`; delete four duplicate validators
  (`:760-801,:803-856,:858-880,:882-962`) and defective hand-rolled `Utf16ToUtf8`
  (`:625-649`, sign-broken surrogates).

  **`UTF8::From(const std::string&)` is deleted, not rewritten (§07 §2.2).** It has
  no callers anywhere in the tree and two defects that make it unusable if it ever
  gained one: it returns an *empty* string for input that is already UTF-8 (the
  result is only assigned inside the conversion branch), and its validity test
  cannot distinguish BOM-prefixed UTF-8 from ANSI — `Encoding::GetType` returns
  `UTF8BOM`, not `UTF8`, for a BOM, so a BOM'd UTF-8 string would be re-decoded
  through `CP_ACP`. Rewriting a dead function to use the strict validator preserves
  the second defect and adds nothing; `Encoding::GetType` is the validator and
  `Unicode::From`/`UTF8::FromUnicode` are the conversions.
- String class arm-disarming: fix shallow-copy `assign(const string&)`
  (`string.h:651-653`), bounds-check `operator[]` (`:1931-1935`), constrain template
  conversion operator (`:1963-1964`).

  **"Bounds-check `operator[]`" needs saying precisely (§07 §2.1).** The obvious
  reading — clamp an out-of-range index to the last character, `m_length - 1` — is
  wrong, and was implemented that way once. `at()` has always returned `L'\0'` out
  of range, and the class invites the `while(s[i])` idiom; clamping to the last
  character turns every such loop into an endless one, and makes `at()` and
  `operator[]` disagree about the same input. The contract is: **every read at or
  beyond `m_length` is `L'\0'`, by all three routes.** The non-const overload
  returns `m_data[i <= m_length ? i : m_length]` — index `m_length` is the
  terminator `terminate()` always writes — and the const overload simply delegates
  to `at()`. Pinned by `test_string_index.cpp`, which fails if the clamp comes back.

  Note also that "semantics already pinned by existing suites" was not true: nothing
  covered indexing or copy assignment before that suite was added.
- PlutoVGWrap fixes: missing return in `clear()` (:428), byte-vs-pixel indexing (:210),
  explicit-dtor-reuse (:475/482). CommandLine explicit-dtor-reuse (`CommandLine.h:187-188`).
  Make `auto_handle`/`File` non-copyable; remove fake `RegistryKey` refcount
  (`Registry.cpp:165-173`).

## 3. System-setting isolation (cross-ref §02.4)

SPI mutations removed/gated as specified there; `showdelay` becomes an explicit,
documented opt-in instead of transient toggling. Default stock config comment about
`showdelay=200` stays accurate ("left unset ⇒ user setting applies").

## 4. Seam extraction order (strangler; no rewrite)

Merged consensus of A1§10 and A2§29, adjusted so every step lands user-visible value
first:

1. `PackageCatalogService` (§02.1) — value + first extraction.
2. `TakeoverSession` shell around hook body (§01.2) — pure consolidation.
3. `NativeMenuBridge` (INIT/UNINIT pairing) (§01.5) — correctness.
4. `CommandDispatcher` + origin table (§01.4) — contract fidelity.
5. `HostContextDetector` / `SelectionProvider` layering (capture-first; Explorer
   `IShellBrowser` becomes enrichment only — A1§12) — compatibility.
6. `MenuModel` neutral representation `{Native|Custom|ExplorerCommand}` — prerequisite
   for §05 features.
7. `Win32MenuPresenter` last (paint/window events leave `ContextMenu.cpp` only after
   1–6 are stable).

Rule throughout: move code, don't improve it in the same commit; each seam lands with
its unit suite where pure (lifecycle/policy/table logic) or harness coverage where
hosted.

### 4a. What each remaining step actually costs — audited 2026-08-24

Steps 1–4 landed. Steps 5–7 were being treated as one block gated on the same
prerequisite, and they are not:

| Step | Where the code lives | Linked by `tests.vcxproj`? | Gate |
|---|---|---|---|
| 5 — selection layering | `Selections.cpp` (1,496 lines); `QueryShellWindow` at `:1308` | **yes** | none |
| 6 — `MenuModel` | `ContextMenu.cpp` (7,559 lines) | no | rendering coverage |
| 7 — `Win32MenuPresenter` | `ContextMenu.cpp`, `OnDrawItem` `:1828`–`:2873` and `CreateLayer` `:6368`–`:6657` | no | rendering coverage |

**Step 5 is unit-testable today.** `tests.vcxproj` has included
`..\dll\src\Selections.cpp` since `test_selection_path_resolver.cpp` was
written, so the capture-first / enrichment-second layering §5 describes can land
with a suite against the real object, in the file it already lives in, without
the composed-menu rendering harness that 6 and 7 need. `08-handoff.md` §3.6 used
to block all three on that harness; that was true of 6 and 7 and never of 5.

The ordering that follows is: **5 first**, because it is provable here and it
shrinks what 6 has to move; then the rendering coverage; then 6; then 7.

### 4b. Step 5 as implemented (2026-08-24) — and the defect it turned out to fix

Landed in two commits, per this section's own rule.

**The move.** `QuerySelected` was one ~300-line function doing two unrelated
things. It is now a dispatcher over two named providers:
`QuerySelectedFromShellBrowser` (walk up from the popup's window for an
`IShellBrowser`, ask it for the active view, read the selection off it) and the
existing `QuerySelectedFromHandler` (read the selection the host handed Shell
through `IShellExtInit`). The 271 moved lines were verified **byte-identical**
to their originals rather than reviewed by eye — the extraction was done by line
range, so nothing was retyped.

**The defect the seam exposed.** Splitting them made visible something hidden
inside the long function: **`Window.has_IShellBrowser` reads like a fact and is
a hypothesis.** Nothing queries an `IShellBrowser` to set it;
`QueryShellWindow` sets it from the popup window's *class hash* —
`SHELLDLL_DefView`, `SysListView32`, `ShellTabWindowClass`, `SysTreeView32`.
Those are Explorer's classes, and they are equally the classes of every
third-party file manager that embeds the real shell view instead of writing its
own.

For such a host the hypothesis fails in the worst direction: the window is
classified as Explorer's, so the browser provider is asked, so the handler is
never asked — and the host had *already handed Shell the selection*. The menu is
composed against nothing. That is the same defect the provider's own comment
describes ("third-party file managers only ever got theming"), fixed for hosts
whose window class does not look like Explorer's and left in place for hosts
whose class does.

**The rule, and why it is deliberately narrow.** `Include/SelectionRoute.h`
holds the policy, pure and tested. The handler answers after the browser
provider only when **no `IShellBrowser` was found at all** — the one failure that
provably happened before anything was read. Every later failure happens after
`Parse` may have run, and `Parse` appends to `Items` and sets the FSO type
counters; the handler appends too, so falling through there would merge two
selections or count an item twice. "The browser answered and selected nothing"
is a real answer as often as it is a failure, and is not second-guessed.

`test_selection_route.cpp` pins both rules and, separately, that they stay
*asymmetric* — the tidier symmetric version of either is wrong, and neither
mistake fails anything else in the tree.

**Verified.** Three platforms green, 32,547 checks, harness 23 native and 27
through takeover — the four `takeover.*` scenarios exercise the capture provider
against a real borrowed shell menu. In a real Explorer the composed desktop menu
is unchanged.

**Not verified, and it is the whole point of the change:** a third-party host
that embeds the shell view. That needs one of the hosts §3.1 of the handoff
describes, with a lister open. The code path is covered; which shipping software
takes it is still a survey question.

That last check nearly produced a false alarm, and the lesson is now in
`AGENTS.md`: the reading taken four seconds after the deploy's Explorer restart
showed 29 items including two Directory Opus entries and the *native* `New`
where the settled menu has Shell's `New+`. Steady-state readings are 27, against
a 28-item baseline differing by one transient OneDrive item. A menu read during
Explorer's startup measures Explorer settling, not the change.

## 5. Selection layering detail

Capture-first rule concretely: `ShellExtMatch` result (exact HMENU-bound capture)
wins whenever semantically sufficient; Explorer window archaeology
(`WM_GETISHELLBROWSER`, class-hash walks in `Selections.cpp`) demotes to enrichment
providers for Home/QuickAccess/Libraries/tree/background specials. `window.is_contextmenuhandler`
already expresses this capability split in config (`shell.nss:6-16`); runtime mirrors it.

## 6. Targeted `moveto` discovery (third native policy)

Today any location-bearing moveto forces whole-tree eager init
(`choose_native_tree_policy`, `NativeMenuLazy.h:43-51`; gate at
`ContextMenu.cpp:4894-4915`). Add:

```cpp
enum class NativeTreePolicy { Lazy, TargetedDiscovery, LegacyEager };
```

At config publish time classify each parent-moving rule:
- **Deterministic**: `location` resolves to a literal path of submenu titles
  (constant segments only) ⇒ record `target_path[]`.
- **Dynamic/wildcard** ⇒ LegacyEager fallback (unchanged).
Runtime: for deterministic rules walk only the named ancestors — initialize
`Open with` to find its child, not the other nine root submenus. Unrelated subtrees stay
lazy. Rules whose `where=`/title depend on `this` of not-yet-materialized items must be
classified dynamic (conservative default; costs latency, not correctness).

Miss and interference semantics (QA-11): classification from rule text alone cannot see
cross-rule effects — another `moveto`/rename may create or retitle a literal ancestor —
so the targeted walk verifies every expected ancestor (title hash + position sanity) as
it descends, and on **any mismatch aborts targeted discovery for that rule and falls
back to LegacyEager** for the remainder of the session. A miss never silently skips the
configured move: dropping it would be a correctness regression; paying eager cost is
the accepted trade (latency over correctness — the same policy the lazy-init gate
already takes).

Tests: pure classifier suite (rule text → class) + real-window test mirroring
`test_native_menu_lazy.cpp` asserting unrelated popups never receive INIT under
TargetedDiscovery.

### 6a. As implemented (2026-08-24) — measured first, and two departures

**The prize was measured before anything was built**, because this section
asserted a "whole-tree cost when triggered" without a number. Forced through the
existing `modify.native_eager` override on a real Explorer (Windows 11
26200.8875 x64, six menus each):

| | pre-display |
|---|---|
| `Lazy` | ~13 ms warm, ~60 ms on the first menu in a process |
| `LegacyEager` | **95.1 ms average**, 31.7 – 359.6 ms |

One third-party submenu accounted for 22.2 ms of a 33.7 ms menu. So the cost is
real and this section was right to want it gone.

Then the same comparison with an actual rule —
`modify(in="View" find="Large icons" menu="")` in the installed `shell.nss` —
rather than through the override:

| | `popup_init` per menu | pre-display average | max |
|---|---|---|---|
| `LegacyEager` | 4 | 85.5 ms | 322.1 ms |
| `TargetedDiscovery` | 2 | **20.2 ms** | 59.1 ms |

Four times faster, and the menus are identical: "Large icons" moves out of View
to the root under both policies, read back through `IAccessible`.

**Departure 1: classification happens at menu time, not at config publish.**
This section proposed classifying each rule's `location` when the configuration
is parsed. That needs a way to ask an `Expression` whether it is constant, and it
throws away the selection — which the rule's own `where` and `fso` already depend
on. By the time `choose_native_tree_policy` runs, `Initialize()` has the context,
the selection and the rule list in front of it, so the locations are simply
*evaluated*, with `_this` cleared because no native item exists yet. A rule that
will not evaluate, or that names the wildcard, falls the whole menu back to
`LegacyEager` — this section's own "conservative default; costs latency, not
correctness".

**Departure 2: both ends of a move are targets.** The sketch collects `location`.
But `moveto` names the submenu an item is moved *into*, and that destination is
resolved through `__map_system_menu`, which only holds levels that were
enumerated. Collecting sources alone would have sent moved items to
`__movable_system_items` instead — a silent behaviour change rather than a slow
menu, which is exactly the class of failure QA-11 is about.

**The miss semantics turned out simpler than QA-11 expected.** There is no
separate targeted *walk* to abort: the decision is made during the ordinary root
enumeration, one submenu at a time, and a target naming a submenu that does not
exist simply never matches. Nothing is opened for it and the rule matches
nothing — the same outcome `LegacyEager` reaches. So no "abort and fall back for
the session" state is needed, and none exists.

**Two things written as failures first**, both corrected before they shipped and
both found by asking what a real configuration looks like rather than by a test:

- An empty value is not "unknown". For a `location` it means the root level; for
  a `moveto` it means the item moves to the root — which is where `menu=""`, the
  commonest rule shape there is, moves things. Treating it as unknown sent
  exactly that case back to the eager walk.
- An empty target *set* is a legitimate answer, meaning every applicable rule
  turned out to be root-only. It means "descend into nothing", which is correct
  and strictly better than eager.

**The wildcard set is exactly one string**, read off `is_location` rather than
assumed: it returns true for a location of exactly `*`, strips one asterisk from
a `**` pair, and compares everything else for equality — so `*foo` would only
match a submenu genuinely called that. Treating any leading asterisk as a
wildcard would be safe, and would give up the optimisation on literal locations.

Tests: `test_native_menu_targets.cpp` pins the path arithmetic, and each rule was
checked to catch its defect — a character-wise prefix test (so "open" counts as
an ancestor of "open with", handing most of the saving back), an empty location
becoming a target that is a prefix of everything, and any leading asterisk
counting as a wildcard. The real-window half of this section's test plan is
covered by the measurement above rather than by a unit test: `TargetedDiscovery`
lives in `ContextMenu.cpp`, which the test project does not link, and
`native.popup_init` appearing twice per menu instead of four times is the same
assertion made where it matters.

## 7. Caches & memoization (after measurement)

- Icon cache key extension: resource/path + size + DPI (+ filetime for file assets);
  covers image files, editable SVG, `GetIcon`, `SHDefExtractIconW` paths (A2§19).
  Same synchronized/bounded/no-destructive-eviction contract as `BitmapCache.h`.

  **Measured and declined for the packaged-verb path (2026-08-24).** Of the
  ~32 ms an icon costs there, only ~11 ms is the extraction a resource-string
  cache could remove; the other ~18 ms is the `GetIcon` COM call, which is
  per-selection and cacheable by nothing. Eleven milliseconds does not justify
  adding a second class of borrowed bitmap to a field that had just been given
  unambiguous ownership — see §02.2a-ii, and the GDI leak that clarifying it
  uncovered. Revisit if the ring's p95s ever show icon work mattering.
- Per-session memoization whitelist (A2§20): literals, `sys.*`, `theme.*`, pure string
  ops, one-shot DWM color reads at snapshot build; explicitly exclude
  `io/reg/clipboard/input/cmd`. Keyed `(Expression*, selection index)` inside session;
  freed on close. Ship behind perf-flag measurement first (AGENTS.md rule).
- Lazy large-selection metadata (A2§21): defer until `selection.preparing` shows up in
  ring-buffer p95s; verbs guidance (first item + count) supports the eventual design.

## 9. `CoCreateInstanceHook`: diagnostics and policy share a branch (§07 A8)

Verified defect, not previously in this plan. `CoCreateInstanceHook`
(`Main.cpp:761-770`):

```cpp
bool test = Keyboard::IsKeyDown(VK_MENU);
if(test)
{
    Timer t; t.start();
    hr = _CoCreateInstance.invoke(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    t.stop();
    _log.write(...);
    return hr;                    // <-- returns before the statics loop
}

for(auto si : cache->statics)     // CLSID suppression lives here
    ...
```

Holding **Alt** while a context menu opens therefore bypasses the entire CLSID
blocklist for every activation in that window. The unreachable `if(test && *ppv)`
inside the suppression loop shows the coupling was never intended. Nothing tells
the user; a suppressed extension simply reappears.

Fix, folded into the §01.9 policy compile: evaluate the policy first and return
`E_NOINTERFACE` for a blocked CLSID regardless of modifier state; make the Alt-timing
path *wrap* the activation it decided to allow rather than replacing the decision.
The timing probe is a diagnostic and belongs behind the same
`ComActivationPolicy::may_affect` fast path as everything else.

Test: policy-level unit test (blocked CLSID stays blocked with every modifier
combination) — the policy object is pure data, so this needs no COM.

## 8. Acceptance criteria

- [ ] All §1 fixes landed with named tests; upstream-facing configs in
      `src/bin/imports/**` evaluated against fixed `<` semantics (grep for `<` usage in
      numeric contexts before landing #1).
- [ ] rg gates: no `d2d1` pragma, no commented-out subsystem headers included by pch.
- [ ] Seam commits are individually revertible; `git bisect` friendly.
