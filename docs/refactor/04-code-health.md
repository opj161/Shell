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

## 7. Caches & memoization (after measurement)

- Icon cache key extension: resource/path + size + DPI (+ filetime for file assets);
  covers image files, editable SVG, `GetIcon`, `SHDefExtractIconW` paths (A2§19).
  Same synchronized/bounded/no-destructive-eviction contract as `BitmapCache.h`.
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
