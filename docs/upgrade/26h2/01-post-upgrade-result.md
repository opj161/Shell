# Windows 11 26H2: what actually changed

Upgraded 2026-11-25, **25H2 26200.9974 → 26H2 26300.9974**. Measured against
[`00-pre-upgrade-baseline.md`](00-pre-upgrade-baseline.md) and the artifacts in
`baseline/`.

## Verdict

**Nothing Shell depends on changed, and nothing in Shell broke.** The enablement
package moved the build number and left the servicing payload alone, exactly as
§1 of the baseline predicted. Every gate is green and every menu Shell composes
is either identical or differs only by third-party handlers coming and going.

The decision not to uninstall Shell first was right, and for the stated reason:
there was no binary swap to protect against.

| | Before | After |
| --- | --- | --- |
| OS | 25H2 26200.9974 | 26H2 26300.9974 |
| `TreatAs` on `{86ca1aa0-…}` | `{BAE3934B-…BAF1}` | unchanged |
| Key owner / `Users` rights | TrustedInstaller / ReadKey | unchanged |
| `ContextMenuHandlers` registrations | 70 across 10 classes | unchanged |
| System binaries Shell depends on | 10.0.26100.9949 | **byte-identical** |
| Installed Shell files | 33 | unchanged |
| x64 build | 0 warnings, 0 errors | same |
| Unit suite | 34 794 checks, 0 failures | **same, and all 78 suites same count** |
| `check-invariants` | OK (15 rules) | same |
| hostprobe native | 10 of 10 | **10 of 10** |
| hostprobe takeover | 10 of 10 | **10 of 10** |
| Desktop menu composition | 141-235 ms to popup | 172-219 ms settled |

## 1. The snapshot diff

```
OS   : 25H2 26200.9974  ->  26H2 26300.9974

--- is Shell still the primary Windows 11 menu? ---
  TreatAs : '{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}'  ->  '{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}'
  owner   : NT SERVICE\TrustedInstaller  ->  NT SERVICE\TrustedInstaller

--- is Shell still registered as a handler? ---
  no change

--- did the files Shell depends on change? ---

--- is the installed Shell itself intact? ---
```

The two empty sections are the result. `explorer.exe`, `shell32.dll`,
`twinui.dll`, `twinui.pcshell.dll`, `win32u.dll`, `windows.storage.dll`,
`uiautomationcore.dll`, `combase.dll`, `comctl32.dll` and
`Windows.UI.FileExplorer.dll` all hash the same before and after, and all still
report `10.0.26100.x` — 26100 being the branch base shared by 24H2, 25H2 and
26H2.

**The redirect survived.** That was risk #1, the only failure mode that would
have put Shell back under "Show more options", and the basis for it was a
measurement rather than a vendor statement: the November cumulative had already
replaced `Windows.UI.FileExplorer.dll` without disturbing the `TreatAs`. An
enablement package did less than that, as argued. Nothing had to be repaired.

## 2. Rollback: the inference was right

`C:\Windows.old` **does not exist** after the upgrade. So the baseline's §4 call
was correct in both halves: the feature-update uninstall path does not apply to
an enablement package, and there is no previous-version image to go back to.
Restore point 42 ("before 26H2") is the only way back, and creating it was not
paranoia.

## 3. The menus: no Microsoft verb appeared

Eight surfaces. `live-taskbar` needed a right-click from the user, as the
baseline said it would.

| Surface | Rows | As sets of titles |
| --- | --- | --- |
| `live-taskbar` | 12 → **12** | **identical**, same order, Shell's `taskbar.nss` items intact |
| `static-bg-desktop` | 19 → 19 | identical set |
| `static-bg-repo` | 20 → 20 | identical set |
| `static-bg-drive-c` | 20 → 20 | identical set |
| `static-item-folder` | 51 → 51 | identical set |
| `live-desktop` | 27 → 25 | lost `Open project in ChatGPT` |
| `static-item-readme` | 40 → 55 | +6 handlers, +9 `Send to` children |
| `static-multi` | 35 → 44 | +6 handlers, +3 `Send to` children |

Every title that appeared is third-party or inbox-but-not-new: `NanaZip`,
`Unlock with File Locksmith`, `Rename with PowerRename`, `Open with Code`,
`Edit in Notepad`, `Move to OneDrive` — the same handler families the baseline
had already recorded as volatile, all contributing after a fresh boot where they
had not been before. The one loss, `Open project in ChatGPT`, is the same effect
in reverse.

The rest of the row growth is the `Send to` submenu, which had no children in the
pre-upgrade reading and nine in this one. It fills on demand, and that is not
deterministic — a nondeterminism two self-test runs minutes apart did not catch.

**No `Ask Copilot`, no `Summarize`, no AI actions.** 26H2's File Explorer
additions have not landed in the menus on this machine. Several of them require a
work or school account and a Copilot licence, which this device does not have, so
this is "not enabled here", not "not shipped".

### The archive formats, and a hole in the baseline

26H2's notes name new archive formats, `nupkg` among them. A `.nupkg` in this
repo does get `E&xtract All...` with verb `extract` — so the behaviour is live.

**It cannot be attributed to the upgrade.** The first baseline censused a
Markdown file, a folder and a multi-selection, and no archive, so there is
nothing to compare against; and Learn is explicit that many 26H2 features
"have already been delivered through monthly servicing updates". A plain `.zip`
produces the identical verb, which says the handler treats `nupkg` as an archive
but dates nothing.

`census.ps1` now censuses `static-item-nupkg` so this is answerable next time.
The general lesson: **census one file of every kind the release notes mention**,
not a representative sample chosen before reading them.

## 4. The one thing that did break, and it is not Shell

`C:\Program Files\PowerShell\7\pwsh.exe` is **gone**. PowerShell is now an MSIX
package, **7.7.0**, at
`C:\Program Files\WindowsApps\Microsoft.PowerShell_7.7.0.0_x64__8wekyb3d8bbwe`,
and the Programs-and-Features entries for the MSI builds (7.6.10.0 installed
2026-11-22, and a 7.5.1.0 bundle) have both vanished. `C:\Program Files\PowerShell`
was modified during the upgrade window and now holds only a stale `Scripts\`
directory from May 2025.

The practical effect: anything invoking that absolute path fails with "The system
cannot find the path specified", which is how this was found — it broke the
agent's own PowerShell tool mid-session. `pwsh` on `PATH` resolves to the MSIX
app-execution alias and works, so the project's scripts are fine; shortcuts,
scheduled tasks or launch configurations pinned to the old path are not.

Whether 26H2 did this or a Store update finalised at the same reboot is not
established. It has nothing to do with Shell either way.

## 5. What the instruments got wrong

Worth more than the result, because the result was "no change" and these are the
reasons a "no change" could have been missed or faked.

- **Position in the row identity makes an insertion look like a rewrite.**
  `static-item-readme` reported 40 → 55 rows with 31 added and 24 removed lines,
  which reads as a menu that was rebuilt. Comparing *sets of titles* instead
  showed 15 additions and nothing removed. The positional diff is still the right
  primary view — order matters in a menu — but **a set comparison must be the
  second pass**, or the report is unreadable.
- **The volatile-row detector inherits that flaw.** It flagged 39 rows as
  volatile across four readings that differed by two items, because the two
  missing items shifted every position below them. It needs to compare on title
  within a surface, not on `pos`.
- **A probe wrapped in a shell pipeline reports the wrapper's exit code.** The
  first taskbar attempt was recorded as "completed, exit 0" when the probe had
  actually timed out with nothing captured; the `echo` after it was what exited
  0. The capture file said `no menu was open within 240 s` all along.
- **`reg add` through the Bash tool silently mangled the key path.** The takeover
  override was never written, and the run tested
  `C:\Program Files\Nilesoft Shell\shell.dll` instead of the build under test —
  which only came to light because `AGENTS.md` says to check the `takeover:` line
  names the build. It did not. The same bits were under test either way, so the
  result stood, but the recipe had not run. Redone from a `.ps1` file, which is
  what `AGENTS.md` already prescribes for anything containing a backslash.
- **Nothing about a popup window identifies its surface**, so `read-open` is
  checked by content: the capture is confirmed as the taskbar because it contains
  `Task Manager` and `Taskbar Settings`.

## 6. Still unverified

- **Jump lists.** 26H2's taskbar feature is "app-specific actions from the
  taskbar", which appear on right-clicking an **app button**, not empty taskbar
  space. That surface was never censused, before or after. `read-open` would
  collect it the same way.
- **Explorer's own folder, file and multi-selection menus.** Still only censused
  through `IContextMenu`, which is the input to Shell's composition rather than
  what Explorer displays. `read-open` closes this too, with a right-click per
  surface.
- **Whether the fresh boot explains the handler differences.** It is the obvious
  confound and it was not controlled for; the families involved were already on
  the volatile list, which is consistent but not proof.
- **The MSI upgrade matrix**, unchanged from the baseline: still not verifiable
  on this machine.
