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

Six rules have held throughout and are worth keeping:

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
6. **Check what the experiment is actually testing.** Two separate mechanisms
   were found this session that made a real-Explorer experiment silently
   measure something else — a deploy that landed one restart late, and registry
   values Explorer could not see. Both are now in `AGENTS.md` under "Two ways an
   experiment can test something other than what you think", because both
   produced results that read as findings about Windows.

## 2. What has landed on this branch

Twenty-seven commits, `940ff6a`..`4f574d1`.

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

## 3. Where to pick up

Ordered by user value, with what is known about each.

### 3.1 A real *file* context menu in a third-party host — the last piece of §3.1

Half of this is done. **Everything 1.5a was restarted onto this build and
driven**, and what it showed is `5085b93` working where it had never run:
Shell's hook sees the host's own frame menus, declines them, and the circuit
breaker stays closed. Before that fix, three such declines — which Everything
produced in ordinary use within seconds — would have switched takeover off for
the rest of the process. It also passes `TPM_RETURNCMD`, like Explorer.

What is left is a menu on an actual *file* inside Everything or Directory Opus,
which is what would exercise the non-`RETURNCMD` replay if any host takes it.
Everything's result list does not respond to a posted `WM_CONTEXTMENU` in
either its keyboard or its point form — it handles right-click in its own
subclass — so driving it means real input into a host, or a person with the
window in front of them. Directory Opus was left running rather than
restarted; it holds an older `shell.dll` and needs a restart to be measurable.

**The tooling for the answer now exists and takes one command.** Right-click a
file in the host, then:

```powershell
.\src\bin\x64\shell.exe -report perf
```

The `decisions` line says whether Shell composed the menu, and `host flags`
says which half of `complete_host_contract` that host exercises.

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

### 3.3 `TakeoverSession` and the WinEvent lifecycle (§01.1, §01.6)

Pure consolidation, no user-visible value, but it unblocks §04.4 steps 5–7 and
it is the last structural item before the seam work. The hook body has grown
several more decisions (gesture, breaker, decision-preservation in the
`__finally`, the host-flags capture) and is the right size to be consolidated
now rather than later.

### 3.4 Reliability Center (§05.1) — its two prerequisites now both exist

The telemetry has existed since `1f17d00`; the export it needed to leave the
process landed in `0b20dde`. `shell.exe -report perf` is already most of the
"Last menus" and "Providers" rows in §05.1's sketch, in text. What is missing
is the window, the provider *names* (the ring carries CLSID hashes, not
names), and the quarantine action.

### 3.5 CoCI router de-dup and `priority` — **landed**, see §01.9b

Measured four ways rather than reasoned about, and the conclusion is stronger
than the plan's: on a machine registered with `-treat`, `priority` cannot
control anything, because COM does not fall back to the original class when a
`TreatAs` substitute fails. The setting is inert, the redirect is in charge,
and `shell.exe -check` now says so. Making `TreatAs` "authoritative when
healthy" — §9's original bullet — would have been the same behaviour with a
different name.

### 3.6 Then

Favorites and the rule inspector (§05.6–7) · seam steps 5–7 of §04.4 · a
*visual* judgement on the flicker wait now that its cost is known (§02.4a).

Two items that were on this list are done: targeted moveto (§04.6a, 85.5 ms →
20.2 ms on a real Explorer) and the cold-start measurement that gated §02.1's
persistence decision (Phase 1.5, declined with numbers).

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

At the time of writing: 32,275 checks / 0 failures on x64 and x86, arm64 builds
and packages, 0 warnings, `check-invariants: OK (10 rules, 0 deferred)`,
23 harness scenarios native and 27 through takeover, 0 failures.

`shell.exe -check` and `shell.exe -report perf` are the two pieces of this
branch a user drives directly:

```powershell
.\src\bin\x64\shell.exe -check:path\to\shell.nss
```

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

**Still unverified there:** a file context menu in a third-party host (§3.1),
and therefore the non-`RETURNCMD` half of `complete_host_contract`, which both
hosts measured so far avoid by setting `TPM_RETURNCMD`.
