# Nilesoft Shell — Revised P0/P1 Implementation Plan

**Date:** 2026-08-20  
**Repository reviewed:** `Shell`, HEAD `5abef86fb47743557dfa5d3a7b95d5c8745b46ad`  
**Source plan reviewed:** `Implementation_Plan_P0_P1_2026-08-20.md`  
**Purpose:** Replace the agent-authored plan with an implementation-ready plan that preserves its valid findings, corrects unsafe/incorrect implementation advice, adds omissions found in a second codebase sweep, and defines concrete verification gates.

---

## 0. Executive decision

The agent plan is **substantively strong on identification of most P0/P1 defects**, but it should **not be implemented verbatim**. Several proposed fixes are either too broad, internally inconsistent, or would carry existing unsafe behavior into a more privileged context.

The most important corrections are:

1. **Expand F-01 from “fix `OnUninstall` sequencing” into a transactional install/uninstall registration lifecycle.** The current `OnInstall` is itself non-transactional: it changes machine state from an immediate CA *after `InstallFinalize`*, ignores failures, shells out with `runas`, and can leave MSI reporting success while registration failed. Microsoft explicitly requires direct system-state changes to be deferred and paired with rollback actions. The current uninstall CA also has the three defects identified by the agent (`FirstSequence`, asynchronous execution, condition before `InstallValidate`).
2. **Do not invoke `shell.exe -... -restart` from a SYSTEM/no-impersonation custom action.** `Windows::Explorer::Restart()` enumerates and terminates every process named `explorer.exe` it can open. From SYSTEM that creates a cross-session hazard. Registration and Explorer/session notification must be separated.
3. **Do not carry the current ACL fallback into the new registration path.** `src/exe/src/Main.cpp::SetPermissions` can take ownership of the Windows 11 `TreatAs` CLSID key and grant `BUILTIN\Users` `GENERIC_ALL` with inheritance. That is a serious machine-wide security design defect. The existing `Permission::SetFile` helper likewise should not be “fixed” by simply adding `FILE_FLAG_BACKUP_SEMANTICS`, because its intended ACL grants Users broad write access.
4. **Do not implement one universal Win32 “dynamic string” helper for F-05.** The APIs in question have materially different size/truncation contracts. Some support `NULL,0` sizing; some return required size; `GetModuleFileNameW` uses equality/truncation; `IShellLink::GetPath` is explicitly bounded to `MAX_PATH`; `SHLoadIndirectString` does not document a zero-buffer sizing query. Use small helpers grouped by documented contract, plus API-specific handling.
5. **Do not migrate `IComPtr` wholesale to WIL as the first fix.** There is no existing WIL dependency and the custom wrapper has unusual ownership behavior. First make the wrapper correct and explicit (`attach`, `detach`, `put`, `release_and_get_address_of`), migrate call sites mechanically, and pin ownership with fake-`IUnknown` refcount tests. A later WIL/WRL migration can be independent.
6. **For F-06, switch whole modules uniformly to `/EHsc` first; do not mass-rewrite 50+ `catch(...)` blocks in the same change, and do not mix per-TU `/EHa` and `/EHsc`.** Microsoft explicitly warns against mixing these models in one module. Under `/EHsc`, `catch(...)` no longer swallows SEH; the immediate security/stability problem is removed without a broad semantic rewrite.
7. **For F-12, do not use `MrtCache` last-write time as the package identity invalidation source.** Package identity comes from the AppModel repository package key, whereas `MrtCache` is a display-resource fallback. Use a TTL/generation mechanism first; a future registry notification should watch the actual package repository source.
8. **Split stable WiX identity data from generated version data.** Merely regex-patching `var.wxi` is better than overwriting it, but still couples stable GUID policy to a mutation script. Generate a version-only include and keep architecture/Product/Component GUIDs in a stable include that the version tool never touches.
9. **Fix both manifest copies.** `version.ps1` overwrites the runtime manifest from `src/tools/version/manifest.xml`, and that template is malformed too. Fixing only `src/shared/Resource/manifest.xml` is not durable.
10. **Add one P0-adjacent security prerequisite to F-01/F-11:** registration under MSI must use explicit machine registry scope and must not permanently widen ACLs on protected registry keys.

**Ship recommendation:** no release candidate should be cut until the P0 phase and the P0-adjacent registration/ACL prerequisite are complete, all three MSI architectures validate, and the clean-VM install/uninstall/upgrade/rollback matrix passes.

---

## 1. Baseline, scope, and change-control rules

### 1.1 Repository state used for this plan

The reviewed tree is HEAD `5abef86fb47743557dfa5d3a7b95d5c8745b46ad` plus the already-retained substantive fixes from the prior audit. The working tree contains unrelated dirty files, including line-ending/format-only changes. Therefore:

- implementation work must be based on **HEAD + reviewed substantive diff**, not “all current dirty files”;
- each fix should be committed or staged independently enough to inspect;
- before and after every phase, run `git diff --check`, `git diff --stat`, and `git ls-files --eol` and reject unrelated line-ending normalization;
- do not rewrite generated/imported/localization files unless the finding requires it.

### 1.2 Existing retained fixes that remain valid

Keep the previously retained fixes:

- `src/dll/src/Main.cpp` — avoid duplicate menu build through hooked `TrackPopupMenu` re-entry;
- `src/dll/src/ContextMenu.cpp` — dynamic menu-text read;
- `src/dll/src/Expression/FuncExpression.cpp` — explicit-type `reg.get` sizing;
- `src/shared/System/Windows/Registry.cpp` — non-empty `REG_SZ`/`REG_EXPAND_SZ` terminator fix;
- associated registry/menu tests.

F-04 below extends the registry writer fix to empty strings.

### 1.3 Implementation rules

For every P0/P1 change:

- **write the failing test/probe first where practical**;
- prefer standard Windows Installer actions/resources over custom actions when MSI can own the resource;
- keep machine-state mutation separate from interactive/session behavior;
- no “best effort” masking of correctness-critical machine registration; if a required registration step fails, the install must fail and rollback;
- every new buffer wrapper must document the exact Win32 return-value contract it implements;
- no new third-party dependency solely to fix a local ownership bug unless the migration itself has a separate acceptance plan;
- no global security relaxation to make an operation succeed;
- use actual return values from second/final API calls as logical string lengths; allocation capacity is never string length;
- keep all architecture-specific MSI/registry-view behavior explicit and tested.

---

# Phase A — Build/identity safety first

These items should land before larger P0 changes because they prevent tools/build paths from silently undoing fixes.

---

## 2. P0 — F-02: Version tooling must not overwrite stable WiX identity

### 2.1 Confirmed code problem

`src/tools/version/version.ps1:117-121` currently copies generated templates over live sources, including:

```powershell
update "manifest.xml" "..\..\shared\Resource\manifest.xml";
update "var.wxi" "..\..\setup\wix\var.wxi";
```

`src/tools/version/var.wxi` contains only `Version` and one `ProductCode`, while `src/setup/wix/var.wxi` contains:

- three architecture-specific `ProductCode`s;
- `ComponentBitness`;
- seven per-architecture component GUIDs;
- architecture-policy comments.

Running the version tool therefore destroys the current installer identity model and, with the present tree, is expected to leave later WiX references undefined.

### 2.2 Revised implementation — split generated and stable data

**Do not use the agent plan’s regex-in-place patch as the final design.** Implement a source-of-truth split:

1. Create `src/setup/wix/version.wxi` containing **only generated version information**:

   ```xml
   <Include xmlns='http://wixtoolset.org/schemas/v4/wxs'>
     <?define Version = '1.9.20' ?>
   </Include>
   ```

2. Rename/refactor the current stable `src/setup/wix/var.wxi` into something explicit such as `identity.wxi` (or retain the filename but remove `Version` from it). It must own only:
   - per-arch `ProductCode`;
   - `ComponentBitness`;
   - all per-arch component GUIDs;
   - F-13 `StartMenuShortcutComponentGuid` after that fix lands.
3. Update `setup.wxs` to include both files.
4. Replace `src/tools/version/var.wxi` with a **version-only template**, preferably `version.wxi`, and change `version.ps1` so it writes only `src/setup/wix/version.wxi`.
5. Do not use generic `Get-Content | Set-Content` as a patcher for stable XML/WiX source. The existing `update` template mechanism can generate the version-only file, provided encoding/EOL is deterministic.
6. Add a script-level invariant: the version tool must never write `identity.wxi`.

### 2.3 Also fix generator ownership of the manifest

`version.ps1` also overwrites `src/shared/Resource/manifest.xml` from `src/tools/version/manifest.xml`. That template is malformed in exactly the same way as the destination. Therefore F-03 must update **both files**, and the version-tool integration test must run before manifest validation.

### 2.4 Required tests/gates

Create a temp-worktree or clean-copy integration test:

1. Hash `identity.wxi` and all architecture GUID definitions.
2. Run the version tool with a synthetic version.
3. Assert the stable identity file hash is unchanged.
4. Assert only intended version-bearing files changed.
5. Build x86, x64, ARM64 setup packages.
6. Inspect MSI `Property`/`Component` tables and assert:
   - ProductCodes remain distinct per architecture;
   - component GUID sets remain distinct where required;
   - no undefined `$(var.*Guid)` references;
   - Start Menu GUIDs follow F-13 once implemented.
7. Run `git diff --check` and EOL checks after the tool.

### 2.5 Definition of done

- Version tool cannot alter stable MSI identity.
- Running it cannot reintroduce the malformed manifest.
- All three MSI packages build and validate after version generation.
- CI contains a regression check that would catch future whole-file overwrite.

---

## 3. P1 infrastructure brought forward — F-20: Make one validated WiX build path authoritative

This is P1 by severity but should be implemented **early** because every installer change depends on reliable validation.

### 3.1 Confirmed drift

- `src/setup/wix/setup.wixproj` uses `WixToolset.Sdk/5.0.2`.
- Root/MSBuild builds automatically run Windows Installer validation.
- `src/setup/build.cmd` invokes `wix.exe build` directly and then deletes the `.wixpdb`; it does **not** run `wix msi validate`.

FireGiant documents that MSBuild runs validation automatically, while direct `wix.exe` builds require `wix msi validate`.

### 3.2 Revised implementation

Preferred:

1. Declare root `build.ps1`/solution MSBuild as the **only release build path**.
2. Replace `src/setup/build.cmd` with a thin wrapper around the canonical PowerShell/MSBuild path, or remove/deprecate it.
3. If direct CLI support is retained for developer convenience:
   - emit `.wixpdb`;
   - run `wix msi validate <msi>`;
   - preserve the PDB until validation and diagnostics finish;
   - return non-zero on validation failure.
4. Do not globally set `SuppressValidation=true` just to accommodate a hosted CI account. FireGiant notes that stock ICEs may fail under non-interactive/non-admin hosted agents; if that occurs, perform validation in a self-hosted/admin validation stage rather than disabling the release gate.

### 3.3 Definition of done

Every path advertised as producing a release MSI either goes through MSBuild validation or explicitly runs `wix msi validate`; there is no unvalidated “alternate release” path.

---

# Phase B — P0 source/runtime correctness

---

## 4. P0 — F-03: Repair the manifest at both source and generator layers

### 4.1 Confirmed defects

Both:

- `src/shared/Resource/manifest.xml`
- `src/tools/version/manifest.xml`

use `asmv3:` and `ws2:` without binding those prefixes and place `activeCodePage` under the inherited 2005 WindowsSettings namespace. The source is not well-formed XML; the built executable inspected in the audit contained the same raw malformed fragment.

### 4.2 Revised implementation

Rebuild the WindowsSettings portion from Microsoft’s manifest examples rather than patching only the missing prefix:

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1"
          xmlns:asmv3="urn:schemas-microsoft-com:asm.v3"
          manifestVersion="1.0">
  ...
  <asmv3:application>
    <asmv3:windowsSettings xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">
      <dpiAware>True/PM</dpiAware>
    </asmv3:windowsSettings>

    <asmv3:windowsSettings xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">
      <dpiAwareness>PerMonitorV2,PerMonitor</dpiAwareness>
    </asmv3:windowsSettings>

    <asmv3:windowsSettings xmlns:ws2="http://schemas.microsoft.com/SMI/2016/WindowsSettings">
      <ws2:longPathAware>true</ws2:longPathAware>
    </asmv3:windowsSettings>

    <asmv3:windowsSettings xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">
      <activeCodePage>UTF-8</activeCodePage>
    </asmv3:windowsSettings>
  </asmv3:application>
</assembly>
```

Notes:

- preserve existing Common Controls dependency, `trustInfo`, and compatibility declarations;
- use the same semantic structure in the generator template, with only version tokens differing;
- follow Microsoft namespace examples exactly unless `mt.exe` validation demonstrates a different schema requirement;
- do not treat `longPathAware` as a substitute for fixing F-05: long-path behavior also depends on OS policy and each API’s contract.

### 4.3 Required validation

1. Run the version tool first.
2. Parse both manifest source/template with a strict XML parser.
3. Run `mt.exe -manifest ... -validate_manifest` on the generated source manifest.
4. Build all architectures.
5. Extract embedded manifests from every PE that actually embeds this resource (at minimum `shell.exe` and `shell.dll` for x86/x64/ARM64) using `mt.exe -inputresource` and validate the extracted content.
6. Assert the extracted manifest contains the expected 2005/2016/2019 settings.

### 4.4 Definition of done

No malformed manifest can be produced by either the source tree or the version generator, and CI validates the embedded artifact rather than only the source XML.

---

## 5. P0 — F-04: Correct empty registry string writes

### 5.1 Confirmed defect

The retained fix in `src/shared/System/Windows/Registry.cpp` adds a terminator only when:

```cpp
value && *value && length > 0
```

For a non-null empty string `L""`, `length == 0`; the writer passes `cbData == 0` despite writing `REG_SZ`/`REG_EXPAND_SZ`. Microsoft requires string data to be null-terminated and requires `cbData` to include the terminator. `lpData == nullptr, cbData == 0` is a separate, valid “null data” case and must not be conflated with an empty string.

### 5.2 Revised implementation

Apply identical semantics to both:

- `RegistryKey::SetString(...)`;
- static `Registry::SetKeyValue(... const wchar_t* value, size_t length, bool expand)`.

Contract:

- `length` means **characters excluding the terminator**;
- `value == nullptr` → `lpData=nullptr, cbData=0` if null-data semantics are intentionally supported;
- `value != nullptr`, including `L""` → `(length + 1) * sizeof(wchar_t)` bytes;
- reject arithmetic overflow before narrowing to `DWORD`.

Suggested shape:

```cpp
if(!value)
    return SetValue(name, type, nullptr, 0);

constexpr size_t maxChars = (std::numeric_limits<DWORD>::max() / sizeof(wchar_t)) - 1;
if(length > maxChars)
    return false;

DWORD cb = static_cast<DWORD>((length + 1) * sizeof(wchar_t));
return SetValue(name, type,
                reinterpret_cast<const BYTE*>(value), cb);
```

Use the project’s actual const-correct `SetValue` signature; do not introduce a cast merely to match the sketch.

### 5.3 Tests

Extend `test_registry.cpp` with raw `RegQueryValueExW` verification for:

- empty `REG_SZ` via `RegistryKey::SetString`;
- empty `REG_EXPAND_SZ`;
- empty via static `Registry::SetKeyValue`;
- empty via script `reg.set(key, name, "", reg.sz)` path (`FuncExpression.cpp:5334-5335`);
- non-empty regression case;
- null pointer behavior, if retained as public semantics;
- overflow guard through a helper-level unit test without allocating a multi-gigabyte string.

For an empty wide string assert raw byte count equals `sizeof(wchar_t)` and first wchar is NUL.

### 5.4 Definition of done

All string writers emit contract-valid string values for empty and non-empty input, and all tests inspect raw bytes rather than relying on forgiving project readers.

---

## 6. P0 — F-05: Repair path/buffer contracts with API-specific strategies

### 6.1 Why the original plan must change

The source plan proposes one `DynamicString` abstraction and includes APIs that do not share a sizing protocol. That is unsafe. Group APIs by **documented return contract**, not by superficial “writes a string” similarity.

### 6.2 Confirmed defects to fix now

#### F-05a — `GetModuleFileNameW`

- `src/shared/System/IO/Path.h:1063-1072`
- `src/shared/System.h:1509-1517`

Current `Path::Module` checks `len > MAX_PATH`. Microsoft documents truncation as `return nSize`; `> nSize` cannot be the truncation signal. `System::Module::path()` never retries at all.

#### F-05b — `GetFullPathNameW`

`Path::Full` preflights a required size that includes NUL when the buffer is too small, then stores that **capacity** as the logical string length. Successful second call returns actual length excluding NUL. Release the second call’s return value, not the preflight size.

#### F-05c — `GetLongPathNameW`

Same logical-length defect as Full: preflight returns required buffer size including NUL; successful fill returns written characters excluding NUL.

#### F-05d — fixed-size wrappers where the API supports resizing

Review and fix at least:

- `Path::Search` (`SearchPathW`);
- `Path::CurrentDirectory` (`GetCurrentDirectoryW`);
- `Path::TempDirectory` (`GetTempPathW` / consider `GetTempPath2W` only if minimum OS policy permits; do not silently raise OS requirement);
- `Path::Short` (`GetShortPathNameW`);
- sibling SearchPath/module wrappers in `src/shared/System/Diagnostics/Shell.h` and related headers.

#### F-05e — dead/broken normalization helper

`Path::Long(wstring_view,bool)` uses `_MAX_PATH`-era logic and overwrites a normalized path with the original `sPath` later in the function. No first-party call sites were found in the audit. Prefer deleting it after a complete reference search rather than spending risk on an unused duplicate implementation.

### 6.3 Required helper model

Use **small contract-specific helpers**, for example:

#### Helper A — preflight returns required capacity; fill returns actual length

Use only for APIs whose documentation explicitly supports `NULL/0` sizing, such as `GetFullPathNameW`, `GetLongPathNameW`, `GetShortPathNameW`.

Invariant:

1. preflight `req` is capacity, normally including NUL;
2. allocate exactly/at least `req`;
3. fill call returns `written` excluding NUL on success;
4. if fill says buffer still insufficient, resize/retry (protect against races or changing environment);
5. `release(written)`.

Do not infer the contract from another API.

#### Helper B — grow on truncation/equality

For `GetModuleFileNameW`:

```cpp
DWORD cap = 260;
for(;;) {
    ensure_capacity(cap);
    SetLastError(ERROR_SUCCESS);
    DWORD n = GetModuleFileNameW(h, buf, cap);
    if(n == 0) return {};
    if(n < cap) return release(n);

    // n == cap: documented truncation on supported Windows.
    if(cap > maxCap / 2) return failure;
    cap *= 2;
}
```

Do not require `GetLastError()==ERROR_INSUFFICIENT_BUFFER` as the only retry condition; older Windows semantics historically differed. `n == cap` is sufficient to retry for the supported API contract.

#### Helper C — API-specific “required size when buffer too small” loop

Use for `SearchPathW`, `GetCurrentDirectoryW`, and `GetTempPathW` according to each function’s own documentation. Keep the contract in the helper name/comment.

### 6.4 Explicit non-members of the generic helper

Do **not** implement these via a claimed `NULL,0` universal sizing call:

- `IShellLink::GetPath`: Microsoft documents a maximum returned path of `MAX_PATH`. If long-target support is functionally required, use the item-ID-list path (`IShellLink::GetIDList` + an appropriate PIDL-to-path API such as `SHGetPathFromIDListEx`) or document the shell-link limitation.
- `IShellLink::GetWorkingDirectory` and `GetIconLocation`: their interfaces take caller-sized buffers and may truncate; no generic preflight protocol is documented.
- `SHLoadIndirectString`: do not call with a null/zero output buffer unless the API documentation explicitly supports it. If current 260/other bounds are functionally inadequate, use a documented bounded retry strategy or redesign around the underlying resource API.
- `OPENFILENAME` dialog buffers: `nMaxFile` is caller-managed state, not a Win32 required-size return contract. Increase the caller allocation separately if long selected paths are required.

### 6.5 Additional related bounds issues to include in this change

- Search for every `GetModuleFileNameW`, `SearchPathW`, `GetFullPathNameW`, `GetLongPathNameW`, `GetShortPathNameW`, `GetCurrentDirectoryW`, `GetTempPathW` call across the tree and classify it by contract.
- Audit wrappers that do `if(ret > capacity)` where an API can return `== capacity` on truncation.
- Preserve `wcslen(result.c_str()) == result.length()` as a test invariant for every path-string wrapper.

### 6.6 Tests

Add `src/tests/test_path.cpp` (or equivalent) with two layers:

**Contract helper unit tests** using injected/fake callbacks:

- required-size then success;
- result changes between sizing and fill and requires retry;
- exact-capacity boundary;
- zero/failure;
- growth overflow/cap protection;
- verify logical length always comes from final success count.

**Windows integration tests:**

- `Full` relative and absolute paths;
- `Long`/`Short` where 8.3 names are available; skip with explicit reason if disabled;
- `CurrentDirectory` in a >260-character directory;
- `SearchPath` at/over old buffer boundary;
- `Module` by launching/copying a test executable from a deep path if host policy permits;
- all results assert content equality and `wcslen == length`;
- failure paths (nonexistent target, inaccessible path, path-policy limitation) are intentional and documented.

Avoid multithreaded tests that mutate process-wide current directory concurrently.

### 6.7 Definition of done

Every touched API has a comment/test matching its own Microsoft return-value contract; no helper pretends unrelated APIs have identical sizing semantics; no path wrapper records allocation capacity as string length.

---

# Phase C — P0 installer/registration redesign

---

## 7. P0 — F-01 expanded: transactional machine registration lifecycle

### 7.1 Confirmed current defects

The original plan correctly finds three defects in `OnUninstall`, but the install side must be included.

#### Existing uninstall defects

`setup.wxs:265`:

```xml
<CustomAction Id='OnUninstall'
              Execute='firstSequence'
              Return='asyncWait'
              BinaryRef='CA'
              DllEntry='Uninstall' />
```

and line 269 schedules it after `FindRelatedProducts` using `REMOVE="ALL"`.

Confirmed:

- `FirstSequence` skips the execute-sequence action if a UI sequence ran;
- `asyncWait` lets MSI proceed concurrently and only waits later;
- Microsoft states an action depending on `REMOVE=ALL` must be after `InstallValidate` because the property may not be finalized before then.

#### Additional P0 defect omitted by the source plan — `OnInstall`

`setup.wxs:264,284` authors an **immediate** custom action and schedules it **after `InstallFinalize`**:

```xml
<CustomAction Id='OnInstall' Return='ignore' ... />
...
<Custom Action='OnInstall' After='InstallFinalize' ... />
```

`src/setup/ca/dllmain.cpp:537-549` shells out to:

```text
shell.exe -r -s -t -restart
```

with `runas`, waits for it, ignores the actual registration result, and returns `ERROR_SUCCESS`.

Microsoft’s Windows Installer guidance is explicit: a custom action that changes system state must be deferred; every such forward action requires a rollback action. Placing the state change after `InstallFinalize` makes it outside the transactional script entirely.

### 7.2 Additional execution hazard — Explorer restart from privileged CA

`shell.exe` registration calls `Windows::Explorer::Restart()` when `-restart` is set. `Windows.h:390-425` enumerates all processes named `explorer.exe` and terminates every one it can open; the 32-on-64 path invokes `taskkill /f /im explorer.exe`.

If registration is correctly moved into a no-impersonation/SYSTEM custom action, preserving `-restart` would expand the process’s ability to terminate Explorer instances across sessions. **Never restart Explorer from the SYSTEM CA.**

### 7.3 Preferred implementation architecture

The safest release architecture is to reduce custom machine-state work, not merely resequence it.

#### 7.3.1 Move MSI-owned normal registry registration into WiX where practical

Most registration performed by `RegistryConfig::Register` is declarative registry state:

- product COM CLSID/InprocServer32/ThreadingModel;
- Shell Extensions `Approved` value;
- eight context-menu-handler keys (`*`, `Directory`, `Drive`, `Folder`, `Directory\Background`, `DesktopBackground`, `LibraryFolder`, `LibraryFolder\Background`);
- icon overlay registration;
- `.nss` association values.

For the MSI install path, author these as WiX `RegistryKey`/`RegistryValue` resources in components. Benefits:

- Windows Installer owns install/remove/repair/rollback automatically;
- no elevated shell-out is needed for normal registration;
- registry resources follow component bitness and machine install semantics;
- rollback fidelity is substantially better than manually deleting/recreating keys.

**Do not set `ForceDeleteOnUninstall=yes` on shared keys just to emulate recursive delete.** MSI already removes values it creates; force-deleting a shared key can remove third-party/user data. Use force-delete only for keys that are provably product-private and may acquire product-created children.

If this declarative migration is too large for the release branch, the fallback is a deferred/no-impersonation CA with rollback, described in §7.4, but the declarative path is preferred.

#### 7.3.2 Keep standalone/manual `shell.exe` registration as a separate code path

`shell.exe -r/-u` may remain for manual/portable scenarios, but MSI should not need to launch it for ordinary product-owned registry resources. Extract common **data/constants** where feasible and add tests that compare MSI-authored registration against `RegistryConfig` expectations to prevent drift.

#### 7.3.3 Isolate Windows 11 `TreatAs`

`TreatAs` touches a protected/shared Windows CLSID and is qualitatively different from product-private registration. Handle it as a narrowly scoped operation with explicit ownership checks and rollback/restore semantics; do not bundle it with generic COM registration.

### 7.4 If a custom action remains necessary

For any machine state that cannot be represented with standard MSI registry resources:

1. Use a Binary-table DLL CA (`CA`) with `Execute='deferred'`, `Impersonate='no'`, `Return='check'`.
2. Schedule it **after `InstallInitialize` and before `InstallFinalize`**.
3. Pass all required data through `CustomActionData` using an immediate Type-51/property action.
4. Schedule a matching `Execute='rollback'`, `Impersonate='no'`, synchronous action **immediately before** the forward deferred action.
5. Forward/rollback functions must be idempotent and return failure for correctness-critical changes.
6. Do not use `asyncWait`, `asyncNoWait`, `FirstSequence`, `runas`, UI, or Explorer restart.
7. Do not rely on normal MSI session properties from a deferred action except the limited deferred API set.

### 7.5 Sequencing requirements

#### Install

- Data-prep immediate action: after costing/path resolution and before its deferred/rollback actions.
- If normal registry resources are WiX-authored, let standard registry actions handle them.
- Any remaining deferred registration requiring installed file paths must run **after `InstallFiles`** and before `InstallFinalize`.
- Its rollback action is sequenced immediately before the forward action.

#### Uninstall

- Condition requiring `REMOVE="ALL"` must be evaluated after `InstallValidate`.
- Any remaining custom unregister action that needs installed binaries/data must execute **before `RemoveFiles`**.
- It must be synchronous.
- Rollback must be able to restore the exact state needed if a later uninstall action fails.

#### Major upgrade

The old-product removal half and new-product install half must be tested together with current `RemoveExistingProducts` sequencing. Preserve the current explicit exclusion of “real uninstall-only cleanup” during `UPGRADINGPRODUCTCODE` where appropriate.

### 7.6 Failure policy

Resolve the source plan’s contradiction between `Return='ignore'` and “fail closed”:

- **required machine registration/unregistration:** `Return='check'`; action returns `ERROR_INSTALL_FAILURE` on a required failed mutation;
- “already absent” during idempotent unregister is success;
- best-effort cleanup such as pruning stale rotated files or deleting a temporary backup can be separately non-fatal, but must not mask failure to establish core registration;
- log enough context for MSI diagnostics, without sensitive data.

### 7.7 Do not convert directly to a Type-18 `shell.exe` CA without fixing exit semantics

The executable’s current command path ultimately returns a C++ `bool` from registration. A true result may therefore become process exit code `1`, whereas Windows Installer EXE custom actions treat process exit code `0` as success. Do not simply swap the DLL CA for a checked EXE CA. Prefer no shell-out; if an EXE is ever used, define a conventional `0=success` process exit contract first.

### 7.8 Registry root/view requirements

For machine registration code, avoid relying on the merged `HKEY_CLASSES_ROOT` view from a service/SYSTEM context. Microsoft documents HKCR as a merged compatibility view and recommends per-machine COM registration under `HKLM\Software\Classes`.

Requirements:

- MSI-authored HKCR resources in a per-machine package are acceptable because Windows Installer maps them to per-machine classes, but custom SYSTEM code should use explicit machine keys;
- centralize the intended registry view per architecture (`always32` vs `always64`) and test it;
- preserve the special 64-bit view behavior required for the Windows 11 Explorer `TreatAs` key;
- never let a SYSTEM custom action accidentally operate on a user-specific merged HKCR view.

### 7.9 P0-adjacent security prerequisite — remove unsafe ACL behavior from registration

Before moving registration into SYSTEM, fix §13 (F-11) at least for the code paths touched by registration:

- no `BUILTIN\Users GENERIC_ALL` on the protected CLSID/TreatAs key;
- no broad write ACL on the install directory;
- no permanent ownership/DACL widening as a “retry” strategy.

If `TreatAs` genuinely cannot be written under normal SYSTEM/elevated rights:

1. first prove the exact key/DACL behavior on supported Windows versions;
2. prefer a supported non-ACL-bypass design;
3. if the product absolutely requires an ACL-assisted mutation, snapshot owner+DACL, grant only the minimum right to the exact principal/key for the shortest possible duration, perform the one mutation, restore owner+DACL immediately, and make rollback restore the pre-action value/ACL. This path requires a dedicated security review and VM proof. Do not grant Users write access.

### 7.10 Clean-VM acceptance matrix

For x86/x64/ARM64 where hardware/VM support exists, capture verbose MSI logs and pre/post registry/ACL snapshots for:

- fresh install: full UI, `/qb`, `/qn`;
- ARP uninstall/full UI;
- `/x` `/qb` and `/qn`;
- feature-absent/full-remove path;
- repair;
- older same-architecture major upgrade;
- older cross-architecture upgrade allowed by current policy;
- same-version cross-architecture attempt (expected policy outcome from F-14);
- rollback fault injection immediately before and after machine registration;
- rollback fault injection during uninstall after unregister but before completion;
- cancellation at representative points;
- multi-session host: verify installer never terminates another user’s Explorer.

Assert:

- MSI result matches registration result;
- rollback restores registry and files to pre-operation state;
- no orphan `InprocServer32`, context handler, Approved, icon overlay, or product-owned TreatAs remains after real uninstall;
- upgrade does not perform real-uninstall-only cleanup;
- Explorer is not force-terminated by the SYSTEM custom action;
- no machine ACL is broadened to Users.

### 7.11 Definition of done

There is no immediate post-finalize machine-registration CA; no asynchronous uninstall CA; no FirstSequence dependency; normal MSI registry state is transactional; any remaining custom state is deferred/no-impersonation + rollback; and no privileged action performs interactive Explorer restart or broad ACL relaxation.

---

# Phase D — P1 correctness/stability fixes

---

## 8. P1 — F-07: Replace process-global taskbar menu state with scoped thread state

### 8.1 Confirmed defects

`src/dll/src/Main.cpp` has namespace-global:

```cpp
bool is_in_taskbar;
```

It is written in `ShowTaskbarContextMenu`, read in the popup hook, and reset unconditionally in `NtUserTrackPopupMenu`’s `__finally`. This creates:

- a C++ data race across threads;
- cross-thread semantic contamination;
- bad nesting/re-entrancy semantics;
- an additional stale-state case: `ShowTaskbarContextMenu` sets the flag even on the branch that directly invokes the saved native target and bypasses `NtUserTrackPopupMenu`, so the hook’s `finally` may never clear it.

### 8.2 Revised implementation

Use `thread_local` scoped state as the minimum-risk fix:

```cpp
thread_local bool t_is_in_taskbar = false;

class ScopedTaskbarOrigin {
    bool previous_;
public:
    explicit ScopedTaskbarOrigin(bool value) noexcept
      : previous_(t_is_in_taskbar) { t_is_in_taskbar = value; }
    ~ScopedTaskbarOrigin() noexcept { t_is_in_taskbar = previous_; }
};
```

- establish the scope in `ShowTaskbarContextMenu` around the operation whose hook should observe taskbar origin;
- replace `Initializer::OnState(is_in_taskbar)` with the TLS value;
- remove the unconditional global reset in `NtUserTrackPopupMenu::__finally`;
- nested/reentrant calls restore prior value;
- both branches of `ShowTaskbarContextMenu` must leave state exactly as they found it.

A later refactor may pass an explicit popup-origin context object, but TLS is lower blast radius for this hook architecture.

### 8.3 Tests

- two threads set opposite taskbar-origin scopes and assert isolation;
- nested scope true→false→restore true;
- direct-saved-target branch does not leak state;
- exception/SEH escape still restores state where C++ RAII is applicable; keep scope outside raw SEH region if C2712 would otherwise apply.

---

## 9. P1 — F-08: Fix unsafe indexing and define path predicate semantics

### 9.1 Confirmed issues

At minimum:

- `Path::GetRoot` indexes `[1]`/`[2]` without proving length >= 3;
- `Path::IsCLSID` checks `if(length >= 40) return false`, rejecting the canonical `::{GUID}` length, indexes `[0..2]` on short inputs, and accepts invalid short prefixes;
- script `path.wsl` guards only non-empty then indexes `[1]`;
- `Path::EndsWithSlash` uses `.back()` without first checking empty input (missed by the source plan).

### 9.2 Revised implementation

1. `EndsWithSlash`: return false on empty before `.back()`.
2. `GetRoot`: require at least 3 characters before `[1]/[2]`; validate drive prefix according to project policy. Accept both slash directions only if callers are expected to normalize both.
3. Define `IsCLSID` narrowly:
   - exact shell namespace token `::` + canonical braced GUID;
   - validate minimum/exact length before indexing;
   - call `CLSIDFromString`/`IIDFromString` on the GUID part rather than checking punctuation only.
4. If callers also need “starts with shell namespace CLSID then has a child path,” introduce a **different** predicate; do not make `IsCLSID` ambiguously accept both exact and prefix forms.
5. `path.wsl`: prove `size >= 2` and intended drive-letter form before reading `[1]`.
6. Audit pointer-based helpers such as `IsDrivePrefix(const wchar_t*)`; either encode their minimum-length precondition in the API/name or add safe length overloads.

### 9.3 Tests

Boundary table: `""`, `"X"`, `"C:"`, `"C:\\"`, `"::"`, `"::{"`, malformed GUIDs, exact canonical `::{00000000-0000-0000-0000-000000000000}`, valid CLSID plus child, UNC, WSL-like/non-drive inputs.

Run under ASan where the Windows toolchain configuration supports it, or use guard-page/fuzz boundary tests if not.

---

## 10. P1 — F-09: Repair `IComPtr<T>` ownership in place before considering WIL/WRL

### 10.1 Confirmed ownership defects

`src/shared/System.h:1776-2025` currently has:

- defaulted move constructor → memberwise pointer copy; moved-from object still releases the same COM pointer;
- `release()` calls `Release()` but does not null the member;
- output conversions/reset call `release()` then return the still-dangling address; if the COM call fails without overwriting, later destruction double-releases;
- copy assignment calls a `swap` that takes non-const reference and is not a valid copy-and-swap implementation;
- `CreateInstance()` overwrites the member without releasing current ownership;
- raw-pointer constructor AddRefs while raw-pointer assignment appears to attach without AddRef — inconsistent ownership semantics;
- implicit `operator Q**` / `void**` makes misuse easy.

The lone `set_release(false)` occurrence identified in the prior audit is in commented code, so the `_release` ownership toggle appears removable after a full reference search.

### 10.2 Revised implementation sequence

**Do not add WIL as the first repair.** First make the local type correct and explicit.

1. Add a fake `IUnknown` test object with atomic reference count and destruction counter.
2. Define unambiguous operations:
   - copy constructor/assignment: AddRef;
   - move constructor/assignment: steal and null source;
   - `reset()`: Release then set null;
   - `attach(T*)`: take existing reference without AddRef;
   - `detach()`: return pointer and clear without Release;
   - `get()`;
   - `get_address_of()` only for a known-null member;
   - `release_and_get_address_of()` / `put()`: reset first then return address;
   - `swap`: swap full ownership state, preferably only the pointer after `_release` is removed.
3. `CreateInstance()` must call `put()`/`release_and_get_address_of()` before `CoCreateInstance`.
4. Replace implicit output-pointer conversions at live call sites mechanically with explicit `put()` or `get_address_of()` depending the COM contract.
5. Remove implicit `operator void**`/`operator Q**` after migration, or mark deprecated during a one-commit transition.
6. Remove ambiguous raw-pointer assignment; use `attach` or an explicit `copy_from`/constructor so ownership is visible at call sites.
7. Remove `_release`/`set_release` if full reference scan proves no active non-owning usage.
8. Delete dead `CoPtr<T>` only after confirming no live call sites; do not combine unrelated ownership refactors if it complicates review.

### 10.3 Why WIL/WRL is deferred

`wil::com_ptr` or `Microsoft::WRL::ComPtr` is a reasonable future standardization, but adding WIL introduces a dependency and converting ~71 uses expands the change surface. Once tests exist and call sites use explicit COM ownership vocabulary, migration is straightforward and can be evaluated separately.

### 10.4 Tests

Pin exact AddRef/Release counts for:

- copy construction;
- copy assignment/self-assignment;
- move construction/assignment;
- `reset`;
- `attach`/`detach`;
- `put()` success and COM-call failure leaving null;
- reusing a non-null pointer for `CreateInstance`;
- QueryInterface success/failure;
- destruction exactly once.

---

## 11. P1 — F-10: Make `reg.keys`/`reg.values` enumeration size-safe and mutation-tolerant

### 11.1 Confirmed defect

`FuncExpression.cpp:5356-5368` allocates 260 characters and loops only while `retCode == ERROR_SUCCESS`. For a registry value name longer than the buffer, `RegEnumValueW` returns `ERROR_MORE_DATA`; the loop exits and hides that value **and every later value**. Microsoft’s registry examples use a 16,383-character maximum value-name size; subkey names have a much smaller documented maximum.

### 11.2 Revised implementation

1. Open the key.
2. Call `RegQueryInfoKeyW` once to obtain:
   - subkey count / max subkey-name length;
   - value count / max value-name length.
3. Allocate the relevant max + terminator instead of hard-coding 260.
4. Enumerate until `ERROR_NO_MORE_ITEMS`.
5. Reset `name_length` to full buffer capacity for every index.
6. If `ERROR_MORE_DATA` occurs because the key changed between the info query and enumeration:
   - grow/re-query;
   - retry the **same index**;
   - do not advance or terminate silently.
7. On other errors, decide/document expression semantics (return partial list vs failure) and log diagnostic information rather than silently pretending enumeration completed.
8. Preserve current ordering semantics unless a separate behavior change is intended.

### 11.3 Tests

- 400-character value name;
- near-maximum supported value name where filesystem/test environment permits;
- ensure a value following the oversized name is still returned;
- concurrent mutation test that forces/resembles `ERROR_MORE_DATA` and confirms same-index retry;
- max-length subkey boundary.

---

## 12. P1 — F-12: Add bounded package-index freshness without rescanning every menu

### 12.1 Confirmed design problem

`PackageIndex::ensure_index()` returns immediately forever once `_state == Ready`. It correctly coalesces concurrent initial scans and resets transient failures, but an Explorer process can outlive package install/uninstall events, so the index becomes permanently stale.

### 12.2 Correction to the source plan

Do **not** use `MrtCache` last-write time as the primary generation source. The index identities are enumerated from the AppModel package repository; `MrtCache` is used for display-resource lookup/fallback. Those stores need not update identically.

### 12.3 Revised P1 implementation — TTL + generation-safe invalidation

Use a cheap TTL first (for example 30–60 seconds; make it a named constant and benchmark):

State additions:

- `_ready_at_ms` (`GetTickCount64` or injectable monotonic clock);
- `_generation` monotonically incremented by `invalidate()`;
- scanner captures generation before it unlocks.

`ensure_index()` behavior:

1. under mutex, if `Ready` and age < TTL → return true;
2. if expired → transition to Empty/Loading using existing coalescing;
3. capture current generation;
4. enumerate outside mutex;
5. on publish, if generation changed while scanning, **do not publish stale list**; set Empty and allow a new scan;
6. otherwise publish list and timestamp;
7. keep current exception guard/condition-variable notification behavior.

Add public/internal `invalidate()` that increments generation and marks ready data stale. Future event-based invalidation can call it.

### 12.4 Optional P2 follow-on

Replace or complement TTL with `RegNotifyChangeKeyValue` on the **actual package repository key/source** after proving behavior across supported Windows versions. Do not add a permanent watcher thread to P1 without shutdown/reload lifecycle analysis in injected hosts.

### 12.5 Tests

Inject a clock and package source:

- no rescan before TTL;
- exactly one rescan at expiry across concurrent callers;
- `invalidate()` during `Loading` prevents stale publish;
- failed refresh returns to retryable Empty;
- no `Sleep`-based flaky unit tests.

---

## 13. P1 / P0-adjacent security — F-11: Remove unsafe ACL widening and redesign `TreatAs` access

### 13.1 Confirmed defects

#### Directory helper

`src/shared/System/Security/Permission.h::SetFile(path)` opens a path as a normal file. Opening directories via `CreateFileW` requires `FILE_FLAG_BACKUP_SEMANTICS`, so current directory calls fail. But the helper then grants Users `GENERIC_ALL`; simply adding the directory flag would turn a latent failure into a security regression by making the Program Files install tree broadly user-writable.

Callers include:

- `src/exe/src/Main.cpp:265` on the install directory;
- `src/dll/src/Main.cpp:1519` on the install directory;
- logging code on a file path.

#### Protected registry ACL fallback — new omission found during plan review

`src/exe/src/Main.cpp:118-185` `SetPermissions(...)`:

- enables take-ownership privilege;
- changes key owner;
- constructs ACEs for Administrators, LocalSystem, **and BUILTIN\Users**;
- grants every SID `GENERIC_ALL` with `CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE`;
- leaves the widened DACL in place.

`disable_modern()` calls it after `ERROR_ACCESS_DENIED` when writing the Windows 11 `TreatAs` path.

This must not be executed from or copied into a privileged installer CA.

### 13.2 Revised implementation

1. **Remove the install-directory `Permission::SetFile(dir)` calls.** Program Files must retain inherited secure ACLs.
2. Do not alter install-directory permissions to make logs/config writable. Mutable per-user data belongs under `%LOCALAPPDATA%`/`%APPDATA%`; shared mutable data belongs under a narrowly scoped `%ProgramData%` folder with only required Modify rights. Data relocation can be a separate migration if not required for this release.
3. For ordinary log files, create them under an already writable location and inherit that location’s ACL; do not grant broad rights after creation.
4. Delete or redesign `Permission::SetFile` so its API requires an explicit desired ACL/purpose. A generic “make writable” security helper is too dangerous.
5. `Permission::SetRegistry` must not open with `KEY_READ` and then attempt DACL changes; if retained, security-descriptor mutation requires the correct rights (`WRITE_DAC`/`READ_CONTROL`) and explicit minimum ACEs. Prefer deleting unused ACL-mutating helpers.
6. Remove the `BUILTIN\Users GENERIC_ALL` TreatAs fallback entirely.
7. Handle `TreatAs` through the P0 registration architecture:
   - check current value and ownership;
   - never overwrite/remove a foreign TreatAs value without an explicit product policy;
   - on uninstall, remove only if it still equals the product CLSID;
   - preserve/restore pre-existing state for rollback where a shared key is touched;
   - no permanent owner/DACL broadening.
8. If supported Windows versions deny SYSTEM/elevated write to the exact key, stop and perform a dedicated supported-design investigation rather than silently weakening ACLs.

### 13.3 Security acceptance tests

Use `GetNamedSecurityInfo`/registry security APIs or PowerShell ACL snapshots before/after:

- install directory;
- `HKLM\Software\Classes\CLSID\{86ca1aa0-...}` and `TreatAs`;
- product COM CLSIDs;
- ProgramData/AppData directories introduced by any migration.

Assert:

- BUILTIN\Users does not gain write/modify/full control to product binaries or protected system CLSID keys;
- owner/DACL of shared Windows key is unchanged after successful install/uninstall and rollback, unless a narrowly documented intended change is required;
- failure to write TreatAs fails or follows explicit fallback policy — never security relaxation.

---

## 14. P1 — F-13/F-14: Correct component identity and state the architecture policy truthfully

### 14.1 F-13 — Start Menu component GUID

The Start Menu component uses one GUID while the emitted x86 component is 32-bit and x64/ARM64 components carry the 64-bit attribute. Microsoft’s “Changing the Component Code” guidance explicitly lists recompiling a 32-bit component into a 64-bit component as requiring a new component code.

Implementation:

1. Add `StartMenuShortcutComponentGuid` to stable `identity.wxi` for x86/x64/ARM64.
2. Use it in `setup.wxs` instead of the literal shared GUID.
3. Preserve the invariant that cross-architecture products do not coexist on the same resource location. Current major-upgrade sequencing removes the old product before installing the new one; F-14 policy blocks same-version side-by-side. Document this because component-code changes plus identical locations require non-coexistence to avoid resource conflict.

### 14.2 F-14 — Upgrade policy/comment mismatch

The existing comment claims same-version x86+x64 can install side-by-side, but the Upgrade table uses same/higher detection and blocks that scenario. Do not “fix” this by changing version bounds unless true side-by-side has been intentionally designed.

Recommended product policy: **one installed architecture at a time** because shell registration is machine-global/shared.

Implementation:

- retain shared UpgradeCode relationship and distinct ProductCodes;
- keep same/higher-version detection;
- rewrite comments to match actual policy;
- change the Type-19/error text from “a newer version is already installed” to something accurate, e.g. “This or a newer version of Nilesoft Shell is already installed (possibly a different architecture). Uninstall it before installing the same-version architecture.”
- explicitly document that older cross-architecture major upgrade is supported only through RemoveExistingProducts removing the old product first.

### 14.3 Tests

MSI matrix:

- x64 N→x64 N+1;
- x86 N→x64 N+1;
- x64 N→ARM64 N+1 where platform permits;
- same-version x86↔x64 attempt is blocked with correct message;
- N+1→N downgrade blocked;
- inspect Component table bitness/GUIDs in all three packages.

---

## 15. P1 — F-19: Replace fixed 250-character window-title reads

### 15.1 Confirmed issue

`FuncExpression.cpp` contains at least two `wchar_t title[250]` + `GetWindowTextW(...,250)` paths. Titles longer than 249 characters are silently truncated.

### 15.2 Revised implementation

Create one helper for same-process/normal top-level title retrieval:

1. call `GetWindowTextLengthW(hwnd)`;
2. allocate `len + 1`;
3. call `GetWindowTextW`;
4. store the **actual second-call return** as logical length;
5. because another process can change the title between calls, if the second call fills the buffer (`n == capacity-1`), re-query/grow and retry a small bounded number of times;
6. set a reasonable hard cap (for example 64 Ki wchar, based on product need) to avoid unbounded allocation from a hostile/changing remote window; if capped, make truncation explicit in helper semantics/tests.

Remember Microsoft notes `GetWindowTextLengthW` can overestimate in mixed ANSI/Unicode situations; over-allocation is acceptable.

Replace all equivalent fixed-title paths, not only the two originally flagged.

### 15.3 Tests

Create a test window with:

- empty title;
- >250-character title;
- title near cap;
- optional title change between sizing/fill through a test seam.

Assert exact content and length.

---

## 16. P1 — F-15: Enable CFG; treat signing as a separate release-pipeline deliverable

### 16.1 CFG implementation

Enable Control Flow Guard for **Release** configurations of:

- `src/dll/Shell.vcxproj`;
- `src/exe/exe.vcxproj`;
- `src/setup/ca/ca.vcxproj`;

for all x86/x64/ARM64 configurations.

Use the Visual Studio `ControlFlowGuard` property where possible and verify the actual compiler/link command lines. Microsoft requires both compiler `/guard:cf` instrumentation and linker `/GUARD:CF`; `/DYNAMICBASE` is also required.

### 16.2 Verification

For every release PE:

```text
dumpbin /headers /loadconfig <binary>
```

Assert:

- `Guard` appears in characteristics;
- Guard Flags include `CF Instrumented`;
- FID table is present;
- ASLR/DYNAMICBASE remains enabled.

Then run the complete hook/IAT/detour smoke matrix. The project intentionally rewrites/import-hooks function pointers, so functional testing matters even though ordinary CFG should remain compatible.

### 16.3 Signing — do not conflate with CFG

Signing is not a source-code flag and must not be marked “implemented” without certificate/pipeline infrastructure.

Release pipeline order:

1. build PEs;
2. sign `shell.dll`, `shell.exe`, `ca.dll` and any other shipped PEs;
3. verify PE signatures;
4. package the **signed** PEs into MSI;
5. sign MSI last;
6. verify final MSI and embedded PE signatures with `signtool verify /pa /all` or organization equivalent;
7. never commit private key material or secrets.

If a release certificate is not available, record signing as an explicit release-process blocker/dependency instead of fabricating a local “signed” state.

---

## 17. P1 — F-06: Change exception model uniformly; narrow catches only after behavior is stable

### 17.1 Confirmed risk

- `src/dll/Shell.vcxproj` sets `<ExceptionHandling>Async</ExceptionHandling>` → `/EHa`;
- `src/exe/exe.vcxproj` does likewise;
- the tree has many `catch(...)` blocks;
- shell.dll is injected into Explorer/other hosts.

Microsoft warns that `/EHa` allows `catch(...)` to catch SEH/asynchronous exceptions such as access violations and that continuing after such faults can corrupt process state. Microsoft strongly recommends `/EHsc` and also recommends **never linking `/EHa` and `/EHs`/`/EHsc` objects in the same executable module**.

### 17.2 Correction to the source plan

Do **not** use “per-file `/EHa` if needed.” That creates exactly the mixed-module model Microsoft advises against.

Do **not** combine the flag change with a mass rewrite of 50+ `catch(...)` blocks. Under `/EHsc`, those handlers already stop catching SEH; changing every C++ exception boundary simultaneously adds unnecessary semantic risk.

### 17.3 Revised implementation

1. Change **all TUs in `shell.dll`** to `/EHsc` uniformly (`ExceptionHandling=Sync`).
2. Build. If C2712/SEH-unwind conflicts appear, fix source structure by:
   - narrowing `__try/__except/__finally` regions;
   - moving C++ objects requiring destruction outside raw SEH regions;
   - using POD-only SEH functions/wrappers as the code already does in several hot paths.
   - **Do not restore `/EHa` for individual TUs.**
3. Run DLL tests/stress.
4. Change **all TUs in `shell.exe`** to `/EHsc` uniformly and repeat.
5. Inspect MSBuild binlogs/effective CL command lines and fail CI if a module contains mixed `/EHa` and `/EHsc` objects.
6. Only after the model transition is stable, review broad `catch(...)` blocks by subsystem:
   - parser/expression boundaries may intentionally convert arbitrary C++ exceptions to expression failure;
   - host-entry boundaries should catch known C++ exceptions, log, and fail closed;
   - empty `catch(...) {}` blocks should be narrowed or justified;
   - never attempt to recover from access violations through C++ catch handlers.

### 17.4 Tests

- isolated subprocess fault-injection test demonstrating an access violation is **not** swallowed by C++ `catch(...)` under release flags;
- existing parser/expression C++ exception paths still produce intended behavior;
- Explorer/PageHeap/Application Verifier stress after the flag change;
- no C2712 workarounds that introduce mixed EH flags.

### 17.5 Performance note

Microsoft documents that `/EHa` can inhibit optimization and increase code size because the compiler must assume asynchronous exceptions broadly. Measure release binary size and popup-path microbenchmarks after `/EHsc`; performance should be neutral or improve, but correctness is the primary objective.

---

# Phase E — Cross-cutting installer/security/performance validation

---

## 18. Additional codebase observations that influence P0/P1 implementation

These are not separate scope-expansion mandates unless stated, but they must shape the fixes above.

### 18.1 `RegistryConfig::Unregister()` removes `.nss` recursively

`src/shared/RegistryConfig.h:235` deletes `HKCR\.nss` as a whole. For the MSI path, prefer MSI-owned `RegistryValue`/specific key resources so uninstall removes what the product installed rather than recursively deleting a shared file-extension key. If standalone unregister retains recursive deletion, document ownership assumptions and consider narrowing it separately.

### 18.2 `RegistryConfig` uses HKCR extensively

This is acceptable as a compatibility view in an interactive process, but SYSTEM custom-action code should not depend on merged HKCR behavior. MSI per-machine HKCR registry authoring maps appropriately to machine classes; custom privileged code should use explicit HKLM classes and explicit view.

### 18.3 Current `OnUpdate`/`OnRestoreConfig` actions also change system files

They were outside the P0/P1 finding set, but while touching installer transaction logic, re-evaluate their rollback behavior. They rename/restore configuration and binaries around major upgrade. Do not opportunistically rewrite them in F-01, but confirm they do not undermine the new rollback model. If they directly mutate state from immediate actions, record a separate follow-up unless required for safe P0 completion.

### 18.4 Long-path manifest support is conditional

Windows 10 1607+ long-path opt-in also depends on system policy/registry configuration. CI must not assume all machines have long paths enabled. Contract-level unit tests should remain deterministic even when deep-path integration tests are skipped.

---

# 19. Revised dependency and implementation order

The source plan’s order is close but should be changed to reduce rework and avoid reintroducing unsafe behavior.

## Phase A — Tool/build invariants

1. **F-02** — split version-only generated data from stable WiX identity; update version template ownership.
2. **F-20** — make the validated MSBuild/WiX path authoritative.
3. Add repository mutation/EOL/manifest/identity CI guards.

## Phase B — P0 runtime primitives

4. **F-03** — fix source + generator manifests and embedded-manifest validation.
5. **F-04** — registry empty-string semantics.
6. **F-05** — path API contracts with contract-specific helpers.

## Phase C — Installer/security P0

7. **F-13 partial** — add Start Menu per-arch GUID while identity files are already being touched.
8. **F-11 P0-adjacent subset** — remove/disable unsafe registration ACL widening before privileged registration is redesigned.
9. **F-01 expanded** — transactional install/uninstall registration lifecycle, standard MSI registry resources where practical, narrow deferred/rollback CA only where unavoidable; no Explorer restart.
10. **F-14** — architecture policy/comments/messages; execute full MSI matrix.

**P0 release gate occurs here. Do not proceed to RC merely because unit tests pass; VM rollback/uninstall evidence is required.**

## Phase D — Low/medium blast-radius P1 correctness

11. **F-07** taskbar TLS scope.
12. **F-08** bounds/predicate cleanup.
13. **F-10** registry enumeration.
14. **F-19** window titles.
15. **F-12** package TTL/generation invalidation.

## Phase E — Ownership/security/build-hardening P1

16. **F-09** COM pointer repair and explicit call-site migration.
17. **F-11 remaining** ACL helper deletion/redesign/data-location cleanup.
18. **F-15 CFG** and hook stress.
19. **F-06 `/EHsc`** uniform module transition, followed by exception-boundary audit.
20. **Release signing pipeline** and final artifact verification.

F-09 may be moved before F-12 if COM ownership defects are encountered during P1 tests. F-06 should remain isolated in its own change because its failure mode is broad host behavior.

---

# 20. Detailed verification matrix

| Area | Automated/unit | Artifact/static | VM/manual | Must block release? |
|---|---|---|---|---|
| F-02 identity/version | run version tool in temp tree; hash stable file | inspect Product/Component tables all arch | no | **Yes** |
| F-03 manifest | XML parse | `mt.exe` validate source + embedded all arch | smoke DPI/long path as relevant | **Yes** |
| F-04 registry string | raw hive byte tests | none | optional regedit/raw query | **Yes** |
| F-05 path | contract helpers + Windows integration | scan API call sites | deep-path subprocess/VM | **Yes** |
| F-01 install/uninstall | CA helper tests where applicable | inspect MSI CustomAction/InstallExecuteSequence/Registry tables | full UI/qb/qn/install/uninstall/upgrade/rollback | **Yes** |
| F-11 registration ACL subset | ACL snapshot tooling | inspect installer resources | protected TreatAs + multi-session | **Yes with F-01** |
| F-07 taskbar | TLS/nesting/thread tests | none | Explorer taskbar/context stress | P1 |
| F-08 bounds | boundary/fuzz tests | static review | optional | P1 |
| F-09 COM pointer | fake IUnknown refcount | call-site grep forbids implicit output ops | Explorer/COM stress | P1 |
| F-10 reg enum | >260 and near-max names | none | optional | P1 |
| F-12 package cache | fake clock/source concurrency | none | install/uninstall AppX while Explorer remains alive | P1 |
| F-13/14 MSI | table/policy tests | inspect GUID/Upgrade rows | cross-arch matrix | **Yes if installer ships** |
| F-15 CFG | none | `dumpbin /headers /loadconfig` | hook smoke | P1/release-hardening |
| F-19 titles | test window >250 chars | none | optional | P1 |
| F-20 validation | build-script tests | ICE validation | no | **Yes** |
| F-06 EH | exception + isolated SEH fault test | inspect CL flags/binlog | PageHeap/Application Verifier/Explorer stress | P1 |
| Signing | pipeline checks | `signtool verify /pa /all` | SmartScreen/reputation is separate | release policy |

---

# 21. MSI-specific artifact assertions

After implementing F-01/F-13/F-14, script inspection of each MSI (x86/x64/ARM64) using `WindowsInstaller.Installer`, `lessmsi`, `wix msi decompile`, or an equivalent deterministic reader.

Assert:

1. No registration/unregistration CA has `FirstSequence`.
2. No registration/unregistration CA has async return flags.
3. Any remaining direct system-state CA is `InScript`/deferred or rollback and, when machine privilege is required, `NoImpersonate`.
4. Every forward state-changing CA has a rollback partner earlier in sequence.
5. Deferred actions are between `InstallInitialize` and `InstallFinalize`.
6. `REMOVE=ALL` custom logic is sequenced only after `InstallValidate`.
7. Unregister that needs installed resources completes before `RemoveFiles`.
8. Normal product registry registration appears in MSI Registry/RemoveRegistry resources when migrated.
9. Component GUIDs/bitness match architecture policy.
10. ProductCodes are unique by architecture; UpgradeCode relationship matches one-architecture policy.
11. Same-version cross-architecture detection message matches actual behavior.
12. ICE validation is clean or every suppression has a documented, reviewed justification.

---

# 22. Performance and stability checks tied to these changes

The implementation should avoid fixing correctness at the cost of a new menu-latency regression.

### 22.1 Path helpers

- Path calls on startup/menu hot paths should use at most one preflight + one fill in the common case.
- Do not allocate 32K buffers unconditionally everywhere; resize only where the API contract requires it.
- Cache only immutable/stable paths if profiling shows repeated cost; do not cache current directory or environment-sensitive results incorrectly.

### 22.2 Package index

- TTL refresh must stay off the first-pixel hot path when the cache is fresh.
- On expiration, current scan coalescing must ensure only one scanner.
- Record refresh duration with existing perf instrumentation before choosing TTL.

### 22.3 COM pointer/EH

- Refcount fixes should remove double-release/leak hazards without adding synchronization to every pointer operation.
- `/EHsc` may reduce code-size/optimization penalties relative to `/EHa`; capture before/after binary size and popup microbenchmarks but do not block correctness on a small noise-level regression.

### 22.4 Installer

- Registration no longer restarting Explorer is expected to improve install determinism and avoid forced process churn. File-in-use/restart behavior should be handled through Windows Installer/Restart Manager and explicit UX policy rather than arbitrary process termination.

---

# 23. “Do not do” list for the coding agent

The following shortcuts should be treated as implementation failures:

- **Do not** only remove `FirstSequence` and leave `OnInstall` immediate/post-finalize.
- **Do not** use `Return='ignore'` for required machine registration and simultaneously claim fail-closed behavior.
- **Do not** run `shell.exe -... -restart` from SYSTEM/deferred CA.
- **Do not** add `FILE_FLAG_BACKUP_SEMANTICS` to the existing ACL helper while retaining `GENERIC_ALL` for Users.
- **Do not** grant BUILTIN\Users write/full-control to the Windows Explorer CLSID/TreatAs key.
- **Do not** mix `/EHa` and `/EHsc` TUs inside the same EXE/DLL as a workaround for C2712.
- **Do not** mass-edit every `catch(...)` in the same commit as the EH model switch.
- **Do not** introduce WIL solely to avoid fixing well-understood `IComPtr` bugs in the release branch.
- **Do not** use one universal Win32 dynamic-buffer helper across undocumented/incompatible APIs.
- **Do not** call `SHLoadIndirectString(nullptr,0)` or similar undocumented sizing patterns based on analogy to another API.
- **Do not** claim `IShellLink::GetPath` supports arbitrary long paths by increasing a buffer; its documented path return is bounded.
- **Do not** use `MrtCache` change time as proof the package identity repository changed.
- **Do not** recursively delete shared registry keys during MSI uninstall when MSI can own individual values/resources.
- **Do not** patch stable `identity.wxi` from the version script.
- **Do not** validate only source manifest/XML; validate the embedded release artifact.
- **Do not** mark signing complete without verifying final binaries/MSI using the actual release signing identity.
- **Do not** suppress WiX/MSI validation globally to accommodate a CI-account limitation.

---

# 24. Definition of done for the complete P0/P1 program

A release candidate may be considered technically ready only when all of the following are true:

### Source correctness

- [ ] Existing four retained fixes remain intact and green.
- [ ] Empty registry strings include terminators.
- [ ] Path wrappers use documented contract-specific sizing and logical lengths.
- [ ] No known OOB path predicates remain in F-08 scope.
- [ ] `IComPtr` ownership tests prove no double-release/leak on copy/move/output paths.
- [ ] Registry enumeration returns long names and continues after resize.
- [ ] Taskbar-origin state is thread/nesting safe.
- [ ] Window titles are not arbitrarily capped at 249 chars.
- [ ] Package index refreshes within documented TTL/invalidation behavior.

### Installer transactionality/security

- [ ] Version tool cannot mutate stable product/component identity.
- [ ] Both manifest source and template are valid.
- [ ] No immediate/post-finalize custom action performs required machine registration.
- [ ] No async/FirstSequence register/unregister behavior remains.
- [ ] Standard MSI resources own ordinary product registry state where practical.
- [ ] Remaining machine-state CAs are deferred/no-impersonation, synchronous, and rollback-paired.
- [ ] Explorer is never force-restarted from SYSTEM custom action.
- [ ] No product action grants Users write/full control to Program Files or protected Windows CLSID keys.
- [ ] Start Menu component codes differ where 32-/64-bit component identity requires it.
- [ ] Architecture upgrade/downgrade messaging matches actual policy.

### Build/hardening

- [ ] x86/x64/ARM64 Release builds succeed.
- [ ] Existing full test suite passes; new tests are included in canonical test build.
- [ ] WiX/MSI validation passes for all three packages.
- [ ] Source/template/embedded manifests validate.
- [ ] CFG is verified in release PE load configuration for shell.dll/shell.exe/ca.dll.
- [ ] `/EHsc` is uniform per module; no mixed `/EHa` objects.
- [ ] Release artifacts are signed and verified if signing is a release requirement.
- [ ] No unreviewed line-ending/generated-file drift.

### End-to-end evidence

- [ ] Clean-VM full UI, `/qb`, `/qn` install/uninstall logs captured.
- [ ] Same-arch upgrade passes.
- [ ] Supported cross-arch upgrade passes.
- [ ] Same-version cross-arch policy behaves and messages correctly.
- [ ] Rollback fault injection restores pre-operation machine state.
- [ ] ACL snapshots show no unintended broadening.
- [ ] Multi-session test proves installer does not kill unrelated Explorer processes.
- [ ] Explorer context-menu/taskbar stress passes under PageHeap/Application Verifier for relevant P1 changes.

---

# 25. Primary official documentation used to revise this plan

## Windows Installer / WiX

- Microsoft — Changing the System State Using a Custom Action  
  https://learn.microsoft.com/en-us/windows/win32/msi/changing-the-system-state-using-a-custom-action
- Microsoft — Deferred Execution Custom Actions  
  https://learn.microsoft.com/en-us/windows/win32/msi/deferred-execution-custom-actions
- Microsoft — Rollback Custom Actions  
  https://learn.microsoft.com/en-us/windows/win32/msi/rollback-custom-actions
- Microsoft — Custom Action In-Script Execution Options  
  https://learn.microsoft.com/en-us/windows/win32/msi/custom-action-in-script-execution-options
- Microsoft — Synchronous and Asynchronous Custom Actions  
  https://learn.microsoft.com/en-us/windows/win32/msi/synchronous-and-asynchronous-custom-actions
- Microsoft — Windows Installer Best Practices  
  https://learn.microsoft.com/en-us/windows/win32/msi/windows-installer-best-practices
- Microsoft — REMOVE property  
  https://learn.microsoft.com/en-us/windows/win32/msi/remove
- Microsoft — Upgrade Table  
  https://learn.microsoft.com/en-us/windows/win32/msi/upgrade-table
- Microsoft — Changing the Component Code  
  https://learn.microsoft.com/en-us/windows/win32/msi/changing-the-component-code
- FireGiant — WiX Validation  
  https://docs.firegiant.com/wix/tools/validation/
- FireGiant — `CustomAction` schema  
  https://docs.firegiant.com/wix/schema/wxs/customaction/
- FireGiant — `RegistryValue` schema  
  https://docs.firegiant.com/wix/schema/wxs/registryvalue/
- FireGiant — `RemoveRegistryKey` schema  
  https://docs.firegiant.com/wix/schema/wxs/removeregistrykey/

## Registry / security

- Microsoft — `RegSetValueExW`  
  https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regsetvalueexw
- Microsoft — `RegEnumValueW`  
  https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regenumvaluew
- Microsoft — `RegQueryInfoKeyW`  
  https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regqueryinfokeyw
- Microsoft — Registry Element Size Limits  
  https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-element-size-limits
- Microsoft — Merged View of HKEY_CLASSES_ROOT  
  https://learn.microsoft.com/en-us/windows/win32/sysinfo/merged-view-of-hkey-classes-root
- Microsoft — Registry for advanced users (HKCR write behavior)  
  https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/windows-registry-advanced-users
- Microsoft — `CreateFileW` (directory handles require backup semantics)  
  https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew

## Paths / shell

- Microsoft — `GetModuleFileNameW`  
  https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulefilenamew
- Microsoft — `GetFullPathNameW`  
  https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfullpathnamew
- Microsoft — `GetLongPathNameW`  
  https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getlongpathnamew
- Microsoft — `GetShortPathNameW`  
  https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getshortpathnamew
- Microsoft — `GetCurrentDirectoryW`  
  https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getcurrentdirectoryw
- Microsoft — `SearchPathW`  
  https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-searchpathw
- Microsoft — `IShellLink::GetPath`  
  https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishelllinkw-getpath
- Microsoft — Application manifests  
  https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests
- Microsoft — Maximum Path Length Limitation  
  https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation

## C++ / mitigations

- Microsoft — `/EH` exception handling model  
  https://learn.microsoft.com/en-us/cpp/build/reference/eh-exception-handling-model
- Microsoft — Structured vs C++ exception handling  
  https://learn.microsoft.com/en-us/cpp/cpp/exception-handling-differences
- Microsoft — `/guard:cf`  
  https://learn.microsoft.com/en-us/cpp/build/reference/guard-enable-control-flow-guard
- Microsoft — `/GUARD:CF`  
  https://learn.microsoft.com/en-us/cpp/build/reference/guard-enable-guard-checks
- Microsoft — Control Flow Guard  
  https://learn.microsoft.com/en-us/windows/win32/secbp/control-flow-guard

## Window text

- Microsoft — `GetWindowTextW`  
  https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowtextw
- Microsoft — `GetWindowTextLengthW`  
  https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowtextlengthw

---

# 26. Final implementation guidance to the coding agent

Implement this as a sequence of **reviewable, test-pinned changes**, not one large “P0/P1” commit. The highest-risk area is installer registration: first remove the security-invalid assumptions, then make ordinary product-owned registry state MSI-owned where practical, and only use a narrow deferred/rollback custom action for state MSI cannot represent. Do not move the existing shell-out/ACL/restart behavior into SYSTEM and call that a fix.

For core runtime fixes, preserve the same principle: encode the external API contract in the implementation. A `GetModuleFileNameW` retry loop, a preflight/fill API, a shell interface with a documented `MAX_PATH` bound, and a remote-window text getter are different problems and should remain visibly different in code and tests.

Finally, treat release evidence as part of the fix. “Builds and unit tests pass” is insufficient for MSI sequencing, rollback, ACLs, PE mitigations, and injected Explorer behavior. The definitions of done above require artifact inspection and clean-VM proof because those are the only places several of these defects are observable.
