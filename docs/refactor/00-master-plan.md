# Shell takeover refactor — master plan

**Date:** 2026-08-22 · **Tree:** `main` @ `a3431df`
**Inputs:** `docs/maintenance/architecture-assessment-2026-08-22.md` (full-scan assessment),
`docs/maintenance/Shell_Architecture_Audit_1.md`, `docs/maintenance/Shell_Architecture_Audit_2.md`,
plus first-hand code verification and Microsoft Learn fetches performed 2026-08-22
(IContextMenu, IShellExtInit, TrackPopupMenu, WM_INITMENUPOPUP, WM_UNINITMENUPOPUP,
WM_MENUCHAR, MENUINFO, Using Menus, IExplorerCommand::GetState,
Exposing Owner-Drawn Menu Items, Verbs Best Practices).
**Supersedes:** §5–§6 of `architecture-assessment-2026-08-22.md` (roadmap), which is
folded into the validated backlog below.

---

## 1. Product thesis (agreed by all three analyses)

Takeover stays. Shell is a context-menu takeover engine, not an additive extension
(Audit 1 §1, Audit 2 preamble). The winning shape:

> **A minimal, isolated interception shim feeding a fast in-process menu engine from
> asynchronously prepared immutable snapshots. Nothing optional or unbounded runs
> synchronously before the first menu pixel.**

**Amended 2026-08-25.** The second sentence is not what the branch delivers, and
saying it anyway is how a later reader comes to "restore" a decision that was
made on evidence. What holds is §4's governing statement: Shell-controlled
engine work before first paint is bounded and measured, package enumeration,
manifest I/O and `GetState(TRUE)` are forbidden, and third-party UI-thread
callbacks are reported and policy-limited - but one in-flight call is not
preemptible. §4a names the two exceptions.

Two governing rules, quoted because every plan decision derives from them:

- **R1 — Bounded first paint.** Before the menu appears, Shell may do only bounded
  local work: inspect the already-created HMENU root, read the config snapshot,
  evaluate local NSS expressions, consume existing captures, read immutable service
  snapshots. Never: package enumeration, manifest disk I/O, UIA calls, unbounded
  registry traversal, `GetState(TRUE)` (Audit 1 §16; adopted here as R1).

  **R1a — no unbounded third-party call, of any kind, before first paint**
  (added by the §07 audit). The original list enumerated Shell's own expensive
  work and missed the largest cost on the path: *activating* a third-party verb
  handler. `create_explorer_command` calls
  `CoCreateInstance(clsid, …, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, IID_IExplorerCommand)`
  (`ExplorerCommand.cpp:186-189`) and then `GetState`/`GetTitle`/`GetFlags`/`GetIcon`
  synchronously, per packaged command. `CLSCTX_LOCAL_SERVER` means a **surrogate
  process launch** can sit between the right-click and the first pixel, and no
  budget exists anywhere on that path. Every such call must run under a deadline
  (§02.2a); a provider that misses it is omitted from *this* menu and its answer
  cached for the next one.

  **What R1a became, 2026-08-25.** "Every such call under a deadline" was not
  achievable and is not implemented: the same page that makes these calls
  expensive - "These methods are called on the UI thread" - is the environment
  handlers are written against, and moving them off it is a divergence with no
  way to test its blast radius here (§02.2a-i). The deadline is therefore a
  *whole-menu* allowance rather than a per-call one, and since §09 R1 it is spent
  cheapest-known-first, so an overrun is charged to the provider that caused it
  instead of to the ones registered behind it. The honest bound is **at most one
  in-flight overrun per menu**, and it is reported (`explorer.commands.over_budget`)
  rather than assumed away.
- **R2 — One unsupported boundary, made explicit.** Takeover requires exactly one
  private compatibility surface (popup interception). Everything else moves behind
  documented APIs or named, health-checked adapters (Audit 1 §6).

## 2. Validation record

Every proposed improvement from the three sources was checked against the tree
(`a3431df`) and vendor documentation. Verdicts:

| Proposal | Source | Verified state in tree | Verdict |
|---|---|---|---|
| Remove `GetState(FALSE)→E_PENDING→GetState(TRUE)` retry | A1§5, A2§5.1 | present, `ExplorerCommand.cpp:204-206` | **Adopt (P0)** |
| Async package/ExplorerCommand catalog (stale-while-revalidate, persistent) | A1§5, A2§14 | sync scan on 30 s TTL expiry, `ExplorerCommand.cpp:17,72-99,106-125`; second index in `Packages.cpp` lives inside config CACHE (`Cache.h:241`) | **Adopt (P0)** |
| Unify the two package discovery systems outside config snapshots | A1§13 | confirmed: `PackagesCache` member of immutable `CACHE` | **Adopt** |
| Reuse captured `IShellItemArray` before `SHParseDisplayName` fallback | A1§5.6 | fallback exists `ExplorerCommand.cpp:271-274`; capture preferred first (verified order) | Keep + assert preference |
| Paired `WM_UNINITMENUPOPUP` for borrowed native popups | A2§4 | handler `ContextMenu.cpp:6829` → `OnUninitMenuPopup:1581-1607` destroys **synthetic** menu only; no UNINIT ever sent to borrowed HMENU. Contract: "If an application receives WM_INITMENUPOPUP, it will receive WM_UNINITMENUPOPUP" (fetched) | **Adopt (P0)** |
| `TPM_RETURNCMD`/notification contract normalization | A2§2 | hook mutates flags (`Main.cpp:902-906,974-975`); `InvokeCommand(id)` consumes return value only (`ContextMenu.cpp:5023+`); `selectid` recorded at `:6538-6539` but unused (call site commented out `:2821`) → for non-`RETURNCMD` hosts, custom items neither run nor map to a valid host command | **Adopt (P0, probe-gated design §01)** |
| Zero-wait taskbar hit-test (+ UIA cache requests, rectangle snapshots) | A1§9, A2§9 | 250 ms budgeted wait `Main.cpp:293,337`; three per-property UIA calls `:486-488` | **Adopt (two stages)** |
| CoCI detour → optional, precompiled CLSID policy | A1§7, A2§24 | global detour evaluates expressions per activation `Main.cpp:685-805` | **Adopt** |
| De-duplicate Win11 modern-menu suppression (TreatAs ⊕ CoCI) | A1§7 | both active today (`Main.cpp:696-708` + TreatAs redirect) | **Adopt (router decides)** |
| Circuit breaker / degraded modes; one-shot native bypass | A1§18, A2§8 | fail-open exists implicitly (`Main.cpp:1013-1028` `invoke()` fallback); no explicit breaker/bypass | **Adopt** |
| Last-known-good config (StaleWithError) | A2§9 | parse failure sets `Status.Error` → `Initializer::query:154-156` refuses menus although previous `_snapshot` is still published in memory; `DllGetClassObject:1468-1469` refuses COM too | **Adopt (small diff, huge QoL)** |
| Config watcher (`ReadDirectoryChangesW`) | A2§9 | today: timestamp poll per menu attempt `Initializer.cpp:179-207` + manual key combos `:786-859` | **Adopt (after LKG)** |
| Remove transient global setting mutations | A2§10 | `SPI_SETMENUSHOWDELAY` set `ContextMenu.cpp:3925` / restored `:4982` with `SPIF_SENDCHANGE`; `SPI_SETSELECTIONFADE` toggled `:6556/6558` (no broadcast, fWinIni=0) | **Adopt** (keep SELECTIONFADE toggle? see §04.3) |
| Benchmark/gate `fix_ugly_flicker()` | A2§11 | exists `ContextMenu.cpp:6052`, called in `WM_NCCALCSIZE` path `:6211`; timeBeginPeriod + DWM vblank Sleep on UI thread | **Adopt (measure → gate/delete)** |
| WinEvent popup state machine (reentrancy) | A2§12 | CREATE/SHOW handled directly (`WinEventProc` → `OnMenuCreate/OnMenuShow`) with ad-hoc guards (`is_prop(UxSubclass)` checks) but no explicit lifecycle states | **Adopt (fold into TakeoverSession)** |
| Targeted `moveto` discovery (third policy) | A2§13 | eager fallback gated at `ContextMenu.cpp:4894-4915`, whole-tree cost when triggered | **Adopt (P1/P2 boundary)** |
| `MSAAMENUINFO` on Shell's owner-drawn items | A2§16 | Shell *reads* foreign MSAA layouts (`MenuItem.h:918-924`, historical comment `:11-12`) but never provides one; all items owner-drawn | **Adopt (cheap, high value)** |
| Mnemonics (`WM_MENUCHAR`) + type-ahead | A2§17 | cases swallow chars `ContextMenu.cpp:6590-6593` | **Adopt** |
| Smart multi-column overflow | A2§18 | `cyMax` scrolling landed (`a3431df`); `column`/MENUBREAK support exists | **Adopt (small)** |
| Provider health + quarantine + Reliability Center | A1§17, A2§6 | Alt-timing embryonic (`Main.cpp:764-774`); CLSID suppression machinery exists | **Adopt (flagship feature)** |
| Per-session pure-expression memoization | A2§20, mine | none today; inert memo fields `FuncExpression.cpp:112-118` | **Adopt (whitelist-based)** |
| Lazy large-selection metadata | A2§21 | Selections materialize eagerly; guidance: verbs should consider first item + count (fetched verbs page) | **Defer behind measurement** |
| Delete dead weight / expression-engine bug fixes / encoding consolidation | my assessment §4.4 | verified live defects listed there | **Adopt (P0)** |
| Seam extraction of `ContextMenu.cpp` (strangler, no rewrite) | A1§10, A2§1 | agreed order merged in §04.4 | **Adopt** |

Sources rejected/deferred deliberately (consensus): WinUI renderer (A1§11),
out-of-process menu broker (A1§23), killing provider threads (A1§23, A2§28),
parser rewrite (all three), wholesale substrate replacement (A1§23, mine),
per-user TreatAs as baseline (A1§8 — experiment only), taskbar button takeover (A2§28),
universal async NSS (A2§28).

## 3. Consolidated backlog (ROI = user impact ÷ effort ÷ risk)

| # | Work item | Doc | Effort | Risk | User impact |
|---|---|---|---|---|---|
| 1 | Expression-engine + dead-weight P0 fixes | §04.1–04.2 | XS | ≈0 | correctness floor |
| 2 | Catalog service: async packages + ExplorerCommands, persistent, unified | §02.1 | M | low-M | cold right-click stops freezing; malformed manifests harmless |
| 3 | Never `GetState(TRUE)` pre-paint + E_PENDING policy | §02.2 | S | low | slow packaged verbs can't stall menu |
| 4 | Paired native `WM_UNINITMENUPOPUP` | §01.5 | S | low | fixes real leak-class infidelity in every submenu close |
| 5 | HostContract normalization (`TPM_*`, `MNS_NOTIFYBYPOS`) | §01.3 | M | medium (probe-gated) | third-party-host fidelity |
| 6 | Last-known-good config + StaleWithError | §03.2 | S | low | typo never kills the shell |
| 7 | TakeoverSession object (state consolidation) | §01.1 | M | low (pure refactor) | enables everything below |
| 8 | PopupInterceptionBackend abstraction + health | §01.9 | M | medium | survivable Windows evolution |
| 9 | CoCI policy compile + attach-only-if-needed + router de-dup | §01.9 | S-M | medium | less process-wide blast radius |
| 10 | Taskbar zero-wait stage 1 (queue-not-wait) | §02.5 | S | low | no 250 ms stalls |
| 11 | Recycle-Bin query off first paint; flicker-hack benchmark/gate; SPI mutations removed | §02.4, §04.3 | S | low | fewer hidden stalls/system mutations |
| 12 | Diagnostics ring buffer (always-on, zero-cost) | §02.6 | S | low | foundation for features |
| 13 | Circuit breaker + one-shot bypass gesture | §01.7, §05.2 | S-M | low | recoverability product feature |
| 14 | Provider health/quarantine + Reliability Center UI | §05.1 | M-L | medium | flagship differentiator |
| 15 | `MSAAMENUINFO`, mnemonics, type-ahead, smart columns | §05.3–05.5 | S each | low | usability/a11y wins |
| 16 | Config watcher auto-reload | §03.3 | S | low | live editing |
| 17 | MenuModel + CommandDispatcher + presenter extraction | §04.4 | L | medium | long-term velocity |
| 18 | Targeted `moveto` discovery | §04.6 | M | medium | power-config latency |
| 19 | Favorites/recent identity model; rule inspector | §05.6–05.7 | M | low-medium | productivity |
| 20 | Icon-cache extension; per-session memoization; lazy selection | §04.7 | M | medium | measured follow-ons |

### 3a. What happened to each of them (2026-08-25)

Kept **here**, beside the list it describes, rather than only in the tracking
documents. `08-handoff.md` §1 rule 6 exists because a tally maintained
somewhere else drifts from the backlog it tracks while every individual update
stays accurate — and that is exactly how item 8 came to be decided by nobody
for several sessions. This table is the anchor to reconcile against.

**Twenty closed, none partial** (2026-08-25, after §09). "Closed" means built,
or measured and declined with the numbers written down; a decline with no
recorded reason is not a result, it is something nobody did.

**Two tallies, and they count different things.** The one above is the outcome
tally: did this item reach a decision, and is the decision recorded? It is the
useful project view and it cannot answer a second, equally fair question — *how
literally was the original architecture built?* An item resolved differently is
a valid outcome and is not an implementation of what was written down. The
strict view is §3b, and both are kept because a single number was how item 8
came to be decided by nobody: an item that reads "closed" in every document
stops being looked at.

| # | | Where it ended up |
|---|---|---|
| 1 | closed | Phase 0; every fix re-broken and watched to fail its own test |
| 2 | closed | in memory (steps 1–2, 4). Persistence **declined**: `catalog.first_wait` never fires — §02.1 step 3 |
| 3 | closed | the `GetState(TRUE)` retry deleted; `E_PENDING` resolves provisionally |
| 4 | closed | §01.5, asserted against a real borrowed shell menu rather than a fake |
| 5 | closed | §01.3a — the harness answered the flag questions and two came back the opposite way round. Reopened 2026-08-25 and closed properly: by-position replay had never been built (§09 R2) |
| 6 | closed | persisted shadow (§03.1b), verified across a real Explorer restart |
| 7 | closed | the *object* **declined** — C2712 in an SEH hook body. The consolidation happened in plain-data form, §01.6a |
| 8 | closed | the interface and the per-entry health check **declined** (§01.9c); the `intercept` line built |
| 9 | closed | policy compile landed; **conditional attach declined**, with reasons in §01.9a that still hold. Installation itself was hardened in §09 R6.7: the patch is not committed unless every live thread was enlisted |
| 10 | closed | stage 1 **withdrawn** by the §07 audit — it met its acceptance by not showing the menu. Stage 2's cached request landed, §02.5a |
| 11 | closed | Recycle Bin query removed, flicker gated and measured at 7.0 ms/menu, `SPIF_SENDCHANGE` dropped |
| 12 | closed | 47.8 ns per phase, ~0.6 µs per menu, §02.6 |
| 13 | closed | one gesture classifier, so no click can mean two things — §01.7a |
| 14 | closed | provider names, quarantine, identity and the window — §05.1a–§05.1d |
| 15 | closed | `MSAAMENUINFO` **declined**, already satisfied (§05.3); mnemonics, type-ahead and smart columns built |
| 16 | closed | §03.3a — and §03.3b, which is where it started working more than once per process |
| 17 | closed | all seven seam steps, and `Win32MenuPresenter` over the surface they exposed (§09 R4) |
| 18 | closed | 85.5 ms → 20.2 ms on a real Explorer, §04.6a |
| 19 | closed | favorites §05.6a and the inspector §05.7a, over parser provenance |
| 20 | closed | all three parts **declined** by measurement, §04.7 |

### 3b. The strict implementation view (2026-08-25)

Not a competing verdict — the same twenty items asked a narrower question: was
*this* built, as written? A redesign is a legitimate outcome and it is counted
as one here rather than as an implementation.

| # | Strict state | Remainder |
|---|---|---|
| 1 | implemented | — |
| 2 | implemented | async catalog landed; package identity/path queries now read the same snapshot (§09 R3). Persistence declined |
| 3 | implemented | failure/pending telemetry corrected in §09 R1.2 |
| 4 | implemented | — |
| 5 | implemented | return/notification normalization, then by-position replay (§09 R2) |
| 6 | implemented | two fresh-machine routes remain as environment validation, §09 R8 |
| 7 | resolved differently | `TakeoverSession` as a class declined under C2712; plain-data consolidation landed |
| 8 | resolved differently | backend interface and per-entry health check declined; the live mechanism is reported |
| 9 | resolved differently | conditional attach declined; enlistment safety implemented instead |
| 10 | resolved differently | zero-wait withdrawn; cached UIA request plus a bounded COM-modal wait |
| 11 | implemented | Recycle Bin removal and flicker measurement; two transient SPI mutations retained and now enumerated in §4 |
| 12 | implemented | — |
| 13 | implemented | — |
| 14 | implemented | admission policy and outcome vocabulary refined in §09 R1 |
| 15 | implemented / measured alternative | `MSAAMENUINFO` declined against the real MSAA surface |
| 16 | implemented | repeated re-point fixed; dead-watcher state fixed in §09 R6.1 |
| 17 | implemented | model, selection seam, presenter split, and the named presenter boundary |
| 18 | implemented | — |
| 19 | implemented | live `where=` evaluation deliberately declined |
| 20 | resolved differently | all three declined against recorded measurements |

**Strict count: 15 implemented, 5 resolved differently, 0 partial, 0 untouched.**

**Where that differs from §09 R7.1, which specified 11 / 5 / 4.** Four of the
partials closed; the honest accounting is that they did not all close the same
way, and a tally that does not say which is a tally a reader cannot check.

- **Closed by code** — items **2**, **5** and **17**. §09 R3 gave item 2 the
  async catalog and moved package identity and path queries onto the published
  snapshot; §09 R2 gave item 5 the by-position replay; item 17 got the presenter
  split and the named boundary. These are implementations, and they count as
  implementations.
- **Closed by reclassification** — items **9** and **11**, and this is the part
  worth stating rather than absorbing into a number:
  - **9** moved from *partial* to *resolved differently*. Conditional attach was
    declined outright, and what exists instead is enlistment safety — a
    different thing that answers the same risk. Nothing that was deferred was
    later built; the item was re-described to match what was decided. §09 R7
    counted it as a partial because half of the original wording was
    unimplemented, which is true and is why it is not counted as *implemented*
    here either.
  - **11** moved from *partial* to *implemented* on the argument that the
    remainder was a documentation gap, not a code gap: the two transient SPI
    mutations were always retained deliberately, and what was missing was that
    nobody had written down which two. §4 now enumerates them. If a reader
    thinks "the mutations are still there" should keep this a partial, the
    disagreement is with that argument and not with the count.

`08-handoff.md` §1 rule 6 exists because a tally maintained by hand drifts from
what it counts; a reclassification is the quietest way for that to happen, which
is why both are named here rather than folded into the total.

The two governing rules of §1 fared differently and it is worth saying so.
**R1 held; R1a was weakened deliberately and is now stated as what it is.** R1
drove the largest wins — the unbounded third-party call before first paint
turned out to be the whole game (§02.2a-i), and the biggest single defect on the
branch was Shell's own work misattributed to it (§02.3a, 645 ms → 30 ms). R1a's
literal form — every third-party call under a deadline — is not implemented and
was never going to be, for the reason recorded beside it in §1; what exists is a
whole-menu allowance, spent cheapest-first, with the breach reported. §4a. **R2 was answered rather than implemented**: the one
unsupported boundary is explicit, but as a *reported fact* (`intercept
win32u import`) instead of an interface, because the two mechanisms are not
interchangeable and a vtable would have asserted a substitutability the PE
format denies (§01.9c).

## 4. Target architecture (end state)

```text
Host right-click
   │
PopupInterceptionBackend (one active; win32u-IAT primary today)
   │
TakeoverSession ────────────── DiagnosticsRing (always-on)
   ├─ HostContract (original call semantics)
   ├─ SelectionContext (capture-first, enrichment-second)
   ├─ NativeMenuBridge (borrowed-HMENU lifecycle, INIT/UNINIT pairing)
   ├─ Composer (NSS rules over MenuModel{Native|Custom|ExplorerCommand})
   ├─ Presenter (Win32 owner-draw; unchanged rendering model)
   └─ CommandDispatcher (origin-aware; completes host contract)
        ▲                     ▲
PackageCatalogService   ConfigSnapshotService (LKG, watcher)
ProviderHealth          WindowsCapabilities    TaskbarAdapter(zero-wait)
```

Invariants enforced by review checklist + trace harness (§06), and by
`scripts/check-invariants.ps1` where the shape is lexical.

**Rewritten 2026-08-25** against what the implementation actually provides.
Three of the seven described a guarantee the code does not make, and a stated
invariant that is not true is worse than a weaker one that is: it is what a
later session "restores" by undoing a decision that was made on evidence.
Each now says what is guaranteed, and names what is not.
docs/refactor/09-remediation-plan.md R5.

1. **No manifest I/O, and no package enumeration, on the menu thread.** Manifest
   reads moved to `PackageCatalogService`'s worker (§02.1). Package identity,
   list and install-path queries - `package.exists`, `appx.id`, `appx.family`,
   `appx.version`, `package.path` - are reads of that same published snapshot
   (§09 R3), so a stock configuration's `package.exists("WindowsTerminal")`
   enumerates nothing. **The exception is `appx.name`/`package.name`**, which
   resolves a localized display name on demand and can call
   `SHLoadIndirectString` (loading the package's `Resources.pri`) and walk
   MrtCache. No shipped configuration uses it. A cold first query may wait once,
   bounded, through `snapshot_for_menu()`, and that wait is counted
   (`catalog.first_wait`).
2. **No *unbounded* UIA wait on the taskbar UI thread.** The zero-wait design was
   withdrawn (§07 A2) because it met its acceptance criterion by not showing the
   menu at all. What is guaranteed instead: the wait is capped at 250 ms
   (`Main.cpp` `BUDGET_MS`), it is entered through `CoWaitForMultipleHandles` so
   the single-threaded apartment keeps pumping, the UIA work itself runs on a
   separate MTA thread that owns no windows, and every outcome is counted
   (`TaskbarHitStats`).
3. zero `GetState(TRUE)` during composition (enforced: `check-invariants` rule
   10). A failed `GetState(FALSE)` omits the item and is reported `failed`;
   `E_PENDING` shows the item provisionally enabled and is reported `pending`
   (§09 R1.2).
4. borrowed HMENU receive paired INIT/UNINIT exactly once;
5. synthetic command IDs never reach a host. A host that passed `TPM_RETURNCMD`
   is answered by the return value; one that did not is sent `WM_COMMAND` with
   its own identifier, or - when it set `MNS_NOTIFYBYPOS` on the menu it handed
   over - `WM_MENUCOMMAND` with the position the item occupies in *its* menu
   (§09 R2). Shell's own composed menu never carries `MNS_NOTIFYBYPOS`
   (enforced: `check-invariants` rule 8).
6. foreign TreatAs state never modified; every failure has a fail-open path.
   Inline detour installation is part of this: the CoCreateInstance patch is not
   committed unless every live thread in the process was enlisted in the
   transaction (§09 R6.7).
7. **No setting mutation may update the user profile or broadcast
   `WM_SETTINGCHANGE`** - `fWinIni` must be 0, enforced by `check-invariants`
   rule 9. Transient, opt-in, restored-on-close mutations are permitted, and are
   enumerated here rather than left implicit: `SPI_SETMENUSHOWDELAY` when the
   configuration sets `showdelay`, and `SPI_SETSELECTIONFADE` around dismissal.
   Deleting the second is two lines; its benefit is a dismissal artifact no
   screenshot can show, which is the same class as the flicker wait (§02.4a) and
   needs a person at the machine rather than a reviewer.

### 4a. What is *not* bounded, and why it is said here

§00.1 used to say that nothing optional or unbounded runs before the first
pixel, and R1a that **every** third-party call runs under a deadline. Neither is
true, and both were load-bearing enough that they need correcting rather than
softening.

The governing statement is:

> Shell-controlled engine work before first paint is locally bounded and
> measured. Package enumeration, manifest I/O and `GetState(TRUE)` are
> forbidden. Third-party UI-thread callbacks and arbitrary user NSS expressions
> are reported and policy-limited where compatible, but **one in-flight call is
> not preemptible**.

Two named exceptions follow from it, and both are validation targets rather than
oversights:

- `IExplorerCommand`'s methods "are called on the UI thread"
  (<https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorercommand>),
  which is the environment handlers are written against. Shell budgets against
  that cost and orders its calls cheapest-first (§09 R1), so an overrun is
  charged to the provider that caused it rather than to the ones behind it - but
  a call already running cannot be interrupted.
- A third-party host that supplies selection paths but no retained
  `IShellItemArray` still causes one `SHParseDisplayName` per selected item
  before the provider budget begins. The Explorer path was fixed (645 ms →
  30 ms, §02.3a); the host case is instrumented (`selection.rebuild_array`,
  annotated with the count) and awaits the measurement in §09 R8.

## 5. Document index

| Doc | Content |
|---|---|
| `01-takeover-contract.md` | TakeoverSession, HostContract translation, NativeMenuBridge, interception backend, CoCI policy, breaker/bypass |
| `02-first-paint-latency.md` | PackageCatalogService, GetState policy, stall removals, diagnostics ring, taskbar zero-wait |
| `03-config-safety.md` | Last-known-good snapshots, watcher reload, error UX |
| `04-code-health.md` | P0 defect/dead-code fixes, encoding/string cleanup, seam extraction, targeted moveto, caches/memoization |
| `05-capabilities.md` | Provider health/Reliability Center, bypass, accessibility, keyboard, columns, favorites, inspector |
| `06-phases-and-tests.md` | Phase sequencing with acceptance criteria, trace harness spec, regression gates, measurement protocol |
| `07-plan-audit.md` | Critical audit of this plan and of Phase 0 as implemented. **Amendments from it are folded into 00–06 in place**; 07 is kept as the reasoning and evidence record, including the probes run against this machine |

| `08-handoff.md` | Where the work actually stands, why it is sequenced the way it is, and what to pick up next. **Start here.** |
## 6. Amendments applied from the §07 audit

| # | Change | Where |
|---|---|---|
| A1 | R1a: no unbounded third-party call before first paint; provider deadline + deferral | §1 above, §02.2a |
| A2 | Taskbar "Stage 1 zero-wait" **withdrawn** — it re-proposed a decision `AGENTS.md` records as rejected, and gives the first right-click of every sequence to Windows | §02.5 |
| A3 | Last-known-good config must be **persisted**; the in-memory version does not cover the failure it was written for (a *new* process parsing a broken file) | §03 |
| A4 | `TPM_RETURNCMD` alone becomes the default internal tracking flag; `TPM_NONOTIFY` demoted to a probe-driven per-host-class opt-in | §01.3 |
| A5 | SPI mutation: keep the feature, drop `SPIF_SENDCHANGE` (a desktop-wide `WM_SETTINGCHANGE` broadcast twice per menu) — pass `fWinIni = 0` | §02.4 |
| A6 | Trace harness becomes **Phase 0 item 1**; it was the linchpin of six other items and was scheduled in no phase | §06 |
| A7 | Persistent catalog cache deferred behind measurement, and its integrity boundary for elevated hosts stated as an invariant | §02.1 |
| A8 | `CoCreateInstanceHook`: the CLSID blocklist is bypassed whenever Alt is held; diagnostics and policy must not share a branch | §04.9 |
