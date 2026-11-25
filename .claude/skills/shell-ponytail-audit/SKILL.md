---
name: shell-ponytail-audit
description: Run the Shell-specific, read-only Ponytail simplification audit over all bounded lanes or one lane, then adversarially verify only the best candidates.
argument-hint: "[all|A|B|C|D|E|F] [optional focus]"
disable-model-invocation: true
---

# Shell Ponytail audit

Read `CLAUDE.md` and `AGENTS.md` before doing anything. This workflow is read-only.

Treat `$ARGUMENTS` as the requested lane/focus; default to `all`.

## Preconditions

1. Record full `git rev-parse HEAD` and `git status --short`.
2. Confirm the project agents `shell-ponytail-auditor` and `shell-ponytail-verifier` are available.
3. Do not invoke upstream Ponytail audit/review skills. This workflow uses their useful simplification questions but replaces LOC-first ranking with Shell-specific contract/evidence gates.
4. The expected project default is Ponytail `off`. If this parent conversation already contains an active persistent Ponytail persona from an earlier/manual activation, stop the audit and report `AUDIT CONTEXT CONTAMINATED — start a fresh Shell-root session with Ponytail off`. Do not try to compensate for conflicting audit incentives in the parent context.

## Lanes

### A — Explorer / menu / COM runtime

Primary scope includes:

- `src/dll/src/ContextMenu.cpp`
- `src/dll/src/ExplorerCommand.cpp`
- `src/dll/src/ShellExt.cpp`
- `src/dll/src/Main.cpp`
- `src/dll/src/Initializer.cpp`
- `src/dll/src/Selections.cpp`
- `src/dll/src/MenuPerf.cpp`
- relevant `src/dll/src/Include/**`
- `src/dll/src/Shell.def`

Follow hooks, window subclass/message flow, COM/class-factory/CLSID activation, menu lifetime/re-entrancy, marshaling, packaged `IExplorerCommand` paths and tests across lane boundaries as evidence.

### B — Parser / expression language

Primary scope:

- `src/dll/src/Parser/**`
- `src/dll/src/Expression/**`

Treat documented `.nss` grammar/functions/properties and examples under `docs/**` as public-semantic evidence. Generated/hash-style parser data is not dead merely because ordinary references are sparse.

### C — Shared / native helpers

Primary scope:

- `src/shared/**`

Look for duplicated first-party helpers, wrappers that can safely collapse, and exact stdlib/native replacements. Compatibility and API return/termination/count semantics are mandatory checks.

### D — Packages / resources / runtime work

Primary scope includes:

- `src/dll/src/Packages.cpp`
- package/manifest/catalog helpers
- caches/bitmap/resource/rendering support in `src/dll/src/Include/**` and adjacent first-party code

Prioritize elimination of unnecessary synchronous work over micro-optimization. Performance/value claims follow `AGENTS.md` measurement rules.

### E — Registration / control executable / installer

Primary scope:

- `src/exe/**`
- `src/setup/ca/**`
- `src/setup/wix/**`
- registration/`TreatAs`/servicing code elsewhere
- `scripts/msi-query.ps1`
- `scripts/validate-msi-lifecycle.ps1`
- `scripts/repair-treatas-acl.ps1`
- `scripts/backup-and-upgrade.ps1` as deployment/servicing evidence only; never execute it during audit

WiX authoring requires `.bin/wix-docs`; underlying MSI/registry/COM behavior requires canonical Microsoft documentation. Treat custom-action IDs, sequencing, component identities, registry resources and `.def` exports as activation edges.

### F — Build / tests / developer tooling

Primary scope:

- `build.ps1`
- solution/project/shared-item build files
- `src/tests/**`
- `src/tools/**`
- remaining first-party build/developer scripts not owned by lane E

Tests are normally evidence and regression protection. Only report test/tooling deletion when it is independently proven duplicate/dead without reducing invariant coverage.

## Global exclusions as optimization targets

Do not propose edits inside:

- `src/3rdparty/**`
- `src/lib/detours/**`
- `src/lib/plutosvg/**`
- `src/bin/**`
- generated/build outputs
- `.bin/**` except reading `.bin/wix-docs/**` as documentation

Third-party sources may be read to establish behavior or whether an already-present dependency provides a capability.

## Lane execution

For a single lane, delegate it to one fresh `shell-ponytail-auditor`.

For `all`, use fresh isolated auditor contexts and at most **two concurrent** auditors:

1. A + E
2. B + C
3. D + F

Include in each delegation:

- exact lane text above;
- audit base HEAD and dirty/clean status;
- any user focus from `$ARGUMENTS`;
- instruction to return at most 6 candidates and no implementation.

## Consolidate

After auditors return:

1. Deduplicate candidates that are the same underlying boundary/root cause.
2. Drop candidates with an unresolved material platform/activation/compatibility gap from the recommendation pool.
3. Rank globally by:
   1. system-wide concepts/contracts/state/work removed;
   2. evidence strength;
   3. strength and cost of behavioral proof;
   4. compatibility/implementation risk;
   5. maintenance/dependency surface;
   6. LOC only as a tiebreaker.
4. Keep at most **8** candidates for adversarial verification (`4` for a single-lane audit).

Do not invent a numeric score: ordinal evidence-backed ranking is less false precision.

## Independent verification

Delegate the selected candidates to fresh `shell-ponytail-verifier` contexts, maximum **3 candidates per verifier**. Verifiers must receive the complete candidate entries, base/current HEAD context, and no Ponytail instructions.

Multiple verifier batches may run concurrently.

Only `ACCEPTED` and `ACCEPTED-WITH-PRECONDITION` survive.

## Final output

Return, in order:

1. audit base HEAD and working-tree state;
2. lanes completed and any coverage gap;
3. ranked accepted candidates, including each verifier's implementation contract;
4. accepted-with-precondition candidates and exact preconditions;
5. rejected/stale/unverified IDs with one-line reasons;
6. non-obvious “investigated and kept” structures only when the retention reason prevents a likely future false positive.

Do not implement anything. Do not convert uncertainty into a recommendation.
