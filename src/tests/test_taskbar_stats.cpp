// Taskbar hit-test accounting.
//
// docs/refactor/02-first-paint-latency.md section 5 makes the *frequency* of
// the bounded UI Automation wait the acceptance criterion for this path, not
// its duration: "the ring records how often it is taken; that frequency, not
// the wait itself, is the number to drive to zero". Nothing counted it.
//
// These pin the counting, and in particular the two rules that are easy to get
// wrong: a cache hit contributes no time, and the worst wait is a maximum
// rather than whichever thread wrote last.

#include "test.h"

#include "..\dll\src\Include\TaskbarHitStats.h"

#include <thread>
#include <vector>

using namespace Nilesoft::Shell;

namespace
{
	using Outcome = TaskbarHitStats::Outcome;
}

TEST(taskbar_stats, a_fresh_set_of_counters_is_empty)
{
	TaskbarHitStats stats;
	auto s = stats.snapshot();

	CHECK_EQ(s.cache_hits, 0ull);
	CHECK_EQ(s.answered, 0ull);
	CHECK_EQ(s.timed_out, 0ull);
	CHECK_EQ(s.unavailable, 0ull);
	CHECK_EQ(s.waited_microseconds, 0ull);
	CHECK_EQ(s.worst_microseconds, 0u);
	CHECK_EQ(s.waits(), 0ull);
	CHECK_EQ(s.total(), 0ull);
}

TEST(taskbar_stats, each_outcome_lands_in_its_own_counter)
{
	TaskbarHitStats stats;
	stats.record(Outcome::CacheHit, 0);
	stats.record(Outcome::CacheHit, 0);
	stats.record(Outcome::Answered, 2500);
	stats.record(Outcome::TimedOut, 250000);
	stats.record(Outcome::Unavailable, 0);

	auto s = stats.snapshot();
	CHECK_EQ(s.cache_hits, 2ull);
	CHECK_EQ(s.answered, 1ull);
	CHECK_EQ(s.timed_out, 1ull);
	CHECK_EQ(s.unavailable, 1ull);
	CHECK_EQ(s.total(), 5ull);
}

// The whole point of the counters is the ratio of waits to clicks, so a cache
// hit must not be counted as a wait however it is reported.
TEST(taskbar_stats, a_cache_hit_is_not_a_wait_and_costs_no_time)
{
	TaskbarHitStats stats;
	stats.record(Outcome::CacheHit, 999999);
	stats.record(Outcome::Unavailable, 999999);

	auto s = stats.snapshot();
	CHECK_EQ(s.waits(), 0ull);
	CHECK_EQ(s.waited_microseconds, 0ull);
	CHECK_EQ(s.worst_microseconds, 0u);
}

TEST(taskbar_stats, waits_are_the_answered_and_the_timed_out_together)
{
	TaskbarHitStats stats;
	stats.record(Outcome::CacheHit, 0);
	stats.record(Outcome::Answered, 1000);
	stats.record(Outcome::Answered, 3000);
	stats.record(Outcome::TimedOut, 250000);

	auto s = stats.snapshot();
	CHECK_EQ(s.waits(), 3ull);
	CHECK_EQ(s.total(), 4ull);
	CHECK_EQ(s.waited_microseconds, 254000ull);
}

// A plain store would make this "the most recent wait", which is a different
// and far less useful number.
TEST(taskbar_stats, the_worst_wait_is_a_maximum_not_the_last_one)
{
	TaskbarHitStats stats;
	stats.record(Outcome::Answered, 4000);
	stats.record(Outcome::Answered, 250000);
	stats.record(Outcome::Answered, 100);

	CHECK_EQ(stats.snapshot().worst_microseconds, 250000u);
}

TEST(taskbar_stats, reset_clears_every_counter)
{
	TaskbarHitStats stats;
	stats.record(Outcome::CacheHit, 0);
	stats.record(Outcome::Answered, 5000);
	stats.record(Outcome::TimedOut, 250000);
	stats.record(Outcome::Unavailable, 0);
	stats.reset();

	auto s = stats.snapshot();
	CHECK_EQ(s.total(), 0ull);
	CHECK_EQ(s.waited_microseconds, 0ull);
	CHECK_EQ(s.worst_microseconds, 0u);
}

// Taskbars on several monitors are serviced by different threads, so every
// counter is written concurrently in the real thing.
TEST(taskbar_stats, concurrent_recording_loses_nothing)
{
	TaskbarHitStats stats;

	constexpr int THREADS = 4;
	constexpr int PER_THREAD = 2000;

	std::vector<std::thread> workers;
	for(int t = 0; t < THREADS; t++)
	{
		workers.emplace_back([&stats, t]
		{
			for(int i = 0; i < PER_THREAD; i++)
			{
				stats.record(Outcome::CacheHit, 0);
				stats.record(Outcome::Answered, static_cast<uint32_t>(t * 1000 + i));
			}
		});
	}
	for(auto &w : workers)
		w.join();

	auto s = stats.snapshot();
	CHECK_EQ(s.cache_hits, static_cast<uint64_t>(THREADS) * PER_THREAD);
	CHECK_EQ(s.answered, static_cast<uint64_t>(THREADS) * PER_THREAD);
	CHECK_EQ(s.waits(), static_cast<uint64_t>(THREADS) * PER_THREAD);

	// The largest value any thread recorded.
	CHECK_EQ(s.worst_microseconds, static_cast<uint32_t>((THREADS - 1) * 1000 + PER_THREAD - 1));
}
