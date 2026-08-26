---
name: shell-ponytail-verify
description: Re-verify complete Shell simplification candidate entries against current HEAD with the independent non-Ponytail verifier before implementation.
argument-hint: "<paste one or more complete candidate entries>"
disable-model-invocation: true
---

# Re-verify Shell simplification candidates

Read `CLAUDE.md` and `AGENTS.md`.

Use `$ARGUMENTS` plus candidate text already present in the current conversation.

If an ID is supplied but its full candidate evidence is not available in current context, stop with:

`INSUFFICIENT CANDIDATE DATA — paste the complete candidate entry.`

Do not reconstruct an old candidate from memory.

1. Record current full HEAD and working-tree state.
2. Delegate supplied candidates to fresh `shell-ponytail-verifier` contexts, maximum 3 candidates per verifier.
3. Do not invoke or preload Ponytail.
4. Require whole-tree/external-activation search, canonical official contracts, compatibility, global-complexity analysis and explicit behavioral proof.
5. Return the verifier verdicts unchanged in meaning.

Only `ACCEPTED` or `ACCEPTED-WITH-PRECONDITION` may proceed to `/shell-ponytail-implement`.
