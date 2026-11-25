# Brief for a whole-repo over-engineering audit

Written 2026-11-25 for a `ponytail-audit` run starting cold. Everything here is
to stop the audit spending its first hour rediscovering the repository, and to
stop it recommending three or four changes that look obviously correct and are
not. Delete this file once the audit is done; it is a handover, not a reference.

## Read AGENTS.md first, and read it as a list of landmines

It is not boilerplate and it is not style guidance. Nearly every paragraph is a
bug that was paid for once. An audit that skips it will re-propose things that
section already explains, and the repository will not thank it.

## The "do not simplify this" list

These all look like excess. They are not. Each is load-bearing, and at least
three of them will tempt a complexity audit specifically.

| Looks like | Actually |
| --- | --- |
| `QueryContextMenu` inserts **zero items** and returns success | The architecture. Shell composes in a `TrackPopupMenu` hook because `IExplorerCommand` and sparse packages can only *add* items, and Shell's value is removing, moving and regrouping. Not fixable, not simplifiable; it is why Directory Opus cannot be served. |
| POD helpers `menu_perf_begin`/`menu_perf_end`, `ShellExtCapture::has` instead of scope guards | MSVC refuses (**C2712**) to compile a function mixing SEH with objects needing unwinding. See `src/dll/src/Main.cpp:701`, `MenuInspector.h:71`, `ContextMenu.cpp:4644`. "Simplifying" these breaks the build with an error pointing at the `__try`, not at the line that caused it. |
| `string::terminate()` clamping to `m_capacity - 1` | `string.h:1954` says in as many words: *"do not 'simplify' this to m_length - 1"* — it would turn a documented iteration idiom into an endless loop. |
| Redundant-looking zero guards before `release(size - 1)` in `Environment.h` and `Windows.h` | Not redundant. `release(0 - 1)` underflows to `SIZE_MAX` and hands back a 15-character string of NULs where `length()` is 15 and `empty()` is false. Both guards carry a Learn citation. |
| `FolderExtensions` CLSID + registration, served by nothing | Live **servicing** code. No build can register it; uninstall must still delete it for machines registered before upstream `ed826e1`. Deleting it strands those machines with an orphaned CLSID. |
| `BorrowedKeyAccess.h`, ~330 lines to write one registry value | The fix for a machine-wide privilege hole: a previous version took ownership of the Windows 11 menu CLSID key and granted `BUILTIN\Users` full control, permanently. It borrows the minimum access and restores the descriptor. |
| ~60 tests across `provider_health`, `provider_schedule`, `provider_quarantine` | Came from measurement, not speculation: six handlers that never contribute were taking **38% of every menu** (`321f54b`). |
| The custom `System::Text::string` class | **Do not propose replacing it with `std::wstring`.** Enormous diff, no behavioural payoff, and every convention in the tree is built on it. Its *contracts* have caused several bug families — that is a separate, narrower conversation than deleting it. |
| `scripts/backup-and-upgrade.ps1` deploying before restarting Explorer | Deliberate and commented. Reversing it makes every "verified in a real Explorer" result one build stale. |

Two more that are genuinely open rather than settled, and should be left alone
unless the audit wants to own the test:

- **The icon-overlay registration** (`{BAE3934B-…BAF2}`) is registered and
  `IShellIconOverlayIdentifier` is implemented nowhere. It occupies slot 15 of
  the 15 the system allows, and it is plausibly what loads `shell.dll` into
  Explorer early enough for a cold-boot taskbar right-click. See
  `docs/refactor/16-third-party-hosts.md` §5 for the named risk and the named
  test — which nobody has run.
- **`src/exe`'s remaining registration paths.** Not reviewed in this pass.

## Where the real excess probably is

Aim here rather than at the product's hot paths.

- **`docs/upgrade/26h2/menucensus.cpp`** has two dead modes. `live-taskbar` and
  `live-folder` were both measured unusable from an agent shell and superseded by
  `read-open`, which does all three surfaces. ~90 lines, plus the comment block
  in `census.ps1` explaining why they do not work. The *finding* has to move into
  `01-post-upgrade-result.md` §7 before the code goes, or it gets rediscovered.
- **`docs/upgrade/26h2/capture-state.ps1`**, 339 lines. Its four-question framing
  is the value; the mechanism could plausibly be `reg export` plus a hash list
  plus `git diff`. Worth asking whether it needed to exist at that size.
- **`docs/refactor/`** — 20 files, 748 KB of QA narrative, with visible
  overlap: `07-plan-audit`, `10-remediation-qa-assessment` *and*
  `10-remediation-qa-assessment-revised`, `11-qa-deep-pass`,
  `14-post-implementation-qa`, `15-independent-qa-assessment`. Several are
  superseded by later ones. Consolidation is a real deletion opportunity and
  touches no code.
- **Root-level strays**: `PR-Description.md` and
  `2026-09-06-002653-assess-current-state-of-development-of-this-takeo.txt`.
- **`backup_installed_shell/`** holds **68** rotated `shell.dll.old.*` and
  **156 MB**. Gitignored, so it is housekeeping rather than repo weight, but the
  deploy script only prunes *unmapped* copies from the install directory, never
  from here.
- **Three conversion paths doing one job** on `string`: `c_str()`,
  `operator wchar_t *()` and `operator wchar_t const *()`. As of `6e0fa7d` the
  const two agree and the mutable one deliberately differs. Whether all three
  need to exist is a fair question.

## How to work here without wasting runs

- **Report unit suites individually**, not the aggregate check count. 78 suites,
  `34794 checks, 0 failure(s)` is the current baseline.
- **Report hostprobe as _N of M_.** A single green run is not a gate result. It
  had its own `select.*` race until `414a953`; 20 of 20 clean since, which is
  consistent with the fix but does not prove it.
- **Never `git add -A`.** The working tree carries long-lived in-flight edits
  that are not yours.
- **Deploying restarts Explorer.** Ask first. `scripts/backup-and-upgrade.ps1`
  self-elevates.
- **A registry write from an agent shell is not visible to `explorer.exe`.**
  Reads agree in both directions; writes need a scheduled task. AGENTS.md has
  the recipe.
- **Line endings**: `.gitattributes` is `eol=crlf`. Never normalise a file you
  are not otherwise changing, and never `git add --renormalize .`.
- **Write source and patch files with a file tool, not a shell heredoc.**
  Backslashes do not survive the shell layers here — this cost two runs in this
  session alone (`reg add` with a mangled key path, and `cl /Fo"dir\"` eating its
  own closing quote).
- **`pwsh` may not be on a long-running shell's PATH.** PowerShell moved to
  `C:\Program Files\PowerShell\7` on 2026-11-25 and shells older than that
  cannot resolve it; call it by absolute path.

## State, so it is not re-derived

- Windows 11 **26H2, 26300.9974** (upgraded 2026-11-25; nothing Shell depends on
  changed — see `docs/upgrade/26h2/01-post-upgrade-result.md`).
- Deployed `shell.dll` is **`E3AB56E8F1413032…`**, built from the working tree at
  `af15eaa`. `TreatAs` redirect in place, Shell is the primary Windows 11 menu.
- Branch `refactor/takeover-master-plan`, ~25 commits ahead of `origin`,
  **unpushed**. `opj161/Shell` is a **public** fork.
- The live desktop menu on this machine carries about **±2 rows of third-party
  noise** (NanaZip and PowerToys' File Locksmith register and deregister). Only
  Shell's own config items and a *named* Microsoft verb are signal there.
