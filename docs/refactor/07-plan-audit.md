# 07 — Critical audit of the takeover refactor plan and Phase 0

**Date:** 2026-08-22 · **Tree audited:** `refactor/takeover-master-plan` @ `abd2ad9` + uncommitted Phase 0 working tree
**Scope:** `docs/refactor/00`–`06`, the Phase 0 diff, `scripts/check-invariants.ps1`
**Method:** every plan claim I rely on below was re-opened at its cited `file:line` in this tree;
every Win32 contract was re-fetched from `learn.microsoft.com` this session (TrackPopupMenuEx,
WM_UNINITMENUPOPUP, WM_MENUCHAR, Using Menus, MSAAMENUINFO, SystemParametersInfo);
`SPIF_*` values read from the installed SDK header; `build.ps1 -Platform x64` and
`check-invariants.ps1` executed.

**Verdict in one line:** the plan's *shape* is right and unusually well evidenced; its
*ordering, emphasis and three specific prescriptions* are wrong, and Phase 0 shipped
without any of the gates the plan itself defined.

---

## 1. Phase 0 as implemented — process gaps

Phase 0 is **uncommitted**, in one blob of 32 files (`+109 / −2746`). `06-phases-and-tests.md`
required nine independently revertible commits, each with named tests.

| Plan requirement (§06 Phase 0) | Actual state |
|---|---|
| 9 commit-sized, individually revertible commits | 1 uncommitted working tree |
| `test_expression.less_numeric`, `.ternary_copy`, `.func_copy_array`, `.msg_right_const`; diamond-import counter; extended `test_encoding` | **none exist.** `src/tests/` is unmodified; there is no `test_expression.cpp` |
| "Items 0.1–0.4 land **after** the trace-harness baseline exists (§2)" | `src/tests/hostprobe/` does not exist |
| §06.3 rg gates "Fails PRs" | `scripts/check-invariants.ps1` is untracked and referenced by neither `build.ps1` nor `.github/workflows/build.yml` |

Build and suite are green — I ran `.\build.ps1 -Platform x64`: 25 080 checks, 0 failures. All
25 080 are pre-existing; **not one line of Phase 0 is covered by a test.** For a change set whose
stated purpose is "correctness floor" and which alters expression-evaluation semantics, that
inverts the plan's own risk model.

### 1.1 `check-invariants.ps1` fails on the very tree it was written for

```
INVARIANT VIOLATION [Recycle Bin query must not return to menu construction]
  src\dll\src\ContextMenu.cpp:4575: // removed SHQueryRecycleBinW(nullptr, ...) recomputation enumerated
  src\dll\src\ContextMenu.cpp:4578: // learn.microsoft.com/.../nf-shellapi-shqueryrecyclebinw).
check-invariants: 2 violation(s)   (exit 1)
```

The rule matches the explanatory comment the same change added. The script was never run.

Two further defects in it:

- `Get-ChildItem -Path 'src\dll\src\**\*.h'` — PowerShell's `**` is **not** a recursive glob. It
  resolves exactly one directory level: 43 of 46 headers. Missed: `pch.h`, `dija.h`,
  `Include\Diagnostics\MenuPerf.h`. Use `-Recurse -Filter`.
- The `this->~Type()` and `memcpy(this,` rules scan `src\dll\src` only, but the explicit-destructor
  and whole-object-copy fixes Phase 0 actually made are in **`src/shared`**
  (`Library/PlutoVGWrap.h`, `System/CommandLine.h`). The gates do not cover the code they exist for.

## 2. Phase 0 as implemented — correctness

Verified correct, each against its cited evidence:

- `FuncExpression.cpp:532` `>` → `<` (numeric branch was inverted; string branch was already `<`).
- `TernaryExpression::Copy` — tested the *fresh* object's members, always null, so both branches
  were dropped. Fix copies `this->True/False`.
- `FuncExpression::Copy` — `Array` now copied independently of `Child`, deref guarded.
- `Array2Expression::Copy` returned a `NumberExpression`. Fixed.
- `Parser.cpp` duplicate import — `break` exited only the scan loop; now returns.
- `Constants.h` `IDENT_MSG_RIGHT` `IDNO` → `MB_RIGHT`, matching `docs/functions/msg.html`.
- `GC<MenuItemInfo>` → `std::vector<std::unique_ptr<MenuItemInfo>>` — pointee stability preserved.
- `RegistryKey::m_ref` removal — I checked all 13 explicit `.Close()` sites: every one operates on a
  prvalue-initialised key (guaranteed elision, sole owner), and `is_system_key()` guards the
  statics. **Benign**, but the plan asserted it without this check and nothing pins it.

Three items need attention before this is committed.

### 2.1 `string::operator[] const` — silent contract change (unpinned)

```cpp
// before                              // after
return at(index);                      if(!valid()) return null;
                                       return m_data[index < m_length ? index : m_length - 1];
```

`at()` returns `null` (`L'\0'`) for an out-of-range index. The replacement returns **the last
character**. Any caller that scans until it reads `0` — the idiom this string class invites, since
`at()` was safe for exactly that — now reads the final character forever instead of terminating.
`at()` and `operator[]` now disagree about the same input. The non-const overload picked a third
behaviour (clamp, with an `m_length ? … : 0` guard the const one omits — safe only because
`valid()` already implies `m_length > 0`).

This was listed as "bounds-check `operator[]`". Bounds-checking to `null` would have preserved the
contract; clamping changed it. Either revert to `at()` semantics or pin the new contract with a test
and a grep of every `[]` call site.

### 2.2 `Encoding::UTF8::From(std::string)` — new latent corruption

`is_utf8()` (structural, BOM-agnostic) was replaced by `Encoding::GetType(...) != UTF8`. `GetType`
returns **`UTF8BOM`** — not `UTF8` — for BOM-prefixed input, so BOM'd UTF-8 now takes the
`MultiByteToWideChar(CP_ACP, …)` branch and is decoded as ANSI. Currently harmless: `UTF8::From` has
**zero callers** in the tree. That is the argument for deleting it, not for rewriting it into a
landmine. (The new strict BOM-less validator in `GetType` itself is a genuine improvement.)

### 2.3 Recycle Bin removal — the guard inverts the plan's premise

The deleted block was gated on `item->disabled` already being **true**:

```cpp
if(!item->is_menu() && is_root && item->disabled)     // ← only when Explorer said "disabled"
    if(item->uid() == IDENT_ID_EMPTY_RECYCLE_BIN) { item->disabled = false; …query…; }
```

It existed to *override a false negative* — Explorer marking "Empty Recycle Bin" disabled when the
bin is not in fact empty. The replacement comment asserts the opposite ("Explorer disables the
command exactly when the bin is empty"); that assertion is the thing the original code was written
because someone did not believe. Two consequences the plan mis-states:

- **Cost.** `SHQueryRecycleBinW(nullptr, …)` ran only for the Recycle Bin item, only when already
  disabled — not on "the menu path" generally, as `02-first-paint-latency.md §4` implies.
- **Risk.** The regression, if the premise is wrong, lands precisely on the case the workaround
  targeted, and is invisible (a greyed item, no error).

The plan's own acceptance criterion was "rg gate + **manual recycle-bin menu pass**". Do the manual
pass, or keep the query behind the async catalog refresh instead of on the thread.

---

## 3. Plan audit — what holds up

These are correct, checked against both tree and documentation, and are the plan's real value:

- **The custom-command drop for non-`TPM_RETURNCMD` hosts is real.** Confirmed: custom wIDs come
  from `ident.get_id()` starting `0x0FFFFFFF` (`ContextMenu.h:457-469`); the hook never adds
  `TPM_RETURNCMD` (`Main.cpp:889-975`); without it `TrackPopupMenuEx` "returns nonzero if the
  function succeeds", so `InvokeCommand(1)` fails `ident.equals(1)` and falls through to
  `delete this; return id`. The user's chosen command never runs.
- **INIT/UNINIT asymmetry is real.** WM_UNINITMENUPOPUP: *"If an application receives a
  WM_INITMENUPOPUP message, it will receive a WM_UNINITMENUPOPUP message."* Shell sends INIT to
  borrowed popups (`NativeMenuLazy.h:108-132`) and never sends UNINIT (`ContextMenu.cpp:1581-1607`
  destroys the synthetic menu only).
- **QA-01 is right and well sourced.** *Using Menus*: *"the low-order word of the return value
  contains the zero-based index of the menu item to be selected"* for `MNC_EXECUTE`. Returning a
  synthetic ID ≥ `0x0FFFFFFF` there would execute an unrelated item.
- **Catalog scan on the menu thread is real** — and worse than stated: `catalog_snapshot()`
  (`ExplorerCommand.cpp:106-125`) returns the registration vector **by value**, so every menu pays a
  deep copy even on a cache hit.
- **`config_has_changed()` is dead code** (QA-05). Confirmed: all nine `has_error()` call sites use
  the `detect_changes = false` default.
- The R1/R2 framing, the explicit `TakeoverDecision` enum over today's implicit `__finally`
  fail-open, the strangler seam order, and the probe-gating discipline are all sound.

---

## 4. Plan audit — defects, in order of consequence

### 4.1 §02.5 Taskbar "Stage 1 zero-wait" re-proposes a decision this repo already rejected

`AGENTS.md`, "The plan is not the specification either":

> One proposal — never blocking the taskbar thread on the UIA worker — **would have broken the
> first right-click of every sequence**; the documentation supplied the correct primitive
> (`CoWaitForMultipleHandles`, which enters the COM modal loop on a single-threaded apartment).

`02-first-paint-latency.md §5` proposes exactly that again — `miss → return false immediately` —
and calls it "Stage 1 (small diff, immediate)", "pure improvement", "lands behind nothing". The
acceptance criterion ("added latency ≤ native baseline ±2 ms") is trivially met *because Shell's
menu does not appear*: `return false` hands the click to Windows.

The proposed prewarm does not close it. The suggested trigger — right-button-down on the taskbar
(`Main.cpp:1247`) — is *the same click*, giving a few milliseconds of head start against a first
UIA query the code itself measures at **~28 ms** (`Main.cpp:277-282`). Hover prewarm misses
keyboard invocation and any click that beats the throttle.

**Recommendation:** drop Stage 1 entirely. Ship **Stage 2** — the worker publishes a plain-data
`std::vector<TaskbarTarget>` rectangle layout, invalidated on taskbar recreate /
`WM_DISPLAYCHANGE` — and the UI thread does rectangle hit-testing with no COM and no wait. Stage 2
is the actual fix; Stage 1 is a regression that Stage 2 makes unnecessary. Keep the
`CoWaitForMultipleHandles` budget as the fallback for a cold layout.

### 4.2 §03 last-known-good does not fix the failure it describes

`Initializer` is **per-process** and `_snapshot` is **in memory only** — every host process parses
`shell.nss` for itself. Tracing the actual behaviour:

- In an **already-running** process, `init()` is not re-run after a bad save (the poll is dead code,
  §3 above), so `Status.Error` is never set and menus keep working. The plan's headline symptom
  does not occur here.
- In a **newly started** process — a freshly launched app, or Explorer after a restart —
  `init()` runs, `parser.Load()` fails, `Status.Error = true`, and both `query()`
  (`Initializer.cpp:154-155`) and `DllGetClassObject` (`Main.cpp:1455`) refuse. **This is the
  failure.** And it is precisely the case where there is no previous in-memory snapshot to serve.

So `§03.2`'s fix — "serve the stale `_snapshot`" — helps only the process that was not broken, and
does nothing for the one that was. The plan's own acceptance test ("Save invalid config while menus
working → menus continue") passes today for the wrong reason.

**Recommendation — the fix that matches the failure mode:**

1. On every successful parse, write a **shadow copy** of the resolved config set (`shell.nss` plus
   each `_imports` entry, content-addressed) to `%LocalAppData%\Nilesoft\Shell\lkg\`.
2. On parse failure at process start, parse the shadow instead, publish it, and set
   `Status.Stale` — menus survive an Explorer restart with a broken config.
3. Add `shell.exe -check [file]` — parse-and-report without publishing. Cheapest possible
   prevention, and the thing users will actually run before saving.

Then the in-memory stale-serve of §03.2 is still worth having, but as the cheap second half.

### 4.3 R1 does not forbid the largest unbounded pre-paint cost

R1 bans "package enumeration, manifest disk I/O, UIA calls, unbounded registry traversal,
`GetState(TRUE)`". It does not ban **activating a third-party verb handler**, which is unbounded by
construction and happens for every packaged command before first paint:

```cpp
// ExplorerCommand.cpp:186-189
CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
                 IID_IExplorerCommand, …);
// then, synchronously, per command: GetState → GetTitle → GetFlags → GetIcon
```

`CLSCTX_LOCAL_SERVER` means a surrogate **process launch** can sit between the user's right-click
and the first pixel. There is no budget anywhere on this path. `02-first-paint-latency.md §2`
removes only the `GetState(TRUE)` retry (`:204-206`); `05-capabilities.md §1` *measures* the rest
afterwards via `ProviderHealth` but nothing *bounds* it.

**This is the single highest-value item missing from the plan**, and it is what "stabilise takeover"
most concretely means to a user. The mechanism is already blessed by this repo:

- Per-provider deadline enforced by the existing worker pattern + `CoWaitForMultipleHandles`
  (same primitive, same reasoning as the taskbar path).
- A provider that misses its deadline is **omitted from this menu and its result cached for the
  next one** — so the cost is paid once, off the visible path, exactly like the taskbar hit cache.
- Extend `PackageCatalogService` to cache **presentation** (state, title, icon) keyed by
  `(CLSID, selection shape)`, not just registrations. §02.2 already picks that key for state;
  widening it to title/icon turns the 2nd..Nth menu into pure local work.
- Surface "deferred because slow" in the Reliability Center — which gives that flagship feature
  something actionable to say rather than only a timing table.

### 4.4 §01.3 makes `TPM_NONOTIFY` the default, then has to engineer around it

The plan's rule is "internally always track with `TPM_RETURNCMD | TPM_NONOTIFY`". Checked against
the TrackPopupMenuEx page: the two flags are grouped as *"flags to control discovery of the user
selection without having to set up a parent window for the menu"*, and the `hwnd` note says the
owner *"does not receive a WM_COMMAND message from the menu until the function returns"*. **Nothing
documents that NONOTIFY is required alongside RETURNCMD**, and nothing enumerates what NONOTIFY
suppresses — which the plan concedes (QA-02) and then schedules a probe for.

Forcing NONOTIFY deliberately destroys the `WM_MENUSELECT` stream that third-party hosts use for
status-bar hints — and §01.3 then proposes *synthesising* `WM_MENUSELECT` to repair damage the rule
itself caused. That is scope generated by the default, not by the requirement.

**Recommendation:** invert the default. Add `TPM_RETURNCMD` only (that is what the selection-capture
requirement actually needs); probe whether a duplicate `WM_COMMAND` reaches the owner; add NONOTIFY
only for the host classes where it does. Smaller diff, nothing to synthesise, identical fidelity
goal, and the probe becomes a confirmation rather than a prerequisite.

### 4.5 §02.4 prescribes something more invasive than the defect

Verified: the `showdelay` mutation is **already opt-in** (only when the config sets it,
`ContextMenu.cpp:3915-3927`) and **already restored** (`:4975-4980`). The plan proposes removing it
and instead offering an opt-in that *permanently changes the user's real system setting* — strictly
more invasive than the transient toggle it replaces.

The actual defect is the fourth argument. `SPIF_SENDCHANGE` is `SPIF_SENDWININICHANGE` (SDK
`WinUser.h:12778-12780`), i.e. a **`WM_SETTINGCHANGE` broadcast to every top-level window on the
desktop — twice per right-click, on the menu thread.** That is a real, measurable, system-wide cost
and it is one character to fix:

```cpp
::SystemParametersInfoW(SPI_SETMENUSHOWDELAY, _showdelay[1], nullptr, 0);   // was SPIF_SENDCHANGE
```

`fWinIni = 0` is transient-only and is exactly what the adjacent `SPI_SETSELECTIONFADE` calls
already do (`:6551/6553` — and those pass the BOOL in `pvParam`, which is correct per the
SystemParametersInfo UI-effects table). Keep the feature; delete the broadcast.

### 4.6 The trace harness is the linchpin and is scheduled nowhere

`06-phases-and-tests.md §2` calls it *"the single highest-leverage testing investment"*. Gated on
it: Phase 2.3 (HostContract), §05.4 Stage 1 (mnemonics), the backend-coverage experiment, the
NONOTIFY rule freeze, the UNINIT-tolerance divergence, and — by the plan's own note — Phase 0 items
0.1–0.4. It appears in **no phase's work list**. Predictably, Phase 0 shipped without it.

Make it Phase 0 item 1. Everything downstream is blocked on it, and it is the only artefact in the
plan that converts "we reasoned about the message stream" into evidence.

### 4.7 The persistent catalog cache adds a trust boundary the plan does not analyse

`§02.1.3` writes `%LocalAppData%\Nilesoft\Shell\cache\catalog.v2` and reads it in **every host
process** — and Shell loads into whatever process raises a shell context menu. If any such host runs
elevated, a medium-integrity-writable file influences high-integrity **CLSID activation**. The plan
has one sentence of mitigation ("corroborated against the live package repository … before its
CLSID is ever activated"). That needs to be a stated invariant with a test asserting no cached field
reaches an activation path uncorroborated — not a hardening bullet inside a design note.

More basically: **measure the benefit first**, per this repo's own rule. Warm-on-start already
removes the stall; persistence buys only the first second or so after Explorer start, in exchange
for a new trust boundary, an on-disk format, multi-writer swap logic, and fail-closed parsing.
Ship the in-memory async service, measure cold start, and only then decide whether persistence earns
its complexity.

### 4.8 Smaller items

- **`CoCreateInstanceHook`: the CLSID blocklist is bypassed whenever Alt is held.**
  `Main.cpp:761-770` — `if(Keyboard::IsKeyDown(VK_MENU))` times the activation, logs, and
  **returns before the `statics` suppression loop**. The dead `if(test && *ppv)` inside that loop
  shows the coupling was unintended. Diagnostics and policy should not share a branch. Not in the
  plan; fold into §01.9's policy compile.
- **The `<` fix changes the meaning of every shipped and user-authored config.** §06 Phase 0.1 says
  grep `src/bin/imports/**` first — good, but there is no gate, and third-party `.nss` files in the
  wild will change behaviour silently. Worth a release note and one release of dual-evaluation
  logging under the `perf` flag.
- **§05.3 MSAA:** the same documentation offers a second sanctioned route (`SPI_GETSCREENREADER` →
  fall back to standard menus) worth recording as the escape hatch. Also, Shell already *reads*
  foreign `dwItemData` as `AASHELLMENUITEM` (`MenuItem.h:918-924`); prepending `MSAAMENUINFO` to
  Shell's own item data must not be applied to mirrored native items that still carry the host's
  layout.

---

## 5. Is this the optimal architecture?

**The shape is right.** "Bounded interception shim → immutable snapshots → in-process engine" is the
correct target for a takeover engine, `TakeoverSession` is the right consolidation, the strangler
seam order is right, and rejecting the WinUI renderer / out-of-process broker / parser rewrite is
right. Nothing in §4 argues for a different architecture.

**The ordering and emphasis are wrong** in three ways: the plan optimises what it can *measure*
(catalog, GetState, SPI) ahead of what actually *stalls* (third-party provider activation, §4.3);
it invests in a persistent cache before measuring whether the async service alone suffices (§4.7);
and it schedules its own evidence-producing tool nowhere (§4.6).

Re-sequenced:

| Phase | Contents | Why here |
|---|---|---|
| **0′** | Trace harness first. Then Phase 0 fixes, one commit each, each with its named test. Fix and wire `check-invariants.ps1` into `build.ps1`. Resolve §2.1–2.3. | Everything downstream is gated on the harness; the fixes change evaluation semantics |
| **1′** | `PackageCatalogService` **in memory only** (async warm, `shared_ptr<const>`, no persistence). Provider presentation cache (state/title/icon). **Bounded provider budget with deferral.** `GetState(TRUE)` removal. Diagnostics ring. | This is where the user-visible freeze lives |
| **2′** | Persisted LKG config + `shell.exe -check`. Then `TakeoverSession`, `NativeMenuBridge` INIT/UNINIT, HostContract with **RETURNCMD-only** default. | Safety first, then contract fidelity behind the harness |
| **3′** | Taskbar **Stage 2 only**. `fWinIni = 0`. Flicker-hack A/B. CoCI policy compile (incl. the Alt bypass). Circuit breaker + bypass gesture. | Cheap, bounded, each independently measurable |
| **4′–5′** | Seams, targeted `moveto`, then the capability wave as written | Unchanged |

Pull **MSAA exposure, mnemonics and smart columns** forward wherever there is slack: they are the
best value-per-unit-risk in the whole document, depend on nothing, and (mnemonics aside, which needs
the QA-01 probe) carry no contract risk.

## 6. Highest-value additions the plan does not contain

1. **Bounded provider budget with deferral and presentation caching** (§4.3). The difference between
   "we can tell you which extension is slow" and "a slow extension can no longer freeze your menu".
2. **Persisted last-known-good config + `shell.exe -check`** (§4.2). Config safety that covers the
   failure mode that actually occurs.
3. **Decouple diagnostics from policy in `CoCreateInstanceHook`** (§4.8) — a correctness fix that
   also unblocks the quarantine feature the Reliability Center is built on.

## 7. Acceptance criteria for acting on this audit

- [ ] `check-invariants.ps1` exits 0 on this tree, uses `-Recurse`, covers `src/shared`, and runs
      from `build.ps1`.
- [ ] Every Phase 0 change has a named test; `src/tests/hostprobe/` exists and records a baseline
      before any expression-semantics commit lands.
- [ ] §2.1 `operator[]` contract decided and pinned; §2.2 `UTF8::From` deleted; §2.3 manual
      Recycle-Bin pass recorded (Windows build + result).
- [ ] `02-first-paint-latency.md §5` amended: Stage 1 removed, Stage 2 retained, with the
      `AGENTS.md` precedent cited in the document.
- [ ] `03-config-safety.md` amended with persisted LKG; its acceptance test rewritten to start a
      *new* process with a broken config.
- [ ] R1 amended to name COM activation of verb handlers, with the deadline design as its remedy.
- [ ] `01-takeover-contract.md §3` default changed to `TPM_RETURNCMD` alone, NONOTIFY demoted to a
      probe-driven per-host-class opt-in.
- [ ] `02-first-paint-latency.md §4` SPI row replaced with `fWinIni = 0`.
