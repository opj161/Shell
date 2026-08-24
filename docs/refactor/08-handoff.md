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

Five rules have held throughout and are worth keeping:

1. **Measure before designing.** Every latency item in this branch was probed
   first, and in five cases the number changed the design — three times it
   changed which problem was worth solving at all. Probes live in the session
   scratchpad, never in the tree; their results live in a comment next to the
   code that depends on them.
2. **Every test is checked to catch its defect.** Re-introduce the bug, rebuild,
   watch that specific test fail, restore. This has found flaws in the *tests*
   as often as it has confirmed them — a test that passed for the wrong reason,
   one that crashed the suite instead of reporting, one that was order-dependent.
3. **Cite the contract, quote the passage.** Both in the code and in the commit
   message. Where documentation is silent, the harness measures it and the
   conclusion is labelled as measurement, not contract.
4. **Declining is a result, and it gets written down.** Three items in this
   branch were measured and *not* built. Each is recorded with its numbers in
   the plan document that proposed it, so it is not re-proposed by the next
   reader: the icon cache (§04.7), `MSAAMENUINFO` (§05.3), and the taskbar
   rectangle model (§02.5a).
5. **Three platforms green before every commit.** `.\build.ps1 -Platform all`,
   0 warnings, invariants clean.

## 2. What has landed on this branch

Seventeen commits, `940ff6a`..`5085b93`.

| Commit | What |
|---|---|
| `940ff6a` | **Trace harness** `src/tests/hostprobe/` — 23 scenarios, baselines committed |
| `b63fdc2` | **`TPM_RETURNCMD` normalization** — custom commands silently did not run in non-Explorer hosts |
| `1f17d00` | **Diagnostics ring** — always-on phase timing at 47.8 ns/phase |
| `5fe6354` | **Provider budget and reuse** — a warm menu went from ~170 ms to ~41 ms |
| `745dd81` | **GDI leak** — ~16 bitmaps leaked per right-click in explorer.exe |
| `08653c0` | **Mnemonics** — typing a letter in the menu did nothing at all |
| `690b807` | **MSAA finding** — §05.3's premise was wrong; the work was not needed |
| `72d2516` | **Taskbar** — one cached UIA round trip instead of four; the bounded wait is now counted; the rectangle model measured and declined |
| `9c18c79` | **`shell.exe -check`** — parse and report; a missing file no longer reports ok |
| `a63c7a2` | **Bypass gesture and circuit breaker** — one gesture classifier, so reload and bypass cannot both fire |
| `46c7a06` | **Config watcher** — save `shell.nss` and the menu follows, with no key combination |
| `cc5550d` | **Type-ahead** — typing a name selects it; mnemonics keep precedence |
| `5e44534` | **CoCI policy compile** — the COM detour stops walking the rule list for CLSIDs no rule names |
| `6ff63c8` | **Takeover harness + last-error fix** — the hook was handing hosts the wrong `GetLastError` after a failed track |
| `2142525` | **MSAA closed** — confirmed against Shell's own menu in a real Explorer; the `MSAAMENUINFO` design deleted |
| `5085b93` | **Replay harness + breaker fix** — three menus Shell does not handle no longer switch takeover off for the process |

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
| The COM detour is installed only inside `if(rt.loader.explorer)` | §01.9's "attach only if needed" buys less than it appears to — third-party hosts never had it. Conditional attach deferred |
| `DllGetClassObject` is where `BootstrapOnce` is called from | The takeover harness needs **no** injected or deployed build — asking Shell for a class object installs the hook in the asking process |
| 22 of 23 takeover traces byte-identical; the 23rd was Shell's bug | The hook destroyed the host's last-error code. Fixed, and the native fixtures became the takeover gate |
| Shell's composed menu in real Explorer reports 22 named items and 6 separators through `IAccessible` | §05.3 closed as already satisfied; the `MSAAMENUINFO` design deleted rather than deferred |
| Three declined popups open the circuit breaker, and a host that is not Explorer produces them constantly | The breaker was counting decisions as failures. Only real failures count now, and the ring gained a `Declined` decision |
| A shell-namespace menu **is** taken over, and its native identifiers survive the round trip intact | `b63fdc2`'s replay and `a634ab6`'s INIT/UNINIT pairing verified against a real borrowed menu rather than a fake |
| COM activates Shell by the path in the registry, not by whichever copy is already mapped | Two knowingly broken builds passed every takeover assertion through `--shell`. The harness now refuses that configuration |

## 3. Where to pick up

Ordered by user value, with what is known about each.

### 3.1 Drive Directory Opus and Everything - *nothing blocks this*

This document used to end by naming Total Commander and Directory Opus as
"exactly the hosts these changes are for and exactly the ones this machine
cannot test". That was wrong. **Directory Opus 13 and Everything 1.5a are both
installed and running here**, and both hold `shell.dll` - they appear in
`backup-and-upgrade.ps1`'s list of processes pinning the module, along with
fifty-five others. Restarting either picks up a freshly deployed build.

The harness now says what to look for, because the properties it asserts
in-process are the ones that matter in a real host: does Shell substitute its
own menu, does a chosen native item reach the host as its own wID exactly once,
and does every borrowed popup that was initialised get told it is finished with.
Directory Opus is the interesting one - it either passes `TPM_RETURNCMD` or it
does not, and which it is decides which half of `complete_host_contract` has
ever run outside a test.

The breaker fix in `5085b93` matters most here and is unverified there: before
it, three of Opus's own internal popups would have switched Shell off for the
rest of the session.

### 3.2 A way to read the ring from outside the process - *and a live puzzle*

`shell.exe -report perf` is named in §06.4 and does not exist. The ring is
process-local, so in a real Explorer there is currently no way to see any of it.

The documented substitute is the `perf` registry value, which writes breaching
phases to `shell.log` - and **it produces nothing at all from `explorer.exe`**,
while the same DLL in the same session logs freely from another host. Measured,
not guessed (2026-08-24, Windows 11 26200.8875 x64, deployed build, `perf` = 1):

| Host | Menu appeared | Phase lines written |
|---|---|---|
| `hostprobe.exe --takeover` | yes | yes - `explorer.commands 42.95ms items=30`, `popup.total_pre_display 45.05ms` |
| `explorer.exe`, started after the deploy, canonical `shell.dll` | yes - 28 items, read back through MSAA | **none** |

Repro: append a marker to the installed `shell.log`, post `WM_CONTEXTMENU` to
Explorer's `SHELLDLL_DefView`, confirm the menu appeared with an `IAccessible`
read, and look after the marker. Nothing follows it.

Ruled out: the registry read (a probe linking the shipping `Registry.cpp`
answers `true, value = 1`), the log path (`Path::Module` resolves to the
canonical `shell.log` for a process that started after the deploy), and
permissions on the log file itself (a medium-integrity process opens it for
append successfully - though **not** the directory, so a log path that does not
already exist cannot be created there, which is worth remembering for any
process still holding a rotated `shell.dll.old.*`).

Do not sink more time into the log sink. Build the export instead: it is on the
plan, the Reliability Center (§05.1) needs it anyway, and it is the only
channel that will work inside a host nobody can attach a debugger to.

### 3.3 `TakeoverSession` and the WinEvent lifecycle (§01.1, §01.6)

Pure consolidation, no user-visible value, but it unblocks §04.4 steps 5–7 and
it is the last structural item before the seam work. The hook body has grown
three more decisions this session (gesture, breaker, decision-preservation in
the `__finally`) and is the right size to be consolidated now rather than later.

### 3.4 Smart columns (§05.5)

Small, self-contained presenter logic over the existing measure pass, and the
machinery exists on both sides already (`cyMax` scrolling landed in `a3431df`;
NSS `column` maps to `MFT_MENUBREAK`). Nothing blocks it.

### 3.5 CoCI: the router de-dup, and conditional attach if it earns itself (§01.9)

The policy compile and the hook's fast path landed in `5e44534`. Two things are
left, and one of them shrank on inspection — see §01.9a:

- **Conditional attach is deferred, not pending.** The detour is already
  installed only inside `if(rt.loader.explorer)`, so third-party hosts never get
  it, and the policy does not exist yet at the point `BootstrapOnce` would
  decide. Doing it means installing the detour later from a config-publish
  thread, which now includes the watcher's. Revisit alongside `TakeoverRouter`,
  where the decision has somewhere natural to live.
- **The router de-dup of the Win11 suppression is still open**: TreatAs
  authoritative when healthy, CoCI override only as a fallback, never both by
  default.

### 3.6 Then

Flicker-hack A/B (§02.4 — note the *visible* half needs eyes on a real menu) ·
Reliability Center UI (§05.1; its telemetry now exists, including the taskbar
counters) · targeted moveto (§04.6) · favorites and the rule inspector
(§05.6–7) · seam steps 5–7 of §04.4.

## 4. Things that will bite

- **`ProviderHealth` judges on a provider's *best* time and never before its
  second sample.** Both rules exist because the first menu in a process is cold
  and makes every provider look pathological; a one-sample rule would defer two
  more providers per menu until the menu had no packaged verbs left, permanently.
  `test_provider_health.cpp` simulates it. Do not "simplify" either rule.
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
  design and serialised by `_reload_mutex`, but it is the one place in the tree
  where a parse runs off a menu thread, and it has not been exercised inside a
  real `explorer.exe`.
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
- Two traps that cost real time this session are now in `AGENTS.md`: a variadic
  template call inside an SEH function (C2712, reported at the `__try`), and
  heredoc'd patch scripts eating a backslash level in **both** directions.

## 5. Verifying the tree

```powershell
.\build.ps1 -Platform all
.\src\bin\x64\hostprobe.exe --verify .\src\tests\hostprobe\fixtures
```

At the time of writing: 26,327 checks / 0 failures on x64 and x86, arm64 builds
and packages, 0 warnings, `check-invariants: OK (10 rules, 0 deferred)`,
23 harness scenarios native and 27 through takeover, 0 failures:

```powershell
.\src\bin\x64\hostprobe.exe --takeover --verify .\src\tests\hostprobe\fixtures
```

`shell.exe -check` is worth running by hand as well — it is the one piece of
this branch a user drives directly:

```powershell
.\src\bin\x64\shell.exe -check:path\to\shell.nss
```

Note that neither `cmd` nor PowerShell waits for a Windows-subsystem process, so
read the exit code with `Start-Process -Wait -PassThru`. §03.1b records why that
is not being fixed yet.

**Some of this branch has now run inside a real Explorer.** Deployed 2026-08-24
and driven by posting `WM_CONTEXTMENU` to `SHELLDLL_DefView`, which raises a real
Shell menu without touching the mouse. What that confirmed: the composed menu
appears, and every item in it is readable through MSAA (§05.3).

What is still unverified there: the provider reuse and the watcher's off-thread
parse have no observation channel from outside the process. The `perf` registry
value is supposed to be one, and **it produced no output from Explorer even
though the value reads back correctly through the same code the DLL runs**
(checked with a probe linking `Registry.cpp`). Unresolved; the honest reading is
that the in-memory ring needs an export — `shell.exe -report perf` is named in
§06.4 and does not exist — rather than that the log sink should be made to work.

The `TPM_RETURNCMD` replay is unverified in a host that did not come from this
tree, but the reason is no longer the machine: see §3.2.
