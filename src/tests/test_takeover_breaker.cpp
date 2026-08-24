// The takeover circuit breaker.
//
// docs/refactor/01-takeover-contract.md section 7: three consecutive failures
// stop this process attempting takeover, a success resets the count, and
// nothing is persisted. The three rules that carry the design are the ones
// worth pinning - consecutive rather than cumulative, exactly one caller told
// it was the one that opened it, and never re-arming.

#include "test.h"

#include "..\dll\src\Include\TakeoverBreaker.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace Nilesoft::Shell;

TEST(takeover_breaker, a_fresh_breaker_attempts_takeover)
{
	TakeoverBreaker breaker;
	CHECK(breaker.should_attempt());
	CHECK(!breaker.is_open());
	CHECK_EQ(breaker.consecutive_failures(), 0u);
	CHECK_EQ(breaker.skipped(), 0ull);
}

TEST(takeover_breaker, two_failures_are_not_enough_to_open_it)
{
	TakeoverBreaker breaker;
	CHECK(!breaker.record_failure());
	CHECK(!breaker.record_failure());

	CHECK(breaker.should_attempt());
	CHECK_EQ(breaker.consecutive_failures(), 2u);
}

TEST(takeover_breaker, the_third_consecutive_failure_opens_it)
{
	TakeoverBreaker breaker;
	breaker.record_failure();
	breaker.record_failure();

	CHECK_MSG(breaker.record_failure(),
			  "the failure that crosses the threshold reports that it did");
	CHECK(breaker.is_open());
	CHECK(!breaker.should_attempt());
}

// The rule that keeps a busy, healthy process from eventually tripping: what
// says "this host will not work" is a run with no success in between.
TEST(takeover_breaker, a_success_resets_the_run)
{
	TakeoverBreaker breaker;
	breaker.record_failure();
	breaker.record_failure();
	breaker.record_success();

	CHECK_EQ(breaker.consecutive_failures(), 0u);

	breaker.record_failure();
	breaker.record_failure();
	CHECK_MSG(breaker.should_attempt(),
			  "four failures with a success among them must not open the breaker");
}

TEST(takeover_breaker, failures_far_apart_never_accumulate)
{
	TakeoverBreaker breaker;
	for(int i = 0; i < 50; i++)
	{
		breaker.record_failure();
		breaker.record_failure();
		breaker.record_success();
	}
	CHECK_MSG(breaker.should_attempt(), "a hundred failures, never three in a row");
}

// Said once. Every later failure has nothing new to report.
TEST(takeover_breaker, only_one_caller_is_told_it_opened_the_breaker)
{
	TakeoverBreaker breaker;
	int announced = 0;
	for(int i = 0; i < 20; i++)
	{
		if(breaker.record_failure())
			announced++;
	}
	CHECK_EQ(announced, 1);
}

// A wedged host must not oscillate between working and not from one click to
// the next, so nothing in the shipping path closes a breaker - not even a
// success.
TEST(takeover_breaker, an_open_breaker_stays_open_even_after_a_success)
{
	TakeoverBreaker breaker;
	breaker.record_failure();
	breaker.record_failure();
	breaker.record_failure();
	CHECK(breaker.is_open());

	breaker.record_success();

	CHECK_MSG(breaker.is_open(), "the breaker never re-arms itself");
	CHECK(!breaker.should_attempt());
}

// "The breaker is open" does not say how much the user has lost. This does.
TEST(takeover_breaker, menus_handed_to_the_host_are_counted)
{
	TakeoverBreaker breaker;
	breaker.record_failure();
	breaker.record_failure();
	breaker.record_failure();

	for(int i = 0; i < 7; i++)
		breaker.record_skipped();

	CHECK_EQ(breaker.skipped(), 7ull);
}

// Several windows in one process raise menus on their own threads, so the
// threshold can be crossed by more than one at once.
TEST(takeover_breaker, exactly_one_thread_is_told_it_opened_the_breaker)
{
	for(int round = 0; round < 20; round++)
	{
		TakeoverBreaker breaker;
		std::atomic<int> announced{ 0 };

		std::vector<std::thread> workers;
		for(int t = 0; t < 8; t++)
		{
			workers.emplace_back([&breaker, &announced]
			{
				for(int i = 0; i < 25; i++)
				{
					if(breaker.record_failure())
						announced.fetch_add(1, std::memory_order_relaxed);
				}
			});
		}
		for(auto &w : workers)
			w.join();

		CHECK_EQ(announced.load(), 1);
		CHECK(breaker.is_open());
	}
}

TEST(takeover_breaker, reset_puts_it_back_the_way_it_started)
{
	TakeoverBreaker breaker;
	breaker.record_failure();
	breaker.record_failure();
	breaker.record_failure();
	breaker.record_skipped();
	breaker.reset();

	CHECK(breaker.should_attempt());
	CHECK(!breaker.is_open());
	CHECK_EQ(breaker.consecutive_failures(), 0u);
	CHECK_EQ(breaker.skipped(), 0ull);
}
