# 08 — Handoff

**Updated 2026-08-25, branch `refactor/takeover-master-plan`.**
Read this first, then `06-phases-and-tests.md` for the per-item status table.
`00`–`05` are the plan; `07` is the audit that corrected it; this is where the
work actually stands and what to do next.

---

## 1. How this work is being done

The plan sequences by architectural layer. That is **not** the order being
followed, and the departure is deliberate: work is sequenced by **what can be
proved on this machine**, taking small verified defects first and large
speculative pieces last, so every commit ships.

Six rules have held throughout and are worth keeping. Rule 6 was learned late,
by an audit that found this document had quietly stopped being true, and rule 8
later still — by two defects that only a deployed build could show:

1. **Measure before designing.** Every latency item in this branch was probed
   first, and in six cases the number changed the design — three times it
   changed which problem was worth solving at all. Probes live in the session
   scratchpad, never in the tree; their results live in a comment next to the
   code that depends on them.
2. **Every test is checked to catch its defect.** Re-introduce the bug, rebuild,
   watch that specific test fail, restore. This has found flaws in the *tests*
   as often as it has confirmed them — a test that passed for the wrong reason,
   one that crashed the suite instead of reporting, one that was order-dependent,
   and one that could not assert its own premise at all (§4).
3. **Cite the contract, quote the passage.** Both in the code and in the commit
   message. Where documentation is silent, the harness measures it and the
   conclusion is labelled as measurement, not contract.
4. **Declining is a result, and it gets written down.** Ten items in this
   branch were measured or reasoned about and *not* built. Each is recorded
   with its numbers in the plan document that proposed it, so it is not
   re-proposed by the next reader: the icon cache (§04.7), `MSAAMENUINFO`
   (§05.3), the taskbar rectangle model (§02.5a), making `priority`
   authoritative over the `TreatAs` redirect (§01.9b), the interception-backend
   interface and its health check (§01.9c), the six-state popup lifecycle
   (§01.6a), catalog persistence (§02.1 step 3), lazy large-selection metadata
   and per-session memoization (§04.7), and the inspector's live `where=`
   evaluation (§05.7a). Declining is a *result* only when the reason is
   recorded; without it, it is just something nobody did.
5. **Three platforms green before every commit.** `.\build.ps1 -Platform all`,
   0 warnings, invariants clean.
6. **Audit this document against the plan, not against itself.** Added
   2026-08-24. Each session updates §3.6 from what it just did, and after a few
   of those the list of remaining work describes the recent past rather than the
   backlog. Reconciling it item by item against `00-master-plan.md` §3 found one
   item missing altogether (the interception backend, §01.9c) and one
   prerequisite attached to the wrong seam (§3.6). Both had been invisible for
   several sessions precisely because every individual update was accurate.
7. **Check what the experiment is actually testing.** Two separate mechanisms
   were found this session that made a real-Explorer experiment silently
   measure something else — a deploy that landed one restart late, and registry
   values Explorer could not see. A third arrived on 2026-08-25 — a menu read
   seconds after an Explorer restart measures Explorer settling, and made a
   no-op change look like it had rewritten the menu. All are in `AGENTS.md`
   under "Three ways an experiment can test something other than what you
   think" — five entries now, the name kept because it is what people look
   for — because each produced a result that read as a finding about Windows
   or as a regression.
8. **Deploy it, drive it, and read the menu back.** Added 2026-08-25, and it is
   the rule this session paid for twice. Both of its most valuable findings
   were defects in work already marked done, and both surfaced while verifying
   *something else* in a real Explorer: a config watcher that served exactly one
   reload per process (§03.3b) and a `std::wstring_view` built from a null
   `c_str()` faulting on the first item of every menu (§05.6a). Neither had a
   crash, a log line, or a failing test, and 32,800 unit checks could not have
   found either.

## 2. What has landed on this branch

Eighty-two commits, `a3431df`..HEAD. The table lists the ones that changed a
decision or fixed something; pure documentation commits are omitted.

| Commit | What |
|---|---|
| `940ff6a` | **Trace harness** `src/tests/hostprobe/` — 23 scenarios, baselines committed |
| `b63fdc2` | **`TPM_RETURNCMD` normalization** — custom commands silently did not run in non-Explorer hosts |
| `1f17d00` | **Diagnostics ring** — always-on phase timing at 47.8 ns/phase |
| `5fe6354` | **Provider budget and reuse** — a warm menu went from ~170 ms to ~41 ms |
| `745dd81` | **GDI leak** — ~16 bitmaps leaked per right-click in explorer.exe |
| `08653c0` | **Mnemonics** — typing a letter in the menu did nothing at all |
| `690b807` | **MSAA finding** — §05.3's premise was wrong; the work was not needed |
| `72d2516` | **Taskbar** — one cached UIA round trip instead of four; the rectangle model measured and declined |
| `9c18c79` | **`shell.exe -check`** — parse and report; a missing file no longer reports ok |
| `a63c7a2` | **Bypass gesture and circuit breaker** — one gesture classifier, so reload and bypass cannot both fire |
| `46c7a06` | **Config watcher** — save `shell.nss` and the menu follows, with no key combination |
| `cc5550d` | **Type-ahead** — typing a name selects it; mnemonics keep precedence |
| `5e44534` | **CoCI policy compile** — the COM detour stops walking the rule list for CLSIDs no rule names |
| `6ff63c8` | **Takeover harness + last-error fix** — the hook was handing hosts the wrong `GetLastError` after a failed track |
| `2142525` | **MSAA closed** — confirmed against Shell's own menu in a real Explorer; the `MSAAMENUINFO` design deleted |
| `5085b93` | **Replay harness + breaker fix** — three menus Shell does not handle no longer switch takeover off for the process |
| `b0d9ec6` | **Harness navigation** — the driver no longer walks past the item it was steering to |
| `00d9e21` | **Smart columns** — a menu taller than the screen can use columns; measured, 239×1031 scrolling → 938×990 |
| `0b20dde` | **Ring export** — `shell.exe -report perf` reads menu timings out of any host on the desktop |
| `4bcf12e` | **Deploy ordering** — Explorer was picking up the build *before* the one just deployed, every time |
| `3b3dc25` | **`priority = 0` on a `-treat` machine** — measured four ways, de-duplicated, and `-check` now says it is inert |
| `64f17b6` | **Host tracking flags in the report** — which half of `complete_host_contract` a real host exercises |
| `b71a082` | **Flicker wait** — 7 ms on every menu, after the phase everybody was measuring; now gated and reported |
| `6658371` | **Two ways an experiment lied** — the deploy ordering and the registry visibility, both now in `AGENTS.md` |
| `d2216c3` | **Catalog persistence declined** — `catalog.first_wait` never fires, so §02.1 step 3 would buy nothing |
| `4f574d1` | **Targeted moveto** — a moveto rule opens the submenu it named, not all of them. 85.5 ms → 20.2 ms |
| `5c63b28` | **Popup stack** — the level stack and the window map removed by two different keys; one disagreement is a dangling pointer |
| `a70124b` | **Provider names** — the report names the extension, not its hash. Found a concurrent-writer gap in the export on the way |
| `2e662c3` | **Quarantine** — `-quarantine:add {clsid}` and Shell stops asking that handler. Verified through MSAA: 29 items against 30 |
| `898c680` | **Takeover status** — the report says whether takeover is set up, which matters most when it has measured nothing |
| `6b26e61` | **`-check` with no argument** — could never find a configuration on any machine. Unblocked §03.5's last acceptance criterion |
| `029b9f8` | **Provider identity** — the 70 ms extension on this machine printed a hash, and `-quarantine:add` takes a CLSID. §05.1c |
| `e49eac6` | **Interception status** — backlog item 8, settled. The interface declined, the `intercept` line built. §01.9c |
| `294d471` | **Selection providers** — `QuerySelected` split in two, 271 lines verified byte-identical |
| `20e061e` | **Selection routing** — a host whose window class looks like Explorer's no longer loses the selection it already gave us. §04.4b |
| `c76c2e9` | **Reliability Center window** — `shell.exe -reliability`. §05.1d |
| `3723e78` | **Selection array reuse** — a menu over 200 files went from **645 ms to 30 ms**. §02.3a |
| `f17373c` | **Rendering coverage** — four `render.*` scenarios read the live menu back through MSAA. The gate steps 6–7 waited on for three sessions. §3.8 |
| `17321b2` | **Seam step 6** — `MenuModel`. Three vectors held one fact between them; `_main_popup` was read by nobody. §04.4 |
| `279340f` | **Seam step 7a** — the item painting leaves `ContextMenu.cpp`, 1,206 lines, byte-identical |
| `f465242` | **Seam step 7b** — the layer compositing follows it, 400 more. `ContextMenu.cpp` 7,542 → 5,852 |
| `4e81381` | **The harness transient** — ours, and a documented contract. 26 clean runs against 2-in-14 |
| `32aaaa4` | **Dead code the split found** — `crop_image` and a `get_item` overload, called by nobody |
| `e6164ad` | **Rule provenance** — every rule records the file and line it was written on. §05.7's long pole |
| `de0216f` | **The config watcher died after one reload** — `start()` joined its own thread. Live editing had worked once per Explorer lifetime since it shipped |
| `7ab9f42` | **Favorites and the inspector** — backlog item 19, and the last one. §05.6a, §05.7a |

### The measurements that changed decisions

| Measured | Consequence |
|---|---|
| Packaged verb providers cost **~700 ms cold / ~170 ms on every subsequent menu** | Became the largest first-paint item, ahead of everything the plan had ranked above it |
| `CoCreateInstance` is ~46 ms of that, **warm**, and takes no selection | Reuse live providers per thread — beats the plan's presentation cache, with *no* staleness risk |
| `IExplorerCommand` methods "are called on the UI thread" | The plan's worker-thread deadline is a divergence; bounded on the menu thread instead, and the residual risk is stated rather than hidden |
| Icon cache would buy only ~11 ms of ~32 ms | Declined; the other ~18 ms is a COM call nothing can cache |
| GDI count 4 → 164 keeping 160 bitmaps | A real leak: ~600 right-clicks to exhaust Explorer's GDI handles |
| `TPM_RETURNCMD`, not `TPM_NONOTIFY`, suppresses `WM_COMMAND` | `TPM_NONOTIFY` must never be added — it suppresses `WM_INITMENUPOPUP`, which the bridge needs |
| Native replay is **posted**, after the call returns | QA-03 inverted: posting, not sending |
| Owner-drawn + `MIIM_STRING` exposes names to a screen reader — **and Shell already sets it** | §05.3's premise is wrong; `MSAAMENUINFO` would be risk spent on a solved problem |
| A rectangle model of the taskbar agrees with `ElementFromPoint` **49.6 %** of the time (84.2 % scoped to the XAML frame) | §02.5's Stage 2 as written would claim the system tray as background. Declined |
| `ElementFromPointBuildCache` matches the four-call form at **1440 / 1440** points | Taken, for the call count rather than the 0.5 ms — a wedged provider now has one call to hang in, not four |
| `DllGetClassObject` is where `BootstrapOnce` is called from | The takeover harness needs **no** injected or deployed build — asking Shell for a class object installs the hook in the asking process |
| 22 of 23 takeover traces byte-identical; the 23rd was Shell's bug | The hook destroyed the host's last-error code. Fixed, and the native fixtures became the takeover gate |
| Three declined popups open the circuit breaker, and a host that is not Explorer produces them constantly | The breaker was counting decisions as failures. Only real failures count now, and the ring gained a `Declined` decision |
| COM activates Shell by the path in the registry, not by whichever copy is already mapped | Two knowingly broken builds passed every takeover assertion through `--shell`. The harness now refuses that configuration |
| A 100-item menu measures 239×1031 and scrolls; with `columns = 4` it measures 938×990 and does not | Smart columns works — and getting there needed two fixes the unit suite had not thought to ask for. §05.5a |
| **Explorer started 17:09:08; the rotation it was meant to precede is stamped 17:09:09** | Every real-Explorer result on this branch had been one build stale. The deploy restarts Explorer last now, and verifies it |
| **Explorer's `HKCU\SOFTWARE` holds 100 subkeys; the agent shell's holds 101** | A registry value set from the agent's shell is invisible to Explorer. That, not the log sink, is why `perf` produced nothing |
| **Pre-display p50 10.3 ms warm in a real Explorer**, 60 ms on the first menu in a process | The first measurement of this branch's first-paint work where it runs, and it is inside §06.4's 15 ms budget |
| **`menu.flicker_wait` averages 7.0 ms, up to 15.1** — and lands *after* `popup.total_pre_display` stops | Right-click to pixels is ~23 ms, not the ~16 ms every phase report showed. Gated and reported; §02.4a |
| Explorer and Everything both pass **`TPM_RETURNCMD`** | The identifier half of `complete_host_contract` is what runs; the posted-notification half is still harness-only |
| A four-level cascade, three ways, produced **no duplicate CREATE, no reentrant SHOW, and no wrong positional pop** | §01.6's six-state machine is declined. The defect was next door: two different keys for one removal |
| `provider e345019d 186 ms` **names nothing a user can act on** | Names moved into the export beside the records; the identifier printed is the CLSID `-quarantine:add` accepts |
| A quarantined provider costs **0.0 ms**, and the menu loses exactly that item (29 against 30, read through MSAA) | Skipping beats refusing the activation: surgical, and the only version that saves the cost |
| Bare `shell.exe -check` answered "no configuration file was found" **on a machine whose config was fine** | The export skips `BootstrapOnce`, so the paths it needs were never set up. The bare form had never worked |
| A broken config, an Explorer restart, and the menu **still came back at 213 × 680 — identical to baseline** | §03.5's last outstanding acceptance criterion, met in a real Explorer for the first time |
| **A menu over 200 selected files costs 645 ms, of which the 27 packaged handlers are 27 ms** | The phase named `explorer.commands` was 96 % something else: an `IShellItemArray` rebuilt one `SHParseDisplayName` at a time. 645 ms → **30 ms**. §02.3a |
| The export kept **8 provider records** for a 27-provider menu, and `dropped_providers` was printed by nobody | The slowest menu on the machine was the one the instrument could least explain. Raised to 32, and drops are now reported |
| A handler costing **209 ms** over 200 files had a single-file best of ~2 ms | `ProviderHealth` keyed on CLSID alone, so nothing could defer it. §02.2a specified `(clsid, selection_shape)`; it does now. 645 ms → ~460 ms on its own |
| `selection.preparing` **0.0 ms** and metadata **1.3 ms for 200 items** | §04.7's lazy large-selection item declined on its own stated gate |
| A menu item's `get_accChild` returns the submenu popup **with `accLocation` (0,0 0x0)**, open or closed | The documented MSAA descent cannot answer where anything was placed. Geometry comes from each popup's own `#32768`; parent and child are told apart by which window appeared. §08.3.8 |
| `get_accName` strips `&` and keeps the accelerator; a separator answers **S_FALSE with a null BSTR** | Comparing MSAA against an HMENU means stripping the title and nothing else, and a reader that tests only `FAILED(hr)` turns a separator into a nameless item |
| `native.modify_rules` is **0.1 ms across eleven consecutive menus**, for ~240 rule evaluations — about **0.4 us** each | Per-session memoization (§04.7) declined on its own gate. The purity whitelist is the real cost: memoizing something impure is a stale menu item, silently |
| A live configuration edit reaches the menu; **the second one never does** | The config watcher had been dying after exactly one reload per Explorer lifetime since it shipped. It re-points by calling `start()` from its own callback, and `start()` joined its own thread. §03.3b |
| `favorites = 2` with two identities pinned: **`Refresh \| NanaZip \| ———`** at the top of a real Explorer's menu, both origins | §05.6's pinned section works, and the identity scheme matches items a menu actually composed rather than only ones a test made up |

## 3. Where to pick up

Ordered by user value, with what is known about each. Everything above §3.6 has
now landed, and so has everything §3.6 listed as remaining except one item.

**If you read nothing else: §3.6 is the tally, and §3.9 is what to do next.**
**The twenty-item backlog is closed.** Item 19 — favorites and the rule
inspector — landed on 2026-08-25, so the tally is nineteen closed and one
partial (item 9's conditional attach, deferred with reasons that still hold).
What is left is a short list of named, deliberate exceptions rather than a
backlog; §3.9 has it.

### 3.1 A real *file* context menu in a third-party host — smaller than it looks

**The code path is verified. What is open is a fact about other people's
software.** That distinction was not clear before and it changes what this item
is worth.

`takeover.a_native_item_replays_its_own_identifier` builds its menu the way a
file manager does — `SHParseDisplayName` → `SHBindToParent` → `GetUIObjectOf` →
`IContextMenu::QueryContextMenu` — tracks it through Shell's hook **without**
`TPM_RETURNCMD`, and asserts the host is told which of *its* items was chosen by
wID exactly once. So the non-`RETURNCMD` half of `complete_host_contract` does
run, against a real borrowed shell menu, on this machine, every time the harness
is run.

What nobody has established is whether any shipping third-party host actually
chooses those flags. Explorer and Everything both set `TPM_RETURNCMD`, so both
take the other half. That is a survey question, not a correctness question, and
answering it needs a menu on an actual file inside Everything or Directory Opus.
Everything's result list does not respond to a posted `WM_CONTEXTMENU` in either
its keyboard or its point form — it handles right-click in its own subclass — so
driving it means real input into a host, or a person with the window in front of
them.

Both hosts run on this desktop with **no top-level window at all**, and both hold
a `shell.dll` mapped from before the current build (`GetModuleFileName` reports
the name the file had at load, so the path in the module list says nothing about
which build it is — `AGENTS.md`). Restarting them is cheap and invisible; opening
a lister to right-click a file in is not, and was not done.

**One command answers it** once somebody has that window open. Right-click a file
in the host, then:

```powershell
.\src\bin\x64\shell.exe -report perf
```

`host flags` says which half that host exercises, and `decisions` says whether
Shell composed the menu at all.

### 3.2 Ring export — **landed**, and both halves of the mystery are solved

`shell.exe -report perf` exists (`0b20dde`). Every host publishes its last
sixteen sessions into a named section of its own; the reader enumerates
processes and formats what it finds. `src/shared/PerfExport.h` has the design
and the contracts.

The "`perf` produces nothing from `explorer.exe`" puzzle this section used to
end with had **two** causes, both found and both now in `AGENTS.md`:

1. **The deploy landed one Explorer restart late.** `backup-and-upgrade.ps1`
   stopped Explorer, then copied; Windows brings the shell back in about a
   second, so the Explorer that came back had mapped the *old* binary.
2. **A registry value set from the agent's shell is not visible to Explorer.**
   A key created there took `HKCU\SOFTWARE`'s subkey count to 101 in that shell
   while Explorer still counted 100 and answered `ERROR_FILE_NOT_FOUND`.
   Elevation does not escape it; a scheduled task does.

So the previous session's probes were all correct — the registry read, the log
path, the permissions. Explorer had simply never seen the value, and was not
running the build that would have logged it.

### 3.3 `TakeoverSession` and the WinEvent lifecycle — **settled, and mostly declined**

§01.6's six-state machine was **measured for and not built**; §01.6a records the
numbers. A four-level cascade driven three ways (escape out, select deep, walk
across siblings) produced no duplicate `EVENT_OBJECT_CREATE`, no
`EVENT_OBJECT_SHOW` inside a CREATE handler, and no case where a positional pop
removed the wrong window. `Created`/`Prepared`/`Closing` would be states nothing
drives.

What *was* wrong sat next to it and did not depend on the events misbehaving at
all: `WM_NCDESTROY` erased the `WND` from `_map` **by handle** and popped the
**last** entry off `_level`. Two keys for one removal; the first disagreement
leaves a pointer to a destroyed object that the next submenu placement
dereferences. `Include/PopupLifecycle.h` removes by handle, and
`_level.size() == 1` became `parent_of_top() == nullptr`, which says the same
thing without assuming the stack is exactly in step.

`TakeoverSession` as an *object* is not buildable in the hook and that is worth
recording rather than re-attempting: the hook body is an SEH function, and MSVC
refuses (C2712) to compile one that holds anything needing unwinding. The
consolidation the section wanted has happened anyway, in plain-old-data form —
`perf::session_*`, `takeover_breaker()`, `plan_host_track`,
`complete_host_contract` — each of which is separately testable in a way a
single struct would not have been.

### 3.4 Reliability Center (§05.1) — complete, window included

Four of the five rows §05.1 sketches now exist, in text, for every host on the
desktop:

```text
Takeover
    handler          registered
    Windows 11 menu  redirected to Shell (-treat)

Explorer.EXE  pid 29804  x64  -  6 menus, 6 held
    pre-display  p50 9.8 ms   p95 55.1 ms   n=6
    decisions  6 takeover
                 provider {CAE3F1D4-...} 7.2 ms  ok           NanaZip
                 provider {AAF1E27D-...} 0.0 ms  quarantined  Unlock with File Locksmith
```

- **Last menus** — `0b20dde`, the ring export.
- **Providers, by name** — §05.1a. Names live in a directory beside the records
  rather than in them, because nothing outside a host that has built a menu can
  resolve a CLSID to a name: it comes from `GetTitle`, which takes the selection.
- **Quarantine** — §05.1b. `shell.exe -quarantine:add {clsid}`, a per-user file,
  effective on the next menu. It **skips** the provider rather than refusing its
  activation, which is both surgical and the only version that saves the cost.
- **Takeover** — `898c680`. Machine state, read from the registry by the manager.

- **The window** — §05.1d, `shell.exe -reliability`. The rows above, merged
  across hosts and sorted slowest first, with Quarantine and Release beside
  them.

"Repair takeover" is deliberately absent: everything else in that window reads
state or writes a per-user file, so it needs no elevation, and repair means
writing HKLM. `shell.exe -register -treat` already does it and the `Takeover`
block says when it is needed.

### 3.5 CoCI router de-dup and `priority` — **landed**, see §01.9b

Measured four ways rather than reasoned about, and the conclusion is stronger
than the plan's: on a machine registered with `-treat`, `priority` cannot
control anything, because COM does not fall back to the original class when a
`TreatAs` substitute fails. The setting is inert, the redirect is in charge,
and `shell.exe -check` now says so. Making `TreatAs` "authoritative when
healthy" — §9's original bullet — would have been the same behaviour with a
different name.

### 3.6 The backlog, item by item

This section said "three things" and was audited on 2026-08-24 against the
master plan's own twenty-item backlog rather than against its own memory. That
found one item it had lost entirely and one prerequisite it had assigned to the
wrong piece of work. Both corrections are below, in place.

The tally against `00-master-plan.md` §3, after the third 2026-08-25 session:
**nineteen items closed** — built, or measured and declined with the numbers
written down — and **one partial**.

| | Items |
|---|---|
| closed | 1–8, 10–20 |
| partial | 9 (conditional attach deferred, §01.9a) |
| open | — |

**Item 19 closed 2026-08-25**: parser provenance (`e6164ad`), then favorites and
the inspector (`7ab9f42`). Its gate had been stated wrongly for several sessions
and the correction is worth keeping, because it is the shape of mistake rule 6
exists for: this section used to say both halves were gated on item 17 *as a
whole*, which put them behind the presenter. They were not. §05.6 needed
origin-stable identity from `MenuModel` (step 6) and §05.7 needed that plus
provenance through the parser; neither needed step 7. The item was unblocked for
a session before anyone noticed.

**Two things came out of building it that are not about it**, and both are the
reason this kind of work is done against a real Explorer:

- **Item 16 was not as done as the table said.** Verifying the pinned section
  needs two consecutive live configuration edits, and the second one never
  arrived: the config watcher had been dying after exactly one reload per
  Explorer lifetime since it shipped, silently, with `watching()` still
  answering true. §03.3b. The original acceptance run made one edit.
- **§05.6's identity scheme was wrong for this product's own users.** It keys a
  custom item on where its rule sits in a file; for people who edit `shell.nss`
  constantly — which is who the config watcher exists for — inserting a line at
  the top un-pins everything below it. §05.6a records the departure.

That tally was itself wrong for a few hours on 2026-08-25 — it said seventeen
closed, counting item 20 whose middle third nobody has looked at, and counting
item 17 as wholly open after step 5 had landed. Rule 6 in §1 exists for exactly
this and was written in the same session that then broke it; the fix is that a
tally now names the item numbers rather than stating a count.

**Nothing in this section is open any more.** Everything below is recorded as
landed or declined, and is kept because the reasoning is what stops each one
being re-proposed. §3.9 lists what remains, which is a set of named exceptions
rather than a backlog.

**The Reliability Center window (§05.1) — landed.** `shell.exe -reliability`:
the providers every host asked, merged and sorted slowest first, with
Quarantine and Release beside them, over the exact text `-report perf` prints.
§05.1d. Looking at the real window found two defects no test written beforehand
would have had reason to check — six CLSIDs from one handler family rendering
as the same truncated string, and a raw mnemonic marker in a title.

**Interception backend abstraction (§01.9, backlog item 8) — settled, and mostly
declined.** *This section used to omit it altogether*, which is how the master
plan's **R2** came to be decided by nobody. Settled 2026-08-24 in §01.9c: the
interface is declined because the two mechanisms are **not** interchangeable —
the PE format makes an import table per-image, so the per-module fallback misses
late-loaded modules, `GetProcAddress` calls, delay-loaded imports and API-set
importers, all of which the one-thunk primary catches. An interface would assert
a substitutability the format denies. The per-hook-entry health check is declined
for the §7a reason: it cannot fail, because a displaced thunk would not have
routed the call into the hook that asks.

What was built is the part that was genuinely missing — *which* mechanism is
live, published per host and printed as an `intercept` line by
`shell.exe -report perf`. Measured in a real Explorer: `win32u import`.

**Seam step 7 of §04.4 — landed, and item 17 with it.** The painting and the
layer compositing are now `src/dll/src/MenuPresenter.cpp`: 1,606 lines out of
`ContextMenu.cpp`, which goes from 7,542 to 5,852. Both halves were extracted
by line range and verified byte-identical against the pre-move blob rather than
reviewed by eye, which is what made step 5 safe and is §04.4's rule for every
seam.

**What remains of step 7 is the naming, and it is deliberately not done.** The
functions are still `ContextMenu::` members. A `Win32MenuPresenter` class needs
an interface onto `ContextMenu`'s private state — `_theme`, `_items`,
`_hTheme`, `symbol`, `composition`, the DC and font caches — and that interface
cannot be designed honestly until the dependency is visible. Splitting the
translation unit is what makes it visible: **whatever `MenuPresenter.cpp`
reaches for is the presenter's surface**, and it is now a list somebody can
read off one file instead of inferring from seven thousand lines. That is the
next commit on this seam, and it is design work rather than moving.

Two things a translation-unit split cannot do by declaring, both recorded
because the next split will hit them too:

- **A definition in a header cannot be included twice.**
  `plutovg_stbi_write_png` has external linkage and is *defined* in
  `Include/stb_image_write.h`, so the second translation unit declares it
  instead. A duplicate symbol reports itself at link time, a long way from the
  mistake.
- **A type cannot be forward-declared into use.** `DWM`, `GET_HMENU` and the
  `ver` singleton were file-scope helpers, correct while there was one file.
  `Include/DwmWindow.h` is where they live now; `ver` became an inline
  variable, which is what keeps it one object with one initialisation across
  both units.

**It was never a capability gate**, and that correction still matters more than
the work: favorites needs `MenuModel`, which landed as step 6.

**Step 5 was not blocked by that, and saying it was is the second correction —
it has now landed.** Step 5 is selection layering, and the code it moves is not
in `ContextMenu.cpp`: it is in `Selections.cpp`, which `tests.vcxproj` **already
links**. It shipped as a byte-identical extraction followed by a behaviour
commit, and the split exposed a real defect — `Window.has_IShellBrowser` is set
from a window *class hash*, so a third-party host that embeds the real shell
view is classified as Explorer, sent to the browser provider, finds no
`IShellBrowser`, and had its `IShellExtInit` selection discarded. §04.4b has the
rule and why it is narrow.

**Two judgements that need a person at the machine**, both already measured and
neither blocking:

- the *visual* half of the flicker wait (§02.4a) — its cost is 7 ms, its benefit
  is a transient a screenshot cannot show;
- whether any real third-party host takes the non-`RETURNCMD` path (§3.1).

**Every measurement close-out is now taken**, which is what closed item 20:

- §04.7's lazy large-selection item — **declined** (2026-08-25).
  `selection.preparing` is 0.0 ms and metadata 1.3 ms for 200 items. Taking
  that measurement is what found the 645 ms menu next to it — see §02.3a,
  which is the largest first-paint defect this branch has fixed.
- §04.7's per-session memoization — **declined** (2026-08-25), the last third
  of item 20 and the only one that had never been measured at all.
  `native.modify_rules` is 0.1 ms across eleven consecutive menus for roughly
  240 rule evaluations, so one costs about 0.4 us. The decline rests on the
  risk rather than the size: the design is a purity whitelist, and memoizing
  something that is not pure shows a stale menu item silently.

**§03.5 still has two untested acceptance criteria**, and they are the smallest
open thing on the branch: a shadow whose manifest fails verification being
refused, and a fresh machine with a corrupt stock config reaching the clean
never-loaded refusal. Both need a machine state this one is not in.

### 3.6a The flagship's loop did not close for the provider that mattered most — fixed

Found 2026-08-24 by reading `shell.exe -report perf` on this desktop rather than
by reading the code that produces it, and fixed the same day (§05.1c). Kept here
because how it was found is more reusable than what it was. The report as it
stood:

```text
provider {CAE3F1D4-7765-4D98-A060-52CD14D56EAB}  5.0 ms  ok  NanaZip
provider 4f1b2d3a                               70.0 ms  ok
provider ab7282d1                                0.0 ms  deferred
```

The second line is **89 % of a 78.6 ms menu** and prints a bare hash.
`-quarantine:add` takes a CLSID, so the one provider on this machine worth
quarantining is the one the user cannot name to the command. Six more deferred
providers below it are in the same position — and those are ones Shell has
*already judged slow*, where quarantine is the difference between a decision
re-probed every 200 menus and a permanent one.

The cause is that provider **identity** was made to depend on provider
**presentation**. `ExplorerCommand.cpp` records the CLSID and the title in one
call, gated on `!item->title.empty()`, and the quarantined and deferred paths
`continue` before reaching it. §05.1a's own comment claims the hash fallback is
only for "a provider this host has never successfully activated — which is also
one there is nothing useful to say about yet"; this machine's data contradicts
both halves, because `4f1b2d3a` reports `ok`.

`reg.clsid` is in scope at the top of the candidate loop, before every one of
those `continue`s, so identity can be recorded there and presentation left where
it is. §05.1c has what was done.

### 3.7 The tools this branch has accumulated

Every defect found in the last three sessions came from running the code, not
from reading it. The cheap tools that made that possible are worth keeping to
hand:

- `hostprobe --takeover` puts Shell into a process you own with no deployment.
- Posting `WM_CONTEXTMENU` to Explorer's `SHELLDLL_DefView` raises a real
  Explorer menu without touching the mouse. Two things to get right, both of
  which cost an attempt: `GetClassNameW` needs `CharSet.Unicode` on the
  `DllImport` or every class name comes back as its first character, and the
  `lParam` is a **screen** point that must be inside the view's own rectangle.
- Reading the `#32768` window back through `AccessibleObjectFromWindow` tells
  you what is in the menu, which is how Shell's menu is told from Windows' own.
- Watching which window *class* appears tells you classic (`#32768`) from
  modern (`Microsoft.UI.Content.PopupWindowSiteBridge`).
- `shell.exe -report perf` now tells you what any host on the desktop paid,
  what Shell decided, and which flags the host passed.
- A per-user `HKCU\Software\Classes\CLSID\…\InprocServer32` override points COM
  at a build without touching HKLM or restarting Explorer. **This is how to run
  the harness against the build you just made rather than the installed one**,
  and it is what made seam steps 6 and 7 verifiable without a deploy. The whole
  recipe, because two thirds of it are not obvious:

  ```powershell
  # 1. the build output has no configuration beside it, so the composed menu
  #    would contain no custom items at all - stage the stock one first
  Copy-Item src\bin\shell.nss src\bin\x64\shell.nss -Force
  Copy-Item src\bin\imports  src\bin\x64\imports -Recurse -Force

  # 2. point COM at it (CLS_ContextMenu, src/shared/Globals.h)
  $k = 'HKCU\SOFTWARE\Classes\CLSID\{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}\InprocServer32'
  reg add $k /ve /t REG_SZ /d "$PWD\src\bin\x64\shell.dll" /f
  reg add $k /v ThreadingModel /t REG_SZ /d Apartment /f

  .\src\bin\x64\hostprobe.exe --takeover --verify .\src\tests\hostprobe\fixtures

  # 3. put the machine back
  reg delete 'HKCU\SOFTWARE\Classes\CLSID\{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}' /f
  Remove-Item src\bin\x64\shell.nss, src\bin\x64\imports -Recurse -Force
  ```

  Three things to know before trusting a run of it:

  - **Check the override is in force rather than assuming.** `hostprobe`'s
    `takeover:` line prints the path it loaded, and it reads the *merged* HKCR
    view, so HKCU shadows HKLM. Pointing it at a nonexistent path is the cheap
    proof: it then says `LoadLibrary failed: 126` instead of silently testing
    the installed copy.
  - **Two `shell.dll`s will be mapped**, and the harness says so. Other
    registered Shell classes still resolve from HKLM; the one that matters is
    the one on the `takeover:` line, because that is where `BootstrapOnce` ran
    and therefore whose hook composed the menu.
  - **Explorer is unaffected**, which is the point — it has `shell.dll` pinned
    from before, so the user's live menus keep running the installed build and
    nothing needs restarting.
- `shell.exe -reliability` is now the fastest way to see every provider a
  machine's menus asked, merged across hosts and sorted slowest first. Faster
  than reading a `-report perf` when the question is "which extension".
- **Shift+Alt+right-click annotates every item with where it came from**, and
  `menu.inspected` in `-report perf` says the gesture registered. When a menu
  contains something you did not expect, that is faster than reading the
  configuration - it names the file and line of every rule that touched the
  item. Section 05.7a.

Added 2026-08-25, because the largest defect on this branch was found with them:

- **Driving a real multi-file selection needs no input injection.**
  `New-Object -ComObject Shell.Application`, `.Open($dir)`, find the window by
  `$w.Document.Folder.Self.Path`, then `$w.Document.SelectItem($item, 1)` per
  item (`SVSI_SELECT`). Then post `WM_CONTEXTMENU` to the `SHELLDLL_DefView`
  *inside that frame* — `$w.HWND` is the frame, not the view. This is what made
  §02.3a measurable; the desktop background menu never produces a selection and
  so never showed the defect.
- **Read a menu back before and after any change to the menu path.** Item count
  plus names through `AccessibleObjectFromWindow` takes seconds and is the only
  cheap regression check that exists for composition. Wait for two identical
  readings first — see `AGENTS.md` on why a menu read during Explorer's startup
  measures Explorer.

Four PowerShell traps, each of which cost an attempt this session and none of
which announces itself:

- **A callback's writes need `$script:`.** `EnumWindows`/`EnumChildWindows`
  delegates run outside the enclosing function's scope, so a plain local
  accumulator is written by nobody and comes back empty. It looks exactly like
  "no such window exists".
- **`$null` to a P/Invoke `string` marshals as `""`, not `NULL`.** So
  `FindWindowW('Progman', $null)` searches for a window whose title is the empty
  string. Enumerate rather than guess a parent chain — the desktop view sits
  under `Progman` or a `WorkerW` depending on the moment.
- **`0xFFFFFFFC` parses as Int32 `-4`.** `OBJID_CLIENT` has to be written
  `[uint32]4294967292` or the call refuses to convert.
- **`GetWindowText` cannot read another process's `EDIT` or `LISTBOX`.** It
  returns the window *caption*, which those controls do not have, so you get an
  empty string from a control that is full. Use the marshalled messages —
  `WM_GETTEXTLENGTH`, `EM_GETLINECOUNT`, `LB_GETCOUNT`, `LB_GETTEXT`. This
  briefly looked like the Reliability Center's details pane was broken when it
  held 2,030 characters.

### 3.8 The rendering coverage — specified here, then built (`f17373c`)

Seam steps 6–7 were gated on harness coverage of composed-menu **rendering**,
and that phrase had been repeated for three sessions without anyone saying what
it would be. This section said; the next session built it. It is kept as
written, because the reasoning is what the scenarios mean — what was actually
built, and the four things that had to be measured first, are in
`src/tests/hostprobe/fixtures/README.md` under "The rendering scenarios have no
fixture, and cannot have one".

Two things the plan below got wrong, both found by building it:

- **The reader must wait for the popup window, not for a message.** Under
  takeover the earliest message saying "a menu is up" is a `WM_INITMENUPOPUP`
  Shell *synthesises* for the borrowed host menu, sent before Shell has
  composed anything. The first run read zero popups and reported that Shell
  had drawn nothing.
- **The documented MSAA descent cannot answer where a submenu was placed.** A
  menu item's `get_accChild` does return the submenu popup, as the Menu Item
  page says — but its `accLocation` is `(0,0 0x0)` whether or not the submenu
  is open. A placement assertion built on it would have compared a rectangle
  at the origin against its parent and passed or failed for the wrong reason.

**The pieces were all present; none of them was joined up.**

- `hostprobe --takeover` already raises **Shell's own composed menu** — the four
  `takeover.*` scenarios build a real shell menu through
  `SHParseDisplayName` → `GetUIObjectOf` → `QueryContextMenu` and track it
  through the hook.
- Reading a live menu back is solved and done twice: §05.3 walked Shell's menu
  in a real Explorer through `AccessibleObjectFromWindow` on the `#32768`
  window, and §05.5a measured the popup's rectangle to prove smart columns.
- What is missing is a scenario that does both and **asserts**.

**The shape it has to take.** The owner thread is blocked inside the tracking
call while the menu is up, so the reading happens on a second thread — exactly
as the MSAA probe did. That thread waits for the `#32768` to appear, reads what
it needs, and posts `WM_CANCELMODE` to the owner to end the menu.

**Assert properties, not traces.** A composed menu's *contents* depend on which
handlers the machine has installed, so recorded fixtures are impossible — the
same constraint the `takeover.*` scenarios already live under, and they solve it
by asserting properties. The properties worth pinning before moving paint code:

- every item Shell composed is readable through MSAA, with separators unnamed
  and `role=separator` (this is §05.3's result, currently proved by a probe that
  no longer exists);
- item **order** matches the composition order, which is what `MenuModel` in
  step 6 is most likely to disturb;
- a submenu reports `STATE_SYSTEM_HASPOPUP`, and a disabled item
  `STATE_SYSTEM_UNAVAILABLE`;
- the popup's rectangle for a known item count, which is what catches a measure
  pass that silently changed — §05.5a's 239×1031 versus 938×990 is the precedent;
- a submenu is placed against its **parent**, not the root — the property
  `parent_of_top()` exists for, verified once by hand in §3.3 and pinned
  nowhere.

**Why this order.** Every one of those is a regression that step 7 could
introduce and that no current test would notice: they look like a menu that
draws slightly wrong. Get them asserting first, then move `MenuModel`, then the
presenter. §04.4's rule — move code, don't improve it in the same commit — is
what made step 5 safe, and step 5 is the proof it works: the extraction was
verified byte-identical and the behaviour change landed separately.

**One caution.** `hostprobe` shows real popups on screen and the reading thread
must always arm a watchdog; a menu left standing blocks its tracking call
forever. `EndMenu` ends "the calling thread's active menu" and the reader is not
that thread, so the documented alternative — posting `WM_CANCELMODE` to the
owner — is the one to use.

### 3.9 What is left, now that the backlog is closed

**The twenty-item backlog of `00-master-plan.md` §3 is closed.** Nineteen items
built or measured-and-declined with their numbers written down, one partial.
That is not the same as "there is nothing to do", and the difference matters to
whoever reads this next: what follows is a set of **named, deliberate
exceptions**, each with the reason it is where it is. None of them is a task
nobody has looked at.

**Sorted by what a reader can actually act on here.**

#### Buildable on this machine

- **`Win32MenuPresenter` as a named class.** The only architectural item left.
  Seam step 7 moved 1,606 lines of painting and layer compositing into
  `MenuPresenter.cpp` and stopped there on purpose: naming the class needs an
  interface onto `ContextMenu`'s private state, and that interface could not be
  designed honestly until the dependency was visible. It is now — whatever
  `MenuPresenter.cpp` reaches for *is* the presenter's surface, readable off one
  file instead of inferred from seven thousand lines. This is design work over
  that list, not moving. §3.6 has the two mechanics a split hits.
- **The `where=` half of the inspector**, if a way is found to show what a rule
  evaluated to *without re-evaluating it*. §05.7a declines re-evaluation because
  it would run the configuration a second time with a different `_this` and
  could disagree with what the menu actually did. Capturing the verdict during
  composition, behind the inspector's own gate, is the shape that would work;
  nobody has costed it.

#### Needs a machine state this one is not in

- **§03.5's two untested acceptance criteria**: a shadow whose manifest fails
  verification being refused, and a fresh machine with a corrupt stock config
  reaching the clean never-loaded refusal.
- **The MSI upgrade matrix.** `AGENTS.md` has the one row that *is* verifiable
  here and how to run it.

#### Needs a person at the machine

- **Whether any shipping third-party host takes the non-`RETURNCMD` path**
  (§3.1). The code path is covered by
  `takeover.a_native_item_replays_its_own_identifier` against a real borrowed
  shell menu; what is open is a survey question about other people's software.
- **The visual half of the flicker wait** (§02.4a). Its cost is 7 ms and its
  benefit is a transient a screenshot cannot show.
- **That the inspector's tooltip renders** (§05.7a). MSAA does not read tips, so
  the machine can confirm the gesture registered — `menu.inspected` in
  `shell.exe -report perf` — and that composition is unchanged, but not what the
  tip looks like.

#### Deferred with reasons that still hold

- **Item 9's conditional attach** (§01.9a). Two reasons: the detour is already
  installed only inside `if(rt.loader.explorer)`, so the blast radius is
  narrower than the plan assumed; and the policy does not exist yet when the
  decision would be made, so it would mean installing an inline detour from a
  background thread while menus are open. Revisit alongside `TakeoverRouter`.
- **Everything in §3.6 and §3.3 recorded as declined.** Each is there with its
  measurement precisely so it is not re-proposed: the icon cache, `MSAAMENUINFO`,
  the taskbar rectangle model, `priority` over `TreatAs`, the interception
  interface and its health check, the six-state lifecycle machine, catalog
  persistence, lazy large-selection metadata, and per-session memoization.

**One habit to carry forward, because it earned its keep twice in the last
session.** Both of that session's most valuable findings were defects in work
that was already marked done, and both surfaced while trying to *verify
something else* in a real Explorer: the config watcher serving exactly one
reload per process (§03.3b), and a `std::wstring_view` built from a null
`c_str()` faulting on the first item of every menu. Neither had a crash, a log
line, or a failing test. The unit suite is 32,800 checks and could not have
found either. **Deploy it, drive it, and read the menu back.**

## 4. Things that will bite

- **A registry value set from an agent's shell is invisible to `explorer.exe`,
  and elevation does not help.** Use a scheduled task; `AGENTS.md` has the
  recipe. Files and HKLM are unaffected.
- **`ShellCheckConfig` must not call `BootstrapOnce`, but it does have to call
  `init(HINSTANCE)`.** The first half is why the export exists at all - asking
  the DLL a question must not install a hook in the asking process. The second
  half was missing, and bare `shell.exe -check` therefore answered "no
  configuration file was found" on every machine ever. The call is guarded on
  `Initializer::instance` being null so it only ever *establishes* paths; the
  export is callable from a live host, where resetting `application.Config`
  underneath an open menu would turn a read-only diagnostic into something that
  changes what it is diagnosing.
- **The export's block is written by several threads.** `export_session` runs
  outside the ring's mutex, so two menu threads publishing at once both drove
  the seqlock - which protects a *reader* and does nothing against concurrent
  writers. `PerfExportWriter` serialises now. The file comment's "exactly one
  writer for its whole life" means one *process* per block, which is what makes
  the security descriptor the right boundary; it was never a claim about
  threads.
- **The provider-name directory is read outside the sequence protocol, on
  purpose.** It is append-only and the entry is written before the count admits
  it, so the worst a reader sees is one name short. Folding it into the seqlock
  would put a shared-memory write on the menu path and make a reader retry
  whenever a provider first appeared.
- **`PopupStack` removes by handle, not by position.** Restoring `pop_back()`
  reintroduces a dangling pointer that only bites when the events arrive out of
  order - which this machine never produced, which is exactly why it has to be
  structural rather than tested for.
- **`scripts/backup-and-upgrade.ps1` restarts Explorer last, and verifies it.**
  Do not reorder it, and do not read the installed file's creation time to
  decide whether a deploy is current — NTFS file tunneling puts the old one
  back.
- **A concurrency test cannot always assert its own premise.** The export's
  seqlock has two sequence reads; a writer in a tight loop tears *every* read
  and a throttled one tears none, so no timing-based test can distinguish a
  working second read from one that refuses everything. `perf_export_load`
  takes a seam that runs at exactly that moment instead. Deleting the second
  read leaves every other test in the file passing, which is how the gap was
  found.
- **`Object` has two ways of being asked "is this true?" and they disagree.**
  A non-template `explicit operator bool` means *not null* and wins
  `static_cast<bool>`; the numeric template means *not zero* and is what a bool
  destination picks. `settings { priority = 0 }` read through a cast reports as
  switched on. Use `to_bool()`; `test_expression` fails if that is
  "simplified".
- **`ProviderHealth` judges on a provider's *best* time and never before its
  second sample.** Both rules exist because the first menu in a process is cold
  and makes every provider look pathological; a one-sample rule would defer two
  more providers per menu until the menu had no packaged verbs left,
  permanently. `test_provider_health.cpp` simulates it. Do not "simplify"
  either rule.
- **`menuitem_t::image` is owned on one path and borrowed on the other.**
  `image_owned` says which. Deleting unconditionally destroys Explorer's bitmaps;
  deleting never is the leak `745dd81` fixed.
- **`MenuItemInfo::Signed()` identifies Shell's own `dwItemData` by `cbSize` at
  offset 0**, and `MenuItem.h:918` identifies a *foreign* one by `MSAA_MENU_SIG`
  at the same offset. Anything that changes what lives at offset 0 breaks both.
- **The provider cache is thread-local and holds live third-party COM objects.**
  That is what makes it apartment-safe without marshalling. Do not hoist it to
  process scope.
- **The hook's `__finally` must not overwrite a decision that was already
  recorded.** `BypassOnce` and `Degraded` are set before `__leave`; blanket
  `FailOpen` would erase the only evidence of why a menu was the host's own.
- **`ConfigWatcher` runs `init()` on its own thread.** Safe by the snapshot
  design and serialised by `_reload_mutex`. It has now been exercised inside a
  real `explorer.exe` — editing the installed `shell.nss` under elevation and
  watching the menu follow — but only for the `priority` setting.
- **`string::c_str()` answers `nullptr` when the string is empty, and a
  `std::wstring_view` built from one faults.** `return valid() ? m_data :
  nullptr`, and `wstring_view`'s pointer constructor calls
  `char_traits::length` on what it is handed. The strings that are empty are
  not exotic: a top-level menu item's `path` is, so this took out the *first
  item of every menu* the moment anything read it. No crash and no log line —
  the access violation lands inside the hook's SEH region, where `catch(...)`
  under `/EHsc` does not catch — so the symptom is a menu that never appears.
  `MenuIdentity::view` is the guard; `AGENTS.md` has the family.
- **The config watcher re-points itself from its own callback.**
  `ConfigWatcher::start` is called by `Initializer::init`, which *is* the
  watcher's callback, on the watcher's thread. It used to begin with `stop()`,
  which joined the calling thread — `resource_deadlock_would_occur`, thrown,
  swallowed by `load_generation`'s `catch(...)`, with the stop event already
  signalled. Live editing worked once per Explorer lifetime. Do not reintroduce
  a `stop()` on that path; §03.3b and `rearm()` are the shape that works.
- **`string::Copy(dst, src, count)` writes `count + 1` slots** — count
  characters and *then* a terminator. Passing the capacity overruns by one. Same
  family as the `release(n - 1)` shape.
- **`hostprobe.exe` is built but never run by `build.ps1`.** It creates a window
  and shows real menus. It does not inject desktop-wide input — keys are posted
  to its own thread queue — but it will put popups on screen.
- **The harness transient is diagnosed and fixed (2026-08-25).** It was ours,
  not Windows': `TrackPopupMenu`'s remarks say that when the caller is already
  the foreground window, *"the second time this menu is displayed, it appears
  and then immediately disappears"* unless a benign message forces a task
  switch. This harness shows 23–31 menus in a row from one window, so every one
  after the first is "the second time". Three things were wrong and all three
  are fixed: the documented `SetForegroundWindow`/`PostMessage(WM_NULL)` pair
  was missing; the driver blind-waited 800 ms for a "menu is up" event that
  **`TPM_NONOTIFY` makes impossible** and then pressed keys at a menu it had
  never confirmed; and scenarios ran back to back with no settle, so teardown
  raced the next `TrackPopupMenu`. 26 consecutive clean runs afterwards,
  against 2 failures in 14 before.

  **What made it findable is worth more than the fix.** A fixture mismatch went
  through `diff()`, which prints the offending line and *no* `FAIL` and *no*
  scenario name — so the run reported a failure count with nothing in the
  output that any filter for "FAIL" would show. That is why two sessions saw
  it and neither could say what failed. It names the scenario now. **When a
  failure is hard to reproduce, check first that it is reported in the same
  words as every other failure.**

  The advice to run on a quiet desktop still stands for a different reason: a
  second popup belonging to another application makes the `render.*` readings
  meaningless, and they now say so rather than asserting anyway.
- **`--takeover` is one-way within a run.** Shell pins its own module once its
  hooks are installed, so a process cannot go back to native afterwards. Never
  mix takeover and native scenarios in one invocation.
- **The order of the harness scenarios is load-bearing.** The `takeover.*` cases
  must stay last, after the plain popups Shell declines. That ordering is what
  exposed the breaker counting declines, and moving them would lose the
  coverage silently.
- **`--shell` cannot redirect the shell-namespace scenarios.** COM activates
  Shell by the path in the registry, so those scenarios test the *installed*
  copy no matter what `--shell` says. Two knowingly broken builds passed every
  assertion before this was caught; the harness refuses the configuration now
  and prints every `shell.dll` mapped in the process.
- **Which Shell `--takeover` loads decides which configuration it runs.**
  `Initializer` derives `shell.nss` from the directory of the module it was
  given, so the build output means no configuration at all (the identity config
  §06.2 asks for) and the installed copy means the user's real rules.
- Re-record fixtures with `--record` and *always* re-run `--verify` twice
  afterwards. Two scenarios were non-reproducible once (`WM_DRAWITEM` is a paint,
  not a contract) and the fix was to stop recording it inline.
- **An instrument that truncates silently is worse than no instrument.** Twice
  now: `PERF_EXPORT_PHASES` at 16 dropped exactly the late phases, and
  `PERF_EXPORT_PROVIDERS` at 8 described a 27-provider menu with eight lines
  summing to 9 ms of 450. Both had a `dropped_*` counter; the second one was
  carried through the export and **printed by nobody**, so it may as well not
  have existed. When adding a cap, print its overflow in the same commit.
- **`src/exe/src/Main.cpp` has three collisions worth knowing before editing
  it.** `std::max(` expands through windows.h's `max` macro to `std::(` — write
  `std::max<int>(`, as the rest of the tree does. `ID_CLOSE` is already
  `#define`d at the top of the file, so a new control id by that name silently
  redefines a button. And there is a global `p`, so any local of that name is a
  C4459 the test project treats as an error.
- **`MenuPerfScope` needs `Include/Diagnostics/MenuPerf.h` included explicitly.**
  `DiagnosticsRing.h` does not pull it in, and the error — "is not a member of
  `Nilesoft::Shell::Diagnostics`" — reads as a namespace problem rather than a
  missing include.
- Traps that cost real time are in `AGENTS.md`: a variadic template call inside
  an SEH function (C2712, reported at the `__try`), heredoc'd patch scripts
  eating a backslash level in **both** directions, and the three ways an
  experiment can test something other than what you think.

## 5. Verifying the tree

```powershell
.\build.ps1 -Platform all
.\src\bin\x64\hostprobe.exe --verify .\src\tests\hostprobe\fixtures
.\src\bin\x64\hostprobe.exe --takeover --verify .\src\tests\hostprobe\fixtures
```

At the time of writing: **32,837 checks / 0 failures** on x64 and x86, arm64
builds and packages, 0 warnings, `check-invariants: OK (10 rules, 0 deferred)` -
both formerly-deferred rules turned on with their phase, as section 06.3
intended - **23 harness scenarios native and 31 through takeover**, 0 failures.

The takeover count is 31 rather than 27 because of the four `render.*`
scenarios (section 3.8). Both modes are stable now: the transient that made a
single failure untrustworthy was diagnosed and fixed on 2026-08-25 (section 4),
so **a harness failure is a finding again** rather than something to re-run
until it goes away. A run takes about 24 s native and 32 s through takeover.

`shell.exe -check`, `-report perf`, `-quarantine` and `-favorites` are the
pieces of this branch a user drives directly:

```powershell
.\src\bin\x64\shell.exe -check:path\to\shell.nss
.\src\bin\x64\shell.exe -report perf
.\src\bin\x64\shell.exe -quarantine:add {CAE3F1D4-7765-4D98-A060-52CD14D56EAB}
.\src\bin\x64\shell.exe -favorites:pin "native:Refresh"
```

`-favorites` is the fourth. It lists what has been recorded and pins or releases
one; promotion into the menu needs `settings { favorites = N }` as well, and
without that setting the list is only a record. An identity contains spaces, so
quote it.

`-check` with no argument works now too, and reads the `shell.nss` beside the
exe it was run from. That is **not** the same as "whatever this machine would
load" when you run the copy in `src\bin\x64\`, which has no configuration beside
it — a distinction that cost an experiment, because the first attempt at §03.5's
acceptance test used the bare form to confirm a file was broken and so confirmed
nothing at all.

Note that neither `cmd` nor PowerShell waits for a Windows-subsystem process, so
read the exit code with `Start-Process -Wait -PassThru`. §03.1b records why that
is not being fixed yet.

**What has now run inside a real Explorer**, deployed 2026-08-24 with the
corrected deploy ordering, driven by posting `WM_CONTEXTMENU` to
`SHELLDLL_DefView`:

- the composed menu appears, and every item in it is readable through MSAA
  (§05.3);
- pre-display costs **10.3 ms p50 warm**, 60 ms on the first menu in a process,
  inside §06.4's 15 ms budget — the first time this branch's first-paint work
  has been measured where it runs;
- the provider reuse shows in that number: `explorer.commands` is 8–15 ms warm
  for 22–25 items, against the ~170 ms `5fe6354` was written for;
- the config watcher reloads an edit to the installed `shell.nss` without an
  Explorer restart;
- the `TreatAs` redirect, not `priority`, decides the Windows 11 menu;
- the vertical-blank wait costs another ~7 ms after pre-display stops.

Added 2026-08-24, same method:

- **submenu placement** — a submenu opens at x=612 against a root whose right
  edge is 614, not at the root's own x=401, which is the shape a broken
  `parent_of_top()` would produce; the stack drains to zero on close (section 3.3);
- **provider names and quarantine** — `NanaZip` reported at 7.2 ms, then
  quarantined and reported at 0.0 ms, with the composed menu read back through
  MSAA at **29 items against 30** (section 05.1b);
- **the last-known-good shadow** — the installed `shell.nss` broken until
  `-check` reported `(43,28): error: Property unexpected`, Explorer restarted,
  and the menu still came back at **213 x 680, identical to the baseline**. This
  is section 03.5's outstanding acceptance criterion and the in-memory design
  could never have covered it;
- **bare `-check`** from the installed directory now exits 0 rather than
  reporting no configuration at all.

Added 2026-08-25, same method:

- **favorites** (section 05.6a) — with `favorites` unset the settled desktop
  menu reads 30 items beginning `View | Sort by | Refresh`; with
  `settings { favorites = 2 }` and `native:Refresh` plus a NanaZip CLSID
  pinned it reads **`Refresh | NanaZip | ———`** at the top and the rest below.
  Both origins promoted, the separator placed, the cap honoured. Two full
  OFF-to-ON round trips live, with no restart;
- **the config watcher, repeatedly** (section 03.3b) — those round trips are
  the verification. Before the fix the first edit reached the menu and no later
  one did, for the life of the process;
- **the inspector** (section 05.7a) — `menu.inspected` appears in
  `shell.exe -report perf` when the menu is raised with Shift+Alt held, and the
  composed menu is byte-identical to the uninspected one. What the *tooltip*
  looks like is not machine-readable and was not read.

One methodological note, because it nearly produced two false findings in one
session. Menus read within a minute of an Explorer restart differed in both
item count and native ordering — 30 items with two Directory Opus entries in
one reading, 27 without them in another, both stable across three consecutive
reads. Only readings taken from the *same settled Explorer session*, toggled
back and forth, are comparable; the item count is not the signal, the position
of the promoted block is. That is `AGENTS.md`'s third trap, met again in a
form where "wait for two identical readings" was not by itself enough.

**Still unverified there:** whether any real third-party host takes the
non-`RETURNCMD` path (section 3.1 — the code path itself *is* covered, by
`takeover.a_native_item_replays_its_own_identifier` against a real borrowed
shell menu), and the *visual* half of the flicker wait (section 02.4a).
