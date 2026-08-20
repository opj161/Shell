# CLAUDE.md

See [AGENTS.md](AGENTS.md). It applies in full; this file exists so the same
instructions are picked up under either name.

The short version, because it is the part most often skipped:

**Read the vendor's reference page for every documented contract you touch —
Win32, COM, shell, MSI/WiX, Detours, MSVC — before changing the code and before
stating a conclusion about it, then verify it on the machine, then pin it with a
test.** Assessment counts: "this is correct" is a claim about a contract exactly
as much as "this is a bug" is. The surrounding code is frequently the thing that
is wrong, and so, twice over, was the implementation plan.

Cite the specific page and quote the sentence you are relying on — in the commit
message, and in every finding you report. Where nothing documents the behaviour,
say so and cite the probe instead.
