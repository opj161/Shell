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

Twelve commits, `940ff6a`..`cc5550d`.

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

## 3. Where to pick up

Ordered by user value, with what is known about each.

### 3.1 Confirm MSAA with a real screen reader — *blocked on a deployed build*

The oldest loose end. §05.3's box records that Shell's items go in with
`MIIM_STRING` and that the mechanism demonstrably works, but **nothing has
confirmed Shell's own composed menu reaching Narrator**. Deploy
(`.\scripts\backup-and-upgrade.ps1`, asks before restarting Explorer) and check.
If it works, close §05.3 as already-satisfied and delete the `MSAAMENUINFO`
design. If it does not, find out *why* before reaching for `MSAAMENUINFO` — the
mechanism is proven, so a failure means something else is interfering.

### 3.2 The takeover half of the harness — *blocked on a deployed build*

Everything in `src/tests/hostprobe/` records **untouched Windows**. Running the
same scenarios through Shell's hook and diffing needs a deployed, injected build.
That is what would verify `b63fdc2`'s replay and `a634ab6`'s INIT/UNINIT pairing
against something other than reasoning. Harness probe 3 (UNINIT tolerance) is
still waiting on it; **probe 4 no longer is** — see §01.7a, the gesture rules
became a pure function and the property is now structural.

### 3.3 `TakeoverSession` and the WinEvent lifecycle (§01.1, §01.6)

Pure consolidation, no user-visible value, but it unblocks §04.4 steps 5–7 and
it is the last structural item before the seam work. The hook body has grown
three more decisions this session (gesture, breaker, decision-preservation in
the `__finally`) and is the right size to be consolidated now rather than later.

### 3.4 Smart columns (§05.5)

Small, self-contained presenter logic over the existing measure pass, and the
machinery exists on both sides already (`cyMax` scrolling landed in `a3431df`;
NSS `column` maps to `MFT_MENUBREAK`). Nothing blocks it.

### 3.5 CoCI policy compile and conditional attach (§01.9)

`Include/ComActivationPolicy.h` already exists — it landed with the Alt-held
blocklist fix (`f2975f9`). What remains is compiling the policy at config
publish time, attaching the detour only when the policy is non-empty, and the
router de-dup of the Win11 suppression.

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

At the time of writing: 26,303 checks / 0 failures on x64 and x86, arm64 builds
and packages, 0 warnings, `check-invariants: OK (10 rules, 0 deferred)`,
23 harness scenarios / 0 failures.

`shell.exe -check` is worth running by hand as well — it is the one piece of
this branch a user drives directly:

```powershell
.\src\bin\x64\shell.exe -check:path\to\shell.nss
```

Note that neither `cmd` nor PowerShell waits for a Windows-subsystem process, so
read the exit code with `Start-Process -Wait -PassThru`. §03.1b records why that
is not being fixed yet.

**Nothing in this branch has run inside a real Explorer.** All of it is
unit-verified or probe-verified. The provider reuse, the INIT/UNINIT pairing, the
`TPM_RETURNCMD` replay, the bypass gesture, the watcher's off-thread parse and
the MSAA question all want a deployed build and, for the replay in particular, a
third-party host — Total Commander or Directory Opus, which are exactly the hosts
these changes are for and exactly the ones this machine cannot test.
