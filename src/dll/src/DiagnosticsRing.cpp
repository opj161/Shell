#include <pch.h>
#include "Include/Diagnostics/DiagnosticsRing.h"
#include "Include/Diagnostics/MenuPerf.h"
#include <PerfExport.h>

namespace Nilesoft
{
	namespace Shell
	{
		namespace Diagnostics
		{
			// One slot per thread. A menu session has exactly one writer for as
			// long as it is open, which is what lets the accumulate path be
			// lock-free; the only synchronisation is the single publish at the
			// end. See the header for why nesting folds rather than stacks.
			static thread_local SessionSlot t_session;

			SessionSlot &current_session() noexcept
			{
				return t_session;
			}

			/*
				Mirror one finished session into this process's shared block, so
				`shell.exe -report perf` can read it from outside.

				Off the measured path by construction: session_end() has already
				stopped the clock and published into the in-process ring before
				this runs, and the menu itself closed before that. The cost is
				one memcpy of about 1.5 KB.

				The copy is not a cast. PhaseRecord::name is a `const wchar_t *`
				into this process's own image, which means nothing at all in the
				process doing the reading, so the characters are copied.
				src/shared/PerfExport.h has the rest of the reasoning.
			*/
			static void export_session(const MenuSessionRecord &record) noexcept
			{
				PerfExportRecord out{};
				out.tick = record.tick;
				out.host_hash = record.host_hash;
				out.host_flags = record.host_flags;
				out.total_microseconds = record.total_microseconds;
				out.decision = static_cast<uint32_t>(record.decision);
				out.dropped_phases = record.dropped_phases;
				out.dropped_providers = record.dropped_providers;

				auto phases = record.phase_count;
				if(phases > PERF_EXPORT_PHASES)
				{
					// The export holds fewer phases than the ring. Losing the
					// tail silently is the failure the ring's own header warns
					// about, so the overflow joins the count that already says
					// how many did not fit.
					auto lost = static_cast<uint32_t>(phases - PERF_EXPORT_PHASES);
					out.dropped_phases += lost;
					phases = PERF_EXPORT_PHASES;
				}
				out.phase_count = phases;

				for(uint8_t i = 0; i < phases; i++)
				{
					out.phases[i].microseconds = record.phases[i].microseconds;
					out.phases[i].count = record.phases[i].count;

					auto name = record.phases[i].name;
					size_t at = 0;
					if(name)
					{
						for(; name[at] && at + 1 < PERF_EXPORT_NAME; at++)
							out.phases[i].name[at] = name[at];
					}
					out.phases[i].name[at] = L'\0';
				}

				auto providers = record.provider_count;
				if(providers > PERF_EXPORT_PROVIDERS)
				{
					out.dropped_providers += static_cast<uint32_t>(providers - PERF_EXPORT_PROVIDERS);
					providers = PERF_EXPORT_PROVIDERS;
				}
				out.provider_count = providers;

				for(uint8_t i = 0; i < providers; i++)
				{
					out.providers[i].clsid_hash = record.providers[i].clsid_hash;
					out.providers[i].microseconds = record.providers[i].microseconds;
					out.providers[i].result = static_cast<uint32_t>(record.providers[i].result);
				}

				PerfExportWriter::instance().store(out);
			}

			void session_begin(uint32_t host_hash, uint32_t host_flags) noexcept
			{
				if(t_session.depth++ > 0)
					return;			// nested: fold into the session already open

				t_session.record.reset();
				t_session.record.tick = ::GetTickCount64();
				t_session.record.host_hash = host_hash;
				t_session.record.host_flags = host_flags;
				::QueryPerformanceCounter(&t_session.started);
			}

			void session_phase(const wchar_t *name, uint32_t microseconds, int32_t count) noexcept
			{
				if(t_session.depth <= 0)
					return;
				t_session.record.add_phase(name, microseconds, count);
			}

			void session_provider(uint32_t clsid_hash, uint32_t microseconds,
								  ProviderResult result) noexcept
			{
				if(t_session.depth <= 0)
					return;
				t_session.record.add_provider(clsid_hash, microseconds, result);
			}

			void provider_identity(uint32_t clsid_hash, const GUID &clsid) noexcept
			{
				// Every candidate, before anything decides to skip it. The
				// CLSID is what the report prints and what -quarantine:add
				// takes, so a provider Shell declined to ask is precisely one
				// the user has to be able to name.
				PerfExportWriter::instance().note_provider(clsid_hash, clsid);
			}

			void provider_name(uint32_t clsid_hash, const GUID &clsid, const wchar_t *name) noexcept
			{
				// No session check: this is a property of the provider, not of
				// the menu that happened to activate it, and it stays useful
				// after that menu's record has aged out of the ring.
				PerfExportWriter::instance().note_provider(clsid_hash, clsid, name);
			}

			void session_decision(TakeoverDecision decision) noexcept
			{
				if(t_session.depth <= 0)
					return;

				// The outermost decision wins. A nested menu that failed open
				// says nothing about how the menu around it resolved.
				if(t_session.depth == 1 || t_session.record.decision == TakeoverDecision::Unknown)
					t_session.record.decision = decision;
			}

			void session_end() noexcept
			{
				if(t_session.depth <= 0)
				{
					// No matching begin. A hook path that bailed before starting
					// one lands here, and doing nothing is the right answer -
					// clamping keeps an unbalanced end() from making depth
					// negative and swallowing the next real session.
					t_session.depth = 0;
					return;
				}

				if(--t_session.depth > 0)
					return;

				LARGE_INTEGER now{};
				::QueryPerformanceCounter(&now);
				auto per_ms = MenuPerf::ticks_per_ms();
				if(per_ms > 0.0)
				{
					auto ms = static_cast<double>(now.QuadPart - t_session.started.QuadPart) / per_ms;
					if(ms < 0.0)
						ms = 0.0;
					t_session.record.total_microseconds = static_cast<uint32_t>(ms * 1000.0);
				}

				DiagnosticsRing::instance().publish(t_session.record);
				export_session(t_session.record);
			}
		}
	}
}
