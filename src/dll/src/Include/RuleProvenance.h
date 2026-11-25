#pragma once

/*
	Where a configuration rule was written.

	docs/refactor/05-capabilities.md section 7 asks for an inspector that can
	answer "why is this here?" about any menu item, and names its long pole:

	  "the long pole is threading rule-provenance (file+line) through the
	   parser - add during the section 04 seam work while touching those very
	   files"

	This is that thread. Every `NativeMenu` the parser builds records the file
	and the line the rule opened on, and nothing else - a rule is found by
	opening its file at that line, which is what a person does with the answer.

	## Why an index rather than a path

	A path per rule would be a `std::wstring` per rule, allocated during the
	parse and held for the life of the configuration generation - roughly 150
	of them for the stock configuration, all duplicates of about ten distinct
	strings. `Parser::LoadedFiles()` already holds exactly those ten, root
	first, in load order, and has since the last-known-good shadow needed the
	same list (docs/refactor/03-config-safety.md section 1b). So the index is
	into that vector, and `CACHE::files` is the copy of it that outlives the
	parse.

	The index is stable within a generation and meaningless outside one. A
	reload re-parses and rebuilds both, together, under the snapshot swap - an
	item holding a `NativeMenu *` from generation N reads `CACHE::files` from
	generation N, because an open menu holds its whole generation until it
	closes (docs/refactor/03-config-safety.md section 3).

	## What `NoFile` means, and why it is not zero

	Zero is the root configuration file, which is the commonest answer there
	is. A rule with no provenance is one built somewhere the parser is not -
	`NativeMenu(bool set_any_type)` exists for exactly that - and answering
	"line 0 of shell.nss" for it would be a wrong answer that reads as a right
	one. `known()` is what a caller asks before printing anything.

	## What is deliberately not recorded

	**The column.** The parser tracks one (`Lexer::column`, and its errors
	print it), so it would cost nothing to carry. But a rule is a statement,
	not a token: `item(title='X' cmd=...)` spans a line and the useful place to
	put a cursor is its start. A column would be recorded accurately and then
	be wrong for every rule whose interesting part is the property that failed
	rather than the keyword that opened it.

	**Which import chain reached the file.** A file imported from two places is
	one file with one set of rules; the chain is a property of the import, not
	of the rule. `Parser::LoadedFiles()` records each file once for the same
	reason.
*/

#include <cstdint>

namespace Nilesoft
{
	namespace Shell
	{
		struct RuleProvenance
		{
			// No file. Not an index, and deliberately not 0 - see the header.
			static constexpr uint16_t NoFile = 0xFFFF;

			// Index into CACHE::files, which is Parser::LoadedFiles() copied
			// out of the parse that produced this generation.
			uint16_t file = NoFile;

			// One-based, as the parser counts and as an editor shows.
			uint32_t line = 0;

			bool known() const noexcept { return file != NoFile && line != 0; }

			// A parse that opened more than 65,534 files has bigger problems,
			// but the cast has to be answerable for itself: an index that will
			// not fit is recorded as unknown rather than wrapping into some
			// other file's identity.
			static RuleProvenance at(size_t file_index, size_t source_line) noexcept
			{
				RuleProvenance out;
				if(file_index < NoFile && source_line > 0)
				{
					out.file = static_cast<uint16_t>(file_index);
					out.line = static_cast<uint32_t>(source_line);
				}
				return out;
			}
		};
	}
}
