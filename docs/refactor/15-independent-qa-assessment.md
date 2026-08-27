# 15 — Independent QA assessment: is the refactor done, and does it work?

**Assembled 2026-08-27** against `refactor/takeover-master-plan`, from
`c3d6079` forward. Every number below was re-measured on this tree and this
machine (Windows 11 Pro for Workstations 26200, x64, MSVC 14.44.35207) rather
than quoted from a session.

The question put to this pass was three-part: is the `docs/refactor` programme
complete and correct; were the later optimisations, improvements and fixes done
correctly; and then a full QA pass with official documentation as the standard.

---

## 0. Verdict

**The refactor is complete. The five accepted audit candidates are complete. The
tree, the gates and the installed product are green.** The eleven closure
workstreams closed in [`14`](14-post-implementation-qa.md); the twenty backlog
items in [`00 §3a/§3b`](00-master-plan.md) reached a recorded decision each;
`C-ENC`, `D-01`, `A-01`, `E-02` and `F-01` all landed with their contracts
discharged or their departures stated.

**This pass found four defects that survived all of it, three of them
user-facing, and all four are fixed and verified here.** They are not
regressions from the refactor: three predate it and one was introduced by the
security hardening that removed a worse problem. What they have in common is
that no gate in this repository could see any of them — two live on paths that
only a real machine exercises, one needed a malformed config nobody had written,
and one is not code at all.

| | Finding | Severity | State |
|---|---|---|---|
| **F-1** | `foreach`'s list argument is counted but never typed; a `.nss` with `foreach(@a, 5, 1)` takes `explorer.exe`'s menu down with an access violation | critical | fixed, `397b445` |
| **F-2** | `BorrowedKeyAccess` asked for the rights it was about to grant itself, so the Windows 11 primary-menu redirect had never once been applied on a stock machine | critical | fixed, `21fcbfc` |
| **F-3** | the installer's custom action had no borrow at all: MSI install silently skipped the redirect, and MSI **uninstall failed outright** once a redirect existed | critical | fixed, `1142e66` |
| **F-4** | five of the seven documentation deep links `D-01` broke were still pointing past end-of-file | low | fixed and gated, `cf9b92e` |

Three items the audit ranked "strong and cheap" were **measured and declined**
here, with the numbers in §7, because measurement said they do not matter.

---

## 1. Method

Read first, then re-derive, then measure, then verify on the machine.

- The three session transcripts were read end to end, including the
  11-subagent audit export, and the consolidated audit report was recovered
  from it (§2.3 — it existed nowhere else).
- Every claim in the six commits under review was checked against the tree
  rather than accepted: the diffs, the deleted symbols, the surviving callers,
  the test-count arithmetic.
- Contracts were re-read from the vendor before any conclusion rested on them.
  Deep links are inline below; `microsoft_docs_fetch` on the canonical page,
  not a search snippet.
- New defects were **demonstrated before being fixed** — each of the two
  crashing inputs on its own run, with the other disabled, so one access
  violation could not stand in for two.
- Everything ends on the machine: three-platform build, both hostprobe modes,
  a deploy, a live Explorer, and the registry read back.

### 1.1 What this pass could not do

The desktop screenshot check was requested and declined by the user, so **no
human or camera has looked at the composed menu on this build.** What stands in
its place is stronger than usual but is not the same thing: hostprobe's takeover
mode drives Shell's real COM handler through 33 scenarios against the same
binary, the deployed DLL is byte-identical to the built one, and `shell.exe
-check` parses the installed configuration clean. §8 keeps this open.

---

## 2. Is the refactor complete?

### 2.1 The programme

| Layer | Where it was closed | Re-checked here |
|---|---|---|
| 20 backlog items (`00 §3`) | 15 implemented, 5 resolved differently, 0 partial (`00 §3b`) | tally reconciles against `00 §3a`; the two reclassifications (items 9 and 11) are named with their arguments, so a reader can disagree with the argument rather than the arithmetic |
| W0–W10 closure workstreams | complete and verified (`14 §0`) | commits present, gates re-run below |
| W11 / R8 | never closable here (`14 §7`) | still open; §8 |
| 5 accepted audit candidates | `C-ENC`, `D-01`, `A-01`, `E-02`, `F-01` | all landed; scope and departures verified below |

### 2.2 The five audit candidates, checked rather than accepted

- **`C-ENC` (`20cd71f`)** — the signed-`short` UTF-8 loop, two leaking
  null-unsafe decode paths, and a null-`src` dereference the audit had not
  named. Re-read: `Utf16ToUtf8` now uses the two-call `WideCharToMultiByte`
  pattern with `dwFlags = 0`, which is the only legal choice besides
  `WC_ERR_INVALID_CHARS` for `CP_UTF8` and is the one that "replaces illegal
  sequences with U+FFFD … and succeeds"
  ([WideCharToMultiByte](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-widechartomultibyte)),
  so one unpaired surrogate cannot abandon a whole `sel.tofile()` write. The
  empty case is answered before the call because `cchWideChar == 0` fails. The
  embedded-NUL truncation is preserved deliberately and says so. **No sibling
  hand-rolled encoder survives** — `rg` over `Encoding.h` for the 3- and 4-byte
  lead-byte constants returns only the comment describing the one that was
  removed.
- **`D-01` (`27d5555`)** — `PackageIndex` and `IPackageSource` gone, 737 lines
  removed, and the only surviving mention is a comment explaining why
  `resolve_path` went with it. The check-count arithmetic reconciles at every
  step (33,781 → 33,723, −58 for 15 deleted tests).
- **`A-01` (`df04eb5`)** — the dead second interception mechanism gone. The
  surviving `hook_all`/`unhook` pair still enumerates both `Shell_TrayWnd` and
  `Shell_SecondaryTrayWnd`, so secondary taskbars are handled by the mechanism
  that remains; the deleted path was never called and could not have changed
  that. Verified live: `Shell_TrayWnd`'s `UxSubclass` prop reads
  `0x7FF6F14EE4E0`, a code address, not the `0x1E34270F` sentinel the dead path
  would have installed.
- **`E-02` (`c3d6079`)** — `FOLDEREXTENSIONS` retired from `REGOP`, the
  commented-out `HKLM` delete restored. The delete target is
  `SOFTWARE\Classes\Drive\shellex\FolderExtensions\{BAE3934B-…}` — Shell's own
  CLSID subkey, **not** the shared `FolderExtensions` key, so it cannot take
  another vendor's registration with it. `-f`/`-force` has no reference left
  anywhere in the tree, the installer, the templates or the docs.
- **`F-01` (`d5de201`)** — VC-LTL now applies or the build fails. Verified by
  reading the PE import directory of the artefacts this pass built:

  | | x64 | x86 | arm64 |
  |---|---|---|---|
  | size | 2,049,024 | 1,787,904 | 1,920,512 |
  | CRT import | `msvcrt.dll` | `msvcrt.dll` | `ucrtbase.dll` |

  Nine `VC-LTL Path` banners per full build — three projects × three platforms,
  exactly the pre-existing application set. `tests.exe` and `hostprobe.exe`
  remain static-CRT, so the scope did not widen.

  The verifier's residual objection is worth restating rather than burying:
  VC-LTL's own instructions say "Make sure that all dependent static libraries
  are also recompiled with VC-LTL"
  (`src/shared/VC-LTL.props:14`), and `plutosvg-x64.lib` is a vendored binary
  that requests `/DEFAULTLIB:LIBCMT`. The configuration is nonetheless sound in
  practice and now measurably so: VC-LTL substitutes the CRT import libraries,
  the link emits **zero** warnings (no `LNK4098` defaultlib conflict) and the
  finished image imports exactly one CRT. It is also unchanged from what CI has
  always shipped. Recorded as a residual, not a defect.

### 2.3 One thing the refactor did not produce

**The ponytail audit exists only in a downloaded session export.** 36
candidates, six lanes, four adversarial verifier runs, and the reasoning behind
three rejections — including the `B-01` rejection that is load-bearing for §5.1
below — live in `session-export-1787781024344`, not in this repository. The
scratchpad copies of the candidate contracts are already gone. Anything that
matters from it should be committed; this document quotes the parts §5 depends
on so at least those survive.

---

## 3. Re-measured gate state

Every row run by this pass, on the final tree.

| Gate | Result |
|---|---|
| x64 build + suite | **33,729 checks / 0 failures**, 0 warnings |
| x86 build + suite | **33,730 / 0**, 0 warnings |
| arm64 | builds and packages; suite skipped (x64 host) |
| `check-invariants` | **OK (12 rules, 0 deferred)** |
| `/analyze` `Shell.vcxproj` | **0 warnings**, 23 translation units |
| `/analyze` `exe.vcxproj`, `ca.vcxproj` | 0 warnings |
| hostprobe native | **23 scenarios, 0 failures**, 10 skipped |
| hostprobe takeover | **33 scenarios, 0 failures**, `takeover:` names the build under test |
| `validate-msi-lifecycle.ps1` | ok × 3 packages |
| `git diff --check main...HEAD` | clean |
| CI | green on every pushed commit |
| deployed = built | `shell.dll` and `shell.exe` hash-identical to `src\bin\x64` |
| live Explorer | pid 14812, started after the copy, `shell.dll` mapped |
| installed config | `shell.exe -check` → ok, 9 files, 150 entries |

The x64/x86 counts differ by one **for a reason worth knowing**: `test_regop`'s
legacy-FolderExtensions test runs one extra assertion when detection does *not*
already answer for the key, and on this machine detection answers on x64 and not
on x86. That difference is the WOW64 divergence in §7.3 showing through the
suite.

---

## 4. F-1 — a `foreach` argument is counted but never asked what it is

**Fixed in `397b445`.**

`8de40c7` guarded three arms that indexed `args` past its end. It did not ask
what the arguments *were*, and the very next line is an unchecked downcast:

```cpp
error_if(args.size() < 2, TokenError::IdentifierArgumentsUnexpected, ident_col);
error_if(!args[0]->IsVariable(), TokenError::IdentifierArgumentsUnexpected, ident_col);

auto id = &args[1]->ident()->Id;      // args[1] is never typed
```

`Expression::ident()` is a `reinterpret_cast`, and "the result of a
`reinterpret_cast` cannot safely be used for anything other than being cast back
to its original type"
([reinterpret_cast Operator](https://learn.microsoft.com/en-us/cpp/cpp/reinterpret-cast-operator)).
`IdentExpression`'s first member is an 800-byte `Ident` — two 100-element
`uint32_t` arrays and a `_size`. A `NumberExpression` is far smaller, so
`Ident::_size` is read from past the end of the allocation, and `Ident::at`
*bounds-checks the index against that garbage* before indexing `_items_id` with
it. `at()` looks exactly like the guard and is not one.

Demonstrated on the pre-guard tree, x64, each on its own run with the other
disabled — both exit the suite with `0xC0000005`:

```
item(title='y' where=foreach(@a, 5, 1))     bare form, argc 3
item(title='y' image=icon.foreach(@a, 5))   wildcard-namespace form
```

Neither spelling is exotic, and they arrive by different routes. The bare one
passes `verify_ident`'s `check(argc == 3)`, which counts and does not inspect.
The dotted one never reaches that case: dispatch is on the **first** segment,
and the `icon`/`image`/`img`/`svg` arms classify any second segment as an
`Identifier` — the same fact the audit used to *reject* candidate B-01, because
it is what keeps `icon.for` live.

This is the same family as the three defects `8de40c7` fixed: an access
violation inside the hook's SEH region, which `catch(...)` under `/EHsc` does
not catch, so the symptom is a menu that never appears with nothing in any log.

The fix names the precondition `ident()` always had.
`Expression::IsIdentExpression()` is true for the three classes that actually
derive from `IdentExpression` — `VariableExpression`, `AssignExpression`,
`FuncExpression`. `IsIdent()` alone would have been wrong: it covers only the
third and would have rejected `sel.paths`, which is what the language accepts
here. `a_well_formed_foreach_still_parses` holds that line so the guard cannot
quietly become a break. `eval_for` and `eval_foreach` get the same check rather
than trusting the parser, because the parser's checks live on the `if(!_hasdot)`
branch only — a trailing dot reaches the evaluator with nothing verified.

---

## 5. F-2 — the borrow asked for the rights it was about to grant itself

**Fixed in `21fcbfc`.**

Found by looking at a warning in the user's own deploy log, not by reading code:

```
5. Registering...
[OK] Registered (-register -treat)
[WARN] TreatAs redirect missing or unexpected ('')
```

`BorrowedKeyAccess` exists because `{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}` —
the CLSID whose `TreatAs` redirect makes Shell the primary Windows 11 menu
rather than an entry under "Show more options" — is owned by
`NT SERVICE\TrustedInstaller`, with Administrators holding `ReadKey`. It takes
ownership, grants Administrators exactly `KEY_CREATE_SUB_KEY`, writes, and puts
owner and DACL back. `69774dc` introduced it to replace a fallback that granted
`BUILTIN\Users` `GENERIC_ALL`, inheritable, permanently.

It had never worked. `acquire()` opened its work handle with

```cpp
READ_CONTROL | WRITE_DAC | temporary_rights
```

*immediately after taking ownership and before widening the DACL.* Ownership
buys one thing, and it is not that: "An object's owner implicitly has
`WRITE_DAC` access to the object"
([Owner of a New Object](https://learn.microsoft.com/windows/win32/secauthz/owner-of-a-new-object))
— `WRITE_DAC` and `READ_CONTROL`, nothing object-specific. At that instant the
DACL still read Administrators: `ReadKey`, and `KEY_CREATE_SUB_KEY` (0x0004) is
not in `KEY_READ` (0x20019). The open was refused, `acquire` returned false,
`restore()` handed ownership straight back, and the caller logged one generic
warning.

**Why nobody saw it for a week.** `69774dc` landed on a machine whose CLSID key
still carried the old `Users`-`FullControl` hole, so the *first*
`CreateTreatAsIfAbsent` succeeded and the borrow path was never entered. Once
the ACL was back to stock the borrow became load-bearing, and 2026-08-27 13:09
is the first recorded run of it — a warning, and no redirect.

The work handle now asks for `READ_CONTROL | WRITE_DAC` only. `temporary_rights`
keeps its real job, the ACE, and each caller opens its own handle afterwards,
which both already did.

Verified on the machine. The deploy that produced this is the first to print
`[OK] Windows 11 context-menu redirect in place`:

```
before   TreatAs absent
after    TreatAs = {BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}
         parent SDDL byte-identical to before, owner NT SERVICE\TrustedInstaller
         no warning in shell.log
```

The SDDL comparison is the point: the borrow was returned *exactly*, not
approximately, and `BUILTIN\Users` gained nothing.

**Diagnostics were the other half of the defect.** One sentence stood for eight
distinct failures with no step and no error code in it.
`BorrowedKeyAccess::fail` now names the step and the Win32 error, and
`report_treatas` names the `TreatAs` state at each way `disable_modern` can give
up. Unelevated, the mechanism now says
`could not enable SeTakeOwnership/SeRestore: error 1300` instead of nothing.

---

## 6. F-3 — the installer had no borrow at all

**Fixed in `1142e66`. This is the most user-facing finding in the pass.**

Fixing F-2 raised an obvious question: the MSI does its own `TreatAs` work
through `PrepareTreatAs`/`TreatAsApply`/`TreatAsRollback`/`TreatAsCommit`
custom actions. Do they have the same defect? They had something worse — no
borrow at all. `TreatAsApply` called `RegOpenKeyEx` with `KEY_CREATE_SUB_KEY`
directly, and deleted through `RegDeleteKeyEx` directly.

Those actions are deferred and no-impersonate (type 11265 / 11521 / 11841, read
out of the built package's `CustomAction` table), so they run as **LocalSystem**
— and LocalSystem holds `ReadKey` on that key and nothing else. Measured from a
SYSTEM scheduled task opening the real key:

```
parent  [ReadKey     ] -> OK
parent  [CreateSubKey] -> Requested registry access is not allowed
TreatAs [Delete      ] -> Requested registry access is not allowed
SeTakeOwnershipPrivilege / SeRestorePrivilege: present, disabled
```

So on any machine whose CLSID key has its stock ACL:

- **install** hit `ERROR_ACCESS_DENIED`, raised `INSTALLMESSAGE_WARNING` and
  returned `ERROR_SUCCESS`. The package installed, reported success, and left
  Shell under "Show more options" — the exact outcome `TreatAs` exists to
  prevent, with a warning as the only trace.
- **uninstall was worse.** Once a redirect exists — which it now routinely does,
  because F-2 made `shell.exe -register -treat` work — `PrepareTreatAs` plans
  `uninstall_ours`, `RemoveTreatAsIfOurs` fails the same way, and `TreatAsApply`
  returns `ERROR_INSTALL_FAILURE`. **The uninstall fails.**

`BorrowedKeyAccess` moved to `src/shared/BorrowedKeyAccess.h` so there is one
implementation rather than one plus a hole, and the borrow went **inside**
`CreateTreatAsIfAbsent` and `RemoveTreatAsIfOurs` rather than at the four call
sites, so a later caller cannot forget it. Both still try the plain write first:
on a machine some earlier install already widened, that succeeds and the
security descriptor is never touched. The delete borrows `DELETE` on the
`TreatAs` key itself, because that is the object `RegDeleteKeyEx` checks the
right on, and calls `release_deleted()` on success since there is no descriptor
left to restore.

A create that succeeds but cannot restore the descriptor is reported as a
**failure**, not a success: leaving that key widened is the machine-wide hole
`69774dc` removed, and it is not worth a redirect.

Verified by running the shared header's own code against the real key, in both
contexts that matter, with a scratch subkey standing in for `TreatAs` so nothing
live was touched:

```
as NT AUTHORITY\SYSTEM (scheduled task — the custom action's context)
  [1] open parent KEY_CREATE_SUB_KEY without borrow -> 5 ACCESS_DENIED
  [2] acquire(KEY_CREATE_SUB_KEY) -> OK, create -> 0, restore() -> OK
      parent SDDL after create+restore identical: YES
  [3] delete without borrow -> 5 ACCESS_DENIED
      acquire(DELETE on the child) -> OK, delete -> 0
  scratch key left behind: no
  parent SDDL at exit identical to start: YES

as an elevated administrator (the exe's context) — identical results.
unelevated — acquire fails at the new per-step diagnostic, error 1300.
```

The key's SDDL on this machine is byte-identical to the snapshot taken before
any of this session's work.

**A portability trap the probe found, worth keeping.**
`SE_TAKE_OWNERSHIP_NAME` and `SE_RESTORE_NAME` are `TEXT()` macros, so they are
only wide in a translation unit that defines `UNICODE`. In a shared header that
is a build break waiting for the first consumer that does not. They are spelled
wide explicitly, with the values from
[Privilege Constants](https://learn.microsoft.com/windows/win32/secauthz/privilege-constants).

---

## 7. Assessed and *not* changed — with the numbers

AGENTS.md: "Implementing something because a document listed it, when
measurement says it does not matter, is as much a mistake as skipping something
that does." Three of the audit's "strong and cheap" items measured out small.

### 7.1 C-01 — `Windows::Version`'s constructor, measured

The audit flagged that the singleton loads `winbrand.dll` and calls
`BrandingFormatString` in every host process. True, and it is on the first-menu
path (`ContextMenu.cpp:1873` is the first touch). Measured in a fresh process,
three runs, warm file cache:

| | ms |
|---|---|
| `LoadLibrary(winbrand.dll)` | 1.21 – 1.79 |
| `BrandingFormatString(%WINDOWS_LONG%)` | 2.04 – 3.01 |
| `CurrentVersion` open + 5 value reads | **0.04 – 0.06** |

So **~3.3–4.8 ms once per process**, before the first menu paints, for a display
string (`Version::Name`) with exactly two readers — an NSS function and the log
file header. Making `Name` lazy is a real improvement and a genuinely small one:
one-time, an order of magnitude below the 28 ms first-UIA-query this branch
treated as the case worth bounding, and it touches a header shared by three
projects. **Recommended, not done.** The number is here so the next reader can
disagree with the number rather than the judgement.

### 7.2 B-02 — an `unordered_map` per AST node, measured

Every `Expression` owns a `Scope`, which owns an
`std::unordered_map<uint32_t, Expression*>`; most nodes never use it, and MSVC's
map allocates on default construction. Measured, 100,000 nodes, three passes:

**0.27 µs per node** construct + destruct, `sizeof` 64 bytes. For a
configuration of a few thousand nodes that is **single-digit milliseconds once at
load**, off the menu path entirely, and 64 bytes of resident memory per node.
**Declined on the measurement.**

### 7.3 The WOW64 registry-view divergence — real, unfixed, and narrower than reported

`Registry::ClassesRoot` / `CurrentUser` / `LocalMachine` are constructed with
`KEY_WOW64_64KEY` (`Registry.cpp:660-662`), while `Registry::Exists` takes a
`view` defaulting to 0 and `Registry::DeleteSubKey` passes no flag at all. From
a 64-bit process these coincide. From a 32-bit process they are different hives
([Accessing an Alternate Registry View](https://learn.microsoft.com/en-us/windows/win32/winprog64/accessing-an-alternate-registry-view)).
The reproduction is the `test_regop` seed that passed as x64 and failed as x86.

The previous session recorded this as "the x86 DLL ships and loads into 32-bit
hosts". **That overstates it, and the correction matters for triage.** Each MSI
is single-architecture: `setup-x64.msi` installs only the x64 binaries and
registers only in the 64-bit view, so a 32-bit host on 64-bit Windows does not
find Shell at all and never loads the x86 DLL. The divergence is reachable in
exactly one shipped configuration — installing `setup-x86.msi` on 64-bit
Windows, which no launch condition prevents — where `shell.exe` would register
into the 64-bit view while `IsRegistered()` reads the 32-bit one, so detection
and uninstall would not see their own registration.

Left unfixed deliberately: it is module-wide, and a partial fix distributes the
question rather than settling it. It wants its own candidate, and now has a
measured reproduction and a bounded blast radius to scope it with.

### 7.4 Smaller open items

- **`ValidatePath` is exported from `ca.dll` and has no `CustomAction` row**, so
  it cannot fire. Confirmed against the built package's `CustomAction` table.
  Dead weight, harmless (audit E-04).
- **`DllRegisterServer` does not occur in the built `shell.dll`**, so the
  self-registration source is dead before it reaches the artefact (audit E-03).
- **`tests.exe` links the static CRT while `shell.dll` ships VC-LTL**, so the
  unit suite exercises a different CRT than the product. Widening VC-LTL to the
  test projects is a deliberate decision nobody has taken.
- **`ForStatement::Eval`'s guard has no test that reaches it.** `Parsed` only
  calls `parser.Load()`, which never evaluates an item title. Honestly recorded
  in the test file rather than covered by a green test that proves nothing.
- **`resolve_display_name` returns `ms-resource:AppStoreName` unresolved** for
  Windows Terminal — the answer is wrong rather than absent. No shipped
  configuration reaches it.
- **Twenty branch-touched files sit `w/lf` against an `eol=crlf` attribute** in
  the working tree, including files this branch never edited
  (`src/dll/src/Parser/IdentHash.h`, `scripts/backup-and-upgrade.ps1`). The
  index blobs are correct and `git diff --check` is clean; it is a checkout
  wart, and AGENTS.md says not to normalise files you are not otherwise
  changing.

---

## 8. Still needs a machine or a person

Unchanged from [`14 §7`](14-post-implementation-qa.md), plus two this pass adds:

- **Third-party host smoke tests** — Total Commander, Directory Opus, Everything:
  the `IShellExtInit`/`IContextMenu` path. Nothing here exercises it.
- **The MSI upgrade matrix** on clean VMs. §6 verifies that the operation the
  custom action performs now succeeds *in the account the custom action runs
  as*, which is the part that was failing; it does not verify an install.
- **A human eye on the composed menu** for this build. Declined during this
  pass; hostprobe takeover and the `-check` parse stand in for it. §1.1.
- **A secondary taskbar.** This machine has none, and it is the configuration
  the deleted `unhook_all` restore bug would have hit had that code ever been
  live.
- **Windows 10.** Nothing here has been shaped by a Win10 run.

---

## 9. Traps this pass hit, so the next one does not

Four, and every one of them produced a *green* result that meant nothing.

**`Copy-Item` preserves the source's timestamp, so MSBuild skipped the
recompile.** Restoring a probed test file from a backup and rebuilding produced
a binary that still contained the probe — and therefore still crashed, which
read exactly like the fix having failed. The tell was the suite header for a
test that no longer existed in the source. Touch the file, or check the build
actually said it compiled it.

**A build that fails still leaves the previous binary runnable.** The second
attempt at isolating one of the two crashing inputs failed on `C4505`
(warnings are errors in the test project), so the run that followed used the
stale executable and reproduced the *first* crash. A red result proving the
wrong thing is as bad as a green one proving nothing.

**`powershell -File .\build.ps1` through the Bash tool lost the backslash** and
reported `EXIT=127` while the harness recorded exit code 0 for the wrapper.
AGENTS.md warns about backslashes in shell heredocs; the same layer eats them in
plain arguments.

**A file written with a file-writing tool arrives as LF** under an `eol=crlf`
attribute, and `git diff` hides it because the clean filter normalises on stage.
`git ls-files --eol` is the only thing that shows it. This is the fourth time
this repository has recorded that trap; the new file in `1142e66` hit it too.

---

## 10. Bottom line

The refactor is finished and it works. What this pass adds is that three things
it could never have caught were nonetheless broken — the config parser on a
malformed but unremarkable input, the registration path for the feature the
whole Windows 11 story depends on, and the installer's version of that same
path, which additionally failed uninstalls. All three are fixed, all three are
verified on this machine in the account that actually runs them, and the twelfth
invariant rule now stops the fourth from recurring silently.

The pattern is the one this repository keeps writing down: **the gates were all
green, and the gates could not see any of it.** Two of the three needed a real
machine in a specific account; the third needed someone to type a malformed
`foreach`. The thing that found them was reading a warning in a deploy log and
asking what it meant.
