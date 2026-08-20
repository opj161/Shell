# CLAUDE.md

See [AGENTS.md](AGENTS.md). It applies in full; this file exists so the same
instructions are picked up under either name.

The short version, because it is the part most often skipped:

**Read the official Microsoft documentation for every Win32, COM, shell or MSI
contract you touch, before changing the code — then verify it on the machine, then
pin it with a test.** The surrounding code is frequently the thing that is wrong,
and so, twice over, was the implementation plan. Cite the URL in the commit
message.
