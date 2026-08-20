# Nilesoft Shell 1.9.20 — Full Codebase Technical Audit and Agent-Audit Validation

**Audit date:** 2026-08-20  
**Repository reviewed:** attached `Shell(7).zip` (`Shell`, 359 Git-tracked files)  
**Prior audit reviewed:** attached `Pasted markdown(20260820-124840).md`  
**Primary focus:** correctness, stability, performance, Win32/COM contracts, installer/MSI/WiX behavior, concurrency, long-path behavior, registry/string handling, build/release hygiene, and functionality regressions.

---

## 1. Executive summary

The attached coding-agent audit is **substantively strong**: its four retained source fixes address real defects, and its major WiX/MSI observations are directionally correct. However, a second independent pass uncovered several important issues that the prior audit either missed or understated.

### Release recommendation

**Do not ship the current tree as 1.9.20 without addressing the release-blocking High findings below.** There is no newly confirmed Critical security vulnerability under a strict severity definition (remote code execution, privilege escalation, catastrophic data loss, etc.), but there are multiple **High / release-blocking correctness and stability defects** with realistic user impact.

### Highest-priority findings

| ID | Severity | Status | Area | Finding |
|---|---|---|---|---|
| F-01 | **HIGH / RELEASE BLOCKER** | Confirmed | MSI/WiX | `OnUninstall` has **three independent sequencing/execution defects**: `FirstSequence` skips normal UI uninstall, `asyncWait` races MSI cleanup, and `REMOVE=ALL` is tested before `InstallValidate`. |
| F-02 | **HIGH / RELEASE BLOCKER** | Confirmed | Release tooling | `src/tools/version/version.ps1` overwrites the authoritative WiX variable file with an obsolete 3-line template, deleting all per-architecture component variables. A subsequent installer build is expected to fail, and the architecture GUID scheme is destroyed. |
| F-03 | **HIGH / RELEASE BLOCKER** | Confirmed | Manifest/runtime config | The embedded application manifest is **not well-formed XML**: `asmv3:` and `ws2:` prefixes are undeclared. The built x64 executable contains the same malformed text. Long-path, DPI, and code-page declarations therefore cannot be trusted. |
| F-04 | **HIGH** | Confirmed | Registry/data correctness | The retained terminator fix is incomplete: both string setters still write an empty non-null `REG_SZ` / `REG_EXPAND_SZ` with `cbData == 0` rather than storing the terminating NUL. |
| F-05 | **HIGH** | Confirmed | Paths/functionality | A family of path wrappers violates Win32 size/return contracts. `Path::Module()` misses `GetModuleFileNameW` truncation; `Path::Full()` / `Long()` record required-buffer size as logical string length; several other wrappers never retry fixed buffers. |
| F-06 | **HIGH stability hardening** | Confirmed configuration risk | Exceptions/host stability | First-party EXE and injected DLL compile with `/EHa`, while the codebase has 52 `catch(...)` sites. In Explorer/third-party hosts, asynchronous SEH such as access violations can be caught as C++ exceptions and execution can continue after process-state corruption. |
| F-07 | **MEDIUM** | Confirmed code race | Concurrency | `is_in_taskbar` is a process-global unsynchronized `bool` used by menu hooks on potentially multiple host UI threads. It is both a C++ data race and semantically thread-local state. |
| F-08 | **MEDIUM** | Confirmed | Script/path functions | `Path::GetRoot`, `Path::IsCLSID`, and `path.wsl` index strings without adequate length checks; `IsCLSID` also rejects the canonical 40-character `::{GUID}` form. |
| F-09 | **MEDIUM, high blast radius** | Confirmed latent | COM ownership | Custom `IComPtr<T>` has unsafe move/output/release semantics; several members are broken or dangerous when exercised. There are 71 first-party declarations/usages. |
| F-10 | **MEDIUM** | Confirmed | Registry functionality | `reg.values` enumerates names with a 260-character buffer, though Unicode registry value names can be 16,383 characters. `ERROR_MORE_DATA` terminates enumeration and hides that value and all subsequent values. |
| F-11 | **MEDIUM design/security** | Confirmed | ACLs/registration | Registration calls `Permission::SetFile()` on the install directory, but the function cannot open directories because it omits `FILE_FLAG_BACKUP_SEMANTICS`. A naïve fix would be unsafe because the function grants BUILTIN\Users `GENERIC_ALL`. |
| F-12 | **MEDIUM** | Confirmed design behavior | Package cache | A successful package scan is cached forever until the entire config cache is cleared; package install/update/uninstall can therefore leave `package.*` results stale for a long-lived Explorer process. |
| F-13 | **MEDIUM/LOW** | Confirmed | MSI component rules | `StartMenuShortcut` reuses one ComponentId across x86 and x64/ARM64 even though the component's 64-bit attribute differs; Microsoft documents 32→64 recompilation as requiring a new component code. |
| F-14 | **MEDIUM** | Confirmed | Upgrade behavior/docs | The `var.wxi` side-by-side rationale contradicts the emitted Upgrade table; same-version cross-architecture coexistence is blocked by `NEWPRODUCTFOUND`. |
| F-15 | **MEDIUM release hardening** | Confirmed artifact/config gap | Build/security | Release binaries are unsigned and the checked x64 PE headers do not advertise CFG (`IMAGE_DLLCHARACTERISTICS_GUARD_CF` absent). Microsoft strongly recommends CFG. |
| F-16 | **LOW/MEDIUM** | Confirmed | Build/perf | Release DLL explicitly disables whole-program optimization and `/OPT:REF`, increasing an injected DLL's code/data footprint. Benchmark before changing because hooks/exports are sensitive. |
| F-17 | **LOW** | Confirmed | Resource lifetime | `TaskbarUiaWorker::ensure_started()` leaks event handles when one event or `CreateThread` fails. |
| F-18 | **LOW** | Confirmed latent | Hook lifetime | `IATHook::~IATHook()` clears `_hModule` before `FreeLibrary`, leaking the reference acquired by `GetModuleHandleExW`; a separate dead helper returns a pointer to stack memory. |
| F-19 | **LOW/MEDIUM** | Confirmed | UI functionality | `window.title` and parent/owner title functions truncate titles to 249 characters. |
| F-20 | **LOW/MEDIUM** | Confirmed process/tooling divergence | Build | `src/setup/build.cmd` uses `wix.exe build` without the separate `wix msi validate` step, while the MSBuild WiX path automatically performs MSI ICE validation. |

### Overall assessment of the prior agent audit

- **4/4 retained source fixes:** technically justified.
- **Installer findings:** mostly valid, but `OnUninstall` is more broken than reported and the version-tool issue is more severe than reported.
- **Registry write fix:** directionally correct but incomplete for empty strings; the audit's claim that `RegistryKey::SetString` was already correct is false for the empty-string case.
- **Performance/concurrency “clean” conclusion:** too optimistic; the global taskbar flag and package-index freshness deserve action.
- **Long-path behavior:** materially under-audited. The source attempts to be long-path aware, but both the manifest and several wrappers undermine that intent.

---

## 2. Scope, methodology, and evidence standard

### 2.1 Repository coverage

The archive contains a Git repository with **359 tracked files**. The first-party Windows-facing code is concentrated in:

- `src/dll/` — injected shell DLL, context menu engine, hooks, expressions, parser, package/taskbar logic.
- `src/shared/` — custom strings, COM wrappers, registry, paths, security, Win32 abstractions, drawing and process utilities.
- `src/exe/` — registration/control executable.
- `src/setup/` — WiX v5 package authoring and custom-action DLL.
- `src/tests/` — 12 existing test suites.
- `src/tools/` — versioning and auxiliary release tooling.
- `.github/workflows/build.yml`, `build.ps1` — build/CI pathways.

The first-party tree contains approximately **2,376 syntactic Win32-style API calls** and **52 `catch(...)` sites**. This was used as a triage map, not as a claim that every API call has an independent runtime probe.

### 2.2 Baseline handling

The working tree is intentionally/incidentally dirty. At audit time it contained changes in 10 files, including large line-ending-only or formatting diffs in `docs/menu.json`, `Selections.h`, `test_stringcompare.cpp`, `hash.sln`, and `tr.nss`. The meaningful retained audit changes are the four source fixes plus the new registry test.

Therefore this pass used:

1. Git `HEAD` and recent history to understand pre-existing fixes;
2. the current working tree for the retained coding-agent fixes;
3. semantic diffs rather than raw dirty-file count;
4. built x86/x64/ARM64 binaries, MSIs, and WiX PDBs as artifact evidence.

This matters because an audit must not accidentally treat line-ending churn as a functional patch.

### 2.3 External verification

Material Win32/MSI contracts were checked primarily against Microsoft Learn; modern WiX validation behavior was checked against current FireGiant/WiX documentation. Key references are collected in §14.

The audit explicitly reviewed both directions:

- **Code → documentation:** suspicious API usage was checked against documented parameter/return contracts.
- **Documentation → code:** documented edge conditions (required-size semantics, MSI sequence rules, registry maximums, manifest namespace requirements, CFG guidance, etc.) were used to search for code that failed to implement them.

### 2.4 Runtime limitation

This review environment is Linux. It can inspect source, Git history, PE/MSI/WiX artifacts, and run platform-independent probes, but **cannot independently execute Explorer, COM shell hosts, Windows Installer UI flows, or the Windows-built unit-test executable**.

Accordingly:

- The prior audit's reported `build.ps1 -Platform x64` result (**24,175 checks, 0 failures**) is treated as supplied Windows evidence, not independently rerun here.
- The prior audit's real-Windows hook probe proving two hook entries is not independently rerun; the control-flow rationale was independently verified from source.
- WiX emitted-table claims were independently cross-checked against the included x64 `.wixpdb` where relevant.
- Manifest well-formedness was independently tested with an XML parser; it fails at the first undeclared prefix.

---

# 3. Validation of the attached coding-agent audit

## 3.1 `Main.cpp` — double menu build through TrackPopupMenu re-entrancy

**Prior verdict:** CRITICAL performance/functionality.  
**This audit:** **CONFIRMED; regrade to HIGH / release-critical performance-functionality** rather than “Critical” in a security/severity taxonomy.

### Why the fix is correct

The pre-fix path calls public `TrackPopupMenu` from inside a hook. The repository also patches `user32.dll`'s import of `win32u!NtUserTrackPopupMenuEx`. Calling back through `user32!TrackPopupMenu` can therefore traverse the patched import and re-enter `TrackPopupMenuExProc`, re-running menu initialization and construction.

The retained change at `src/dll/src/Main.cpp:835-849` bypasses that patched user32 import by invoking the saved original `NtUserTrackPopupMenuEx` pointer when the IAT hook is installed.

Microsoft documents:

- `TrackPopupMenu`'s `prcRect` parameter is ignored.
- `TrackPopupMenuEx` accepts a null `LPTPMPARAMS`.

So the public API behavior used by the fix is compatible at the parameter level.

### Residual concern

`NtUserTrackPopupMenuEx` is a native implementation detail rather than a supported public API contract. The project already intentionally hooks it, so this is not introduced by the patch, but it remains a **compatibility risk** across Windows builds.

**Long-term improvement:** preserve the current fix for 1.9.20, but add a scoped/thread-local re-entrancy guard at the hook boundary so an internal call can safely use the public API path where feasible. Keep the direct native call as a tested fallback only if required.

### Test recommendation

Add a deterministic hook probe that asserts one `ContextMenu::CreateAndInitialize` per outer popup, including:

- `TrackPopupMenu` path;
- `TrackPopupMenuEx` path;
- with/without `TPM_RETURNCMD`;
- Explorer and at least one third-party host.

---

## 3.2 `ContextMenu.cpp` — fixed-size menu title reader

**Prior verdict:** HIGH functionality.  
**This audit:** **CONFIRMED.**

Microsoft's `GetMenuItemInfoW` documentation explicitly prescribes the two-call pattern: first call with `dwTypeData = NULL` to obtain `cch`, allocate `cch + 1`, then call again.

The retained conversion to `read_menu_text(...)` matches that contract and aligns this site with the other corrected menu readers. This is a real functionality defect for extensions producing long menu text.

**Severity note:** High is defensible where long host text causes visible menu corruption/misclassification; otherwise Medium/High. The fix should remain.

---

## 3.3 `FuncExpression.cpp` — explicit-type `reg.get()` reads with zero-sized buffer

**Prior verdict:** HIGH functionality.  
**This audit:** **CONFIRMED.**

The explicit-type branch set `dwtype` but never performed the first `RegQueryValueExW` sizing call, leaving `cbdata == 0`. A subsequent non-null data call therefore has no usable output buffer for a non-empty value.

The retained first sizing query at `src/dll/src/Expression/FuncExpression.cpp:5259-5260` follows the documented pattern and is correct.

### Missing test

There is no obvious expression-level regression test proving:

- explicit `REG_SZ`;
- explicit `REG_EXPAND_SZ`;
- explicit `REG_DWORD`;
- explicit `REG_QWORD`;
- empty string;
- malformed/non-terminated legacy string.

Add those before release.

---

## 3.4 `Registry.cpp` — string writes omitted the terminator

**Prior verdict:** HIGH stability/data; fixed.  
**This audit:** **PARTIALLY CONFIRMED; FIX INCOMPLETE.**

For non-empty strings, the retained change correctly adds one `wchar_t` before converting character count to bytes.

However, both setters use this guard:

```cpp
if(value && *value && length > 0)
{
    length++;
    length *= sizeof(wchar_t);
}
```

Affected locations:

- `src/shared/System/Windows/Registry.cpp:498-507` — `RegistryKey::SetString`.
- `src/shared/System/Windows/Registry.cpp:659-678` — `Registry::SetKeyValue` retained fix.

For a non-null empty string (`L""`), `length == 0`, so both write `cbData == 0`.

Microsoft documents that for `REG_SZ`, `REG_EXPAND_SZ`, and `REG_MULTI_SZ`, `cbData` **must include the terminating null character(s)**. A null `lpData` with `cbData == 0` is allowed, but that is a different state from a non-null empty string.

### Reachability

`reg.set(...)` converts a script value to `string v` and calls:

`Registry::SetKeyValue(..., v, v.length(), ...)` at `FuncExpression.cpp:5334-5335`.

Thus `reg.set(key, name, "", reg.sz)` reaches the bug.

### Correction

Use explicit string semantics, e.g. conceptually:

- if `value == nullptr`: preserve whatever “no data” semantics are intended;
- otherwise bytes = `(length + 1) * sizeof(wchar_t)` for SZ/EXPAND_SZ.

Do not key terminator storage on `*value != 0`.

### Test gap

The new test at `test_registry.cpp:275+` covers only non-empty values (`"written"`, `"%SystemRoot%"`). Add empty-string assertions for **both** setter families.

---

# 4. Release-blocking findings not fully captured by the prior audit

## F-01 — `OnUninstall` has three independent Windows Installer contract violations

**Severity:** **HIGH / RELEASE BLOCKER**  
**Status:** Confirmed in source and emitted x64 WiX/MSI intermediate data  
**Files:** `src/setup/wix/setup.wxs:265,269`; `src/setup/ca/dllmain.cpp:553-567`

Authoring:

```xml
<CustomAction Id='OnUninstall'
              Execute='firstSequence'
              Return='asyncWait'
              BinaryRef='CA'
              DllEntry='Uninstall' />
...
<Custom Action='OnUninstall'
        After='FindRelatedProducts'
        Condition='(REMOVE ="ALL") AND (NOT UPGRADINGPRODUCTCODE)' />
```

The included x64 WiX PDB independently shows:

- `OnUninstall` CustomAction Type **385** (`DLL + Async + FirstSequence`).
- Execute sequence **27**.
- `InstallValidate` at **1400**.
- `InstallInitialize` at **1500**.
- `RemoveFiles` at **3500**.

### Defect 1 — `FirstSequence` skips the execute sequence after a UI sequence

Microsoft documents `msidbCustomActionTypeFirstSequence`:

> It always skips the action in the execute sequence if the UI sequence has run; the action does not need to be present in the UI sequence to be skipped.

`OnUninstall` is only authored in `InstallExecuteSequence`. A normal full-UI maintenance/uninstall therefore reaches the execute sequence after UI processing and can skip this cleanup action entirely.

The prior audit correctly identified this part.

### Defect 2 — `asyncWait` allows destructive MSI actions to run concurrently

Microsoft documents asynchronous custom actions as running simultaneously while the main installation continues. `asyncWait` waits before the sequence ends, not before subsequent standard actions begin.

The custom action launches:

`ShellExec(shell.exe, "-u -s -t -restart", ..., wait=true)`

and then deletes the upgrade backup.

Meanwhile MSI can continue into component/file/registry removal. This creates a direct race among:

- executing `shell.exe`;
- unregistering shell state;
- restarting Explorer;
- deleting files/registry rows owned by the MSI;
- deleting cleanup resources.

Even the silent-uninstall path where `FirstSequence` does not suppress the action can therefore be unreliable.

### Defect 3 — `REMOVE=ALL` is evaluated too early

Microsoft's `REMOVE` property documentation explicitly warns:

> If a product is removed by setting its top feature to absent, `REMOVE` may not equal `ALL` until after `InstallValidate`; therefore any custom action that depends on `REMOVE=ALL` must be sequenced after `InstallValidate`.

This action runs at sequence 27, far before `InstallValidate` 1400.

### Impact

Depending on uninstall entry point, one or more of these can occur:

- shell unregister logic never runs;
- Explorer is not restarted;
- shell handler/TreatAs/Approved registrations survive until MSI-owned rows happen to remove them (and custom registrations not in MSI remain);
- the custom action races deletion of the executable it needs;
- the saved config backup is not discarded or is discarded at the wrong time;
- behavior differs between ARP/UI uninstall, command-line feature removal, and silent `/qn` uninstall.

### Required fix

For the current design:

1. **Remove `Execute='firstSequence'`.**
2. Make the action **synchronous**. If failure must not abort uninstall, use synchronous `Return='ignore'`; otherwise check the result normally.
3. Sequence the action **after `InstallValidate`** so `REMOVE=ALL` is reliable.
4. Keep it **before destructive file removal** while `shell.exe` is still present and executable.
5. Keep `NOT UPGRADINGPRODUCTCODE` if the intent is to exclude the removal half of a major upgrade.

Consider whether this work should be a deferred elevated custom action. If registration cleanup requires machine-write privileges, immediate impersonated behavior can vary with how the MSI was launched. If converted to deferred/no-impersonate, pass only the required path via `CustomActionData` and obey the limited deferred-session API contract.

### Required test matrix

Use verbose MSI logs on a clean VM for:

- ARP interactive uninstall;
- `msiexec /x product.msi` full UI;
- `/qb`;
- `/qn`;
- removal by setting top feature absent;
- older-version same-arch upgrade;
- older-version cross-arch upgrade;
- cancellation before/after the action;
- repair followed by uninstall.

Assert that `shell.exe -u -s -t -restart` is invoked exactly once where intended and finishes before MSI removes its prerequisites.

---

## F-02 — the versioning tool overwrites required WiX architecture definitions

**Severity:** **HIGH / RELEASE BLOCKER**  
**Status:** Confirmed  
**Files:** `src/tools/version/version.ps1:117-121`, `src/tools/version/var.wxi`, `src/setup/wix/var.wxi`

`version.ps1` runs:

```powershell
update "var.wxi" "..\..\setup\wix\var.wxi";
```

The source template contains only:

```xml
<?define Version = '...' ?>
<?define ProductCode = '{...BEE77}' ?>
```

The current authoritative `src/setup/wix/var.wxi` now contains:

- three architecture-specific `ProductCode` values;
- `ComponentBitness`;
- seven per-architecture component GUID definitions.

`setup.wxs` references all of these definitions, including:

- `$(var.ImportsComponentGuid)` line 72;
- `$(var.LangComponentGuid)` line 83;
- `$(var.Lang2ComponentGuid)` line 97;
- `$(var.ApplicationComponentGuid)` and `$(var.ComponentBitness)` line 112;
- `$(var.ConfigComponentGuid)` line 151;
- `$(var.CleanupComponentGuid)` line 177;
- `$(var.DisplayIconComponentGuid)` line 185.

### Impact

The prior audit said this “destroys the per-arch GUID scheme.” That is true but understates the immediate failure mode: after the overwrite, the next WiX preprocessing/build has **undefined variables referenced by `setup.wxs`** and is expected to fail.

If someone manually repairs only the missing variables while retaining the old single `ProductCode`, architecture identity is still regressed.

### Required fix

Do not copy a stale whole-file template over an evolving installer definition.

Preferred options:

1. Keep `src/setup/wix/var.wxi` authoritative and have the version tool update **only the Version define**.
2. Better: make version a build property (`DefineConstants`) supplied by CI/build tooling, removing file mutation entirely.
3. If a generated file is retained, the generator/template must own **all** declarations and be tested as code.

### Required CI guard

In a temporary clean worktree:

1. run the version tool;
2. build x86, x64, and ARM64 MSIs;
3. inspect ProductCode and component GUID tables;
4. fail CI if unrelated GUID definitions disappear or change unexpectedly.

---

## F-03 — the embedded manifest is malformed and namespace assignments are wrong

**Severity:** **HIGH / RELEASE-BLOCKING CONFIGURATION DEFECT**  
**Status:** Confirmed at source and built-binary text level  
**File:** `src/shared/Resource/manifest.xml:1-45`

The root element declares only the default asm.v1 namespace:

```xml
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
```

but later uses:

```xml
<asmv3:application>
  <asmv3:windowsSettings ...>
    ...
    <ws2:longPathAware>true</ws2:longPathAware>
    <activeCodePage>UTF-8</activeCodePage>
  </asmv3:windowsSettings>
</asmv3:application>
```

Neither `asmv3` nor `ws2` is declared. A standard XML parse fails:

`ParseError: unbound prefix: line 37, column 4`.

The built x64 `shell.exe` contains the same raw malformed fragment, so this is not merely a source file unused by the resource build.

### Official manifest requirements

Microsoft's current application-manifest documentation shows:

- root declaration `xmlns:asmv3="urn:schemas-microsoft-com:asm.v3"` for `asmv3:*` elements;
- `longPathAware` under the 2016 WindowsSettings namespace (commonly `xmlns:ws2="http://schemas.microsoft.com/SMI/2016/WindowsSettings"`);
- `activeCodePage` under the **2019** WindowsSettings namespace.

The current unqualified `activeCodePage` inherits the surrounding 2005 default WindowsSettings namespace, so even after fixing the undeclared prefixes its namespace would still not match Microsoft's documented schema example.

### Impact

At minimum, the project cannot safely claim that these requested settings are represented by a valid manifest:

- Common Controls dependency/activation-context processing;
- DPI awareness;
- long-path awareness;
- UTF-8 active code page.

Because the application is evidently buildable/usable, this audit does **not** assert that Windows necessarily refuses process startup in every context. The exact Windows activation-context behavior should be verified on a VM with `mt.exe`/SxS diagnostics. But shipping a syntactically invalid embedded manifest is itself unacceptable.

### Required fix

Rebuild this block directly from Microsoft's documented manifest samples rather than patching individual prefixes by guesswork. Ensure each setting uses its documented namespace.

### CI requirement

Add a release step that:

1. parses source manifest as XML;
2. invokes `mt.exe` manifest validation on Windows;
3. extracts the manifest from each produced EXE/DLL and validates the **embedded artifact**, not just the source file.

This would have caught the current issue immediately.

---

## F-04 — empty registry strings remain malformed after the retained fix

See §3.4. This is a release-worthy correction because it is a simple, deterministic contract violation in a user-facing script API.

**Required patch and tests:** fix both `RegistryKey::SetString` and `Registry::SetKeyValue` for empty non-null strings and add raw-byte tests.

---

## F-05 — path wrappers systematically mishandle Win32 dynamic-size contracts

**Severity:** **HIGH functionality/stability**  
**Status:** Confirmed  
**Primary file:** `src/shared/System/IO/Path.h`

This is not one isolated `MAX_PATH` call. It is a pattern.

### F-05a — `Path::Module()` never retries the documented truncation case

Current code (`Path.h:1063-1072`):

```cpp
string path(MAX_PATH);
auto len = ::GetModuleFileNameW(handle, path.data(), MAX_PATH);
if(len > MAX_PATH)
{
    ...
}
```

Microsoft documents that if the buffer is too small, `GetModuleFileNameW` truncates and returns **exactly `nSize`**, setting `ERROR_INSUFFICIENT_BUFFER` on modern Windows.

Therefore `len > MAX_PATH` can never detect the documented truncation signal from this call.

A second helper, `System::Module::path()` (`src/shared/System.h:1509-1517`), is hard-limited to 260 with no retry at all.

#### Impact

Module/executable paths feed core behavior such as initialization, config/import path derivation, registration, executable/DLL location, and loader classification. A deep install location can silently turn into a valid-looking truncated prefix.

### F-05b — `Path::Full()` stores the required-buffer count as the logical string length

At `Path.h:950-957`:

1. `GetFullPathNameW(..., 0, nullptr, ...)` returns the **required buffer size including NUL**.
2. The second successful call returns the number of characters copied **excluding NUL**.
3. The code ignores the second result and calls `full_path.release(ccFull)` using the first count.

The custom `string::release(n)` sets `m_length = n` and writes a terminator at index `n`. Therefore the logical string contains the API's terminator as a counted embedded character and then gets an additional terminator.

This is visible to length/equality/hash/append logic even though `c_str()` looks normal to APIs that stop at the first NUL.

`Path::Full()` is used by parser import canonicalization and the script `path.full()` function, so this is reachable.

### F-05c — `Path::Long()` has the same required-size/success-size mismatch

`GetLongPathNameW` has the same asymmetric contract: too-small/size-query result includes NUL, successful result excludes it.

`Path::Long(path)` calls `release(length)` with the first required size instead of the actual second-call result.

### F-05d — fixed-buffer wrappers fail to retry

Examples:

- `Path::Search()` — `SearchPathW` with `MAX_PATH`, no retry.
- `Path::CurrentDirectory()` — fixed `MAX_PATH`.
- `Path::TempDirectory()` / `Temp()` — fixed buffer pattern.
- `Path::Short()` — fixed `MAX_PATH` even though `GetShortPathNameW(NULL,0)` can obtain required size.
- Open-file dialogs use `MAX_PATH` output buffers.
- shell link target and working-directory retrieval use `MAX_PATH`.
- `System::Diagnostics::Process` module-name paths use fixed buffers.

Some separate helpers (for example `Diagnostics::Shell::getpath`) already implement a retry, showing the project has both correct and incorrect patterns.

### F-05e — legacy `Path::Long(path, bool)` defeats its own normalization

At `Path.h:386-391`, a successful `GetFullPathNameW` is immediately followed by copying `sPath` back over `fullPath`, undoing the normalization it just performed. No first-party call site was found in this pass, so treat this as **latent dead/legacy code** rather than a current release blocker.

### Long-path manifest interaction

Even a perfect `longPathAware` manifest does not make fixed-size wrappers correct. Conversely, the current manifest is malformed, so the project currently has **both layers wrong**: opt-in configuration cannot be trusted and wrappers still impose/truncate at `MAX_PATH`.

### Required engineering change

Introduce one tested internal dynamic-output helper for Win32 APIs with clear policy for:

- whether the required-size result includes NUL;
- whether successful return excludes NUL;
- equality (`>=` vs `>`) at the boundary;
- retry loops when the value can change between calls;
- error propagation.

Do not duplicate bespoke `MAX_PATH` logic at every call site.

### Required tests

On Windows with long paths enabled:

- module path >260 and >1024;
- current directory >260;
- `Path::Full` for relative and absolute input;
- `Path::Long` and `Short` round-trip where 8.3 names are enabled;
- SearchPath result exactly at buffer edge and well above it;
- assert **both** `wcslen(result.c_str()) == result.length()` and content equality;
- parser import equality/canonicalization under deep paths.

---

# 5. Stability and concurrency findings

## F-06 — `/EHa` plus broad `catch(...)` is dangerous in an injected Explorer DLL

**Severity:** **HIGH stability hardening**  
**Status:** Confirmed build configuration; specific crash continuation requires runtime testing  
**Files:** `src/dll/Shell.vcxproj:70`; `src/exe/exe.vcxproj:75`; 52 `catch(...)` sites across first-party source.

MSVC's `/EHa` enables C++ exception handling for asynchronous structured exceptions. Microsoft recommends `/EHsc` for standard C++ exception handling; with `/EHsc`, C++ `catch(...)` does not catch asynchronous SEH such as access violations.

The codebase deliberately uses targeted `__try/__except` at some hook/memory-inspection boundaries. That is preferable to making **every** C++ `catch(...)` eligible to catch process faults.

### Why this matters here

`shell.dll` is injected into Explorer and possibly third-party shell hosts. Continuing execution after catching an access violation can leave:

- corrupted heap/object state;
- partially modified menu structures;
- locks or refcounts inconsistent;
- host process behavior unpredictable.

A “stability” catch that swallows such a fault can be worse than fail-fast behavior.

### Recommendation

1. Build ordinary C++ code with `/EHsc`.
2. Retain SEH only in narrow leaf wrappers where probing foreign process structures or hook boundaries genuinely requires it.
3. Convert broad `catch(...)` blocks to expected exception types where possible.
4. At outer host boundaries, log and fail closed rather than continue through an unknown hardware fault.
5. Run Explorer stress + Application Verifier/PageHeap after the transition.

Do not switch the whole project blindly: the current code contains `__try/__except`, and MSVC restrictions around C++ unwinding/SEH need to be handled per function/TU.

---

## F-07 — `is_in_taskbar` is a process-global data race

**Severity:** **MEDIUM**  
**Status:** Confirmed code-level race  
**File:** `src/dll/src/Main.cpp:806,872,1008,1041`

```cpp
bool is_in_taskbar;
```

`ShowTaskbarContextMenu()` sets it true, the popup hook reads it in `Initializer::OnState(is_in_taskbar)`, and the hook finally resets it false.

Menu hooks are process-wide, and Explorer/other hosts can have multiple UI threads. Concurrent popup execution means unsynchronized read/write of a plain `bool`, which is a C++ data race. Semantically it is also wrong: “this popup originated from taskbar” is **call/thread context**, not process-global state.

### Failure modes

- a normal file menu can be initialized with taskbar state;
- a taskbar menu can see false if another menu resets the global;
- undefined behavior from the data race itself.

### Fix

Prefer `thread_local` scoped state or, better, pass an explicit popup-context object through the hook path. Use RAII to restore nested/reentrant state rather than assigning false unconditionally.

### Test

Create two host threads that enter the hook concurrently with different taskbar states; assert each context observes only its own value.

---

## F-08 — unsafe path indexing and broken shell-namespace predicate

**Severity:** **MEDIUM**  
**Status:** Confirmed  
**Files:** `Path.h:227-231,715-724`; `FuncExpression.cpp:3036,3121,3465-3474`

### `Path::GetRoot`

```cpp
if(path[1] == ':' && path[2] == '\\')
```

No minimum-length check. Script `path.root(arg)` passes arbitrary user input.

### `Path::IsCLSID`

```cpp
if(path.length() >= 40)
    return false;
return path[0] == ':' && path[1] == ':' && path[2] == '{';
```

Problems:

1. strings shorter than 3 are indexed out of bounds;
2. canonical `::{00000000-0000-0000-0000-000000000000}` is **40 characters**, so the function always rejects a properly sized canonical form;
3. short strings beginning with `::{` are accepted without validating a GUID.

`IsNameSpace()` is just `IsCLSID()`, so the bug affects path classification and script `path.isnamespace`.

### `path.wsl`

The function checks only `!arg0.empty()` and then reads `path[1]`; a one-character input is out of bounds.

### Fix

- add explicit minimum lengths before indexing;
- validate namespace GUID syntax with a real GUID parser (`CLSIDFromString`/`IIDFromString`) on the brace portion;
- add boundary tests for empty, 1-char, 2-char, exact canonical, malformed brace/GUID, and non-drive WSL inputs.

---

## F-09 — custom `IComPtr<T>` has unsafe ownership semantics

**Severity:** **MEDIUM, high blast radius**  
**Status:** Confirmed latent defects; no currently proven crash path for every broken member  
**File:** `src/shared/System.h:1776-2025`  
**Usage:** 71 first-party `IComPtr<...>` declarations/references.

Important defects:

### Defaulted move constructor is not a move for a raw pointer

```cpp
IComPtr(IComPtr<T> &&other) noexcept = default;
```

Default memberwise move copies the raw pointer and `_release` flag. The source still owns the same pointer, so both destructors can `Release()` it.

A separate templated move assignment correctly nulls the source, which makes the defaulted move constructor particularly inconsistent.

### `release()` does not clear ownership

```cpp
unsigned long release()
{
    return pointer ? pointer->Release() : 0;
}
```

`pointer` remains dangling after a final `Release`.

### Output-address conversion releases but leaves the stale pointer

```cpp
operator Q **()
{
    this->release();
    return (Q **)&pointer;
}
```

If the called COM API fails without replacing the output value, the smart pointer still contains the old pointer that has already been released; the destructor can release it again.

`reset()` has the same problem.

### Copy assignment/swap are defective

`operator=(const IComPtr&)` calls `swap(rhs)`, but `swap` takes a non-const reference. The `std::swap(this->pointer, (T*)other.pointer)` expression is also not a sound lvalue-to-lvalue swap. These members may only remain unnoticed because they are not instantiated by current call patterns.

### `CreateInstance()` overwrites without first releasing/nulling existing ownership

Both overloads pass `&pointer` directly to `CoCreateInstance`. Reusing a non-empty `IComPtr` leaks or creates ambiguous ownership.

### Recommendation

The lowest-risk path is to replace this class with a proven COM smart pointer such as:

- WIL `wil::com_ptr`;
- WRL `Microsoft::WRL::ComPtr`;
- ATL `CComPtr` if ATL is acceptable.

If custom ownership must remain, add a fake-IUnknown unit test with an atomic refcount and cover:

- copy construction/assignment;
- move construction/assignment;
- `put()`/output pointer behavior on success and failure;
- reset/release;
- reusing `CreateInstance`;
- cross-interface `QueryInterface`.

Also remove the unused `CoPtr<T>` class, which has shallow-copy ownership semantics of its own.

---

## F-17 — Taskbar UIA worker leaks handles on startup failure

**Severity:** LOW  
**Status:** Confirmed  
**File:** `src/dll/src/Main.cpp:369-391`

`ensure_started()` publishes `_work` and `_done` as members immediately. If one event creation fails, the other is not closed. If `CreateThread` fails, both events remain open. A later retry overwrites member handles, permanently leaking the earlier handles.

### Fix

Create resources into local RAII handles, create the thread, then move/publish the handles only after all startup steps succeed.

### Related reentrancy risk to probe

The caller uses `CoWaitForMultipleHandles`, which can dispatch calls/messages for STA responsiveness. If a dispatched path re-enters the same query while `_caller_mutex` is held, a non-recursive mutex could deadlock. This needs a Windows probe before promotion to a confirmed defect; keep it on the stress-test list.

---

## F-18 — `IATHook` leaks its module reference on destruction

**Severity:** LOW  
**Status:** Confirmed  
**File:** `src/dll/src/Include/Hooker.h:205-210,248-263`

`init(HMODULE)` acquires a module reference using `GetModuleHandleExW` without `GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT`.

Destructor:

```cpp
uninstall(true);
if(_hModule) ::FreeLibrary(_hModule);
```

But `uninstall(true)` first sets `_hModule = {}`. `FreeLibrary` is therefore never called.

For process-lifetime hook objects this is low impact, but it is incorrect RAII and matters in failure/reinitialization scenarios.

### Additional dead unsafe helper

`Hooker.h:575` returns a pointer to a local stack array from `orignal()`. No use was found. Delete dead unsafe code rather than preserving a latent trap.

---

# 6. Registry and string correctness beyond the retained fixes

## F-10 — `reg.values` truncates enumeration on long value names

**Severity:** MEDIUM functionality  
**Status:** Confirmed  
**File:** `src/dll/src/Expression/FuncExpression.cpp:5349-5369`

The loop allocates a 260-character name buffer for both subkeys and values:

```cpp
DWORD name_length = 260;
string name(name_length);
...
retCode = ::RegEnumValueW(...);
```

Microsoft documents:

- registry key names: up to 255 characters;
- Unicode registry **value names: up to 16,383 characters**.

When `RegEnumValueW` returns `ERROR_MORE_DATA`, this loop's condition (`retCode == ERROR_SUCCESS`) terminates enumeration entirely. Thus a long value name hides itself and every subsequent value.

### Fix

Use `RegQueryInfoKeyW` to retrieve maximum value-name length once, allocate accordingly, or dynamically retry `ERROR_MORE_DATA`. Keep the smaller key-name bound for `reg.keys` if desired.

### Test

Create a value name >260 and assert `reg.values` returns it plus values that sort/enumerate after it.

---

## Additional registry areas validated clean

The recent read-side hardening is good:

- size-first query is used where needed;
- malformed/non-terminated legacy `REG_SZ` is not blindly trusted;
- `REG_EXPAND_SZ` expansion now uses the returned expansion value;
- fixed-buffer string reads have termination checks.

Do not regress those while simplifying the write side.

---

# 7. Security/permissions and release-hardening findings

## F-11 — directory ACL helper fails closed; fixing it literally would create a security problem

**Severity:** MEDIUM design/security  
**Status:** Confirmed  
**Files:** `src/shared/System/Security/Permission.h:85-149`; callers `src/exe/src/Main.cpp:265`, `src/dll/src/Main.cpp:1519`

Registration computes the module parent directory (typically the installation directory) and calls:

`Security::Permission::SetFile(dir.c_str());`

`SetFile(path)` opens with:

```cpp
CreateFileW(path,
            READ_CONTROL | WRITE_DAC,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
```

Microsoft documents that obtaining a directory handle with `CreateFile` requires `FILE_FLAG_BACKUP_SEMANTICS`. The current directory call therefore normally fails.

The return value is ignored.

### Why not just add the flag

`SetFile(HANDLE)` adds an inheritable ACE for BUILTIN\Users with:

```cpp
ea.grfAccessPermissions = GENERIC_ALL;
ea.grfInheritance = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
```

If the directory opening were “fixed” literally, ordinary users could potentially gain full control over the installed executable/DLL tree. For a DLL injected into Explorer and shell hosts, user-writable installed binaries are an unacceptable trust boundary.

### Recommendation

- **Remove the directory-wide `GENERIC_ALL` design.**
- Keep program binaries under standard Program Files ACLs.
- Put mutable per-user data in `%LOCALAPPDATA%`/`%APPDATA%`.
- If a shared data directory genuinely needs user writes, grant only the minimum rights (typically Modify, not Full Control) to that narrowly scoped data directory.
- Check and log ACL-operation failures instead of ignoring them.

`Permission::SetRegistry(root, subkey)` also opens with only `KEY_READ` before attempting DACL modification; no active first-party call site was found, so treat it as dead/broken helper code to remove or correct.

---

## F-15 — release binaries lack CFG and signatures

**Severity:** MEDIUM release hardening  
**Status:** Confirmed in included artifacts/configuration  
**Artifacts inspected:** x64 `shell.dll`, `shell.exe`, `ca.dll`; included MSIs.

The x64 PE headers show:

- ASLR / high-entropy VA where applicable;
- NX compatibility;
- **no `IMAGE_DLLCHARACTERISTICS_GUARD_CF` bit**.

Project files also do not set `ControlFlowGuard`.

Microsoft states `/guard:cf` is off by default and strongly encourages developers to enable Control Flow Guard. The linker must also emit `/GUARD:CF` metadata.

The included PE Security Directory is zero for inspected first-party x64 binaries; the prior audit also reported Authenticode `NotSigned` for first-party binaries/MSIs.

### Recommendation

1. Enable CFG for first-party EXE/DLL/custom-action projects (`/guard:cf` compiler + `/GUARD:CF` linker) and verify with `dumpbin /loadconfig`.
2. Run the test/hook matrix because detouring and dynamic function pointers are sensitive to control-flow protections.
3. Sign release EXE/DLL/MSIs with the project's release certificate/process.
4. Verify signatures in CI after packaging.

This is hardening, not evidence of an existing exploit.

---

# 8. Package-index functionality and performance

## F-12 — successful package scans never become stale automatically

**Severity:** MEDIUM functionality  
**Status:** Confirmed design behavior  
**Files:** `src/dll/src/Packages.cpp:306-375,503-509`; `src/dll/src/Include/Cache.h:274-292`

`PackageIndex::ensure_index()` does:

```cpp
if(_state == State::Ready)
    return true;
```

Once a scan succeeds, there is no age check or package-repository generation check. `PackageIndex::clear()` is called when the larger `CACHE::clear()` runs, but package changes can occur independently while Explorer remains running for days.

### Result

After install/update/uninstall of an AppX/MSIX package, script functions backed by the index can return stale results until unrelated config/cache invalidation or Explorer restart.

### The current design's good part

The recent performance design is otherwise sensible:

- one scan rather than per-query registry walks;
- scans happen outside the mutex;
- concurrent callers coalesce behind `State::Loading`;
- failed scans reset to `Empty` so transient failures retry;
- expensive path/display resolution is lazy.

Do not throw that away by rescanning synchronously for every context menu.

### Improvement

Use one low-cost freshness mechanism:

- registry key last-write generation;
- change notification/event;
- or a short TTL checked on demand.

Invalidate only the package index; rebuild lazily on the next query. Preserve the current lazy path/display subcache.

### Tests

Extend `test_packages.cpp` with an injected source generation/version so a “repository changed” signal forces exactly one rescan across concurrent callers.

---

# 9. MSI/WiX architecture and upgrade findings

## F-13 — StartMenuShortcut ComponentId is reused across 32- and 64-bit forms

**Severity:** MEDIUM/LOW installer correctness  
**Status:** Confirmed  
**File:** `src/setup/wix/setup.wxs:191-196`

The component uses one hard-coded GUID for all architectures:

`{41AFAF39-4B3F-4D1E-800E-DD3231C004C9}`.

The emitted packages differ in component attributes (x86 vs x64/ARM64 64-bit bit). Microsoft “Changing the Component Code” guidance explicitly includes recompiling a 32-bit component as a 64-bit component as a case requiring a new component code.

The current comment in `var.wxi` says the component is deliberately shared because its resources have the same logical name/location. That does not override the documented bitness rule.

### Fix

Give the shortcut component per-architecture component GUIDs, or author it in a genuinely architecture-neutral way that produces the same component semantics in every package.

---

## F-14 — side-by-side comment contradicts the Upgrade table

**Severity:** MEDIUM docs/functionality  
**Status:** Confirmed

`var.wxi` says same-version x86 and x64 packages can install side by side. But `setup.wxs:221-223` authors:

- `NEWPRODUCTFOUND` minimum current version, `OnlyDetect=yes`;
- `UPGRADEFOUND` from 1.0.0 up to (exclusive) current version.

A same-version related product is therefore detected by `NEWPRODUCTFOUND`, and `OnDowngrading` displays:

“A newer version ... is already installed.”

So same-version cross-architecture coexistence is not the current behavior.

### Fix

Decide the intended product policy first:

- **single installed architecture:** keep cross-arch detection/removal and rewrite the comment/error text accordingly;
- **true side-by-side architectures:** requires a deliberate related-product/UpgradeCode strategy, install-directory separation, registrations, and component ownership design. Do not achieve it accidentally by changing one version bound.

Given shell-extension registration is machine-global, single-architecture installation is likely the safer policy.

---

## Existing WiX areas that appear sound

The prior audit's clean findings are consistent with source/artifact review for:

- architecture-specific ProductCodes in current `var.wxi`;
- per-architecture component GUIDs for components under the Program Files install tree (except StartMenuShortcut);
- `RemoveExistingProducts` immediately after `InstallValidate`, which intentionally removes the old product before installing new files;
- deferred elevated `OnUpdateElevated` / `OnRestoreConfig` use of property-to-`CustomActionData` flow;
- targeted `RemoveFile` entries rather than broad directory wildcards;
- installer platform matrix x86/x64/ARM64.

These should be preserved while fixing F-01/F-02/F-13/F-14.

---

# 10. UI/functionality and smaller correctness findings

## F-19 — window title functions truncate at 249 characters

**Severity:** LOW/MEDIUM functionality  
**Status:** Confirmed  
**File:** `src/dll/src/Expression/FuncExpression.cpp:1438-1460`

Both current-window and parent/owner title paths use:

```cpp
wchar_t title[250]{};
GetWindowTextW(h, title, 250);
```

Long titles are silently truncated. Microsoft provides `GetWindowTextLengthW` specifically to size an output buffer; it guarantees an allocation based on the returned length is at least large enough for the actual title (with documented ANSI/Unicode caveats).

### Fix

Dynamically size from `GetWindowTextLengthW` + 1, then call `GetWindowTextW`, using the actual returned length.

This is not a hot path worth trading correctness for a 250-character stack buffer.

---

## Other fixed-size buffers worth follow-up, but not all are defects

A codebase-wide `MAX_PATH`/fixed-buffer scan found additional sites in:

- shell link target/working-directory retrieval;
- image/path search helpers;
- process module-path helpers;
- Open/Save dialogs;
- indirect-string loading;
- package/MRT registry enumeration.

Not every fixed buffer is wrong: some APIs/types have strict small documented maxima (e.g. registry key component names), and some recent helpers already retry correctly. The recommendation is to **standardize per API contract**, not mechanically replace every array with a vector.

---

# 11. Build, CI, and performance improvements

## F-20 — the direct WiX CLI build path skips MSI validation

**Severity:** LOW/MEDIUM process reliability  
**Status:** Confirmed against current WiX documentation  
**File:** `src/setup/build.cmd`

The script runs only:

```bat
wix.exe build ...
```

Current WiX documentation states:

- building a package through WiX MSBuild automatically runs stock MSI SDK ICE validation;
- when using `wix.exe`, validation is a separate `wix msi validate` subcommand.

The canonical `build.ps1` builds `src/Shell.sln`, which includes `setup.wixproj`, so it gets the MSBuild path.

### Recommendation

Either:

- delete/deprecate `src/setup/build.cmd` and make it invoke the canonical build; or
- add `wix msi validate` with the produced `.wixpdb` for source-line reporting.

Avoid two release paths with different validation guarantees.

---

## F-16 — release DLL explicitly disables dead-code/reference optimization

**Severity:** LOW/MEDIUM performance/footprint opportunity  
**Status:** Confirmed configuration  
**File:** `src/dll/Shell.vcxproj:103-121`

Release settings include:

- `WholeProgramOptimization=false`;
- `OptimizeReferences=false` (`/OPT:NOREF` behavior);
- `EnableCOMDATFolding=true`;
- MaxSpeed optimization.

For a DLL injected into long-lived host processes, code/data footprint matters. Dead sections retained by `/OPT:NOREF` add memory/image cost in every host that maps the DLL.

### Recommendation

Benchmark a branch with:

1. `/OPT:REF` enabled;
2. WPO/LTCG enabled if build-time cost is acceptable;
3. exact export table and hook behavior verified;
4. startup/menu first-pixel latency, image size, private working set and commit compared.

Do not combine this with functional fixes in one patch; linker optimization changes complicate regression attribution.

---

## Warning/security-setting consistency

Current state:

- tests: C++20, conformance mode, warnings-as-errors;
- DLL: Level 4 + SDL + conformance, but `TreatWarningAsError=false`;
- EXE: Level 4 + SDL + conformance;
- custom-action DLL: sparse compile-policy settings (C++20 and optimization, but no matching explicit warning/SDL/conformance policy visible in the project file).

### Recommendation

Make first-party warning/security policy explicit and consistent in a shared `.props` file. Third-party projects may keep separate policies.

Suggested release baseline:

- `/W4`;
- warnings as errors for first-party CI;
- `/permissive-` / conformance mode;
- SDL checks;
- CFG after compatibility test;
- explicit exception model;
- `/GS` (default, verify not disabled);
- static analysis job (`/analyze`) on at least x64;
- optional AddressSanitizer configuration for non-hook unit/integration components where supported.

---

# 12. Performance review — what matters most

The prior audit's most important performance fix is the TrackPopup re-entrancy removal. That aligns with the project's stated 1.9.20 objective of reducing synchronous work before first menu pixel.

### Confirmed positive architecture choices

- Native package display/path resolution is lazy.
- Package scanning coalesces concurrent callers.
- Taskbar UI Automation runs on a dedicated MTA worker rather than owning UIA objects on menu threads.
- The taskbar result cache has bounded TTL/capacity tests.
- Menu text dynamic sizing avoids allocation for separators/zero-length text.
- Recent code avoids holding the package-index mutex across slow package-manager operations.
- The prior audit's manual check that the native-menu lazy path does not hold its hook-map lock across `SendMessageW(WM_INITMENUPOPUP)` is consistent with the intended architecture.

### Highest-value performance improvements still available

1. **Keep the double-build fix.** This is by far the clearest synchronous-menu win.
2. **Do not rescan packages per menu.** Add cheap freshness invalidation, not eager rescanning.
3. **Measure `/OPT:REF` and WPO** for the injected DLL.
4. **Centralize Win32 dynamic buffer sizing.** This primarily fixes correctness but also eliminates repeated failed/truncated operations and ad-hoc over-allocation.
5. Keep perf tracing opt-in; avoid file I/O/log open-close on hot paths unless diagnostics are enabled.
6. Add a first-pixel performance gate to CI/manual release testing: cold and warm Explorer menus, empty/minimal config, complex config, third-party shell extensions.

### Suggested metrics

Record median/P95/P99 for:

- hook entry → `ContextMenu::CreateAndInitialize` complete;
- initializer state refresh;
- parser/config load on cold and warm path;
- synthetic `WM_INITMENUPOPUP` work;
- first visible menu pixel;
- package-index first scan;
- taskbar UIA query/cache-hit path.

A regression budget is more useful than one-time anecdotal timings.

---

# 13. Recommended test additions and release gates

## 13.1 P0 tests before shipping

### MSI/uninstall matrix

Automate or script VM tests described in F-01. Parse verbose MSI logs and assert custom-action sequence/order, not just final files.

### Manifest validation

- XML parse source manifest.
- `mt.exe` validate source.
- extract/validate manifest from x86/x64/ARM64 EXE and DLL outputs.

### Version-tool destructive-regression test

Run the version tool in a disposable worktree, then build all three installer architectures and inspect ProductCode/component GUIDs.

### Registry string tests

Add:

- empty `REG_SZ` through `RegistryKey::SetString`;
- empty `REG_EXPAND_SZ` through same;
- empty `REG_SZ` through static `SetKeyValue`;
- empty `REG_EXPAND_SZ` through static path;
- script `reg.set` / typed `reg.get` end-to-end.

### Path API contract tests

Add a dedicated `test_path.cpp` with long/deep paths and exact logical-length assertions.

### Taskbar state concurrency test

After converting state to thread-local/explicit context, prove two concurrent popup contexts do not contaminate one another.

## 13.2 P1 tests

- `IComPtr` fake-IUnknown ownership suite or replacement-library migration tests.
- long registry value-name enumeration.
- long window titles.
- package-index freshness/invalidation.
- UIA worker startup failure injection.
- hook cleanup/module-reference lifecycle.

## 13.3 Release artifact checks

For each architecture:

- build succeeds;
- tests run where host-compatible;
- MSI ICE validation passes;
- manifest validates;
- ProductCode and architecture-specific component GUIDs match policy;
- Authenticode signature valid;
- CFG present if enabled;
- expected machine type;
- no accidental `.old`/temporary release payloads;
- no unreviewed dirty-tree/line-ending diffs.

---

# 14. Primary documentation used

## Win32 menu/string APIs

- TrackPopupMenu: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenu
- TrackPopupMenuEx: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenuex
- GetMenuItemInfoW: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmenuiteminfow
- GetWindowTextW: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowtextw
- GetWindowTextLengthW: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowtextlengthw

## Registry

- RegQueryValueExW: https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regqueryvalueexw
- RegGetValueW: https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-reggetvaluew
- RegSetValueExW: https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regsetvalueexw
- RegSetKeyValueW: https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regsetkeyvaluew
- RegEnumValueW: https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regenumvaluew
- Registry element size limits: https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-element-size-limits

## Paths/files

- GetModuleFileNameW: https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulefilenamew
- GetFullPathNameW: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfullpathnamew
- GetLongPathNameW: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getlongpathnamew
- GetShortPathNameW: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getshortpathnamew
- SearchPathW: https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-searchpathw
- GetCurrentDirectoryW: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getcurrentdirectoryw
- Maximum path length limitation: https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation
- CreateFileW: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew

## Manifests

- Application manifests: https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests

## Windows Installer / MSI

- Custom action execution scheduling options: https://learn.microsoft.com/en-us/windows/win32/msi/custom-action-execution-scheduling-options
- Custom action return processing options: https://learn.microsoft.com/en-us/windows/win32/msi/custom-action-return-processing-options
- REMOVE property: https://learn.microsoft.com/en-us/windows/win32/msi/remove
- RemoveExistingProducts: https://learn.microsoft.com/en-us/windows/win32/msi/removeexistingproducts-action
- Upgrade table: https://learn.microsoft.com/en-us/windows/win32/msi/upgrade-table
- Changing the Component Code: https://learn.microsoft.com/en-us/windows/win32/msi/changing-the-component-code
- Component table: https://learn.microsoft.com/en-us/windows/win32/msi/component-table
- Deferred custom-action context: https://learn.microsoft.com/en-us/windows/win32/msi/obtaining-context-information-for-deferred-execution-custom-actions

## WiX v5

- Validation: https://docs.firegiant.com/wix/tools/validation/
- MSBuild: https://docs.firegiant.com/wix/tools/msbuild/
- wix.exe command reference: https://docs.firegiant.com/wix/tools/wixexe/

## MSVC/security

- `/EH` exception handling model: https://learn.microsoft.com/en-us/cpp/build/reference/eh-exception-handling-model?view=msvc-170
- `/guard:cf`: https://learn.microsoft.com/en-us/cpp/build/reference/guard-enable-control-flow-guard?view=msvc-170
- `/GUARD:CF`: https://learn.microsoft.com/en-us/cpp/build/reference/guard-enable-guard-checks?view=msvc-170
- Control Flow Guard: https://learn.microsoft.com/en-us/windows/win32/secbp/control-flow-guard

---

# 15. Findings from the prior audit that were intentionally not promoted

The prior audit listed several “remaining probe work” items. This pass did not promote them without enough evidence:

- third-party host behavior without `TPM_RETURNCMD` — needs host probe;
- `CoCreateInstanceHook` script re-entrancy — plausible but host/script repro needed;
- taskbar subclass `DefSubclassProc` ordering — needs message-order trace;
- UIA `CoWaitForMultipleHandles` reentrancy deadlock — plausible; needs forced reentrant test;
- dead `ComputerName()` failure-path bug — real but low severity and difficult to trigger;
- `tr.nss` line-ending churn — no semantic code defect.

This separation is intentional: a useful audit should not blur confirmed release defects with hypotheses.

---

# 16. Lower-priority latent/dead-code cleanup

These are worth removing or unit-testing but should not delay the release ahead of P0/P1 items.

### `File` ownership/helper issues

`src/shared/System/IO/File.h` contains shallow FILE* copy construction/assignment, so copying an open `File` would cause shared ownership and potential double `fclose`. No active copy use was found in this pass.

The static `File::Copy` / `GetFileEncoding` helpers also contain questionable resource/size behavior and appear unused. Prefer deleting dead helpers to carrying an untested second I/O abstraction.

### `CoPtr<T>`

Shallow copy ownership with destructor `Release`; no active first-party use found. Delete it rather than maintaining two unsafe custom COM pointer types.

### `Path::Long(path, bool)`

As described above, normalization is overwritten; no call site found.

### Hook helper returning stack memory

`Hooker.h:575` should be deleted.

Dead code is not harmless in a low-level Windows project: future callers assume helpers are production-ready because they compile.

---

# 17. Areas that were reviewed and appear sound

Subject to the Windows-runtime limitation, the following areas were specifically checked and did not reveal a new material defect beyond findings above:

- `DllMain` for `shell.dll` is minimal on attach (`HINSTANCE` + `DisableThreadLibraryCalls`) and does not perform heavy initialization under loader lock.
- Dedicated taskbar UIA worker initializes COM as MTA and keeps UI Automation interface usage on that worker.
- Package-index scan state has an exception guard that prevents permanent `Loading` deadlock.
- Recent package API calls correctly use documented two-call sizing for `GetPackagePathByFullName` and both pointer/string buffers for `GetPackagesByPackageFamily`.
- Recent native menu text helper follows the documented `GetMenuItemInfoW` size-first contract.
- Registry read-side NUL hardening is substantially improved.
- Encoding changes referenced by recent history are covered by dedicated tests.
- Bitmap cache, shell-extension capture, native-menu lazy behavior, package parsing/indexing, taskbar cache, menu text and registry have dedicated test suites.
- CI builds release x86, x64 and ARM64 and executes tests on x86/x64.
- Current installer uses architecture-specific ProductCodes.
- Current update/restore deferred custom actions use the expected property/CustomActionData pattern.
- `RemoveExistingProducts` placement is internally consistent with the project's “remove old fully before install new” component-identity strategy.
- `RemoveFile` authoring is targeted rather than blanket deletion.

These clean areas should remain regression-tested while the P0 findings are corrected.

---

# 18. Recommended remediation order

## P0 — before release candidate

1. **F-01 MSI `OnUninstall`:** make synchronous, remove FirstSequence, move after InstallValidate, validate privileges/context.
2. **F-03 manifest:** make well-formed and schema-correct; add embedded-artifact validation.
3. **F-02 version tooling:** stop replacing `var.wxi`; add all-arch generator/build guard.
4. **F-04 registry empty strings:** correct both setters + tests.
5. **F-05 path-size family:** fix `Module`, `Full`, `Long` first; add long-path/length-consistency tests.
6. Re-run complete x86/x64/ARM64 build; x86/x64 tests; MSI ICE validation; clean-VM install/update/uninstall matrix.

## P1 — very strongly recommended for 1.9.20 or immediately following

7. **F-07** replace process-global taskbar state with thread-local/explicit scoped context.
8. **F-08** harden path boundary predicates and namespace validation.
9. **F-10** dynamic registry value-name enumeration.
10. **F-11** remove unsafe directory ACL intent rather than adding BACKUP_SEMANTICS blindly.
11. **F-13/F-14** correct shortcut ComponentId bitness and upgrade-policy comments/error behavior.
12. **F-20** unify installer validation path.

## P2 — stability/hardening/performance program

13. Migrate `/EHa` → scoped SEH + `/EHsc` where feasible.
14. Replace `IComPtr`/remove `CoPtr`.
15. Add package-index freshness generation/TTL.
16. Enable/validate CFG and release signing.
17. Benchmark `/OPT:REF` and WPO.
18. Fix long window titles, worker startup leaks, and hook module-ref leak.
19. Delete dead unsafe helper code.

---

# 19. Final verdict

The coding-agent audit should be **accepted as a useful first pass, but not as release-complete**.

Its retained code changes are mostly correct and valuable, especially the popup re-entrancy fix, long menu-text reader, and typed registry-read sizing. Its installer work also found genuine issues. The primary shortcomings are breadth and final-contract verification:

- one retained fix still violates the registry contract for empty strings;
- the uninstall action has two additional contract failures beyond `FirstSequence`;
- the version tool can erase the newly introduced architecture definitions entirely;
- the embedded manifest is syntactically invalid;
- the path abstraction still contains multiple documented size-contract errors despite long-path intent;
- concurrency and host-stability hardening (`is_in_taskbar`, `/EHa`, custom COM pointer ownership) warrant explicit action.

The most important next step is not another broad speculative scan. It is to fix the **P0 set**, add the missing targeted tests, and run a clean Windows release matrix that exercises Explorer and MSI behavior end-to-end.

**Release status after this audit: NOT READY until P0 findings are resolved and verified.**
