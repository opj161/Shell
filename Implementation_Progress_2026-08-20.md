# P0/P1 Implementation Record

Implements `Implementation_Plan_P0_P1_REVISED_2026-08-20.md` against the findings
in `Shell_Codebase_Technical_Audit_2026-08-20.md`, under the AGENTS.md rule: read
the vendor's reference page for every contract, verify it on the machine, pin it
with a test.

Branch `fix/p0-p1-audit-2026-08-20`, based on `5abef86`.

## State

All three architectures build. x64 and x86 suites pass at **24,416 checks, 0
failures**, up from 24,175 — 241 new checks across 8 new test files. ARM64 tests
cannot run on an x64 host and `build.ps1` says so rather than skipping quietly.

| ID | Item | Status | Commit |
| --- | --- | --- | --- |
| F-01 | Registration outside the install transaction | done | `8569e3e` |
| F-02 | Version tool overwrote stable WiX identity | done | `c2e0ad6` |
| F-03 | Malformed application manifest | done | `14bf374` |
| F-04 | Empty registry string writes | done | `b16d04e` |
| F-05 | Path/buffer Win32 size contracts | done | `d9364f3` |
| F-06 | `/EHa` with 50-odd `catch(...)` | done | `d28b6d7` |
| F-07 | `is_in_taskbar` process-global race | done | `1d9d700` |
| F-08 | Unsafe path indexing, `IsCLSID` inverted | done | `1d9d700` |
| F-09 | `IComPtr<T>` ownership | done | `362d7b7` |
| F-10 | `reg.values` truncated the whole enumeration | done | `b2795ab` |
| F-11 | ACL widening on registration | done | `69774dc` |
| F-12 | Package index never refreshed | done | `b2795ab` |
| F-13 | Start Menu component GUID per architecture | done | `c2e0ad6` |
| F-14 | Upgrade policy comment contradicted the table | done | `b2795ab` |
| F-15 | CFG enabled; signing is a pipeline blocker | partial | `d28b6d7` |
| F-16 | WPO / `/OPT:REF` disabled | **not done — see below** | |
| F-17 | UIA worker leaked event handles | done | `b2795ab` |
| F-18 | `IATHook` leaked its module reference | done | `b2795ab` |
| F-19 | Window titles cut at 249 characters | done | `b2795ab` |
| F-20 | Unvalidated alternate MSI build path | done | `76c8f70` |
| §18.1 | Uninstall deleted the shared `.nss` key | done | `d28b6d7` |

## What is not done, and why

**F-16 — whole-program optimisation.** Measured rather than assumed:

| build | shell.dll x64 |
| --- | --- |
| as shipped | 2,235,392 bytes |
| WPO + `/OPT:REF` + LTCG | 1,466,368 bytes |

**−769 KB, −34.4%** — considerably more than the audit's "increasing footprint"
suggests, and it lands in every process that has ever raised a context menu.
It is still off, because it changes code generation in the one area this
codebase documents as sensitive — inline detours and IAT hooks — and the
validation that would justify it is the third-party host matrix (Total
Commander, Directory Opus, Everything) that does not exist here. Note that
`/OPT:ICF` is already on, which is the riskier of the two for detours, so the
current setting is not even internally consistent. This is the highest-value
follow-up in the set and it now has a number attached to it.

**Signing.** Not a source flag and not claimable: there is no certificate on
this machine. The order is build, sign the PEs, package the signed PEs, sign
the MSI last, verify with `signtool verify /pa /all`. Recorded as a release
blocker.

**F-01's preferred form.** The plan prefers migrating declarative registration —
CLSID, InprocServer32, the eight handler keys, Approved, the icon overlay — into
WiX registry components so Windows Installer owns install, repair, uninstall and
rollback. That is a large rewrite whose failure modes are upgrade and repair
behaviour, observable only on clean VMs. What is implemented is the plan's
sanctioned fallback: deferred, non-impersonating, `Return='check'`, with a
rollback action ahead of it — all verifiable from the emitted tables, and all
verified.

**The clean-VM matrix** (§7.10) is not run. Install, upgrade, repair, rollback
fault injection and the multi-session Explorer check need real VMs. Everything
asserted about the installer here comes from the emitted MSI tables.

## Verified in the built artifacts

```
CustomAction   OnInstall=3073  OnInstallRollback=3329  OnUninstall=3073  OnRestartExplorer=65
               3073 = deferred(1024) + no-impersonate(2048) + Binary DLL(1) + return check(0)
               3329 = the same, plus rollback(256)
Sequence       InstallValidate 1400 < InstallInitialize 1500 < OnUninstall 3499 < RemoveFiles 3500
               InstallFiles 4000 < OnInstallRollback 4004 < OnInstall 4006 < InstallFinalize 6600
ProductCode    x86 …BE320   x64 …BE640   arm64 …BEAA0     ProductVersion 1.9.20 everywhere
StartMenu      x86 {2530D345…} attrs=4   x64 {41AFAF39…} attrs=260   arm64 {CFA3E8DD…} attrs=260
PE mitigations Control Flow Guard + Dynamic base on shell.dll, shell.exe and ca.dll, all three arches
               Guard Flags 10017500, CF instrumented, Guard CF function count 93D (shell.dll x64)
```

---

## Evidence log

### F-03 — the manifest was doing nothing at all

Three otherwise identical executables differing only in the manifest compiled
into them, reporting `GetThreadDpiAwarenessContext`:

| embedded manifest | DPI awareness | PerMonitorV2 |
| --- | --- | --- |
| none (control) | `UNAWARE` | no |
| the tree's | `UNAWARE` | no |
| documented | `PER_MONITOR_AWARE` | yes |

The tree's manifest was worth exactly as much as no manifest. What hid it is a
*partial* parse: the Common-Controls 6.0 dependency sits above the undeclared
prefix and is honoured, so comctl32 6.0 loads where an unmanifested process
loads 5.82, and the application looks correctly manifested while every
`windowsSettings` entry — DPI, long paths, code page — is discarded. `mt.exe`
refuses the shipped binaries outright: `general error c101008c: ... Windows was
unable to parse the requested XML data`.

### F-04 — the overflow guard was load-bearing

The empty-string defect is as the audit describes. The overflow check turned out
to matter more: HEAD's `Registry::SetKeyValue` multiplied `length * sizeof(wchar_t)`
unguarded, so `SIZE_MAX` becomes a `DWORD` of 4,294,967,294 and `RegSetKeyValueW`
reads four gigabytes out of a two-character buffer. Probed directly — `0xC0000005`,
and `tests.exe` died the same way before the guard went in.

### F-08 — `IsCLSID` rejected the only thing it was for

The old predicate, lifted verbatim into a probe:

```
canonical CLSID is 40 characters
old_IsCLSID(canonical) = false
old_IsCLSID(L"::{")    = true
```

`if(path.length() >= 40) return false` — and the canonical shell-namespace CLSID
is exactly 40 characters.

### F-11 — a live hole, not a latent one

Registration's ACL fallback took ownership of the Windows CLSID key that decides
which handler owns the Windows 11 context menu, and granted `BUILTIN\Users`
`GENERIC_ALL`, inheritable, permanently. Read off the machine this was developed
on, months after whatever install did it:

```
HKLM\SOFTWARE\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}
  Owner: BUILTIN\Administrators
  BUILTIN\Users  FullControl   (not inherited)
  BUILTIN\Users  GENERIC_ALL   ContainerInherit, ObjectInherit
```

against a comparable Windows-owned CLSID key:

```
  Owner: NT SERVICE\TrustedInstaller
  BUILTIN\Users  ReadKey
```

Any unprivileged account could repoint `InprocServer32` or `TreatAs` at its own
DLL and have every other user's Explorer — administrators included — load it.
The code no longer does this; a machine already in that state stays in it until
the ACL is put back.

### F-01 — the uninstall action never ran

`Execute='firstSequence'` is documented to "always skip action in execute
sequence if UI sequence has run", and "the action is not required to be present
or run in the UI sequence to be skipped". Uninstalling through Programs and
Features runs a UI sequence, so the ordinary uninstall path never unregistered
anything.

### Found while implementing, not in the audit

- The version tool generated build **19** against a tree at **1.9.20**, so
  running it silently downgraded `Resource.h`, the manifest, `Shell.def` and the
  MSI `ProductVersion`. MSI `Version` drives the Upgrade table.
- `shell.exe` returned `Register`'s `bool` straight out of `wWinMain`, so a
  successful registration exited **1** and a failure **0**. Nothing read it,
  which is why nothing noticed — and why the installer could not check it.
- `Uninstall` deleted the whole shared `HKCR\.nss` key, taking any other
  application's registration for that extension with it (plan §18.1).
