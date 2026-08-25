#pragma once

/*
	Which items a menu puts at the top, given what this user reaches for.

	docs/refactor/05-capabilities.md section 6: "rendered as a pinned section
	with usage counters". This is the deciding half - pure, so the whole order
	can be enumerated by a test - and `ContextMenu::apply_favorites` is the
	half that moves MENUITEMINFOs about. Same split, and for the same reason, as
	Include/MenuColumns.h.

	`settings { favorites = N }` is the cap. Unset and 0 both mean off, so no
	existing configuration changes; the feature costs a menu nothing at all
	until somebody asks for it.

	## What it promotes, and what it deliberately does not

	**The root level only.** A favourite is moved, not copied, and the candidates
	are the items already in the menu being composed. Reaching into a submenu to
	pull something out is `moveto`'s job, and doing it here would mean
	materialising native submenus before first paint to find out what is in them
	- which is the eager walk docs/refactor/04-code-health.md section 6a spent a
	whole item removing. An item inside a submenu is not a candidate, and that
	is a stated limit rather than an oversight.

	**Never a separator.** They have no identity and moving one moves a hole.

	**Never the whole menu.** If every item that could go in the section is
	every item there is, the section is the menu and the rule has done nothing
	except add a separator to the bottom of it. Refused, with a reason.

	**Never something with nothing behind it.** An entry that is neither pinned
	nor ever used is not evidence, so it cannot promote anything - which is what
	stops a hand-written file full of `use 0` lines from silently reordering a
	menu.

	## The order inside the section

	Pinned first, because a pin is a decision and a count is an observation, and
	a decision that could be outvoted by a count is not a pin. Within each
	group, more uses first; ties keep the order the menu already had, so the
	section is stable between right-clicks rather than shuffling whenever two
	items draw level.

	## Why `refused` names the rule

	Refusing is a legitimate and common answer - most menus contain nothing
	worth promoting - so "nothing happened" has to be distinguishable from "this
	menu had no favourites in it". Include/MenuColumns.h has the same field, and
	it is what made its first defect findable at all: the log said
	`column-too-tall`, which was the whole diagnosis in one word.
*/

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		// One item of the menu being composed, as the planner sees it.
		struct FavoriteCandidate
		{
			// MenuIdentity::Identity::hash, or 0 for an item whose origin
			// could not be established. Zero never matches, which is the right
			// answer: an item nobody can name cannot be a favourite.
			uint32_t hash{};
			bool separator{};
		};

		// One line of the favorites file, as the planner sees it.
		struct FavoriteRank
		{
			uint32_t hash{};
			bool pinned{};
			uint32_t uses{};
		};

		struct FavoritePlan
		{
			// Indices into the candidate list, in the order they should appear
			// at the top of the menu. Empty when nothing is promoted.
			std::vector<size_t> promoted;

			// Which rule declined, or nullptr when something was promoted.
			const wchar_t *refused{};

			bool empty() const { return promoted.empty(); }
		};

		inline FavoritePlan plan_favorites(const std::vector<FavoriteCandidate> &items,
										   const std::vector<FavoriteRank> &ranks,
										   size_t cap)
		{
			FavoritePlan plan;

			if(cap == 0)
			{
				plan.refused = L"off";
				return plan;
			}
			if(items.empty() || ranks.empty())
			{
				plan.refused = L"nothing-to-promote";
				return plan;
			}

			struct Scored
			{
				size_t index{};
				bool pinned{};
				uint32_t uses{};
			};

			std::vector<Scored> scored;
			size_t promotable_items = 0;

			for(size_t i = 0; i < items.size(); i++)
			{
				const auto &item = items[i];
				if(item.separator)
					continue;

				promotable_items++;

				if(item.hash == 0)
					continue;

				for(const auto &rank : ranks)
				{
					if(rank.hash != item.hash)
						continue;

					// Neither pinned nor ever used is not evidence of
					// anything. See the header.
					if(!rank.pinned && rank.uses == 0)
						break;

					Scored one;
					one.index = i;
					one.pinned = rank.pinned;
					one.uses = rank.uses;
					scored.push_back(one);
					break;
				}
			}

			if(scored.empty())
			{
				plan.refused = L"no-favourite-in-this-menu";
				return plan;
			}

			// stable_sort, not sort: the tie-break is "keep the order the menu
			// already had", and that is exactly what stability means here. An
			// unstable sort would reorder equal entries between right-clicks
			// for no reason the user could see.
			std::stable_sort(scored.begin(), scored.end(),
							 [](const Scored &a, const Scored &b)
			{
				if(a.pinned != b.pinned)
					return a.pinned;
				return a.uses > b.uses;
			});

			if(scored.size() > cap)
				scored.resize(cap);

			// A section that is the whole menu is not a section.
			if(scored.size() >= promotable_items)
			{
				plan.refused = L"section-would-be-the-whole-menu";
				return plan;
			}

			plan.promoted.reserve(scored.size());
			for(const auto &one : scored)
				plan.promoted.push_back(one.index);

			return plan;
		}

		/*
			Apply a plan to any sequence that indexes like the candidate list.

			Kept here, and templated, because the reordering is the half most
			likely to be got subtly wrong - erasing while iterating by index is
			how a menu loses an item - and the unit suite can then drive the
			real function over a vector of integers.

			The promoted entries come out at the front in the plan's order; the
			rest keep the order they had.
		*/
		template<typename T>
		void apply_favorite_plan(std::vector<T> &items, const FavoritePlan &plan)
		{
			if(plan.promoted.empty())
				return;

			std::vector<T> ordered;
			ordered.reserve(items.size());

			std::vector<bool> taken(items.size(), false);
			for(auto index : plan.promoted)
			{
				if(index < items.size() && !taken[index])
				{
					taken[index] = true;
					ordered.push_back(items[index]);
				}
			}

			for(size_t i = 0; i < items.size(); i++)
			{
				if(!taken[i])
					ordered.push_back(items[i]);
			}

			items.swap(ordered);
		}
	}
}
