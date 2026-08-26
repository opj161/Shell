#include "test.h"

#include <windows.h>
#include "Include/ProviderHealth.h"
#include "Include/ProviderSchedule.h"

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

	// Every test below that is about the *rules* rather than the key uses one
	// shape throughout, so the rule reads without a bucket argument on every
	// line. The shape key has its own tests at the end of the file.
	constexpr auto ONE = SelectionShape::Single;

	// Two samples is the threshold for judging at all, so this is "it has now
	// been seen enough times to be condemned".
	void sample(ProviderHealth &health, uint32_t clsid, uint32_t us, int times = 1,
				SelectionShape shape = ONE)
	{
		for(int i = 0; i < times; i++)
			health.record(clsid, shape, us, true);
	}

	enum class Decision { Try, DeferSlow, DeferBudget };

	/*
		One provider through the admission sequence the product actually runs.

		These tests used to call ProviderHealth::consider(), which was the
		original single-pass policy: it decided and mutated in one step. The
		two-phase scheduler replaced it - planning reads through classify() and
		the decisions that change state are named (note_slow_deferral,
		note_reprobe_started, note_budget_deferral) - and consider() was left
		behind with no caller in the product and twenty-nine here.

		That is the shape AGENTS.md warns about in as many words: a test that
		only ever calls a function the way the test calls it. Twenty-two tests
		were pinning a policy that no longer shipped, while the policy that did
		ship carried a defect - a known provider whose estimate exceeded the
		whole menu budget could never be admitted, never be called, and so never
		correct the estimate that refused it - that none of them could see,
		because none of them ran it.

		So consider() is gone and this helper takes its place, and it is a
		transcription of ExplorerCommand.cpp's loop rather than a convenience:
		classify, build the candidate, plan, charge the deferral or check the
		live budget, in that order. If the product's sequence changes and this
		does not, the tests below stop describing the product - which is exactly
		the failure being corrected, so keep the two in step.
	*/
	Decision decide(ProviderHealth &health, uint32_t clsid, SelectionShape shape,
					uint32_t budget_remaining_us)
	{
		ProviderCandidate candidate;
		candidate.ordinal = 0;
		candidate.hash = clsid;

		ProviderTiming timing;
		if(health.classify(clsid, shape, &timing))
		{
			candidate.has_timing = true;
			candidate.samples = timing.samples;
			candidate.best_us = timing.best_us;
			candidate.last_us = timing.last_us;

			if(timing.samples >= MIN_SAMPLES_TO_JUDGE
			   && timing.best_us > SLOW_PROVIDER_US)
			{
				candidate.slow = true;
				candidate.reprobe_due =
					static_cast<uint32_t>(timing.since_probe) + 1 >= REPROBE_AFTER;
			}

			candidate.budget_reprobe_due =
				static_cast<uint32_t>(timing.budget_deferrals) + 1 >= BUDGET_REPROBE_AFTER;
		}

		auto plan = plan_providers({ candidate }, health.next_exploration_cursor());

		// Planned-out before the menu started: slow and not due, or a prediction
		// no menu could satisfy.
		for(const auto &skipped : plan.deferred)
		{
			if(skipped.why == ProviderDeferral::Budget)
			{
				health.note_budget_deferral(skipped.hash, shape);
				return Decision::DeferBudget;
			}
			health.note_slow_deferral(skipped.hash, shape);
			return Decision::DeferSlow;
		}

		for(const auto &step : plan.order)
		{
			// Affordability first, then the re-probe is granted - so a probe
			// that is refused by the live budget does not reset its counter and
			// lose its turn.
			if(!provider_step_fits(step, budget_remaining_us))
			{
				health.note_budget_deferral(step.hash, shape);
				return Decision::DeferBudget;
			}

			if(step.call == ProviderCall::Reprobe)
				health.note_reprobe_started(step.hash, shape);

			return Decision::Try;
		}

		return Decision::Try;		// one candidate always produces one outcome
	}
}

TEST(provider_health, an_unknown_provider_is_always_tried)
{
	ProviderHealth health;
	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::Try);
}

TEST(provider_health, an_unknown_provider_is_deferred_only_when_nothing_is_left)
{
	ProviderHealth health;
	CHECK(decide(health, A, ONE, 0) == Decision::DeferBudget);
}

TEST(provider_health, a_provider_that_has_been_quick_keeps_its_place)
{
	ProviderHealth health;
	sample(health, A, FAST_US, 2);

	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::Try);
}

TEST(provider_health, a_provider_that_has_never_been_quick_is_deferred)
{
	ProviderHealth health;
	sample(health, A, PATHOLOGICAL_US, 2);

	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::DeferSlow);
}

TEST(provider_health, one_slow_sample_is_not_enough_to_condemn_a_provider)
{
	// The cold-start rule. A single sample is always the cold one, and a policy
	// that acted on it would defer a provider that is perfectly quick from its
	// second appearance onward.
	ProviderHealth health;
	sample(health, A, COLD_US, 1);

	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::Try);
}

TEST(provider_health, one_quick_answer_rehabilitates_a_provider_permanently)
{
	ProviderHealth health;
	sample(health, A, COLD_US, 2);
	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::DeferSlow);

	// Judged on its best time, so a single quick answer is enough - which is
	// what makes a cold first sample survivable rather than fatal.
	health.record(A, ONE, FAST_US, true);
	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::Try);
	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::Try);
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
		if(decide(health, A, ONE, MENU_BUDGET_US) == Decision::DeferSlow)
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
	//
	// Under the shipped two-phase policy the *reason* differs from the one
	// consider() gave, and the difference is an improvement rather than a
	// relaxation. consider() answered DeferSlow throughout, because it decided
	// the re-probe and the affordability in one step. Planning and admission are
	// now separate: once the provider is due, the planner emits a Reprobe step
	// and the live check refuses it for budget. So the assertion is on the
	// property that matters - it is never *tried* in a menu with nothing left -
	// rather than on which of two deferral words was printed.
	ProviderHealth health;
	sample(health, A, PATHOLOGICAL_US, 2);

	for(int i = 0; i < REPROBE_AFTER * 2; i++)
		CHECK(decide(health, A, ONE, 0) != Decision::Try);

	// And it is not lost - the next menu with room in it takes the probe.
	// note_reprobe_started is granted only after the affordability check, so
	// none of the refusals above consumed the turn.
	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::Try);
}

TEST(provider_health, a_provider_that_cannot_fit_in_what_is_left_waits)
{
	ProviderHealth health;
	sample(health, A, 10000, 2);		// it costs 10 ms

	CHECK(decide(health, A, ONE, 20000) == Decision::Try);
	CHECK(decide(health, A, ONE, 5000) == Decision::DeferBudget);
}

TEST(provider_health, deferrals_and_failures_are_counted_for_reporting)
{
	ProviderHealth health;
	sample(health, A, PATHOLOGICAL_US, 2);
	decide(health, A, ONE, MENU_BUDGET_US);
	decide(health, A, ONE, MENU_BUDGET_US);
	health.record(A, ONE, FAST_US, false);

	ProviderTiming timing;
	CHECK(health.lookup(A, ONE, &timing));
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

	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::DeferSlow);
	CHECK(decide(health, B, ONE, MENU_BUDGET_US) == Decision::Try);
}

TEST(provider_health, an_unseen_provider_reports_nothing_rather_than_zeroes)
{
	ProviderHealth health;
	ProviderTiming timing;
	CHECK(!health.lookup(A, ONE, &timing));
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
		if(decide(health, 0x1000 + i, ONE, MENU_BUDGET_US) == Decision::Try)
			health.record(0x1000 + i, ONE, COLD_US, true);
	}

	// Menu 2: they have one sample each, which is not enough to judge. Every one
	// of them must still be tried, and now they answer warm.
	uint32_t tried = 0;
	for(uint32_t i = 0; i < COUNT; i++)
	{
		if(decide(health, 0x1000 + i, ONE, MENU_BUDGET_US) == Decision::Try)
		{
			tried++;
			health.record(0x1000 + i, ONE, FAST_US, true);
		}
	}
	CHECK_EQ(tried, COUNT);

	// Menu 3 onward: all of them have a fast best time, so none is ever deferred
	// for being slow again.
	for(uint32_t i = 0; i < COUNT; i++)
		CHECK(decide(health, 0x1000 + i, ONE, MENU_BUDGET_US) == Decision::Try);
}

TEST(provider_health, a_genuinely_pathological_provider_is_still_caught)
{
	// The other half of the same story: the rule that protects a cold provider
	// must not protect one that is simply slow. Two samples, then it is out.
	ProviderHealth health;

	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::Try);
	health.record(A, ONE, PATHOLOGICAL_US, true);

	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::Try);
	health.record(A, ONE, PATHOLOGICAL_US, true);

	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::DeferSlow);
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

/*
	The selection shape is part of the key.

	docs/refactor/02-first-paint-latency.md section 2a specifies the key as
	`(clsid, selection_shape)`; the first implementation dropped the shape, and
	on 2026-08-25 that was measured to matter in a real Explorer. With 200 files
	selected, one handler cost 209 ms and the whole menu 634 ms, three times
	running, against ~10 ms for the same menu over one file. Nothing deferred it,
	because judgement is on a provider's best-ever time and its best had been
	measured on a single-file selection.

	These are the tests that fail if the shape is dropped from the key again.
*/
TEST(provider_health, a_cost_learned_on_one_file_says_nothing_about_two_hundred)
{
	ProviderHealth health;

	// Fast over a single file, twice, so it is judgeable.
	sample(health, A, FAST_US, 2, SelectionShape::Single);

	// A large selection has never been measured, so there is nothing to judge
	// on and the provider is tried rather than assumed affordable.
	ProviderTiming timing{};
	CHECK(!health.lookup(A, SelectionShape::Many, &timing));

	// And it is judged separately once it has been.
	health.record(A, SelectionShape::Many, PATHOLOGICAL_US, true);
	health.record(A, SelectionShape::Many, PATHOLOGICAL_US, true);

	CHECK(decide(health, A, SelectionShape::Single, MENU_BUDGET_US) == Decision::Try);
	CHECK(decide(health, A, SelectionShape::Many, MENU_BUDGET_US) == Decision::DeferSlow);
}

// The direction that costs a user their menu items rather than their time:
// being slow over two hundred files must not exclude a provider from the
// single-file menu, which is the common case and where it is fast.
TEST(provider_health, being_slow_over_many_does_not_condemn_the_single_case)
{
	ProviderHealth health;

	sample(health, A, PATHOLOGICAL_US, 2, SelectionShape::Many);
	sample(health, A, FAST_US, 2, SelectionShape::Single);

	CHECK(decide(health, A, SelectionShape::Many, MENU_BUDGET_US) == Decision::DeferSlow);
	CHECK(decide(health, A, SelectionShape::Single, MENU_BUDGET_US) == Decision::Try);
}

TEST(provider_health, each_shape_keeps_its_own_timing)
{
	ProviderHealth health;

	health.record(A, SelectionShape::Single, FAST_US, true);
	health.record(A, SelectionShape::Many, COLD_US, true);

	ProviderTiming single{}, many{};
	CHECK(health.lookup(A, SelectionShape::Single, &single));
	CHECK(health.lookup(A, SelectionShape::Many, &many));

	CHECK_EQ(single.best_us, FAST_US);
	CHECK_EQ(many.best_us, COLD_US);
	CHECK_EQ(health.snapshot().size(), 2u);
}

// The buckets themselves. The boundaries are behavioural rather than round, so
// they are stated once here instead of being rediscovered from the branch.
TEST(provider_health, the_shape_buckets_are_where_they_say_they_are)
{
	CHECK(selection_shape(0) == SelectionShape::Background);
	CHECK(selection_shape(1) == SelectionShape::Single);
	CHECK(selection_shape(2) == SelectionShape::Few);
	CHECK(selection_shape(16) == SelectionShape::Few);
	CHECK(selection_shape(17) == SelectionShape::Many);
	CHECK(selection_shape(200) == SelectionShape::Many);
}

// A background click and a one-item selection are different work, and were the
// same key before. Explorer raises far more of the first than of anything else,
// so folding them together would let the commonest menu on the desktop be
// judged by the cost of a different one.
TEST(provider_health, a_background_click_is_not_a_selection_of_one)
{
	ProviderHealth health;

	sample(health, A, PATHOLOGICAL_US, 2, SelectionShape::Background);

	CHECK(decide(health, A, SelectionShape::Background, MENU_BUDGET_US) == Decision::DeferSlow);
	CHECK(decide(health, A, SelectionShape::Single, MENU_BUDGET_US) == Decision::Try);
}

// ---- one spike must not be a life sentence ------------------------------
//
// The whole sequence, through the shipping path, because a snapshot of any one
// step in it looks reasonable. This is the defect the two-phase scheduler
// introduced and that twenty-nine tests against the retired consider() could
// not see.

TEST(provider_health, a_single_spike_does_not_exclude_a_provider_for_the_process_lifetime)
{
	ProviderHealth health;

	// Two samples. The second is a spike above the whole menu budget - ordinary
	// rather than exotic: section 02.2a records cold handlers at ~30 ms each and
	// one at 209 ms on a large selection.
	health.record(A, ONE, FAST_US, true);
	health.record(A, ONE, MENU_BUDGET_US + 20000, true);

	// It is not slow. best_us is 2 ms, far under SLOW_PROVIDER_US, so no amount
	// of REPROBE_AFTER can ever apply to it.
	ProviderTiming timing;
	CHECK(health.lookup(A, ONE, &timing));
	CHECK(timing.best_us <= SLOW_PROVIDER_US);

	// And yet it is refused against a completely unspent budget, because the
	// estimate is max(best, last).
	int refused = 0;
	auto last = Decision::DeferBudget;
	for(int i = 0; i < BUDGET_REPROBE_AFTER * 3; i++)
	{
		last = decide(health, A, ONE, MENU_BUDGET_US);
		if(last != Decision::DeferBudget)
			break;
		refused++;
	}

	// Exactly BUDGET_REPROBE_AFTER - 1 refusals, and then a turn. Before the
	// fix this loop ran to its limit and the provider was gone for good: it was
	// never called, so record() never ran, so last_us could never come down and
	// best_us could never rise into "slow".
	CHECK_EQ(refused, BUDGET_REPROBE_AFTER - 1);
	CHECK(last == Decision::Try);

	// The forced turn was granted, so the streak restarts rather than the
	// provider being due on every menu from now on.
	CHECK(health.lookup(A, ONE, &timing));
	CHECK_EQ(timing.budget_deferrals, 0);

	// It answers quickly, which is the point of asking: the estimate that
	// refused it is corrected by the only thing that can correct it.
	health.record(A, ONE, FAST_US, true);
	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::Try);
	CHECK(decide(health, A, ONE, MENU_BUDGET_US) == Decision::Try);
}

TEST(provider_health, a_budget_deferral_still_does_not_consume_a_slow_providers_turn)
{
	// The two counters must stay independent. A provider refused for cost has
	// not had its turn - that is why note_budget_deferral leaves since_probe
	// alone - and adding budget_deferrals must not have quietly changed it.
	ProviderHealth health;
	sample(health, A, 10000, 2);

	for(int i = 0; i < 5; i++)
		CHECK(decide(health, A, ONE, 1000) == Decision::DeferBudget);

	ProviderTiming timing;
	CHECK(health.lookup(A, ONE, &timing));
	CHECK_EQ(timing.since_probe, 0);
	CHECK_EQ(timing.budget_deferrals, 5);
	CHECK_EQ(timing.deferrals, 5);
}

TEST(provider_health, any_answer_ends_the_budget_deferral_streak)
{
	ProviderHealth health;
	sample(health, A, 10000, 2);

	for(int i = 0; i < 5; i++)
		CHECK(decide(health, A, ONE, 1000) == Decision::DeferBudget);

	// A failed call is still an answer: it updates last_us, so the estimate that
	// caused the refusals is no longer unchallenged.
	health.record(A, ONE, FAST_US, false);

	ProviderTiming timing;
	CHECK(health.lookup(A, ONE, &timing));
	CHECK_EQ(timing.budget_deferrals, 0);
}
