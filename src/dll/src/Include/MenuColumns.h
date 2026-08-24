#pragma once

/*
	Where to break a menu that is taller than the screen.

	docs/refactor/05-capabilities.md section 5. `cyMax` scrolling already stops a
	long menu running off the display - MENUINFO's page is explicit that Windows
	installs scroll affordances once the items exceed it
	(https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-menuinfo)
	- but scrolling a context menu is a poor way to find anything, and the
	machinery for the alternative is already on both sides: NSS `column` maps to
	`MFT_MENUBREAK`, which "places the item on a new line (for a menu bar) or in
	a new column (for a drop-down menu, submenu, or shortcut menu)"
	(https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-menuiteminfow).

	This decides *where*. It is deliberately a pure function over measured
	heights: the part that can be wrong is the arithmetic and the group
	handling, and that is the part a test can hold still. The caller does the
	measuring and the SetMenuItemInfo calls.

	Four rules, and the reasons matter more than the code:

	1. **An explicit `column` in the configuration switches this off entirely.**
	   Someone who has laid their menu out by hand has said what they want, and
	   a second opinion arriving on top of it is worse than no feature.

	2. **Breaks prefer a separator boundary.** Breaking mid-group splits things
	   the configuration deliberately put together - the four "open with"
	   entries, a submenu and its siblings - and the reader then hunts across a
	   column boundary for something that was never meant to be apart. A
	   boundary is only taken if it is close enough to the ideal split not to
	   unbalance the columns, which is what `regroup_slack` bounds.

	3. **It gives up rather than producing something worse.** Too many columns
	   for the width, or more columns than allowed, and the answer is "no
	   columns" - the caller falls back to scrolling, which is at least
	   familiar. A three-column menu wider than the monitor is not an
	   improvement on a scrollbar.

	4. **The last column may be short; no column may be empty.** Splitting into
	   N columns and leaving one of them with nothing in it is a layout bug that
	   looks like a rendering bug.
*/

#include <cstddef>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		// One row as the caller measured it, in the order Windows will lay them
		// out. Separators are included: they take vertical space, and they are
		// where the good break points are.
		struct ColumnRow
		{
			int height{};
			bool separator{};

			// The configuration already asked for a break here. Its presence
			// anywhere disables the whole plan - see rule 1.
			bool explicit_break{};
		};

		// Why a menu was left to scroll. Refusing is the common answer and a
		// legitimate one, so it is worth being able to say which rule did it -
		// otherwise "nothing happened" is the whole diagnosis.
		enum class ColumnRefusal
		{
			None,
			Disabled,			// settings.columns is unset, 0 or 1
			Fits,				// it was never too tall
			ExplicitBreak,		// the configuration lays this menu out itself
			TooManyColumns,		// more than settings.columns would be needed
			TooWide,			// they would not fit side by side
			NoBreaks,			// the walk found nowhere to break
			ColumnTooTall,		// a planned column still overflows
			EmptyColumn,		// a planned column has nothing in it
		};

		struct ColumnPlan
		{
			bool apply{};				// false means "leave it to cyMax"
			int columns{ 1 };
			ColumnRefusal refused{ ColumnRefusal::None };

			// Row indices that should carry MFT_MENUBREAK. Never contains 0.
			std::vector<size_t> breaks;
		};

		inline const wchar_t *column_refusal_name(ColumnRefusal r)
		{
			switch(r)
			{
			case ColumnRefusal::None:			return L"none";
			case ColumnRefusal::Disabled:		return L"disabled";
			case ColumnRefusal::Fits:			return L"fits";
			case ColumnRefusal::ExplicitBreak:	return L"explicit-break";
			case ColumnRefusal::TooManyColumns:	return L"too-many-columns";
			case ColumnRefusal::TooWide:		return L"too-wide";
			case ColumnRefusal::NoBreaks:		return L"no-breaks";
			case ColumnRefusal::ColumnTooTall:	return L"column-too-tall";
			case ColumnRefusal::EmptyColumn:	return L"empty-column";
			}
			return L"?";
		}

		struct ColumnBudget
		{
			int available_height{};		// what one column may occupy
			int menu_width{};			// the width of a single column
			int available_width{};		// what all columns together may occupy
			int max_columns{};			// 0 or 1 disables the feature

			// How far back a break may be pulled to land after a separator,
			// as a fraction of a column: 4 means "up to a quarter of a column".
			// Larger values keep more groups together and make the columns less
			// even; this is the balance point between rules 2 and 4.
			int regroup_slack{ 4 };
		};

		inline ColumnPlan plan_columns(const std::vector<ColumnRow> &rows,
									   const ColumnBudget &budget)
		{
			auto refuse = [](ColumnRefusal why)
			{
				ColumnPlan p;
				p.refused = why;
				return p;
			};

			if(budget.max_columns < 2 || budget.available_height <= 0 || rows.empty())
				return refuse(ColumnRefusal::Disabled);

			// Running heights, so the height of any span is one subtraction.
			// prefix[i] is the height of rows [0, i).
			std::vector<int> prefix(rows.size() + 1, 0);
			for(size_t i = 0; i < rows.size(); i++)
			{
				if(rows[i].explicit_break)
					return refuse(ColumnRefusal::ExplicitBreak);	// rule 1
				prefix[i + 1] = prefix[i] + rows[i].height;
			}

			auto total = prefix.back();
			if(total <= budget.available_height)
				return refuse(ColumnRefusal::Fits);

			auto needed = (total + budget.available_height - 1) / budget.available_height;
			if(needed > budget.max_columns)
				return refuse(ColumnRefusal::TooManyColumns);		// rule 3

			if(budget.menu_width > 0 && budget.available_width > 0
			   && static_cast<long long>(budget.menu_width) * needed > budget.available_width)
				return refuse(ColumnRefusal::TooWide);				// rule 3

			ColumnPlan plan;
			int columns = 1;
			size_t last_break = 0;

			/*
				The target is recomputed after every break, over what is left.

				Filling to a fixed target - the average column - is the obvious
				version and it does not survive rule 2. Every break pulled back
				to a separator leaves height behind for the columns that follow,
				and three such pulls in a row hand the last column everything
				they each declined to take. Measured on a real 133-row menu:
				four columns of a nominal 930 each, and the last one came out
				over the 1040 the screen had. The menu would have been three
				times as wide *and* still scrolling.

				Dividing the remaining height by the remaining columns after
				each break makes that self-correcting: whatever a pull-back
				pushed forward is immediately shared out over the columns that
				are left, rather than accumulating in the last one.
			*/
			auto retarget = [&](size_t from, int &target, int &slack)
			{
				auto remaining = total - prefix[from];
				auto columns_left = needed - columns + 1;
				if(columns_left < 1)
					columns_left = 1;
				target = (remaining + columns_left - 1) / columns_left;
				if(target > budget.available_height)
					target = budget.available_height;
				slack = budget.regroup_slack > 0 ? target / budget.regroup_slack : 0;
			};

			int target = 0, slack = 0;
			retarget(0, target, slack);

			for(size_t i = 1; i < rows.size(); i++)
			{
				auto used = prefix[i] - prefix[last_break];
				if(used + rows[i].height <= target || columns >= needed)
					continue;

				/*
					Rule 2: move the break to just after the nearest separator,
					in *either* direction, within the slack window.

					Searching only backwards is the obvious version and it is
					not enough. The arithmetic tends to land the ideal split on
					or just before a separator - groups are what make a menu
					tall - and from there the nearest boundary is one step
					forward. A backward-only search walks past it into the
					previous group and gives up, which left a break in the
					middle of a group with a good boundary one row away.

					A boundary is "just after a separator", never the separator
					itself: starting a column with a horizontal rule looks like
					a rendering fault.
				*/
				/*
					A break may not be so early that what is left could not fit
					in the columns that are left. Without this, a boundary a
					few rows back looks free - it only shortens the column
					being closed - and the height it declines is inherited by
					the last column, which has no further column to pass it to.
					On the 133-row menu above the last column came out at
					1148px against 1040 of screen, and the whole plan was
					thrown away for it.
				*/
				auto columns_after = needed - columns;		// columns still to come
				auto floor_height = total - columns_after * budget.available_height;

				auto feasible = [&](size_t candidate)
				{
					if(candidate <= last_break || candidate >= rows.size())
						return false;
					if(prefix[candidate] < floor_height)
						return false;
					return prefix[candidate] - prefix[last_break] <= budget.available_height;
				};

				auto at = i;
				int best = -1;

				int walked = 0;
				for(size_t j = i; j > last_break + 1; j--)
				{
					walked += rows[j - 1].height;
					if(walked > slack)
						break;
					if(rows[j - 1].separator)
					{
						if(feasible(j))
						{
							best = walked;
							at = j;
						}
						break;
					}
				}

				walked = 0;
				for(size_t j = i + 1; j < rows.size(); j++)
				{
					walked += rows[j - 1].height;
					if(walked > slack)
						break;

					// Forward is the one direction that grows the column being
					// closed, so it is the one that can overflow the screen.
					if(used + walked > budget.available_height)
						break;

					if(rows[j - 1].separator)
					{
						if((best < 0 || walked < best) && feasible(j))
							at = j;
						break;
					}
				}

				plan.breaks.push_back(at);
				columns++;
				last_break = at;
				retarget(at, target, slack);

				// Resume at the first row of the new column. When the break
				// moved forward, that is past where the walk had reached.
				if(at > i)
					i = at - 1;
			}

			if(plan.breaks.empty() || columns != needed)
				return refuse(ColumnRefusal::NoBreaks);

			// Check the plan against the thing it exists to achieve, rather
			// than trusting the walk that produced it.
			size_t start = 0;
			for(size_t b = 0; b <= plan.breaks.size(); b++)
			{
				auto end = b < plan.breaks.size() ? plan.breaks[b] : rows.size();
				auto height = prefix[end] - prefix[start];
				if(height == 0)
					return refuse(ColumnRefusal::EmptyColumn);		// rule 4
				if(height > budget.available_height)
					return refuse(ColumnRefusal::ColumnTooTall);
				start = end;
			}

			plan.apply = true;
			plan.columns = columns;
			return plan;
		}
	}
}
