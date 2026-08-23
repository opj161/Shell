#include "test.h"

#include <windows.h>
#include "Include/ProviderHealth.h"

// Which packaged verb handlers get asked, and which wait for the next menu.
//
// The measurements this policy exists for are in Include/ProviderHealth.h:
// 23 handlers, ~700 ms on the first menu in a process and ~170 ms on every menu
// after it, all of it on the thread between the right-click and the first pixel.
//
// The tests that matter most here are not the obvious ones. A policy that defers
// slow providers is easy to write and easy to write *wrongly*, in a way that
// looks fine in isolation and empties the menu in practice - because on the
// first menu every provider is cold and therefore slow. The last two tests
// simulate that sequence directly.

using namespace Nilesoft::Shell;

namespace
{
	constexpr uint32_t A = 0xAAAA0001;
	constexpr uint32_t B = 0xBBBB0002;

	constexpr uint32_t FAST_US = 2000;			// what a warm provider costs here
	constexpr uint32_t COLD_US = 60000;			// what any of them costs cold
	constexpr uint32_t PATHOLOGICAL_US = 2000000;	// the fake provider in the plan's acceptance test

	// Two samples is the threshold for judging at all, so this is "it has now
	// been seen enough times to be condemned".
	void sample(ProviderHealth &health, uint32_t clsid, uint32_t us, int times = 1)
	{
		for(int i = 0; i < times; i++)
			health.record(clsid, us, true);
	}
}

TEST(provider_health, an_unknown_provider_is_always_tried)
{
	ProviderHealth health;
	CHECK(health.consider(A, MENU_BUDGET_US) == ProviderVerdict::Try);
}

TEST(provider_health, an_unknown_provider_is_deferred_only_when_nothing_is_left)
{
	ProviderHealth health;
	CHECK(health.consider(A, 0) == ProviderVerdict::DeferBudget);
}

TEST(provider_health, a_provider_that_has_been_quick_keeps_its_place)
{
	ProviderHealth health;
	sample(health, A, FAST_US, 2);

	CHECK(health.consider(A, MENU_BUDGET_US) == ProviderVerdict::Try);
}

TEST(provider_health, a_provider_that_has_never_been_quick_is_deferred)
{
	ProviderHealth health;
	sample(health, A, PATHOLOGICAL_US, 2);

	CHECK(health.consider(A, MENU_BUDGET_US) == ProviderVerdict::DeferSlow);
}

TEST(provider_health, one_slow_sample_is_not_enough_to_condemn_a_provider)
{
	// The cold-start rule. A single sample is always the cold one, and a policy
	// that acted on it would defer a provider that is perfectly quick from its
	// second appearance onward.
	ProviderHealth health;
	sample(health, A, COLD_US, 1);

	CHECK(health.consider(A, MENU_BUDGET_US) == ProviderVerdict::Try);
}

TEST(provider_health, one_quick_answer_rehabilitates_a_provider_permanently)
{
	ProviderHealth health;
	sample(health, A, COLD_US, 2);
	CHECK(health.consider(A, MENU_BUDGET_US) == ProviderVerdict::DeferSlow);

	// Judged on its best time, so a single quick answer is enough - which is
	// what makes a cold first sample survivable rather than fatal.
	health.record(A, FAST_US, true);
	CHECK(health.consider(A, MENU_BUDGET_US) == ProviderVerdict::Try);
	CHECK(health.consider(A, MENU_BUDGET_US) == ProviderVerdict::Try);
}

TEST(provider_health, a_deferred_provider_is_tried_again_eventually)
{
	// A deferral must not be a life sentence: an application update can make a
	// slow handler fast and nothing else would ever find out.
	//
	// This test caught a real defect rather than confirming one. The re-probe
	// originally fell through into the affordability check below it - and a
	// provider being re-probed *never* affords, because being slow is why it was
	// deferred. The re-probe therefore never happened, and a provider excluded
	// once stayed excluded for the life of the process.
	ProviderHealth health;
	sample(health, A, PATHOLOGICAL_US, 2);

	int deferred = 0;
	for(int i = 0; i < REPROBE_AFTER * 2; i++)
	{
		if(health.consider(A, MENU_BUDGET_US) == ProviderVerdict::DeferSlow)
			deferred++;
		else
			break;
	}

	CHECK_EQ(deferred, REPROBE_AFTER - 1);
}

TEST(provider_health, a_reprobe_does_not_start_in_a_menu_with_nothing_left)
{
	// The re-probe ignores affordability, so the one thing left to check is that
	// it does not begin just as a menu runs out of budget - which is the worst
	// possible moment to pay a slow provider's full cost.
	ProviderHealth health;
	sample(health, A, PATHOLOGICAL_US, 2);

	for(int i = 0; i < REPROBE_AFTER * 2; i++)
		CHECK(health.consider(A, 0) == ProviderVerdict::DeferSlow);

	// And it is not lost - the next menu with room in it takes the probe.
	CHECK(health.consider(A, MENU_BUDGET_US) == ProviderVerdict::Try);
}

TEST(provider_health, a_provider_that_cannot_fit_in_what_is_left_waits)
{
	ProviderHealth health;
	sample(health, A, 10000, 2);		// it costs 10 ms

	CHECK(health.consider(A, 20000) == ProviderVerdict::Try);
	CHECK(health.consider(A, 5000) == ProviderVerdict::DeferBudget);
}

TEST(provider_health, deferrals_and_failures_are_counted_for_reporting)
{
	ProviderHealth health;
	sample(health, A, PATHOLOGICAL_US, 2);
	health.consider(A, MENU_BUDGET_US);
	health.consider(A, MENU_BUDGET_US);
	health.record(A, FAST_US, false);

	ProviderTiming timing;
	CHECK(health.lookup(A, &timing));
	CHECK_EQ(timing.deferrals, 2);
	CHECK_EQ(timing.failures, 1);
	CHECK_EQ(timing.worst_us, PATHOLOGICAL_US);
	CHECK_EQ(timing.best_us, FAST_US);
}

TEST(provider_health, providers_are_tracked_separately)
{
	ProviderHealth health;
	sample(health, A, PATHOLOGICAL_US, 2);
	sample(health, B, FAST_US, 2);

	CHECK(health.consider(A, MENU_BUDGET_US) == ProviderVerdict::DeferSlow);
	CHECK(health.consider(B, MENU_BUDGET_US) == ProviderVerdict::Try);
}

TEST(provider_health, an_unseen_provider_reports_nothing_rather_than_zeroes)
{
	ProviderHealth health;
	ProviderTiming timing;
	CHECK(!health.lookup(A, &timing));
}

TEST(provider_health, the_hash_separates_two_clsids_that_differ_in_one_bit)
{
	GUID a{ 0x1FA0E654, 0xC9F2, 0x4A1F, { 0x98, 0x00, 0xB9, 0xA7, 0x5D, 0x74, 0x4B, 0x00 } };
	GUID b{ 0x1FA0E654, 0xC9F2, 0x4A1F, { 0x98, 0x00, 0xB9, 0xA7, 0x5D, 0x74, 0x4B, 0x01 } };

	// Real registrations on this machine differ exactly like this - OneDrive
	// registers six handlers whose GUIDs differ only in the last byte - so a
	// hash that folded them together would attribute all six to one timing.
	CHECK(provider_hash(a) != provider_hash(b));
}

TEST(provider_health, a_cold_first_menu_does_not_progressively_empty_the_menu)
{
	// The failure this policy was one line away from having, played out.
	//
	// Twenty-three handlers, all cold and slow on the first menu, with a budget
	// that only lets a couple be sampled per menu. If a single slow sample
	// condemned a provider, each menu would quietly mark two more as slow and
	// within a dozen right-clicks the menu would contain no packaged verbs at
	// all - a far worse outcome than the latency being fixed, and one that would
	// look like a different bug entirely.
	ProviderHealth health;
	const uint32_t COUNT = 23;

	// Menu 1: everything is unknown and cold. Whatever fits the budget gets one
	// slow sample.
	for(uint32_t i = 0; i < COUNT; i++)
	{
		if(health.consider(0x1000 + i, MENU_BUDGET_US) == ProviderVerdict::Try)
			health.record(0x1000 + i, COLD_US, true);
	}

	// Menu 2: they have one sample each, which is not enough to judge. Every one
	// of them must still be tried, and now they answer warm.
	uint32_t tried = 0;
	for(uint32_t i = 0; i < COUNT; i++)
	{
		if(health.consider(0x1000 + i, MENU_BUDGET_US) == ProviderVerdict::Try)
		{
			tried++;
			health.record(0x1000 + i, FAST_US, true);
		}
	}
	CHECK_EQ(tried, COUNT);

	// Menu 3 onward: all of them have a fast best time, so none is ever deferred
	// for being slow again.
	for(uint32_t i = 0; i < COUNT; i++)
		CHECK(health.consider(0x1000 + i, MENU_BUDGET_US) == ProviderVerdict::Try);
}

TEST(provider_health, a_genuinely_pathological_provider_is_still_caught)
{
	// The other half of the same story: the rule that protects a cold provider
	// must not protect one that is simply slow. Two samples, then it is out.
	ProviderHealth health;

	CHECK(health.consider(A, MENU_BUDGET_US) == ProviderVerdict::Try);
	health.record(A, PATHOLOGICAL_US, true);

	CHECK(health.consider(A, MENU_BUDGET_US) == ProviderVerdict::Try);
	health.record(A, PATHOLOGICAL_US, true);

	CHECK(health.consider(A, MENU_BUDGET_US) == ProviderVerdict::DeferSlow);
}

TEST(provider_budget, a_fresh_budget_has_all_of_it_and_spends_down)
{
	auto budget = ProviderBudget::begin(50000);

	// Nothing has happened yet, so essentially all of it is left. The bound is
	// loose on purpose: this asserts the arithmetic, not the clock.
	CHECK(budget.remaining_us() > 40000);
	CHECK(budget.remaining_us() <= 50000);

	::Sleep(1);
	CHECK(budget.spent_us() > 0);
}

TEST(provider_budget, an_exhausted_budget_reports_zero_rather_than_wrapping)
{
	// remaining_us() is unsigned. Subtracting past zero would produce an
	// enormous allowance and turn the budget into no budget at all.
	auto budget = ProviderBudget::begin(1);
	::Sleep(5);

	CHECK_EQ(budget.remaining_us(), 0u);
}
