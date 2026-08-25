#pragma once

/*
	"Why is this here?", answered on the item itself.

	docs/refactor/05-capabilities.md section 7 asks for a "modifier-hover or
	context submenu on any item ... dumping, from data the session already
	holds: source kind, matched modify/moveto rule locations (file:line),
	evaluation of the rule's `where=` against current selection, construction
	timing from ring".

	Shift+Alt+right-click composes the menu with every item's tooltip replaced
	by that. Hover an item and it tells you where it came from.

	## Why the tip, and not a submenu

	A "Why is this here?" submenu on every item would change what every menu
	contains for every user, to serve a question almost nobody is asking at any
	given moment - and it would be a submenu on items that have one already.
	The tip is a surface this product already owns (Include/Tip.h), it is
	per-item, it appears on hover, and it costs a menu that is not being
	inspected exactly nothing.

	Tips are forced on while inspecting. A user who has turned tips off in their
	configuration has not thereby asked for the inspector to do nothing.

	## What it can say, and what it cannot

	Everything below is read from state the composed menu already holds:

	  - **The origin.** Which of the three ways this item reached the menu.
	  - **The identity.** src/shared/MenuIdentity.h - and it is printed in full
	    because it is exactly the argument `shell.exe -favorites:pin` takes.
	    That is the lesson docs/refactor/05-capabilities.md section 1c learned
	    the expensive way: a diagnostic that names a thing differently from the
	    command that acts on it names it for nobody.
	  - **The rules.** A custom item's own rule, and for a native item every
	    `modify`/`moveto` rule that matched it - `menuitem_t::native_items`
	    already collects those while the rules are applied, so this is a read
	    rather than a new pass. Include/RuleProvenance.h turns each into a
	    file and a line.

	What is deliberately absent is section 7's "evaluation of the rule's
	`where=` against current selection". The rules were evaluated during
	composition and only their *effect* was kept; re-evaluating an expression to
	display it would run the configuration a second time, with a different
	`_this`, and could disagree with what the menu actually did. A wrong answer
	about why something happened is worse than no answer. The rule's location is
	given instead, which is where somebody goes to read the `where=` themselves.

	Timing is not per-item either: the ring records phases, and composition of
	one item is not a phase. `shell.exe -report perf` already prints what the
	menu cost and what each provider cost, which is the granularity that exists.
*/

#include <string>
#include <vector>

#include "RuleProvenance.h"

namespace Nilesoft
{
	namespace Shell
	{
		namespace Inspector
		{
			/*
				Whether this thread is composing an inspected menu.

				A plain thread-local bool, because it is set from the popup hook
				and that is an SEH function: MSVC refuses (C2712) to compile one
				holding anything that needs unwinding, so nothing here may be an
				object with a destructor. Same constraint, and the same shape,
				as perf::menu_perf_begin. docs/refactor/08-handoff.md section 4.

				Thread-local rather than process-wide because two windows of one
				host raise menus on their own threads, and inspecting one must
				not annotate the other.
			*/
			inline thread_local bool armed = false;

			inline void arm(bool on) noexcept { armed = on; }
			inline bool active() noexcept { return armed; }

			// What an item can be asked about, as plain data, so the formatting
			// is testable without a menu.
			struct Facts
			{
				// "Windows", "your configuration", "a packaged extension".
				const wchar_t *origin{};

				// The identity `shell.exe -favorites:pin` takes, or empty for
				// an item that cannot be named.
				std::wstring identity;

				// "shell.nss:41". Every rule that had a hand in this item: its
				// own, for a custom item, and every modify/moveto rule that
				// matched, for a native one.
				std::vector<std::wstring> rules;
			};

			// "C:\dir\shell.nss" -> "shell.nss". The full path is too wide for
			// a tooltip and the leaf is what a person recognises; the rules
			// live beside each other in one configuration directory, so the
			// leaf is nearly always unambiguous. Where it is not, the line
			// number still lands in the right file once opened.
			inline std::wstring leaf_of(const wchar_t *path)
			{
				if(!path || !*path)
					return {};
				std::wstring full(path);
				auto cut = full.find_last_of(L"\\/");
				return cut == std::wstring::npos ? full : full.substr(cut + 1);
			}

			inline std::wstring location(const wchar_t *path, const RuleProvenance &at)
			{
				auto leaf = leaf_of(path);
				if(leaf.empty() || !at.known())
					return {};

				wchar_t line[16]{};
				::wsprintfW(line, L":%u", at.line);
				return leaf + line;
			}

			/*
				The tooltip text for one item.

				Short lines, because it is a tooltip and not a report. An item
				with nothing to say beyond its origin says only that, rather
				than padding out to a fixed shape - a tip that is mostly empty
				labels reads as a broken tip.
			*/
			inline std::wstring describe(const Facts &facts)
			{
				std::wstring out;

				if(facts.origin && *facts.origin)
				{
					out += L"From ";
					out += facts.origin;
				}

				if(!facts.rules.empty())
				{
					out += out.empty() ? L"Rule " : L"\nRule ";
					for(size_t i = 0; i < facts.rules.size(); i++)
					{
						if(i)
							out += L", ";
						out += facts.rules[i];
					}
				}

				if(!facts.identity.empty())
				{
					if(!out.empty())
						out += L'\n';
					out += facts.identity;
				}

				return out;
			}
		}
	}
}
