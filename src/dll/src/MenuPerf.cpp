#include <pch.h>
#include "RegistryConfig.h"
#include "Include/Diagnostics/MenuPerf.h"
#include "Include/Diagnostics/DiagnosticsRing.h"

#define _log Logger::Instance()

namespace Nilesoft
{
	namespace Shell
	{
		namespace Diagnostics
		{
			// Probed once per process. Logger::_write opens, appends and closes the
			// log file for every line, so an always-on timer would put file I/O on
			// the exact path it is meant to measure - hence the explicit opt-in.
			int MenuPerf::probe() noexcept
			{
				int value = 0;
				try
				{
					if(!RegistryConfig::get(nullptr, L"perf", value))
						value = 0;
				}
				catch(...)
				{
					value = 0;
				}
				return value < 0 ? 0 : value;
			}

			// One reporting implementation shared by the scope timer and the
			// SEH-safe mark pair.
			static void emit(const wchar_t *name, double ms, double warn_ms,
							 uint64_t menu_id, long count, long depth) noexcept
			{
				// The ring first, and unconditionally. Every phase lands here
				// whatever the registry says; the floor below decides only
				// whether a line is also written to the log file.
				auto microseconds = ms > 0.0 ? static_cast<uint32_t>(ms * 1000.0) : 0u;
				session_phase(name, microseconds, count >= 0 ? static_cast<int32_t>(count) : -1);

				if(!MenuPerf::logging())
					return;

				auto floor = static_cast<double>(MenuPerf::floor_ms());
				if(floor < warn_ms)
					floor = warn_ms;

				if(ms < floor)
					return;

				try
				{
					// Stack-only formatting: the timer must not allocate to describe
					// itself, and a phase line is never long.
					wchar_t detail[128]{};
					int n = 0;
					if(menu_id)
						n += ::swprintf(detail + n, static_cast<size_t>(std::size(detail) - n),
										L" menu=0x%llx", static_cast<unsigned long long>(menu_id));
					if(count >= 0 && n >= 0)
						n += ::swprintf(detail + n, static_cast<size_t>(std::size(detail) - n),
										L" items=%d", count);
					if(depth >= 0 && n >= 0)
						::swprintf(detail + n, static_cast<size_t>(std::size(detail) - n),
								   L" depth=%d", depth);

					if(ms >= MENUPERF_LOUD_MS)
						_log.warning(L"perf !! %s %.2fms%s", name, ms, detail);
					else if(ms >= MENUPERF_WARN_MS)
						_log.warning(L"perf %s %.2fms%s", name, ms, detail);
					else
						_log.info(L"perf %s %.2fms%s", name, ms, detail);
				}
				catch(...)
				{
				}
			}

			static double elapsed_since(const LARGE_INTEGER &start) noexcept
			{
				LARGE_INTEGER now{};
				::QueryPerformanceCounter(&now);
				auto per_ms = MenuPerf::ticks_per_ms();
				if(per_ms <= 0.0)
					return 0.0;
				return static_cast<double>(now.QuadPart - start.QuadPart) / per_ms;
			}

			MenuPerfMark menu_perf_begin() noexcept
			{
				MenuPerfMark mark;
				mark.armed = MenuPerf::enabled();
				if(mark.armed)
					::QueryPerformanceCounter(&mark.start);
				return mark;
			}

			void menu_perf_end(MenuPerfMark &mark, const wchar_t *name, long count) noexcept
			{
				if(!mark.armed)
					return;
				mark.armed = false;
				emit(name, elapsed_since(mark.start), MENUPERF_DEBUG_MS, 0, count, -1);
			}

			void MenuPerfScope::report() noexcept
			{
				if(!_armed)
					return;

				_armed = false;
				emit(_name, elapsed_since(_start), _warn_ms, _menu_id, _count, _depth);
			}
		}
	}
}
