#pragma once

/*
	The order packaged verb handlers are asked in, and who is left out.

	Separated from the calling loop, and from COM, so the policy can be run
	against numbers instead of against a desktop. Everything here is pure: it
	takes what ProviderHealth remembers and produces an order plus a list of
	deferrals, and it activates nothing.

	Why it exists at all
	--------------------

	The loop this replaces walked the registry's registration order and spent a
	whole-menu budget as it went. Measured against the deployed build on
	2026-08-25 (37 handlers, single-file selection, `shell.exe -report perf:all`
	over 16 held sessions), one session exhausted the budget and dropped six
	providers:

		{8F491918-...}  deferred   Resize with Image Resizer     (0.4 ms warm)
		{1861E28B-...}  deferred   Rename with PowerRename       (1.6 ms warm)
		{1C6DF0C0-...}  deferred   Open with Code                (1.3 ms warm)
		{BFE0E2A4-...}  deferred   Edit with Photos              (6.7 ms warm)
		{7A53B94A-...}  deferred   Create with Designer         (13.9 ms warm)
		{CA6CC9F1-...}  deferred   Edit in Notepad               (0.3 ms warm)

	Four of those cost between 0.3 and 1.6 ms. They lost their place in the menu
	for sitting *after* one that costs 13.9 ms - which is the opposite of what
	docs/refactor/02-first-paint-latency.md section 2a describes ("a provider
	that misses its deadline is omitted"). Six items vanished from that
	right-click and came back on the next, with nothing marking the omission.

	Resolution order is not presentation order
	------------------------------------------

	The output of this header is the order in which providers are *called*. What
	the user sees stays in registration order, and duplicate resolution still
	runs in registration order, so a cheaper later provider cannot take the
	first-registration-wins identity from an earlier one. Nothing about the menu
	changes; only which providers get asked when the menu cannot afford all of
	them.

	What it does not promise
	------------------------

	Not "at most two providers are dropped", and not a hard bound on the menu.
	`IExplorerCommand`'s methods "are called on the UI thread"
	(https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorercommand)
	and a call already in flight cannot be interrupted, so the honest bound is
	still **one in-flight overrun**. What this does guarantee is that the cheap
	providers are resolved before the expensive one gets the chance to overrun,
	rather than after.
*/

#include <algorithm>
#include <cstdint>
#include <vector>

#include "ProviderHealth.h"

namespace Nilesoft
{
	namespace Shell
	{
		// Why a provider is being called, which decides where it sits in the
		// order and how the live budget check treats it.
		enum class ProviderCall : uint8_t
		{
			// Two or more samples, best time under SLOW_PROVIDER_US. Its cost is
			// predictable, so it is ordered by that prediction and refused when
			// the prediction does not fit.
			Known,

			// Fewer than MIN_SAMPLES_TO_JUDGE. Nothing is known about it, so
			// nothing can be predicted; it is admitted while any budget remains.
			Explore,

			// Condemned as slow, but REPROBE_AFTER menus have passed. Runs last:
			// a re-probe must not evict known healthy items from the same menu.
			Reprobe,
		};

		enum class ProviderDeferral : uint8_t
		{
			Slow,		// judged slow and not due for a re-probe
			Budget,		// the menu ran out before it was reached
		};

		struct ProviderStep
		{
			uint32_t ordinal{};			// index into the caller's registration list
			uint32_t hash{};
			ProviderCall call{ ProviderCall::Explore };

			// Conservative prediction, for ProviderCall::Known only. max(best,
			// last) rather than best alone: `best` is the fastest it has *ever*
			// managed, and ordering by a number a provider hits once flatters
			// every provider that has since got slower.
			uint32_t estimate_us{};
		};

		struct ProviderDeferred
		{
			uint32_t ordinal{};
			uint32_t hash{};
			ProviderDeferral why{ ProviderDeferral::Slow };
		};

		// One registration, with everything the schedule needs read out of it.
		// Filled by the caller from ProviderHealth::classify, which does not
		// mutate the re-probe counters - deciding that a probe is due is an
		// explicit policy operation, not a side effect of looking.
		struct ProviderCandidate
		{
			uint32_t ordinal{};
			uint32_t hash{};

			bool has_timing{};
			uint16_t samples{};
			uint32_t best_us{};
			uint32_t last_us{};

			// best_us > SLOW_PROVIDER_US, with at least MIN_SAMPLES_TO_JUDGE.
			bool slow{};

			// since_probe + 1 >= REPROBE_AFTER. Computed by the caller so this
			// header holds no counter state.
			bool reprobe_due{};
		};

		struct ProviderPlan
		{
			std::vector<ProviderStep> order;
			std::vector<ProviderDeferred> deferred;
		};

		inline uint32_t provider_estimate_us(const ProviderCandidate &c) noexcept
		{
			return c.best_us > c.last_us ? c.best_us : c.last_us;
		}

		/*
			`exploration_cursor` rotates across menus so the unknown set is
			sampled fairly.

			Without it, exploration always starts at the head of registration
			order, and an expensive cold provider near the front can consume the
			budget on every menu while an unknown tail is never sampled at all -
			so it never acquires the two samples it needs to be judged, and never
			becomes schedulable. The cursor is advanced by the caller once per
			menu; any monotonically increasing value works.
		*/
		inline ProviderPlan plan_providers(const std::vector<ProviderCandidate> &candidates,
										   uint64_t exploration_cursor)
		{
			ProviderPlan plan;
			plan.order.reserve(candidates.size());

			std::vector<const ProviderCandidate *> known;
			std::vector<const ProviderCandidate *> unknown;
			std::vector<const ProviderCandidate *> reprobe;

			for(const auto &c : candidates)
			{
				if(c.slow)
				{
					if(c.reprobe_due)
						reprobe.push_back(&c);
					else
						plan.deferred.push_back({ c.ordinal, c.hash, ProviderDeferral::Slow });
					continue;
				}

				if(c.has_timing && c.samples >= MIN_SAMPLES_TO_JUDGE)
					known.push_back(&c);
				else
					unknown.push_back(&c);
			}

			// Cheapest first, ties by registration order so the plan is
			// deterministic for a given set of timings.
			std::stable_sort(known.begin(), known.end(),
							 [](const ProviderCandidate *a, const ProviderCandidate *b)
							 {
								 auto ea = provider_estimate_us(*a);
								 auto eb = provider_estimate_us(*b);
								 if(ea != eb)
									 return ea < eb;
								 return a->ordinal < b->ordinal;
							 });

			// Rotate the unknown set so a different one leads each menu.
			if(!unknown.empty())
			{
				auto at = static_cast<size_t>(exploration_cursor % unknown.size());
				std::rotate(unknown.begin(), unknown.begin() + static_cast<ptrdiff_t>(at),
							unknown.end());
			}

			auto push = [&plan](const ProviderCandidate *c, ProviderCall call)
			{
				ProviderStep step;
				step.ordinal = c->ordinal;
				step.hash = c->hash;
				step.call = call;
				step.estimate_us = call == ProviderCall::Known ? provider_estimate_us(*c) : 0;
				plan.order.push_back(step);
			};

			// One exploration first, so every menu makes progress on the unknown
			// set even when the known set could fill the whole budget. Exactly
			// one: the point is to guarantee progress, not to let unmeasured
			// work push out work whose cost is known and affordable.
			size_t explored = 0;
			if(!unknown.empty())
			{
				push(unknown[0], ProviderCall::Explore);
				explored = 1;
			}

			for(auto *c : known)
				push(c, ProviderCall::Known);

			for(size_t i = explored; i < unknown.size(); i++)
				push(unknown[i], ProviderCall::Explore);

			// Last, and only what is actually due. Charging a 200-menu re-probe
			// before ordinary affordable work is how a rehabilitation attempt
			// ends up costing the user six items it had no quarrel with.
			std::stable_sort(reprobe.begin(), reprobe.end(),
							 [](const ProviderCandidate *a, const ProviderCandidate *b)
							 {
								 return provider_estimate_us(*a) < provider_estimate_us(*b);
							 });
			for(auto *c : reprobe)
				push(c, ProviderCall::Reprobe);

			return plan;
		}

		/*
			Whether the next step still fits, asked immediately before the call.

			A predicted cost is only refused for a provider whose cost is
			actually predicted. An exploration or a re-probe has no prediction to
			test - refusing one on a number nobody measured would be a guess
			dressed as a policy - so those are admitted while any budget remains,
			exactly as an unknown provider was before this existed.
		*/
		inline bool provider_step_fits(const ProviderStep &step, uint32_t remaining_us) noexcept
		{
			if(remaining_us == 0)
				return false;
			if(step.call != ProviderCall::Known)
				return true;
			return step.estimate_us <= remaining_us;
		}
	}
}
