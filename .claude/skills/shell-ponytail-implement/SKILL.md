---
name: shell-ponytail-implement
description: Implement exactly one independently accepted Shell simplification, with contract preflight before the implementation worker activates upstream Ponytail, then run a separate final reviewer.
argument-hint: "<paste one ACCEPTED verifier result and implementation contract>"
disable-model-invocation: true
---

# Implement one accepted Shell simplification

Read `CLAUDE.md` and `AGENTS.md`.

Input is `$ARGUMENTS` plus relevant verifier text in this conversation.

## Admission

Do not edit unless there is exactly one complete verifier result marked `ACCEPTED` or `ACCEPTED-WITH-PRECONDITION` with its implementation contract.

If candidate evidence is missing, stale, or ambiguous, stop and report which evidence must be re-verified; do not invoke the manual `/shell-ponytail-verify` skill on the user's behalf.

Record current HEAD and working-tree state, then delegate exactly one candidate to `shell-ponytail-implementer`. Send:

- complete verifier result and implementation contract;
- current HEAD/dirty state;
- invariant and allowed/must-not-change scope;
- established official-document URLs;
- preconditions;
- required tests/probes/measurements/builds.

The implementer itself must perform its admission/documentation preflight **before** invoking `ponytail:ponytail full`.

Never combine several accepted cleanups into one implementation task.

## Final gate

After the implementer returns:

1. inspect the actual current diff and reported checks;
2. if there is a relevant diff, delegate it plus the full accepted implementation contract and check results directly to `shell-ponytail-reviewer`;
3. return implementation results and the reviewer's `PASS` / `CHANGES REQUIRED` verdict.

A reviewer `PASS` is the completion gate. Do not silently edit review findings in the parent workflow.

Never deploy, mutate registration, or restart Explorer without explicit user approval.
