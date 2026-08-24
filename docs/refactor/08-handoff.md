# 08 — Handoff

**Updated 2026-08-24, branch `refactor/takeover-master-plan`.**
Read this first, then `06-phases-and-tests.md` for the per-item status table.
`00`–`05` are the plan; `07` is the audit that corrected it; this is where the
work actually stands and what to do next.

---

## 1. How this work is being done

The plan sequences by architectural layer. That is **not** the order being
followed, and the departure is deliberate: work is sequenced by **what can be
proved on this machine**, taking small verified defects first and large
speculative pieces last, so every commit ships.

Six rules have held throughout and are worth keeping. A seventh (rule 6) was
learned late, by an audit that found this document had quietly stopped being
true:

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
4. **Declining is a result, and it gets written down.** Four items in this
   branch were measured and *not* built. Each is recorded with its numbers in
   the plan document that proposed it, so it is not re-proposed by the next
   reader: the icon cache (§04.7), `MSAAMENUINFO` (§05.3), the taskbar
   rectangle model (§02.5a), and making `priority` authoritative over the
   `TreatAs` redirect (§01.9b).
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
   values Explorer could not see. Both are now in `AGENTS.md` under "Two ways an
   experiment can test something other than what you think", because both
   produced results that read as findings about Windows.

## 2. What has landed on this branch

Thirty-six commits, `940ff6a`..`6b26e61`.

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

## 3. Where to pick up

Ordered by user value, with what is known about each. Everything above §3.6 has
now landed. What is left is one large item, one presentation item, one decision
that was being made by omission, and — found by an audit of this section against
the master plan's backlog — one small defect in the flagship feature (§3.6a).

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

### 3.4 Reliability Center (§05.1) — everything except the window

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

What is left is **the window**, and the "Repair takeover" action. Everything it
would present is available and actionable from the command line today, which is
why it is last rather than first — this is presentation over data that exists.

### 3.5 CoCI router de-dup and `priority` — **landed**, see §01.9b

Measured four ways rather than reasoned about, and the conclusion is stronger
than the plan's: on a machine registered with `-treat`, `priority` cannot
control anything, because COM does not fall back to the original class when a
`TreatAs` substitute fails. The setting is inert, the redirect is in charge,
and `shell.exe -check` now says so. Making `TreatAs` "authoritative when
healthy" — §9's original bullet — would have been the same behaviour with a
different name.

### 3.6 What is genuinely left

This section said "three things" and was audited on 2026-08-24 against the
master plan's own twenty-item backlog rather than against its own memory. That
found one item it had lost entirely and one prerequisite it had assigned to the
wrong piece of work. Both corrections are below, in place.

The tally against `00-master-plan.md` §3: **fourteen items closed** — built, or
measured and declined with the numbers written down — **three partial**, **three
open**.

**The Reliability Center window (§05.1).** Additive, no risk to the menu path,
and verifiable by launching it. It is presentation over data that all exists.

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

**Seam steps 5–7 of §04.4** — selection layering, `MenuModel`,
`Win32MenuPresenter`. This is the one large item on the branch, and it is the
gate for both remaining capabilities: favorites (§05.6) needs origin-stable
identity from `MenuModel`, and the rule inspector (§05.7) needs that plus
file-and-line provenance threaded through the parser.

**Steps 6–7 have not been started, deliberately.** `ContextMenu.cpp` is 7,559
lines, the test project does **not** link it, and those two seams are the paint
and window-message halves — the parts whose regressions look like a menu that
draws slightly wrong rather than a test that fails. The verification available
here is one Explorer and a screenshot, which is not enough to move that code
confidently. §04.4's own rule ("move code, don't improve it in the same commit;
each seam lands with its unit suite where pure or harness coverage where
hosted") is the right one, and satisfying it needs the harness to grow coverage
of composed-menu *rendering* first.

**Step 5 is not blocked by that, and saying it was is the second correction.**
Step 5 is selection layering, and the code it moves is not in
`ContextMenu.cpp`: `Selections::QueryShellWindow` is `Selections.cpp:1308`, and
`tests.vcxproj` **already links `..\dll\src\Selections.cpp`** — which is how
`test_selection_path_resolver.cpp` exists at all. So step 5 can land with a unit
suite today, on this machine, with no rendering harness in front of it. It is
the cheapest of the three and it shrinks what 6–7 have to move.

**Two judgements that need a person at the machine**, both already measured and
neither blocking:

- the *visual* half of the flicker wait (§02.4a) — its cost is 7 ms, its benefit
  is a transient a screenshot cannot show;
- whether any real third-party host takes the non-`RETURNCMD` path (§3.1).

**Two measurement close-outs**, both cheap and both now answerable because the
ring exists:

- §04.7's lazy large-selection item is gated on `selection.preparing` appearing
  in the ring's p95s. It reads **0.0 ms** warm in a real Explorer — but for a
  single-file selection, which is not the case the item is about. One
  large-selection measurement closes it either way.
- §03.5 still has two untested acceptance criteria: a shadow whose manifest
  fails verification being refused, and a fresh machine with a corrupt stock
  config reaching the clean never-loaded refusal.

### 3.6a The flagship's loop does not close for the provider that matters most

Found 2026-08-24 by reading `shell.exe -report perf` on this desktop rather than
by reading the code that produces it. The report as it stands:

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
  at a build without touching HKLM or restarting Explorer.

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
- **`string::Copy(dst, src, count)` writes `count + 1` slots** — count
  characters and *then* a terminator. Passing the capacity overruns by one. Same
  family as the `release(n - 1)` shape.
- **`hostprobe.exe` is built but never run by `build.ps1`.** It creates a window
  and shows real menus. It does not inject desktop-wide input — keys are posted
  to its own thread queue — but it will put popups on screen.
- **Run the harness on a quiet desktop.** Observed once on 2026-08-24: a native
  run started seconds after `backup-and-upgrade.ps1` restarted Explorer, and
  while a script was posting `WM_CONTEXTMENU` at the desktop, reported
  `23 scenario(s), 1 failure(s)`. Five consecutive runs afterwards were clean,
  and which scenario failed was not captured. So this is recorded as an
  observation rather than a diagnosis: a run that overlaps an Explorer restart
  or another menu on the same desktop is not a run to trust. Re-run before
  believing a single harness failure — and capture the output when you do,
  which is what was not done here.
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
- Traps that cost real time are in `AGENTS.md`: a variadic template call inside
  an SEH function (C2712, reported at the `__try`), heredoc'd patch scripts
  eating a backslash level in **both** directions, and the two ways an
  experiment can test something other than what you think.

## 5. Verifying the tree

```powershell
.\build.ps1 -Platform all
.\src\bin\x64\hostprobe.exe --verify .\src\tests\hostprobe\fixtures
.\src\bin\x64\hostprobe.exe --takeover --verify .\src\tests\hostprobe\fixtures
```

At the time of writing: **32,509 checks / 0 failures** on x64 and x86, arm64
builds and packages, 0 warnings, `check-invariants: OK (10 rules, 0 deferred)` -
both formerly-deferred rules turned on with their phase, as section 06.3
intended - 23 harness scenarios native and 27 through takeover, 0 failures.

`shell.exe -check`, `-report perf` and `-quarantine` are the three pieces of
this branch a user drives directly:

```powershell
.\src\bin\x64\shell.exe -check:path\to\shell.nss
.\src\bin\x64\shell.exe -report perf
.\src\bin\x64\shell.exe -quarantine:add {CAE3F1D4-7765-4D98-A060-52CD14D56EAB}
```

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

**Still unverified there:** whether any real third-party host takes the
non-`RETURNCMD` path (section 3.1 — the code path itself *is* covered, by
`takeover.a_native_item_replays_its_own_identifier` against a real borrowed
shell menu), and the *visual* half of the flicker wait (section 02.4a).
