#pragma once

/*
	Turning a host's exported sessions into the report `shell.exe -report perf`
	prints.

	Split from src/shared/PerfExport.h, which is the transport, because the
	arithmetic here is the part that can be quietly wrong: a percentile off by
	one index, an age that goes backwards across a tick wrap, a "slowest" that
	picks the first record when every session cost the same. None of that needs
	a section, a second process or a menu to test, and all of it would be
	invisible in a report that still looks plausible.

	So this header holds no I/O. It takes records and produces numbers and
	lines; Main.cpp opens the sections and writes what comes back.

	Two conventions worth stating, because the opposite of each is defensible:

	**Percentiles are nearest-rank, and they are computed over whatever the
	block held.** The export keeps sixteen sessions, so a p95 over sixteen
	samples is the maximum and says so by being labelled with its sample count.
	Interpolating between samples would put a number in the report that no menu
	ever took, which for a latency budget is worse than a blunt one.

	**Ages are clamped at zero rather than allowed to go negative.**
	GetTickCount64 "is limited to the resolution of the system timer, which is
	typically in the range of 10 milliseconds to 16 milliseconds"
	(https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-gettickcount64),
	and the reader samples it after the writer did, so a session recorded a
	moment ago can read as being in the future by a tick. "0.0s ago" is right;
	"-0.0s ago" is a bug report.
*/

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

#include "PerfExport.h"

namespace Nilesoft
{
	namespace Shell
	{
		namespace Diagnostics
		{
			/*
				Nearest-rank percentile over `count` samples, in place.

				`values` is sorted by this call - the caller owns a scratch copy,
				not the records. `percentile` is 0..100. An empty set answers 0,
				because a report line reading "p95 0.0 ms   n=0" is honest and a
				caller that has to special-case emptiness is one more place to
				get it wrong.
			*/
			inline uint32_t perf_report_percentile(uint32_t *values, size_t count, int percentile)
			{
				if(!values || count == 0)
					return 0;

				// Insertion sort: sixteen samples at most, and it keeps this
				// header free of <algorithm> so the DLL can include it too.
				for(size_t i = 1; i < count; i++)
				{
					auto value = values[i];
					size_t j = i;
					while(j > 0 && values[j - 1] > value)
					{
						values[j] = values[j - 1];
						j--;
					}
					values[j] = value;
				}

				if(percentile <= 0)
					return values[0];
				if(percentile >= 100)
					return values[count - 1];

				// Nearest rank: ceil(p/100 * n), 1-based, then to an index.
				auto rank = (static_cast<size_t>(percentile) * count + 99) / 100;
				if(rank == 0)
					rank = 1;
				if(rank > count)
					rank = count;
				return values[rank - 1];
			}

			// Milliseconds since a session started, clamped at zero. See the
			// file comment for why the clamp is not paranoia.
			inline uint64_t perf_report_age_ms(uint64_t now_tick, uint64_t session_tick)
			{
				return now_tick > session_tick ? now_tick - session_tick : 0;
			}

			inline constexpr wchar_t PERF_REPORT_PRE_DISPLAY_FWD[] = L"popup.total_pre_display";
			inline uint32_t perf_report_phase(const PerfExportRecord &record, const wchar_t *name);

			/*
				Which of these sessions to show the phase breakdown for.

				The slowest *to appear*, not the one that was on screen longest -
				ranking by the session total would reliably pick whichever menu
				the user left open while they thought about it, which is the one
				session guaranteed to say nothing about Shell.

				On a tie, the most recent: records arrive newest first, so a
				stable "first wins" scan already gives that, and it matters -
				a host whose menus all cost the same should show the one the user
				just opened, not the one about to fall off the end of the ring.

				Returns count when there is nothing to pick.
			*/
			inline size_t perf_report_slowest(const PerfExportRecord *records, size_t count,
											  const wchar_t *phase = PERF_REPORT_PRE_DISPLAY_FWD)
			{
				if(!records || count == 0)
					return count;

				size_t best = 0;
				auto best_cost = perf_report_phase(records[0], phase);
				for(size_t i = 1; i < count; i++)
				{
					auto cost = perf_report_phase(records[i], phase);
					if(cost > best_cost)
					{
						best = i;
						best_cost = cost;
					}
				}
				return best;
			}

			/*
				The phase the whole plan's latency budget is written against:
				docs/refactor/06-phases-and-tests.md section 4 sets "pre-display
				added by Shell <= 15 ms p95", and this is the mark that measures
				it. Recorded in Main.cpp at the point the composed menu is about
				to be shown.
			*/
			inline constexpr const wchar_t *PERF_REPORT_PRE_DISPLAY = PERF_REPORT_PRE_DISPLAY_FWD;

			// The named phase's cost, or 0 when this session did not record it -
			// a menu Shell declined never reaches the mark.
			inline uint32_t perf_report_phase(const PerfExportRecord &record, const wchar_t *name)
			{
				if(!name)
					return 0;

				auto phases = record.phase_count;
				if(phases > PERF_EXPORT_PHASES)
					phases = PERF_EXPORT_PHASES;

				for(uint32_t i = 0; i < phases; i++)
				{
					auto a = record.phases[i].name;
					auto b = name;
					size_t at = 0;
					for(; at + 1 < PERF_EXPORT_NAME && a[at] && a[at] == b[at]; at++)
						;
					if(a[at] == L'\0' && b[at] == L'\0')
						return record.phases[i].microseconds;
				}
				return 0;
			}

			// What one host's block adds up to.
			struct PerfReportSummary
			{
				size_t sessions{};			// records this reader got hold of
				uint64_t published{};		// sessions ever, which keeps counting past a wrap

				// Percentiles over the pre-display phase, which is the number
				// the budget is about and the number a user feels.
				uint32_t p50_microseconds{};
				uint32_t p95_microseconds{};
				uint32_t max_microseconds{};

				// How many of these sessions recorded the phase at all. A menu
				// Shell declined never reaches it, so a host where this is zero
				// gets no latency line rather than a line of zeroes.
				size_t measured{};

				size_t slowest{};			// index into the records, == sessions when none

				// Decisions seen, indexed by TakeoverDecision. A host whose
				// menus are all `declined` is a host Shell is not composing for,
				// which is the first thing to know and is invisible from a
				// latency number.
				uint32_t decisions[8]{};
			};

			/*
				Summarize, measuring `phase` rather than the session.

				The distinction is not pedantry and it was got wrong first:
				`total_microseconds` runs from the hook being entered to the hook
				returning, so it *includes the time the menu sat on screen
				waiting for the user*. Measured in a real Explorer on 2026-08-24
				it read 1,435 ms for a menu whose pre-display cost was 11.0 ms -
				a report headlining that number would send somebody chasing a
				latency problem that does not exist.
			*/
			inline PerfReportSummary perf_report_summarize(const PerfExportRecord *records,
														   size_t count, uint32_t *scratch,
														   size_t scratch_capacity,
														   const wchar_t *phase = PERF_REPORT_PRE_DISPLAY)
			{
				PerfReportSummary summary{};
				summary.sessions = count;
				summary.slowest = count;

				if(!records || count == 0)
					return summary;

				for(size_t i = 0; i < count; i++)
				{
					auto decision = records[i].decision;
					if(decision < 8)
						summary.decisions[decision]++;

					auto measured = perf_report_phase(records[i], phase);
					if(measured > summary.max_microseconds)
						summary.max_microseconds = measured;
				}

				summary.slowest = perf_report_slowest(records, count);

				// Percentiles need somewhere to sort. A caller that supplied
				// less room than there are records gets percentiles over the
				// prefix it did supply rather than a buffer overrun; the sample
				// count in the report is what says so.
				if(scratch && scratch_capacity > 0)
				{
					auto room = count < scratch_capacity ? count : scratch_capacity;
					size_t samples = 0;
					for(size_t i = 0; i < room; i++)
					{
						auto measured = perf_report_phase(records[i], phase);
						if(measured > 0)
							scratch[samples++] = measured;
					}

					summary.measured = samples;
					summary.p50_microseconds = perf_report_percentile(scratch, samples, 50);
					summary.p95_microseconds = perf_report_percentile(scratch, samples, 95);
				}

				return summary;
			}

			/*
				Drop the mnemonic markers a handler puts in its title.

				`IExplorerCommand::GetTitle` answers with the string meant for a
				menu item, so it carries them: this machine's Acrobat handler
				returns "E&dit with Adobe Acrobat", and there are also
				"Convert to Ado&be PDF" and "&Move to OneDrive".

				Here rather than in one of the two callers, because it was in
				one of them. The Reliability window stripped them - a LISTBOX is
				not a menu and gives `&` no meaning, so a raw title renders as a
				typo - and `-report perf` printed them raw, so one machine had
				two names for one extension and neither report could be searched
				for the other's spelling.
				docs/refactor/09-remediation-plan.md finding J.

				The Win32 rule, applied rather than invented: a doubled `&&` is
				a literal ampersand, a single one marks the character after it.
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

			// Microseconds as milliseconds with one decimal, without pulling
			// floating point or a formatter into the caller. 45123 -> 45, 1.
			inline void perf_report_split_ms(uint32_t microseconds, uint32_t &whole, uint32_t &tenth)
			{
				whole = microseconds / 1000;

				// Round the tenth rather than truncating: 1999 us is 2.0 ms,
				// not 1.9, and a report that consistently reads low is one that
				// will be argued with.
				tenth = ((microseconds % 1000) + 50) / 100;
				if(tenth >= 10)
				{
					whole++;
					tenth = 0;
				}
			}
		}
	}
}
