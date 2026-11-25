#pragma once

/*
	How long to sleep to land on the next vertical blank.

	This is the arithmetic of ContextMenu.cpp's fix_ugly_flicker, extracted for
	one reason: the inputs that break it are the ones a running desktop never
	produces. DwmGetCompositionTimingInfo fills DWM_TIMING_INFO only "when this
	function returns successfully"
	(https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcompositiontiminginfo),
	and the caller zeroed the structure first - so a failed call left
	qpcRefreshPeriod at 0, and the code divided by it.

	That is an integer divide by zero on explorer.exe's UI thread, inside
	WM_NCCALCSIZE, with no SEH frame over it and catch(...) unable to catch SEH
	under /EHsc even if there were one (AGENTS.md, "Structured exception
	handling"). It terminates the host. The release build's assert(m < period)
	two lines further down is compiled out and could never have said so.

	Deliberately free of <windows.h> and dwmapi.h: the inputs are four integers
	the caller has already read, so the suite drives this function rather than a
	copy of it. src/tests/test_vertical_blank.cpp.
*/

#include <cstdint>

namespace Nilesoft
{
	namespace Shell
	{
		// A wait longer than this is not describing a display. One refresh
		// period is the documented bound on this wait; 100 ms is four times
		// the slowest panel anybody ships, and sleeping the UI thread for
		// longer than that is worse than the flicker the wait exists to avoid.
		inline constexpr long long VBLANK_MAX_WAIT_MS = 100;

		/*
			Milliseconds to sleep, or -1 for "there is nothing to wait for".

			`qpc_vblank` and `refresh_period` are DWM_TIMING_INFO's qpcVBlank
			and qpcRefreshPeriod; `now` is QueryPerformanceCounter read *after*
			the DWM call, so the call's own cost is inside the answer;
			`frequency` is QueryPerformanceFrequency.

			DWM reports some vertical blank - past or future, possibly many
			frames away - so the phase within the refresh period is what is
			wanted, and that needs a floor division rather than C++'s truncating
			one. The original wrote `-1 + dt / period` for a negative dt, which
			is the floor except when dt is an exact multiple of the period: then
			it answers a full period instead of zero, and sleeps one whole frame
			for nothing. Rare, and the assert that would have caught it does not
			run in Release.
		*/
		inline int vblank_wait_ms(long long qpc_vblank, long long refresh_period,
								  long long now, long long frequency) noexcept
		{
			// The failed-DwmGetCompositionTimingInfo case, and the reason this
			// function exists. QueryPerformanceFrequency is documented never to
			// fail on Windows XP or later, but it is an out-parameter and this
			// costs one comparison.
			if(refresh_period <= 0 || frequency <= 0)
				return -1;

			// A refresh period of a second or more is not a display. This is
			// also what keeps the millisecond conversion below inside int64:
			// it bounds `phase` by `frequency`, and the performance counter
			// runs in the megahertz.
			if(refresh_period > frequency)
				return -1;

			const long long dt = qpc_vblank - now;

			long long whole = dt / refresh_period;
			if(dt < 0 && whole * refresh_period != dt)
				whole--;	// floor: C++ truncates toward zero

			const long long phase = dt - refresh_period * whole;
			// phase is in [0, refresh_period) by construction.

			const long long ms = (1000 * phase + frequency / 2) / frequency;
			if(ms > VBLANK_MAX_WAIT_MS)
				return -1;

			return static_cast<int>(ms);
		}
	}
}
