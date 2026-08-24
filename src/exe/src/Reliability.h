#pragma once

/*
  The Reliability Center window - docs/refactor/05-capabilities.md section 1.

  The last piece of the flagship. Everything it shows has existed for a while
  and is reachable from the command line: `-report perf` says what each host's
  menus cost and which extension was slow, `-quarantine` stops Shell asking one,
  and the takeover status block says whether any of it is set up. What was
  missing is the place where a person who has just noticed a slow context menu
  can see the answer and act on it in the same breath, without being told to
  open a terminal.

  Three decisions, each with a plausible opposite:

  **The details pane shows the exact text `-report perf` prints, not a second
  rendering of the same numbers.** BuildPerfReport is shared. Two formatters
  over one set of facts is how a window and a report come to disagree about a
  machine, and the report is the thing people paste into bug threads.

  **The provider list is merged across hosts, keyed by CLSID hash.** A user
  thinks about "the NanaZip extension", not about NanaZip-as-seen-by-this-
  Explorer-window. Each row keeps the worst time any host measured, because the
  worst case is what makes a menu feel slow. The per-host detail stays in the
  pane below for anyone who wants it.

  **Plain user32 controls, no comctl32.** A ListView would look better and would
  add a dependency to a binary whose whole job is to be small and to run before
  anything else is known to work. LISTBOX with a fixed-pitch font is enough for
  a table nobody sorts, and it keeps the manager's import table as it was.

  Not here, deliberately: "Show Windows menu once" is a gesture
  (Ctrl+Alt+right-click, docs/refactor/01 section 7) rather than something a
  window can do on the user's behalf, and putting a button on it would suggest
  it applies to the next menu anywhere rather than the next click the user
  makes.
*/

#include <vector>
#include <string>
#include <algorithm>

#include <PerfExport.h>
#include <PerfReport.h>
#include <ProviderQuarantine.h>

namespace Nilesoft
{
	namespace Shell
	{
		namespace Reliability
		{
			namespace Diag = Nilesoft::Shell::Diagnostics;

			// One extension, merged across every host that asked it.
			struct ProviderRow
			{
				uint32_t hash{};
				GUID clsid{};
				bool has_clsid{};
				std::wstring name;

				// The worst any host measured. A menu feels as slow as its
				// slowest provider, and an average over hosts that never asked
				// it would report a fast extension for a slow one.
				uint32_t worst_us{};
				uint32_t samples{};
				bool quarantined{};
			};

			struct Snapshot
			{
				std::vector<ProviderRow> providers;
				std::wstring text;          // exactly what `-report perf` prints
				size_t hosts{};
			};

			/*
				Width of the first column, and it is 38 for a reason: that is
				exactly the length of a formatted CLSID including its braces,
				so an extension with no name is shown by its full identity and
				never elided. Anything narrower cuts a GUID's tail, and the
				tail is the only part that distinguishes members of a handler
				family - this machine has six registered as
				{1FA0E654-...-B9A75D744B00} through -B05, which at 34 columns
				all render as the same string.
			*/
			inline constexpr size_t NameColumn = 38;

			/*
				Shorten from the middle, not the end.

				The end is where the information is, for both kinds of label a
				row can carry: a GUID is distinguished by its last group, and
				extension names share prefixes far more often than suffixes
				("Microsoft Defender scan..." / "...submit..."). Eliding the
				tail is the conventional choice and the wrong one here.
			*/
			inline std::wstring elide(std::wstring label, size_t width)
			{
				if(label.size() <= width || width < 3)
					return label;

				auto keep = width - 1;              // one cell for the ellipsis
				auto head = keep / 2;
				auto tail = keep - head;
				return label.substr(0, head) + L"\x2026"
					+ label.substr(label.size() - tail);
			}

			/*
				Drop the mnemonic markers a handler puts in its title.

				`IExplorerCommand::GetTitle` answers with the string meant for a
				menu item, so it carries them: this machine's Acrobat handler
				returns "E&dit with Adobe Acrobat". A LISTBOX is not a menu and
				gives `&` no meaning, so it renders literally and the row reads
				as a typo.

				The Win32 rule, applied here rather than invented: a doubled
				`&&` is a literal ampersand, a single one marks the character
				after it.
			*/
			inline std::wstring without_mnemonics(std::wstring_view title)
			{
				std::wstring out;
				out.reserve(title.size());

				for(size_t i = 0; i < title.size(); i++)
				{
					if(title[i] != L'&')
					{
						out += title[i];
						continue;
					}
					if(i + 1 < title.size() && title[i + 1] == L'&')
					{
						out += L'&';
						i++;
					}
				}
				return out;
			}

			inline std::wstring format_row(const ProviderRow &row)
			{
				std::wstring label = without_mnemonics(row.name);
				if(label.empty())
				{
					// No host has successfully activated it, so there is no
					// title - but there is always an identity now, which is
					// the thing the user needs. docs/refactor/05 section 1c.
					label = row.has_clsid
						? Nilesoft::Shell::Quarantine::format_guid(row.clsid)
						: L"(unknown extension)";
				}

				label = elide(std::move(label), NameColumn);
				label.resize(NameColumn, L' ');

				uint32_t ms = 0, tenth = 0;
				Diag::perf_report_split_ms(row.worst_us, ms, tenth);

				wchar_t tail[96]{};
				::swprintf_s(tail, L"%6u.%u ms   %s",
							 ms, tenth,
							 row.quarantined ? L"quarantined"
											 : (row.samples ? L"ok" : L"not asked yet"));
				return label + tail;
			}

			/*
				Read every host's block, merge the providers, and format the
				text pane.

				Read-only with respect to the hosts: perf_export_read maps their
				sections, copies, and unmaps. Nothing here can change what a
				menu does.
			*/
			Snapshot take();

			// Opens the window and pumps until it closes. Returns 0.
			int show();
		}
	}
}
