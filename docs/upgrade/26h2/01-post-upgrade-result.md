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
  second pass**, or the report is unreadable. `census.ps1 -Diff <a> -Against <b>
  -Sets` now does that pass; it was a throwaway script when this was written.
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

---

## 7. Addendum, later the same day

### The handler churn was churn, and that settles §3

The build carrying the `c_str()` fix was deployed and the census retaken
(`baseline/census-deployed/`). Compared as sets against the **pre-upgrade**
baseline, every static surface is **identical**:

```
same set static-bg-desktop      14 -> 14      same set static-item-folder  43 -> 43
same set static-bg-repo         15 -> 15      same set static-item-readme  30 -> 30
same set static-bg-drive-c      15 -> 15      same set static-multi        26 -> 26
```

`static-item-readme` and `static-multi` are back to 30 and 26 titles — the
numbers they held *before* the upgrade, after reading 45 and 35 immediately
after it. So the +15 and +9 in §3 were third-party handler availability and
nothing else, which is what §3 suspected and could not prove. The fresh-boot
confound named there is now the confirmed explanation.

It does **not** settle the `.nupkg` question, which still has no pre-upgrade
reading of its own. `census.ps1` records that surface from now on.

### The live desktop menu no longer settles, and it is NanaZip

Ten readings after the deploy gave 26, 25, 25, 27, 27, 25, 27, 27, 25, 27 and
never three in a row. The recorder refused to write a canonical file and said so,
which is the behaviour it was given for exactly this case.

Diffing a 27-row reading against a 25-row one, the entire difference is:

```
submenu|NanaZip
sep|
```

One handler and its separator. And in **all ten** readings Shell's own five
config items — `Repomix`, `Terminal`, `File manage`, `Develop`, `Go To` — are
present. So composition is intact in every reading and the oscillation is a
third-party handler registering and deregistering under Explorer, not the string
change and not the upgrade.

Two consequences. The volatile list should grow to include `NanaZip` with its
separator. And this machine is a poor one for a 27-vs-25 judgement: the live
desktop surface has about ±2 rows of third-party noise, so only Shell's own items
and a *named Microsoft verb* are signal there.

### What the string fix looks like in the real product

`6e0fa7d` is deployed as `E3AB56E8F1413032…`; Explorer restarted after the copy
and the `TreatAs` redirect was reapplied. The gates had already passed on this
exact build (34 794 checks, `check-invariants` OK, hostprobe 10 of 10 in both
modes), and the live reading above adds what the gates cannot: Shell composes a
real Explorer menu, with its own items, ten times out of ten, through the changed
`c_str()`.

### PowerShell, resolved

The MSI is back and the split is gone:

| | |
| --- | --- |
| `C:\Program Files\PowerShell\7\pwsh.exe` | 7.6.10 (LTS, end-of-support 2028-11-14) |
| MSIX 7.7.0 | removed |
| Fresh-shell `pwsh` | one candidate, the MSI |
| Machine `PATH` | still `ExpandString`, all 7 `%VAR%` tokens intact |

Two things worth keeping. 7.7.0 ships **no MSI at all** — only `.zip` and
`.msixbundle`, confirmed from the release assets and from Learn: *"Beginning with
the PowerShell 7.7.0 release, there is no MSI package available. WinGet only
installs the MSIX package."* So anything depending on
`C:\Program Files\PowerShell\7\pwsh.exe` — the Claude Code PowerShell tool
included — pins you to the 7.6 LTS line.

And reading the machine `PATH` through
`[Environment]::GetEnvironmentVariable('Path','Machine')` **expands** it. Writing
that back would have baked `%NVM_SYMLINK%` to a literal and quietly broken
`nvm use` for new shells. Read it with
`RegistryValueOptions::DoNotExpandEnvironmentNames` and write with
`-Type ExpandString`.

**Still outstanding:** no winget pin. Without
`winget pin add --id Microsoft.PowerShell --version "7.6.*"`, the next
`winget upgrade --all` reinstalls the MSIX and breaks the path again. Microsoft
Update will not do it — it *"will never upgrade an LTS release to a stable non-LTS
release"* — but winget will.
