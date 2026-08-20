#pragma once

/*
	Opt-in, high-resolution phase timing for the context-menu path.

	Every optimization in the menu-opening path has to be justified by a number,
	not by feel, so this gives the QA process an answer to "which phase cost the
	milliseconds?" without adding measurable cost when nobody is measuring.

	It is OFF unless the user opts in:

		HKCU\SOFTWARE\Nilesoft\Shell
			perf	REG_DWORD	1

	The value doubles as the floor in milliseconds - 1 keeps the documented
	5/20/100 ms policy, a larger value only reports phases at least that slow.
	That matters because Logger opens, writes and closes the log file on every
	line; leaving it always-on would put file I/O on the very path being
	measured.

	Microsoft's high-resolution interval timer is QueryPerformanceCounter, not
	GetTickCount:
	https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter
*/

#include <profileapi.h>
#include <atomic>

namespace Nilesoft
{
	namespace Shell
	{
		namespace Diagnostics
		{
			// Reported thresholds, in milliseconds.
			inline constexpr double MENUPERF_DEBUG_MS = 5.0;
			inline constexpr double MENUPERF_WARN_MS = 20.0;
			inline constexpr double MENUPERF_LOUD_MS = 100.0;

			class MenuPerf
			{
				// -1 = not probed yet, 0 = off, >0 = floor in milliseconds.
				inline static std::atomic<int> _state{ -1 };

			public:
				// Ticks per millisecond. The frequency is fixed at boot, so it is
				// read once and never re-queried.
				static double ticks_per_ms() noexcept
				{
					static const double v = []() noexcept
					{
						LARGE_INTEGER f{};
						if(!::QueryPerformanceFrequency(&f) || f.QuadPart <= 0)
							return 0.0;
						return static_cast<double>(f.QuadPart) / 1000.0;
					}();
					return v;
				}

				// Explicit override, used by tests and by anything that already
				// knows the setting. Skips the registry probe.
				static void configure(int floor_ms) noexcept
				{
					_state.store(floor_ms < 0 ? 0 : floor_ms, std::memory_order_relaxed);
				}

				static int floor_ms() noexcept
				{
					auto v = _state.load(std::memory_order_relaxed);
					if(v < 0)
					{
						v = probe();
						_state.store(v, std::memory_order_relaxed);
					}
					return v;
				}

				static bool enabled() noexcept
				{
					return floor_ms() > 0 && ticks_per_ms() > 0.0;
				}

			private:
				static int probe() noexcept;
			};

			/*
				Trivially-destructible timing mark.

				Functions that use structured exception handling (__try/__finally)
				cannot hold objects that require unwinding, so the popup hook in
				Main.cpp measures with this pair instead of a scope object.
			*/
			struct MenuPerfMark
			{
				LARGE_INTEGER start{};
				bool armed{};
			};

			MenuPerfMark menu_perf_begin() noexcept;

			// Reports the phase if it breached the floor, then leaves the mark
			// disarmed so a second call is a no-op.
			void menu_perf_end(MenuPerfMark &mark, const wchar_t *name,
							   long count = -1) noexcept;

			/*
				Scoped phase timer. Constructing and destroying one costs a
				QueryPerformanceCounter pair and no allocation; the message is only
				formatted when the phase actually breaches the reporting floor.
			*/
			class MenuPerfScope
			{
			public:
				MenuPerfScope(const wchar_t *name, uint64_t menu_id = 0,
							  double warn_ms = MENUPERF_DEBUG_MS) noexcept
					: _name(name), _menu_id(menu_id), _warn_ms(warn_ms)
				{
					_armed = MenuPerf::enabled();
					if(_armed)
						::QueryPerformanceCounter(&_start);
				}

				~MenuPerfScope() noexcept { report(); }

				MenuPerfScope(const MenuPerfScope &) = delete;
				MenuPerfScope &operator=(const MenuPerfScope &) = delete;

				double elapsed_ms() const noexcept
				{
					if(!_armed)
						return 0.0;
					LARGE_INTEGER now{};
					::QueryPerformanceCounter(&now);
					auto per_ms = MenuPerf::ticks_per_ms();
					if(per_ms <= 0.0)
						return 0.0;
					return static_cast<double>(now.QuadPart - _start.QuadPart) / per_ms;
				}

				// Extra context for the reported line - an item count, a depth, a
				// number of native popups touched.
				void annotate(long count) noexcept { _count = count; }
				void annotate(long count, long depth) noexcept { _count = count; _depth = depth; }

				// Reports early and disarms, for phases that end before the scope does.
				void report() noexcept;

			private:
				const wchar_t *_name{};
				LARGE_INTEGER _start{};
				uint64_t _menu_id{};
				double _warn_ms{ MENUPERF_DEBUG_MS };
				long _count{ -1 };
				long _depth{ -1 };
				bool _armed{};
			};
		}
	}
}
