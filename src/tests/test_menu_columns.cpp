// Where a menu taller than the screen gets broken into columns.
//
// docs/refactor/05-capabilities.md section 5. The wiring in ContextMenu.cpp is
// measurement and SetMenuItemInfo; everything that can be *wrong* is the
// arithmetic and the group handling, and that is what lives in
// Include/MenuColumns.h and is pinned here.
//
// The rules worth stating, because each has a plausible opposite:
//
//   - refusing is a valid answer, and often the right one - a menu three
//     columns wide and still taller than the monitor is worse than a scrollbar
//   - a break lands after a separator when one is close enough, because
//     splitting a group is what makes a two-column menu hard to read
//   - no column is ever empty, and none is ever taller than the budget it was
//     created to respect

#include "test.h"

#include "..\dll\src\Include\MenuColumns.h"

#include <vector>

using namespace Nilesoft::Shell;

namespace
{
	constexpr int ROW = 20;
	constexpr int SEP = 8;

	ColumnBudget budget(int height, int max_columns = 3)
	{
		ColumnBudget b;
		b.available_height = height;
		b.menu_width = 200;
		b.available_width = 1920;
		b.max_columns = max_columns;
		return b;
	}

	std::vector<ColumnRow> plain(int count)
	{
		std::vector<ColumnRow> rows;
		for(int i = 0; i < count; i++)
			rows.push_back(ColumnRow{ ROW, false, false });
		return rows;
	}

	// Groups of `per_group` items with a separator between them.
	std::vector<ColumnRow> grouped(int groups, int per_group)
	{
		std::vector<ColumnRow> rows;
		for(int g = 0; g < groups; g++)
		{
			if(g)
				rows.push_back(ColumnRow{ SEP, true, false });
			for(int i = 0; i < per_group; i++)
				rows.push_back(ColumnRow{ ROW, false, false });
		}
		return rows;
	}

	int column_count(const std::vector<ColumnRow> &rows, const ColumnPlan &plan)
	{
		return static_cast<int>(plan.breaks.size()) + (rows.empty() ? 0 : 1);
	}

	int tallest_column(const std::vector<ColumnRow> &rows, const ColumnPlan &plan)
	{
		int tallest = 0, current = 0;
		size_t next = 0;
		for(size_t i = 0; i < rows.size(); i++)
		{
			if(next < plan.breaks.size() && plan.breaks[next] == i)
			{
				if(current > tallest) tallest = current;
				current = 0;
				next++;
			}
			current += rows[i].height;
		}
		return current > tallest ? current : tallest;
	}
}

TEST(menu_columns, a_menu_that_fits_is_left_alone)
{
	auto rows = plain(10);					// 200
	auto plan = plan_columns(rows, budget(400));

	CHECK(!plan.apply);
	CHECK(plan.breaks.empty());
}

TEST(menu_columns, the_feature_is_off_until_it_is_asked_for)
{
	auto rows = plain(100);
	// max_columns 0 and 1 both mean "never break".
	CHECK(!plan_columns(rows, budget(200, 0)).apply);
	CHECK(!plan_columns(rows, budget(200, 1)).apply);
}

TEST(menu_columns, an_overflowing_menu_is_split)
{
	auto rows = plain(30);					// 600
	auto plan = plan_columns(rows, budget(400));

	CHECK(plan.apply);
	CHECK_EQ(plan.columns, 2);
	CHECK_EQ(column_count(rows, plan), 2);
	CHECK(tallest_column(rows, plan) <= 400);
}

TEST(menu_columns, the_columns_come_out_even_rather_than_full)
{
	// Greedy filling would put 20 rows in the first column and 10 in the
	// second. Aiming for the average puts 15 in each, which is what a reader
	// expects to see.
	auto rows = plain(30);
	auto plan = plan_columns(rows, budget(400));

	CHECK(plan.apply);
	CHECK_EQ(plan.breaks.size(), size_t(1));
	if(plan.breaks.size() == 1)
		CHECK_EQ(plan.breaks[0], size_t(15));
}

TEST(menu_columns, a_break_never_lands_on_the_first_row)
{
	auto rows = plain(40);
	auto plan = plan_columns(rows, budget(300));

	CHECK(plan.apply);
	for(auto b : plan.breaks)
		CHECK(b != 0);
}

TEST(menu_columns, three_columns_when_two_would_not_be_enough)
{
	auto rows = plain(45);					// 900
	auto plan = plan_columns(rows, budget(320));

	CHECK(plan.apply);
	CHECK_EQ(plan.columns, 3);
	CHECK(tallest_column(rows, plan) <= 320);
}

TEST(menu_columns, more_columns_than_allowed_falls_back_to_scrolling)
{
	auto rows = plain(100);					// 2000, needs 5 at 400
	auto plan = plan_columns(rows, budget(400, 3));

	CHECK(!plan.apply);
}

TEST(menu_columns, a_menu_too_wide_for_the_screen_falls_back_to_scrolling)
{
	auto rows = plain(30);
	auto b = budget(400);
	b.menu_width = 700;
	b.available_width = 1000;				// two 700px columns do not fit

	CHECK(!plan_columns(rows, b).apply);
}

TEST(menu_columns, a_break_prefers_the_row_after_a_separator)
{
	// Six groups of five, separators between: the ideal split lands inside a
	// group, and the nearest boundary is close enough to take.
	auto rows = grouped(6, 5);
	auto plan = plan_columns(rows, budget(340));

	CHECK(plan.apply);
	CHECK_EQ(plan.breaks.size(), size_t(1));
	if(plan.breaks.size() != 1)
		return;

	auto at = plan.breaks[0];
	CHECK(at > 0);
	if(at > 0)
		CHECK(rows[at - 1].separator);
}

TEST(menu_columns, a_distant_separator_is_not_worth_an_uneven_split)
{
	// One long group then a separator far behind the ideal split. Pulling the
	// break all the way back would leave a stub of a first column, so the
	// break stays where the arithmetic put it.
	std::vector<ColumnRow> rows;
	rows.push_back(ColumnRow{ SEP, true, false });
	for(int i = 0; i < 40; i++)
		rows.push_back(ColumnRow{ ROW, false, false });

	auto plan = plan_columns(rows, budget(460));

	CHECK(plan.apply);
	CHECK_EQ(plan.breaks.size(), size_t(1));
	if(plan.breaks.size() != 1)
		return;
	CHECK(plan.breaks[0] > size_t(2));
	CHECK(tallest_column(rows, plan) <= 460);
}

TEST(menu_columns, a_configured_column_switches_the_whole_thing_off)
{
	auto rows = plain(40);
	rows[12].explicit_break = true;

	// Without the explicit break this menu would certainly be split.
	CHECK(plan_columns(plain(40), budget(300)).apply);
	CHECK(!plan_columns(rows, budget(300)).apply);
}

TEST(menu_columns, no_column_is_empty_and_none_overflows)
{
	// Walk a range of shapes rather than trusting one: the two invariants that
	// would be visible to a user must hold for every plan this produces.
	for(int count = 2; count <= 120; count++)
	{
		for(int height = 120; height <= 600; height += 60)
		{
			auto rows = plain(count);
			auto plan = plan_columns(rows, budget(height));
			if(!plan.apply)
				continue;

			CHECK(tallest_column(rows, plan) <= height);

			size_t previous = 0;
			for(auto b : plan.breaks)
			{
				CHECK(b > previous);		// nothing empty between two breaks
				previous = b;
			}
			CHECK(previous < rows.size());	// the last column has rows in it
		}
	}
}

TEST(menu_columns, grouped_menus_also_never_overflow_or_empty_a_column)
{
	for(int groups = 2; groups <= 14; groups++)
	{
		for(int per = 1; per <= 9; per++)
		{
			auto rows = grouped(groups, per);
			for(int height = 140; height <= 520; height += 40)
			{
				auto plan = plan_columns(rows, budget(height));
				if(!plan.apply)
					continue;

				CHECK(tallest_column(rows, plan) <= height);
				CHECK_EQ(column_count(rows, plan), plan.columns);

				size_t previous = 0;
				for(auto b : plan.breaks)
				{
					CHECK(b > previous);
					previous = b;
				}
				CHECK(previous < rows.size());
			}
		}
	}
}

TEST(menu_columns, an_empty_menu_is_not_a_special_case)
{
	std::vector<ColumnRow> rows;
	CHECK(!plan_columns(rows, budget(100)).apply);
}

TEST(menu_columns, group_boundaries_do_not_pile_up_in_the_last_column)
{
	// The shape a real menu produced, and the one the first version of this got
	// wrong: 133 rows needing four columns, with a separator sitting just
	// before each ideal split. Every break pulls back to its boundary and
	// leaves height behind, and with a fixed target all three lots of it land
	// in the last column - which then still overflowed the screen. Measured on
	// a live menu (133 rows, 3719px, 1040px of screen): refused as
	// "column-too-tall", so the menu stayed one column and kept scrolling.
	//
	// The fix is to divide what is left by the columns that are left after
	// every break. This asserts the outcome rather than the mechanism.
	std::vector<ColumnRow> rows;
	for(int i = 0; i < 133; i++)
	{
		bool sep = (i == 27 || i == 59 || i == 91);
		rows.push_back(ColumnRow{ 28, sep, false });
	}

	auto plan = plan_columns(rows, budget(1040, 4));

	CHECK(plan.apply);
	CHECK_EQ(static_cast<int>(plan.refused), static_cast<int>(ColumnRefusal::None));
	CHECK_EQ(plan.columns, 4);
	CHECK(tallest_column(rows, plan) <= 1040);
}

TEST(menu_columns, a_refusal_says_which_rule_refused)
{
	// "Nothing happened" is not a diagnosis. Each refusal names itself, which
	// is what made the case above findable at all.
	CHECK_EQ(static_cast<int>(plan_columns(plain(100), budget(200, 1)).refused),
			 static_cast<int>(ColumnRefusal::Disabled));

	CHECK_EQ(static_cast<int>(plan_columns(plain(5), budget(400)).refused),
			 static_cast<int>(ColumnRefusal::Fits));

	CHECK_EQ(static_cast<int>(plan_columns(plain(100), budget(400, 3)).refused),
			 static_cast<int>(ColumnRefusal::TooManyColumns));

	auto rows = plain(40);
	rows[12].explicit_break = true;
	CHECK_EQ(static_cast<int>(plan_columns(rows, budget(300)).refused),
			 static_cast<int>(ColumnRefusal::ExplicitBreak));

	auto narrow = budget(400);
	narrow.menu_width = 700;
	narrow.available_width = 1000;
	CHECK_EQ(static_cast<int>(plan_columns(plain(30), narrow).refused),
			 static_cast<int>(ColumnRefusal::TooWide));

	// And a plan that works names no rule at all.
	CHECK_EQ(static_cast<int>(plan_columns(plain(30), budget(400)).refused),
			 static_cast<int>(ColumnRefusal::None));
}

TEST(menu_columns, separators_anywhere_never_overflow_a_column)
{
	// The same two invariants as the sweeps above, but with a separator every
	// `every` rows so the pull-back path is exercised at every offset.
	for(int every = 2; every <= 17; every++)
	{
		for(int count = 20; count <= 140; count += 20)
		{
			std::vector<ColumnRow> rows;
			for(int i = 0; i < count; i++)
				rows.push_back(ColumnRow{ 28, (i % every) == every - 1, false });

			for(int height = 300; height <= 1100; height += 100)
			{
				auto plan = plan_columns(rows, budget(height, 4));
				if(!plan.apply)
					continue;

				CHECK(tallest_column(rows, plan) <= height);
				CHECK_EQ(column_count(rows, plan), plan.columns);

				size_t previous = 0;
				for(auto b : plan.breaks)
				{
					CHECK(b > previous);
					previous = b;
				}
				CHECK(previous < rows.size());
			}
		}
	}
}
