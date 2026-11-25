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

				/*
					The two deferrals, kept apart, because they ask the user for
					opposite things.

					`slow_deferrals` is Shell's judgement about this extension:
					it has never once answered quickly. Quarantine is the
					remedy, and this window offers it.

					`budget_deferrals` is a statement about a *menu*: this
					extension was never judged at all - the menu ran out of its
					allowance before reaching it. Quarantining it would punish
					it for something it did not do; what that reading calls for
					is a larger budget or fewer handlers.

					They were one word until docs/refactor/09-remediation-plan.md
					finding D, and merged this window told a user to quarantine
					an extension whose only fault was its neighbours'.
				*/
				uint32_t slow_deferrals{};
				uint32_t budget_deferrals{};
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

			// Moved to shared/PerfReport.h so `-report perf` and this window
			// cannot spell one extension two ways: the report printed a raw
			// "E&dit with Adobe Acrobat" while this list showed "Edit with
			// Adobe Acrobat", so neither could be searched for the other's
			// spelling. Kept as a name here because test_reliability_rows.cpp
			// pins its rules under it.
			// docs/refactor/09-remediation-plan.md finding J.
			using Nilesoft::Shell::Diagnostics::without_mnemonics;

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

				// The verdict, and it is not "quarantined or fine". A row whose
				// only records are budget deferrals has never been measured at
				// all - reporting it as "not asked yet" would be true and
				// useless, and reporting it as slow would be false.
				const wchar_t *verdict = L"ok";
				if(row.quarantined)
					verdict = L"quarantined";
				else if(row.samples == 0 && row.budget_deferrals && !row.slow_deferrals)
					verdict = L"menu ran out of budget";
				else if(row.samples == 0 && row.slow_deferrals)
					verdict = L"deferred as slow";
				else if(row.samples == 0)
					verdict = L"not asked yet";

				wchar_t tail[96]{};
				::swprintf_s(tail, L"%6u.%u ms   %s", ms, tenth, verdict);
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
