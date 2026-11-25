---
name: shell-ponytail-review
description: "Read-only final review of the current Shell simplification diff: official contract/correctness/scope review first, then evidence-backed Ponytail-style simplification review."
disable-model-invocation: true
---

# Review the current Shell simplification diff

Read `CLAUDE.md` and `AGENTS.md`.

If there is no relevant working-tree diff, report `NO RELEVANT DIFF` and stop.

Record current HEAD/working-tree state and delegate the actual diff, accepted implementation contract when available, and reported verification results to `shell-ponytail-reviewer`.

Do not invoke upstream Ponytail review/audit skills. The dedicated reviewer applies the useful simplification lens only after the contract/correctness/scope gates.

Return its:

- blocking findings;
- safe simplification findings;
- verification gaps;
- `PASS` or `CHANGES REQUIRED` verdict.

Do not modify the diff.
