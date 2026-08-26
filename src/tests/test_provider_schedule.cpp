#include "test.h"

#include <windows.h>
#include "..\dll\src\Include\ProviderSchedule.h"

// Which packaged verb handlers get asked, in what order, and who is left out.
//
// The loop this replaces walked registration order and spent the menu's
// allowance as it went. Measured against the deployed build on 2026-08-25
// (37 handlers, single file, 16 held sessions in `shell.exe -report perf:all`),
// one session at 52.2 ms dropped six providers - four of which cost between
// 0.3 and 1.6 ms and lost their place for sitting *after* one costing 13.9 ms.
// docs/refactor/02-first-paint-latency.md section 2a describes the opposite:
// "a provider that misses its deadline is omitted".
//
// Everything below is the policy alone. It activates nothing and needs no
// desktop, which is the point of extracting it.

using namespace Nilesoft::Shell;

namespace
{
	// A provider that has answered twice at `us` and is therefore predictable.
	ProviderCandidate known(uint32_t ordinal, uint32_t us)
	{
		ProviderCandidate c;
		c.ordinal = ordinal;
		c.hash = 0x1000 + ordinal;
		c.has_timing = true;
		c.samples = MIN_SAMPLES_TO_JUDGE;
		c.best_us = us;
		c.last_us = us;
		c.slow = us > SLOW_PROVIDER_US;
		return c;
	}

	// Never yet sampled enough to be judged.
	ProviderCandidate unknown(uint32_t ordinal)
	{
		ProviderCandidate c;
		c.ordinal = ordinal;
		c.hash = 0x2000 + ordinal;
		return c;
	}

	ProviderCandidate slow(uint32_t ordinal, uint32_t us, bool due)
	{
		auto c = known(ordinal, us);
		c.slow = true;
		c.reprobe_due = due;
		return c;
	}

	/*
		A provider that is cheap at its best and expensive on its last answer.

		The shape that opened the starvation trap, and it is not exotic: two
		samples where the second is a spike. Section 02.2a records cold handlers
		at ~30 ms each and one at 209 ms on a large selection, so a second sample
		above MENU_BUDGET_US is ordinary.

		It is deliberately *not* slow. Judgement is on best_us alone, and 2 ms is
		far under SLOW_PROVIDER_US, so nothing here is condemned - which is
		precisely why the slow-provider re-probe could never rescue it.
	*/
	ProviderCandidate spiked(uint32_t ordinal, uint32_t best_us, uint32_t last_us,
							 bool budget_due = false)
	{
		ProviderCandidate c;
		c.ordinal = ordinal;
		c.hash = 0x3000 + ordinal;
		c.has_timing = true;
		c.samples = MIN_SAMPLES_TO_JUDGE;
		c.best_us = best_us;
		c.last_us = last_us;
		c.slow = false;
		c.budget_reprobe_due = budget_due;
		return c;
	}

	/*
		Indexing a plan that turned out shorter than expected.

		CHECK_EQ records and continues, so a size assertion does not protect the
		lines below it: on a short plan they run anyway and index past the end.
		That is tolerable noise in a passing suite and actively harmful in the
		one run that matters most - the one where a defect is deliberately
		re-introduced to prove these tests catch it. Measured while doing exactly
		that: the run died with 0xC0000005 and printed one FAIL out of five, so
		the evidence the gate exists to produce was destroyed by the gate.

		AGENTS.md names this shape: a failure reported in different words from
		every other failure is a failure nobody can find. A default-constructed
		step fails the comparison and says so.
	*/
	ProviderStep step_at(const ProviderPlan &plan, size_t i)
	{
		return i < plan.order.size() ? plan.order[i] : ProviderStep{};
	}

	ProviderDeferred deferred_at(const ProviderPlan &plan, size_t i)
	{
		return i < plan.deferred.size() ? plan.deferred[i] : ProviderDeferred{};
	}

	std::vector<uint32_t> ordinals(const ProviderPlan &plan)
	{
		std::vector<uint32_t> out;
		for(const auto &s : plan.order)
			out.push_back(s.ordinal);
		return out;
	}

	bool contains(const std::vector<uint32_t> &v, uint32_t x)
	{
		for(auto e : v)
			if(e == x)
				return true;
		return false;
	}

	// Walks the plan the way ExplorerCommand.cpp does: check the live budget,
	// then "call", charging the provider's real cost. Returns the ordinals that
	// were actually resolved.
	std::vector<uint32_t> run(const ProviderPlan &plan,
							  const std::vector<uint32_t> &real_cost_us,
							  uint32_t budget_us, uint32_t *refused = nullptr)
	{
		std::vector<uint32_t> resolved;
		uint32_t spent = 0;
		uint32_t refused_count = 0;

		for(const auto &step : plan.order)
		{
			auto remaining = spent >= budget_us ? 0u : budget_us - spent;
			if(!provider_step_fits(step, remaining))
			{
				refused_count++;
				continue;
			}
			spent += real_cost_us[step.ordinal];
			resolved.push_back(step.ordinal);
		}

		if(refused)
			*refused = refused_count;
		return resolved;
	}
}

TEST(provider_schedule, the_cheap_are_resolved_before_the_expensive_can_overrun)
{
	// The shape of the recorded failure, scaled to the numbers in the plan's
	// worked example: one 20 ms provider registered first, four cheap ones
	// behind it, and a budget that cannot hold all five.
	std::vector<ProviderCandidate> c{
		known(0, 20000), known(1, 1000), known(2, 1000),
		known(3, 1000), known(4, 1000)
	};
	std::vector<uint32_t> cost{ 20000, 1000, 1000, 1000, 1000 };

	auto plan = plan_providers(c, 0);
	uint32_t refused = 0;
	auto resolved = run(plan, cost, 15000, &refused);

	// All four cheap ones survive; only the one that genuinely does not fit is
	// refused. In registration order this was exactly backwards.
	CHECK_EQ(resolved.size(), (size_t)4);
	CHECK(contains(resolved, 1));
	CHECK(contains(resolved, 2));
	CHECK(contains(resolved, 3));
	CHECK(contains(resolved, 4));
	CHECK(!contains(resolved, 0));
	CHECK_EQ(refused, 1u);
}

// The claim the first version of this plan made and could not support: that
// reordering *admission* fixes it. It does not, if the calls still happen in
// registration order - a provider that overruns its own prediction still burns
// the budget in front of the cheap tail.
TEST(provider_schedule, a_runtime_overrun_can_no_longer_starve_the_cheap_tail)
{
	std::vector<ProviderCandidate> c{
		known(0, 5000), known(1, 300), known(2, 400), known(3, 500)
	};

	// Provider 0 was predicted at 5 ms and actually takes 40. Everything is
	// predicted to fit, so no plan-time check can see this coming.
	std::vector<uint32_t> cost{ 40000, 300, 400, 500 };

	auto plan = plan_providers(c, 0);
	auto resolved = run(plan, cost, 50000);

	// The cheap three ran first and are unaffected; the overrun is charged to
	// nobody but itself.
	CHECK_EQ(resolved.size(), (size_t)4);
	CHECK_EQ(resolved[0], 1u);
	CHECK_EQ(resolved[1], 2u);
	CHECK_EQ(resolved[2], 3u);
	CHECK_EQ(resolved[3], 0u);
}

TEST(provider_schedule, known_providers_are_ordered_cheapest_first)
{
	std::vector<ProviderCandidate> c{ known(0, 9000), known(1, 1000), known(2, 5000) };
	auto plan = plan_providers(c, 0);
	auto o = ordinals(plan);

	CHECK_EQ(o.size(), (size_t)3);
	CHECK_EQ(o[0], 1u);
	CHECK_EQ(o[1], 2u);
	CHECK_EQ(o[2], 0u);
}

// `best` is the fastest a provider has *ever* managed. Ordering by it alone
// flatters every provider that has since got slower, and the ordering exists
// precisely to protect the menu from those.
TEST(provider_schedule, the_estimate_is_conservative_rather_than_optimistic)
{
	auto a = known(0, 1000);
	a.best_us = 1000;
	a.last_us = 9000;			// it used to be quick; it is not any more
	auto b = known(1, 2000);

	std::vector<ProviderCandidate> c{ a, b };
	auto o = ordinals(plan_providers(c, 0));

	CHECK_EQ(o[0], 1u);
	CHECK_EQ(o[1], 0u);
	CHECK_EQ(provider_estimate_us(a), 9000u);
}

TEST(provider_schedule, a_provider_that_cannot_fit_does_not_end_the_pass)
{
	// One expensive provider in the middle. The walk must skip it and keep
	// looking rather than stopping, which is what a single budget countdown in
	// registration order effectively did.
	std::vector<ProviderCandidate> c{ known(0, 1000), known(1, 40000), known(2, 1000) };
	std::vector<uint32_t> cost{ 1000, 40000, 1000 };

	auto resolved = run(plan_providers(c, 0), cost, 10000);

	CHECK_EQ(resolved.size(), (size_t)2);
	CHECK(contains(resolved, 0));
	CHECK(contains(resolved, 2));
}

TEST(provider_schedule, a_slow_provider_that_is_not_due_is_deferred_as_slow)
{
	std::vector<ProviderCandidate> c{ known(0, 1000), slow(1, 60000, false) };
	auto plan = plan_providers(c, 0);

	CHECK_EQ(plan.order.size(), (size_t)1);
	CHECK_EQ(plan.order[0].ordinal, 0u);
	CHECK_EQ(plan.deferred.size(), (size_t)1);
	CHECK_EQ(plan.deferred[0].ordinal, 1u);
	CHECK(plan.deferred[0].why == ProviderDeferral::Slow);
}

// A re-probe means paying a known-pathological provider's full cost again.
// Doing that before ordinary affordable work is how a rehabilitation attempt
// ends up costing the user items it had no quarrel with.
TEST(provider_schedule, a_due_reprobe_runs_after_everything_affordable)
{
	std::vector<ProviderCandidate> c{ slow(0, 60000, true), known(1, 1000), unknown(2) };
	auto o = ordinals(plan_providers(c, 0));

	CHECK_EQ(o.size(), (size_t)3);
	CHECK_EQ(o[o.size() - 1], 0u);
}

TEST(provider_schedule, a_due_reprobe_is_not_refused_on_a_prediction)
{
	// It is slow - that is why it was deferred - so an affordability check on
	// its remembered cost would mean the re-probe never actually happens and a
	// provider that has since become fast stays excluded for the life of the
	// process. Only an exhausted budget stops it.
	std::vector<ProviderCandidate> c{ slow(0, 60000, true) };
	auto plan = plan_providers(c, 0);

	CHECK_EQ(plan.order.size(), (size_t)1);
	CHECK(plan.order[0].call == ProviderCall::Reprobe);
	CHECK(provider_step_fits(plan.order[0], 1));
	CHECK(!provider_step_fits(plan.order[0], 0));
}

TEST(provider_schedule, one_exploration_is_guaranteed_before_the_known_set)
{
	// Otherwise a full slate of cheap known providers can fill the budget every
	// menu and an unknown one is never sampled - never sampled means never
	// judged, and never judged means never schedulable.
	std::vector<ProviderCandidate> c{
		known(0, 1000), known(1, 1000), known(2, 1000), unknown(3)
	};
	auto plan = plan_providers(c, 0);

	CHECK_EQ(plan.order[0].ordinal, 3u);
	CHECK(plan.order[0].call == ProviderCall::Explore);
}

TEST(provider_schedule, exploration_rotates_so_no_unknown_is_starved)
{
	std::vector<ProviderCandidate> c{ unknown(0), unknown(1), unknown(2) };

	CHECK_EQ(ordinals(plan_providers(c, 0))[0], 0u);
	CHECK_EQ(ordinals(plan_providers(c, 1))[0], 1u);
	CHECK_EQ(ordinals(plan_providers(c, 2))[0], 2u);
	CHECK_EQ(ordinals(plan_providers(c, 3))[0], 0u);
}

// The concrete starvation: an expensive cold provider at the head of
// registration order eats a small budget every menu. With a rotating cursor the
// second unknown is sampled on the second menu instead of never.
TEST(provider_schedule, an_expensive_cold_head_does_not_starve_the_unknown_tail)
{
	std::vector<ProviderCandidate> c{ unknown(0), unknown(1) };
	std::vector<uint32_t> cost{ 30000, 100 };

	auto first = run(plan_providers(c, 0), cost, 20000);
	auto second = run(plan_providers(c, 1), cost, 20000);

	CHECK(contains(first, 0u));
	CHECK(!contains(first, 1u));		// budget gone after the expensive one
	CHECK(contains(second, 1u));		// its turn came round
}

TEST(provider_schedule, an_exploration_is_never_refused_on_a_number_nobody_measured)
{
	std::vector<ProviderCandidate> c{ unknown(0) };
	auto plan = plan_providers(c, 0);

	CHECK(plan.order[0].call == ProviderCall::Explore);
	CHECK_EQ(plan.order[0].estimate_us, 0u);
	CHECK(provider_step_fits(plan.order[0], 1));
	CHECK(!provider_step_fits(plan.order[0], 0));
}

TEST(provider_schedule, nothing_is_lost_when_everything_fits)
{
	std::vector<ProviderCandidate> c{ known(0, 1000), unknown(1), known(2, 2000) };
	std::vector<uint32_t> cost{ 1000, 1000, 2000 };

	uint32_t refused = 0;
	auto resolved = run(plan_providers(c, 0), cost, 50000, &refused);

	CHECK_EQ(resolved.size(), (size_t)3);
	CHECK_EQ(refused, 0u);
}

TEST(provider_schedule, an_empty_registration_set_plans_nothing)
{
	std::vector<ProviderCandidate> c;
	auto plan = plan_providers(c, 7);

	CHECK(plan.order.empty());
	CHECK(plan.deferred.empty());
}

// Resolution order is not presentation order. The scheduler only says which
// providers to *ask* and when; every ordinal it emits indexes back into the
// caller's registration list, and the caller publishes in that order. This pins
// the property the publishing pass depends on: no ordinal is emitted twice, and
// none is invented.
TEST(provider_schedule, every_ordinal_appears_exactly_once_across_order_and_deferrals)
{
	std::vector<ProviderCandidate> c{
		known(0, 3000), unknown(1), slow(2, 60000, false),
		known(3, 1000), slow(4, 70000, true), unknown(5)
	};
	auto plan = plan_providers(c, 2);

	std::vector<int> seen(c.size(), 0);
	for(const auto &s : plan.order)
		seen[s.ordinal]++;
	for(const auto &d : plan.deferred)
		seen[d.ordinal]++;

	for(auto n : seen)
		CHECK_EQ(n, 1);
}

TEST(provider_schedule, the_plan_is_deterministic_for_a_given_set_of_timings)
{
	std::vector<ProviderCandidate> c{ known(0, 1000), known(1, 1000), known(2, 1000) };

	// Equal estimates fall back to registration order, so two runs of the same
	// input cannot disagree about which provider a duplicate identity goes to.
	auto a = ordinals(plan_providers(c, 5));
	auto b = ordinals(plan_providers(c, 5));

	CHECK_EQ(a.size(), (size_t)3);
	CHECK_EQ(a[0], 0u);
	CHECK_EQ(a[1], 1u);
	CHECK_EQ(a[2], 2u);
	CHECK(a == b);
}

// ---- a spike must not be a life sentence --------------------------------
//
// The most serious defect on this branch, and one the scheduler introduced.
// Take best_us = 2 000 and last_us = 70 000 - two samples, the second a spike:
//
//   - `slow` is false, because judgement is on best_us alone and 2 ms is well
//     under SLOW_PROVIDER_US. So the candidate is Known and can never be
//     Reprobe, and REPROBE_AFTER does not apply to it;
//   - provider_estimate_us is max(best, last) = 70 ms, above MENU_BUDGET_US,
//     so provider_step_fits refused it against a completely unspent budget;
//   - note_budget_deferral deliberately does not touch since_probe, because a
//     provider the menu could not afford has not had its turn.
//
// Nothing moved. It was never called, so record() never ran, so last_us could
// never come down and best_us could never rise into "slow". The item left every
// menu for the life of the process - the same harm the scheduler was written to
// remove, made permanent.

TEST(provider_schedule, a_known_step_that_no_menu_could_admit_is_never_planned)
{
	// The structural half of the fix. A Known step is refused on its
	// prediction, and what is *left* of a budget is never more than the whole
	// budget - so a prediction above MENU_BUDGET_US describes a step that will
	// be refused every single time it is planned.
	std::vector<ProviderCandidate> c{
		known(0, 1000),
		spiked(1, 2000, MENU_BUDGET_US + 20000),
		known(2, 3000),
	};
	auto plan = plan_providers(c, 0);

	CHECK(plan_is_admissible(plan));

	for(const auto &step : plan.order)
		CHECK(step.ordinal != 1u);

	CHECK_EQ(plan.deferred.size(), (size_t)1);
	CHECK_EQ(deferred_at(plan, 0).ordinal, 1u);

	// Charged as a budget refusal, not as a slow one: it is not slow, and
	// calling it slow would send it down the REPROBE_AFTER path that cannot
	// help it.
	CHECK(deferred_at(plan, 0).why == ProviderDeferral::Budget);
}

TEST(provider_schedule, the_admissibility_invariant_holds_across_a_mixed_plan)
{
	std::vector<ProviderCandidate> c{
		known(0, 3000), unknown(1), slow(2, 60000, false),
		known(3, 1000), slow(4, 70000, true), unknown(5),
		spiked(6, 2000, MENU_BUDGET_US + 1),
		spiked(7, 500, MENU_BUDGET_US),			// exactly the budget: admissible
	};
	auto plan = plan_providers(c, 3);

	CHECK(plan_is_admissible(plan));

	// The boundary is `>`, not `>=`. A provider predicted to cost exactly the
	// whole budget can be admitted by a menu that has spent nothing, so it is
	// still planned.
	bool seven_planned = false;
	for(const auto &step : plan.order)
		if(step.ordinal == 7u)
			seven_planned = true;
	CHECK(seven_planned);
}

TEST(provider_schedule, a_provider_refused_too_often_is_given_a_forced_turn)
{
	// The liveness half. Once the streak reaches BUDGET_REPROBE_AFTER the
	// candidate is planned as a Reprobe, which reuses machinery that is already
	// correct: provider_step_fits never refuses a non-Known step on a
	// prediction, so the turn actually happens.
	std::vector<ProviderCandidate> c{
		known(0, 1000),
		spiked(1, 2000, MENU_BUDGET_US + 20000, true),
	};
	auto plan = plan_providers(c, 0);

	CHECK(plan.deferred.empty());
	CHECK_EQ(plan.order.size(), (size_t)2);

	// Ordered last, like every other re-probe: a forced retry must not evict
	// known-healthy work from the same menu.
	CHECK_EQ(step_at(plan, 1).ordinal, 1u);
	CHECK(step_at(plan, 1).call == ProviderCall::Reprobe);

	// And admitted, against a budget its estimate could not possibly fit.
	CHECK(provider_step_fits(step_at(plan, 1), 1000));
	CHECK(!provider_step_fits(step_at(plan, 1), 0));
}

TEST(provider_schedule, a_forced_turn_does_not_displace_affordable_work)
{
	std::vector<ProviderCandidate> c{
		spiked(0, 2000, MENU_BUDGET_US + 20000, true),		// registered first
		known(1, 9000),
		known(2, 1000),
	};
	auto plan = plan_providers(c, 0);

	// Cheapest known first, then the forced re-probe - even though the
	// re-probed provider is ordinal 0.
	CHECK_EQ(plan.order.size(), (size_t)3);
	CHECK_EQ(step_at(plan, 0).ordinal, 2u);
	CHECK_EQ(step_at(plan, 1).ordinal, 1u);
	CHECK_EQ(step_at(plan, 2).ordinal, 0u);
	CHECK(step_at(plan, 2).call == ProviderCall::Reprobe);
}

TEST(provider_schedule, an_affordable_provider_is_untouched_by_any_of_this)
{
	// The over-correction guard. A known provider whose estimate fits is still
	// planned as Known and still ordered by its prediction; nothing about the
	// spike handling reaches it.
	std::vector<ProviderCandidate> c{ spiked(0, 2000, 9000), known(1, 1000) };
	auto plan = plan_providers(c, 0);

	CHECK(plan.deferred.empty());
	CHECK_EQ(plan.order.size(), (size_t)2);
	CHECK_EQ(step_at(plan, 0).ordinal, 1u);
	CHECK(step_at(plan, 0).call == ProviderCall::Known);
	CHECK_EQ(step_at(plan, 1).ordinal, 0u);
	CHECK(step_at(plan, 1).call == ProviderCall::Known);
	CHECK_EQ(step_at(plan, 1).estimate_us, 9000u);
}
