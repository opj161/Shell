# 08 — Handoff

**Written 2026-08-24, branch `refactor/takeover-master-plan`.**
Read this first, then `06-phases-and-tests.md` for the per-item status table.
`00`–`05` are the plan; `07` is the audit that corrected it; this is where the
work actually stands and what to do next.

---

## 1. How this work is being done

The plan sequences by architectural layer. That is **not** the order being
followed, and the departure is deliberate: work is sequenced by **what can be
proved on this machine**, taking small verified defects first and large
speculative pieces last, so every commit ships.

Four rules have held throughout and are worth keeping:

1. **Measure before designing.** Every latency item in this branch was probed
   first, and in four cases the number changed the design — twice it changed
   which problem was worth solving at all. Probes live in the session scratchpad,
   never in the tree; their results live in a comment next to the code that
   depends on them.
2. **Every test is checked to catch its defect.** Re-introduce the bug, rebuild,
   watch that specific test fail, restore. This has found flaws in the *tests*
   as often as it has confirmed them — a test that passed for the wrong reason,
   one that crashed the suite instead of reporting, one that was order-dependent.
3. **Cite the contract, quote the passage.** Both in the code and in the commit
   message. Where documentation is silent, the harness measures it and the
   conclusion is labelled as measurement, not contract.
4. **Three platforms green before every commit.** `.\build.ps1 -Platform all`,
   0 warnings, invariants clean.

## 2. What landed in this session

Seven commits, `940ff6a`..`08653c0` plus the MSAA finding.

| Commit | What |
|---|---|
| `940ff6a` | **Trace harness** `src/tests/hostprobe/` — 23 scenarios, baselines committed |
| `b63fdc2` | **`TPM_RETURNCMD` normalization** — custom commands silently did not run in non-Explorer hosts |
| `1f17d00` | **Diagnostics ring** — always-on phase timing at 47.8 ns/phase |
| `5fe6354` | **Provider budget and reuse** — a warm menu went from ~170 ms to ~41 ms |
| `745dd81` | **GDI leak** — ~16 bitmaps leaked per right-click in explorer.exe |
| `08653c0` | **Mnemonics** — typing a letter in the menu did nothing at all |

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

## 3. Where to pick up

Ordered by user value, with what is known about each.

### 3.1 Confirm MSAA with a real screen reader — *blocked on a deployed build*

The single loose end from this session. §05.3's box records that Shell's items go
in with `MIIM_STRING` and that the mechanism demonstrably works, but **nothing
has confirmed Shell's own composed menu reaching Narrator**. Deploy
(`.\scripts\backup-and-upgrade.ps1`, asks before restarting Explorer) and check.
If it works, close §05.3 as already-satisfied and delete the `MSAAMENUINFO`
design. If it does not, find out *why* before reaching for `MSAAMENUINFO` — the
mechanism is proven, so a failure means something else is interfering.

### 3.2 Taskbar Stage 2 (§02.5)

Replace three per-property UIA calls with a cached request producing plain
rectangles, published atomically; the UI thread then hit-tests rectangles with no
COM and no wait. The 250 ms bounded wait stays as the cold-layout fallback and
the ring records how often it is taken. **Do not** reinstate Stage 1 — §07 A2
explains why, and `AGENTS.md` records the same decision.

### 3.3 `shell.exe -check` (§03.1b)

Parse and report; publish nothing; non-zero exit on error. The plan called this
XS and it is not: `shell.exe` does not link the parser and is a Windows-subsystem
binary with no console. It needs an export from `shell.dll` plus
`AttachConsole(ATTACH_PARENT_PROCESS)`.

### 3.4 Circuit breaker and bypass gesture (§01.7, §05.2)

Both now have somewhere to record themselves — `TakeoverDecision` is already
plumbed through the ring, with `TakeOver` and `FailOpen` populated. `BypassOnce`
and `Degraded` are defined and unused, which is the shape of the remaining work.
Default gesture `Ctrl+Alt+right-click`; `Ctrl+Shift` is taken by config reload
and is evaluated in the same hook body, so harness probe 4 (gesture
non-interference) belongs with this.

### 3.5 The takeover half of the harness

Everything in `src/tests/hostprobe/` records **untouched Windows**. Running the
same scenarios through Shell's hook and diffing needs a deployed, injected build.
That is what would verify `b63fdc2`'s replay and `a634ab6`'s INIT/UNINIT pairing
against something other than reasoning. Harness probes 3 (UNINIT tolerance) and 4
(gesture non-interference) are both waiting on it.

### 3.6 Then

`TakeoverSession` and the WinEvent lifecycle (§01.1, §01.6, pure consolidation,
no user-visible value but it unblocks §04.4 steps 5–7) · flicker-hack A/B
(§02.4) · config watcher (§03.3) · CoCI policy compile and conditional attach
(§01.9) · smart columns (§05.5) · type-ahead (§05.4 Stage 2) · Reliability
Center UI (§05.1, the telemetry it needs now exists) · targeted moveto (§04.6) ·
favorites and the rule inspector (§05.6–7).

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
- **`hostprobe.exe` is built but never run by `build.ps1`.** It creates a window
  and shows real menus. It does not inject desktop-wide input — keys are posted
  to its own thread queue — but it will put popups on screen.
- Re-record fixtures with `--record` and *always* re-run `--verify` twice
  afterwards. Two scenarios were non-reproducible once (`WM_DRAWITEM` is a paint,
  not a contract) and the fix was to stop recording it inline.

## 5. Verifying the tree

```powershell
.\build.ps1 -Platform all
.\src\bin\x64\hostprobe.exe --verify .\src\tests\hostprobe\fixtures
```

At the time of writing: 25,940 checks / 0 failures on x64 and x86, arm64 builds
and packages, 0 warnings, `check-invariants: OK (10 rules, 0 deferred)`,
23 harness scenarios / 0 failures.

**Nothing in this branch has run inside a real Explorer.** All of it is
unit-verified or probe-verified. The provider reuse, the INIT/UNINIT pairing, the
`TPM_RETURNCMD` replay and the MSAA question all want a deployed build and, for
the replay in particular, a third-party host — Total Commander or Directory Opus,
which are exactly the hosts these changes are for and exactly the ones this
machine cannot test.
