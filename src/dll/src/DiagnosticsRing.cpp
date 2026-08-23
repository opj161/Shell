#include <pch.h>
#include "Include/Diagnostics/DiagnosticsRing.h"
#include "Include/Diagnostics/MenuPerf.h"

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

			void session_begin(uint32_t host_hash) noexcept
			{
				if(t_session.depth++ > 0)
					return;			// nested: fold into the session already open

				t_session.record.reset();
				t_session.record.tick = ::GetTickCount64();
				t_session.record.host_hash = host_hash;
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
			}
		}
	}
}
