// The favorites file, and the rule that decides what a menu promotes.
//
// src/shared/Favorites.h is the format; src/dll/src/Include/MenuFavorites.h is
// the planner. docs/refactor/05-capabilities.md section 6.
//
// Two families of property here, and they fail differently. A format defect
// loses a user's data quietly - a line that will not parse simply is not there
// next time. A planner defect reorders a menu, which is loud but easy to
// mis-attribute to something else entirely.

#include "test.h"

#include <windows.h>
#include "..\shared\Favorites.h"
#include "..\dll\src\Include\MenuFavorites.h"

#include <string>
#include <vector>

using namespace Nilesoft::Shell;
using namespace Nilesoft::Shell::Favorites;

namespace
{
	MenuIdentity::Identity item(const wchar_t *signature)
	{
		return MenuIdentity::make(MenuIdentity::Kind::Item, signature);
	}

	// A file that removes itself.
	struct TempFile
	{
		std::wstring path;

		TempFile()
		{
			wchar_t base[MAX_PATH]{};
			::GetTempPathW(MAX_PATH, base);
			wchar_t unique[MAX_PATH]{};
			::swprintf_s(unique, L"%sfav_%lu_%llu.txt", base,
						 ::GetCurrentProcessId(), (unsigned long long)::GetTickCount64());
			path = unique;
		}

		~TempFile() { ::DeleteFileW(path.c_str()); }
	};

	FavoriteCandidate named(uint32_t hash)
	{
		FavoriteCandidate c;
		c.hash = hash;
		return c;
	}

	FavoriteCandidate separator()
	{
		FavoriteCandidate c;
		c.separator = true;
		return c;
	}

	FavoriteRank rank(uint32_t hash, bool pinned, uint32_t uses)
	{
		FavoriteRank r;
		r.hash = hash;
		r.pinned = pinned;
		r.uses = uses;
		return r;
	}
}

// ---- the file -----------------------------------------------------------

TEST(favorites, a_line_is_a_state_a_count_and_the_rest)
{
	Entry entry;
	CHECK(parse_line(L"use 7 item:tools/terminal", entry));
	CHECK(!entry.pinned);
	CHECK_EQ(entry.uses, 7u);
	CHECK_EQ(entry.identity.hash, item(L"tools/terminal").hash);

	CHECK(parse_line(L"pin 0 item:terminal", entry));
	CHECK(entry.pinned);
	CHECK_EQ(entry.uses, 0u);
}

TEST(favorites, an_identity_with_spaces_in_it_survives)
{
	// The whole reason the identity goes last on the line, and the reason this
	// file's field order is the inverse of quarantine.txt's. Menu titles have
	// spaces; nearly all of them do.
	Entry entry;
	CHECK(parse_line(L"use 3 native:view/Large icons", entry));
	CHECK_EQ(entry.identity.hash,
			 MenuIdentity::make(MenuIdentity::Kind::Native, L"view/Large icons").hash);
}

TEST(favorites, comments_and_blank_lines_and_rubbish_are_skipped_not_fatal)
{
	auto entries = parse(
		L"# a comment\r\n"
		L"\r\n"
		L"use 2 item:one\r\n"
		L"nonsense\r\n"
		L"use notanumber item:two\r\n"
		L"maybe 1 item:three\r\n"          // neither pin nor use
		L"use 5 item:four\r\n");

	// A half-readable list must still do the half it can read.
	CHECK_EQ(entries.size(), (size_t)2);
	CHECK_EQ(entries[0].identity.hash, item(L"one").hash);
	CHECK_EQ(entries[1].identity.hash, item(L"four").hash);
}

TEST(favorites, a_repeated_identity_keeps_the_first)
{
	auto entries = parse(L"pin 9 item:one\r\nuse 1 item:one\r\n");
	CHECK_EQ(entries.size(), (size_t)1);
	CHECK(entries[0].pinned);
	CHECK_EQ(entries[0].uses, 9u);
}

TEST(favorites, the_entry_cap_bounds_what_a_menu_walks)
{
	std::wstring text;
	for(size_t i = 0; i < MaxEntries + 40; i++)
		text += L"use 1 item:x" + std::to_wstring(i) + L"\r\n";

	CHECK_EQ(parse(text).size(), MaxEntries);
}

TEST(favorites, a_count_that_saturates_stays_saturated)
{
	// Wrapping would take the most-used item in the menu to the bottom of the
	// order, which is the one visible thing this must never do.
	Entry entry;
	CHECK(parse_line(L"use 99999999999999999999 item:one", entry));
	CHECK_EQ(entry.uses, MaxUses);

	std::vector<Entry> entries{ entry };
	CHECK(record_use(entries, entry.identity));
	CHECK_EQ(entries[0].uses, MaxUses);
}

TEST(favorites, a_list_round_trips_through_the_file)
{
	TempFile file;

	std::vector<Entry> written;
	CHECK(set_pinned(written, item(L"tools/terminal"), true));
	CHECK(record_use(written, item(L"tools/terminal")));
	CHECK(record_use(written, item(L"go to")));
	CHECK(record_use(written, item(L"go to")));

	CHECK(save(file.path, written));

	auto read = load(file.path);
	CHECK_EQ(read.size(), (size_t)2);
	CHECK(read[0].pinned);
	CHECK_EQ(read[0].uses, 1u);
	CHECK_EQ(read[0].identity.hash, item(L"tools/terminal").hash);
	CHECK(!read[1].pinned);
	CHECK_EQ(read[1].uses, 2u);

	// The spelling survives the trip, because `-favorites list` prints it.
	CHECK_MSG(read[0].identity.text == L"item:tools/terminal",
			  "the identity text is stored as written");
}

TEST(favorites, a_missing_file_is_an_empty_list_and_not_a_failure)
{
	// Nobody has favourites until they use something, so "no file" and "no
	// entries" must be the same answer as far as the caller is concerned.
	TempFile file;
	CHECK_EQ(load(file.path).size(), (size_t)0);
	CHECK_EQ(load(L"").size(), (size_t)0);
}

TEST(favorites, recording_a_use_counts_rather_than_reorders)
{
	// Ordering is the planner's job. A store that also sorted itself would be
	// two answers to one question, and the file would churn on every use.
	std::vector<Entry> entries;
	record_use(entries, item(L"first"));
	record_use(entries, item(L"second"));
	record_use(entries, item(L"second"));
	record_use(entries, item(L"second"));

	CHECK_EQ(entries.size(), (size_t)2);
	CHECK_MSG(entries[0].identity.hash == item(L"first").hash,
			  "the less-used entry stays where it was written");
	CHECK_EQ(entries[1].uses, 3u);
}

TEST(favorites, pinning_something_never_used_records_it_with_no_count)
{
	std::vector<Entry> entries;
	CHECK(set_pinned(entries, item(L"terminal"), true));
	CHECK_EQ(entries.size(), (size_t)1);
	CHECK(entries[0].pinned);
	CHECK_EQ(entries[0].uses, 0u);

	// Unpinning something absent adds nothing: there is nothing to record.
	CHECK(!set_pinned(entries, item(L"never seen"), false));
	CHECK_EQ(entries.size(), (size_t)1);

	// And unpinning keeps the count, so a released pin does not lose its
	// history.
	record_use(entries, item(L"terminal"));
	CHECK(set_pinned(entries, item(L"terminal"), false));
	CHECK(!entries[0].pinned);
	CHECK_EQ(entries[0].uses, 1u);
}

TEST(favorites, an_invalid_identity_is_never_recorded)
{
	std::vector<Entry> entries;
	CHECK(!record_use(entries, MenuIdentity::Identity{}));
	CHECK(!set_pinned(entries, MenuIdentity::Identity{}, true));
	CHECK_EQ(entries.size(), (size_t)0);
}

// ---- the planner --------------------------------------------------------

TEST(menu_favorites, nothing_is_promoted_until_somebody_asks)
{
	// Unset and 0 both mean off, which is what keeps every existing
	// configuration unchanged by this feature.
	std::vector<FavoriteCandidate> items{ named(1), named(2), named(3) };
	std::vector<FavoriteRank> ranks{ rank(2, false, 10) };

	auto plan = plan_favorites(items, ranks, 0);
	CHECK(plan.empty());
	CHECK_MSG(plan.refused && std::wstring(plan.refused) == L"off",
			  "and it says which rule declined");
}

TEST(menu_favorites, a_used_item_moves_to_the_top)
{
	std::vector<FavoriteCandidate> items{ named(1), named(2), named(3) };
	std::vector<FavoriteRank> ranks{ rank(3, false, 4) };

	auto plan = plan_favorites(items, ranks, 3);
	CHECK_EQ(plan.promoted.size(), (size_t)1);
	CHECK_EQ(plan.promoted[0], (size_t)2);
	CHECK(plan.refused == nullptr);

	std::vector<int> order{ 10, 20, 30 };
	apply_favorite_plan(order, plan);
	CHECK_EQ(order.size(), (size_t)3);
	CHECK_EQ(order[0], 30);
	CHECK_EQ(order[1], 10);
	CHECK_EQ(order[2], 20);
}

TEST(menu_favorites, a_pin_outranks_any_count)
{
	// A decision that could be outvoted by an observation is not a decision.
	std::vector<FavoriteCandidate> items{ named(1), named(2), named(3), named(4) };
	std::vector<FavoriteRank> ranks{
		rank(1, false, 900),
		rank(2, true, 1),
	};

	auto plan = plan_favorites(items, ranks, 4);
	CHECK_EQ(plan.promoted.size(), (size_t)2);
	CHECK_MSG(plan.promoted[0] == (size_t)1, "the pinned item comes first");
	CHECK_EQ(plan.promoted[1], (size_t)0);
}

TEST(menu_favorites, ties_keep_the_order_the_menu_already_had)
{
	// stable_sort, not sort. An unstable one would shuffle equal entries
	// between right-clicks for no reason the user could see, which reads as the
	// menu being unreliable rather than as a sorting choice.
	std::vector<FavoriteCandidate> items{ named(1), named(2), named(3), named(4) };
	std::vector<FavoriteRank> ranks{
		rank(1, false, 5), rank(2, false, 5), rank(3, false, 5),
	};

	auto plan = plan_favorites(items, ranks, 3);
	CHECK_EQ(plan.promoted.size(), (size_t)3);
	CHECK_EQ(plan.promoted[0], (size_t)0);
	CHECK_EQ(plan.promoted[1], (size_t)1);
	CHECK_EQ(plan.promoted[2], (size_t)2);
}

TEST(menu_favorites, the_cap_is_a_cap)
{
	std::vector<FavoriteCandidate> items{ named(1), named(2), named(3), named(4), named(5) };
	std::vector<FavoriteRank> ranks{
		rank(1, false, 1), rank(2, false, 9), rank(3, false, 5),
	};

	auto plan = plan_favorites(items, ranks, 2);
	CHECK_EQ(plan.promoted.size(), (size_t)2);
	CHECK_MSG(plan.promoted[0] == (size_t)1, "most used first");
	CHECK_EQ(plan.promoted[1], (size_t)2);
}

TEST(menu_favorites, a_separator_is_never_promoted)
{
	// It has no identity, and moving one moves a hole.
	std::vector<FavoriteCandidate> items{ separator(), named(1), separator(), named(2) };
	std::vector<FavoriteRank> ranks{ rank(2, true, 0) };

	auto plan = plan_favorites(items, ranks, 4);
	CHECK_EQ(plan.promoted.size(), (size_t)1);
	CHECK_EQ(plan.promoted[0], (size_t)3);
}

TEST(menu_favorites, an_item_with_no_identity_matches_nothing)
{
	// Zero is what an item whose origin could not be established carries, and
	// a rank whose hash happened to be zero must not sweep them all up.
	std::vector<FavoriteCandidate> items{ named(0), named(0), named(7) };
	std::vector<FavoriteRank> ranks{ rank(0, true, 5), rank(7, false, 1) };

	auto plan = plan_favorites(items, ranks, 3);
	CHECK_EQ(plan.promoted.size(), (size_t)1);
	CHECK_EQ(plan.promoted[0], (size_t)2);
}

TEST(menu_favorites, an_entry_with_nothing_behind_it_promotes_nothing)
{
	// Neither pinned nor ever used is not evidence. This is what stops a
	// hand-written file full of `use 0` lines from reordering a menu.
	std::vector<FavoriteCandidate> items{ named(1), named(2) };
	std::vector<FavoriteRank> ranks{ rank(1, false, 0) };

	auto plan = plan_favorites(items, ranks, 2);
	CHECK(plan.empty());
	CHECK_MSG(plan.refused && std::wstring(plan.refused) == L"no-favourite-in-this-menu",
			  "and it names the rule that declined");
}

TEST(menu_favorites, a_section_that_would_be_the_whole_menu_is_refused)
{
	// Promoting everything reorders nothing and adds a separator to the bottom
	// of the menu, which is strictly worse than doing nothing.
	std::vector<FavoriteCandidate> items{ named(1), named(2), separator() };
	std::vector<FavoriteRank> ranks{ rank(1, true, 0), rank(2, true, 0) };

	auto plan = plan_favorites(items, ranks, 5);
	CHECK(plan.empty());
	CHECK_MSG(plan.refused && std::wstring(plan.refused) == L"section-would-be-the-whole-menu",
			  "and says so rather than silently doing it");

	// One short of all of them is fine.
	std::vector<FavoriteRank> fewer{ rank(1, true, 0) };
	CHECK_EQ(plan_favorites(items, fewer, 5).promoted.size(), (size_t)1);
}

TEST(menu_favorites, applying_a_plan_keeps_every_item_exactly_once)
{
	// The defect this guards is how a menu loses an item: erase-while-indexing.
	// Every input must come out, once.
	std::vector<FavoriteCandidate> items{
		named(1), separator(), named(2), named(3), separator(), named(4)
	};
	std::vector<FavoriteRank> ranks{ rank(4, true, 0), rank(2, false, 3) };

	auto plan = plan_favorites(items, ranks, 4);
	CHECK_EQ(plan.promoted.size(), (size_t)2);

	std::vector<int> order{ 1, -1, 2, 3, -2, 4 };
	apply_favorite_plan(order, plan);

	CHECK_EQ(order.size(), (size_t)6);
	CHECK_EQ(order[0], 4);
	CHECK_EQ(order[1], 2);

	int seen = 0;
	for(auto value : order)
		seen += value;
	CHECK_MSG(seen == 1 + -1 + 2 + 3 + -2 + 4, "nothing was dropped or duplicated");
}

TEST(menu_favorites, a_plan_naming_an_index_twice_still_moves_it_once)
{
	// plan_favorites cannot produce one, because it walks the menu once. The
	// apply half is templated and separately callable, so it defends itself
	// rather than trusting its caller.
	FavoritePlan plan;
	plan.promoted = { 2, 2, 0 };

	std::vector<int> order{ 10, 20, 30 };
	apply_favorite_plan(order, plan);

	CHECK_EQ(order.size(), (size_t)3);
	CHECK_EQ(order[0], 30);
	CHECK_EQ(order[1], 10);
	CHECK_EQ(order[2], 20);
}

TEST(menu_favorites, a_plan_naming_an_index_that_is_not_there_is_ignored)
{
	FavoritePlan plan;
	plan.promoted = { 9, 1 };

	std::vector<int> order{ 10, 20 };
	apply_favorite_plan(order, plan);

	CHECK_EQ(order.size(), (size_t)2);
	CHECK_EQ(order[0], 20);
	CHECK_EQ(order[1], 10);
}
