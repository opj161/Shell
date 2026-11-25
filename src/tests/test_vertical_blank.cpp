#include "test.h"

#include "..\dll\src\Include\VerticalBlank.h"

// The sleep that lands a popup on the next vertical blank.
//
// Every case here is one a running desktop never produces, which is why the
// arithmetic was extracted from ContextMenu.cpp to be reachable at all. The
// first of them was live: DwmGetCompositionTimingInfo fills DWM_TIMING_INFO
// only "when this function returns successfully", the caller zeroed it and
// discarded the HRESULT, and a refresh period of 0 was divided into.
// https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcompositiontiminginfo

using namespace Nilesoft::Shell;

namespace
{
	// A 60 Hz display on a 10 MHz performance counter, which is what this
	// machine and most others report.
	constexpr long long FREQ = 10'000'000;
	constexpr long long PERIOD = FREQ / 60;		// 166666 ticks, 16.67 ms
	constexpr long long NOW = 1'000'000'000;
}

TEST(vertical_blank, a_failed_timing_call_asks_for_no_wait_at_all)
{
	// The whole structure is zero because DwmGetCompositionTimingInfo failed
	// and left the caller's memset behind. This used to divide by zero and
	// terminate explorer.exe.
	CHECK(vblank_wait_ms(0, 0, NOW, FREQ) == -1);
}

TEST(vertical_blank, a_zero_performance_frequency_asks_for_no_wait)
{
	CHECK(vblank_wait_ms(NOW + PERIOD, PERIOD, NOW, 0) == -1);
}

TEST(vertical_blank, a_negative_refresh_period_asks_for_no_wait)
{
	CHECK(vblank_wait_ms(NOW, -PERIOD, NOW, FREQ) == -1);
}

TEST(vertical_blank, a_refresh_period_slower_than_one_hertz_is_not_a_display)
{
	// Also what keeps the millisecond conversion inside int64.
	CHECK(vblank_wait_ms(NOW, FREQ + 1, NOW, FREQ) == -1);
}

TEST(vertical_blank, a_vblank_one_full_period_ahead_waits_a_whole_frame)
{
	// dt is exactly one period, so the phase is the whole period... which is
	// the same instant as zero. Floor division puts it at zero.
	CHECK(vblank_wait_ms(NOW + PERIOD, PERIOD, NOW, FREQ) == 0);
}

TEST(vertical_blank, a_vblank_half_a_period_ahead_waits_half_a_frame)
{
	auto ms = vblank_wait_ms(NOW + PERIOD / 2, PERIOD, NOW, FREQ);
	CHECK(ms == 8);		// 8.33 ms, rounded
}

TEST(vertical_blank, a_vblank_in_the_past_wraps_into_the_next_period)
{
	// DWM reports "SOME vertical blank - past or future, possibly many frames
	// away". A blank 10.5 periods behind leaves half a period to wait.
	auto behind = NOW - (PERIOD * 10 + PERIOD / 2);
	CHECK(vblank_wait_ms(behind, PERIOD, NOW, FREQ) == 8);
}

TEST(vertical_blank, a_vblank_an_exact_number_of_periods_in_the_past_waits_nothing)
{
	// The off-by-one the old form had: `-1 + dt / period` is the floor for
	// every negative dt except an exact multiple, where it answers a whole
	// period and sleeps a frame for nothing. Release builds compiled out the
	// assert that would have caught it.
	CHECK(vblank_wait_ms(NOW - PERIOD * 3, PERIOD, NOW, FREQ) == 0);
}

TEST(vertical_blank, a_vblank_many_periods_ahead_still_waits_less_than_one_frame)
{
	// The wait is bounded by one refresh period however far away the blank
	// DWM chose to report is.
	for(int frames = 1; frames <= 500; frames++)
	{
		auto ms = vblank_wait_ms(NOW + PERIOD * frames + PERIOD / 4,
								 PERIOD, NOW, FREQ);
		CHECK(ms >= 0);
		CHECK(ms <= 17);
	}
}

TEST(vertical_blank, an_implausibly_long_wait_is_refused_rather_than_slept)
{
	// A 50 Hz-of-nothing period that would put the UI thread to sleep for
	// longer than the flicker it exists to avoid. The guard is the wait, not
	// the period, so it holds however the inputs conspire.
	auto slow = FREQ / 2;			// half a second
	auto ms = vblank_wait_ms(NOW + slow / 2, slow, NOW, FREQ);
	CHECK(ms == -1);
}

TEST(vertical_blank, a_144_hertz_display_waits_less_than_a_60_hertz_one)
{
	auto fast = FREQ / 144;
	auto ms = vblank_wait_ms(NOW + fast / 2, fast, NOW, FREQ);
	CHECK(ms >= 0);
	CHECK(ms <= 4);
}
