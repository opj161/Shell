// How the Reliability Center labels a row.
//
// src/exe/src/Reliability.h has the window; this covers the one part of it
// that is pure, and the part that was wrong the first time it was looked at on
// a real machine. Six extensions from one handler family registered as
// {1FA0E654-...-B9A75D744B00} through -B05 all rendered as the same truncated
// string, because the column was narrower than a CLSID and the elision took
// the tail - which is the only part that told them apart. A list of six
// identical rows with different timings is worse than no list.

#include "test.h"

#include <windows.h>
#include "../exe/src/Reliability.h"

using Nilesoft::Shell::Reliability::elide;
using Nilesoft::Shell::Reliability::format_row;
using Nilesoft::Shell::Reliability::NameColumn;
using Nilesoft::Shell::Reliability::ProviderRow;

namespace
{
	// {1FA0E654-C9F2-4A1F-9800-B9A75D744B00}, and its sibling ...B05.
	const GUID FamilyFirst = { 0x1FA0E654, 0xC9F2, 0x4A1F,
							   { 0x98, 0x00, 0xB9, 0xA7, 0x5D, 0x74, 0x4B, 0x00 } };
	const GUID FamilyLast = { 0x1FA0E654, 0xC9F2, 0x4A1F,
							  { 0x98, 0x00, 0xB9, 0xA7, 0x5D, 0x74, 0x4B, 0x05 } };

	ProviderRow named(const wchar_t *name, uint32_t us)
	{
		ProviderRow row;
		row.name = name;
		row.worst_us = us;
		row.samples = 1;
		return row;
	}

	ProviderRow anonymous(const GUID &clsid, uint32_t us)
	{
		ProviderRow row;
		row.clsid = clsid;
		row.has_clsid = true;
		row.worst_us = us;
		row.samples = 1;
		return row;
	}
}

/*
	The column has to fit a CLSID exactly, or every unnamed extension is
	elided - and unnamed is the common case for a handler that returns no
	title, which is precisely the one a user is trying to identify.
	A formatted GUID is 38 characters including its braces.
*/
TEST(reliability_rows, the_name_column_fits_a_whole_clsid)
{
	auto guid = Nilesoft::Shell::Quarantine::format_guid(FamilyFirst);
	CHECK_EQ(guid.size(), 38u);
	CHECK(NameColumn >= guid.size());
}

// The defect itself: two members of one family must not render alike.
TEST(reliability_rows, two_clsids_from_one_family_look_different)
{
	auto first = format_row(anonymous(FamilyFirst, 19100));
	auto last = format_row(anonymous(FamilyLast, 4500));

	CHECK(first != last);

	// And specifically the identifying part survives, rather than the rows
	// merely differing by their timings.
	auto first_label = first.substr(0, NameColumn);
	auto last_label = last.substr(0, NameColumn);
	CHECK(first_label != last_label);
}

TEST(reliability_rows, a_clsid_is_shown_whole_and_not_elided)
{
	auto row = format_row(anonymous(FamilyFirst, 1000));
	auto label = row.substr(0, NameColumn);

	CHECK(label.find(L'\x2026') == std::wstring::npos);
	CHECK(label.find(L"{1FA0E654") == 0u);
	CHECK(label.find(L"744B00}") != std::wstring::npos);
}

/*
	Elision keeps both ends. Extension names share prefixes far more often
	than suffixes, so cutting the tail - the conventional choice - is the one
	that loses the distinguishing part.
*/
TEST(reliability_rows, elision_keeps_both_ends)
{
	auto out = elide(L"Microsoft Defender scan this folder for threats", 20);

	CHECK_EQ(out.size(), 20u);
	CHECK(out.find(L"Microsoft") == 0u);
	CHECK(out.find(L"threats") != std::wstring::npos);
	CHECK(out.find(L'\x2026') != std::wstring::npos);
}

TEST(reliability_rows, a_label_that_fits_is_left_alone)
{
	CHECK(elide(L"NanaZip", 38) == L"NanaZip");
	CHECK(elide(L"", 38) == L"");
}

// Exactly at the limit is not elided; one over is. Off by one here would put
// an ellipsis on the CLSIDs this whole change is about.
TEST(reliability_rows, the_boundary_is_where_it_says_it_is)
{
	std::wstring exact(38, L'x');
	CHECK(elide(exact, 38) == exact);

	std::wstring over(39, L'x');
	auto out = elide(over, 38);
	CHECK_EQ(out.size(), 38u);
	CHECK(out.find(L'\x2026') != std::wstring::npos);
}

// Every row is the same width up to the timing column, or the numbers do not
// line up and the list stops being readable as a table.
TEST(reliability_rows, every_row_starts_its_timing_at_the_same_column)
{
	auto a = format_row(named(L"NanaZip", 10600));
	auto b = format_row(anonymous(FamilyFirst, 19100));
	auto c = format_row(named(L"Unlock with File Locksmith", 6100));

	CHECK(a.compare(0, NameColumn, b, 0, NameColumn) != 0);   // different labels
	CHECK_EQ(a.find(L"ms"), b.find(L"ms"));
	CHECK_EQ(a.find(L"ms"), c.find(L"ms"));
}

// A quarantined extension says so rather than reporting the 0.0 ms it now
// costs, which reads as an extension that is simply fast.
TEST(reliability_rows, a_quarantined_row_says_so)
{
	auto row = named(L"NanaZip", 0);
	row.quarantined = true;

	CHECK(format_row(row).find(L"quarantined") != std::wstring::npos);
}

/*
	A handler's title is written for a menu item, so it carries mnemonic
	markers - this machine's Acrobat handler answers "E&dit with Adobe
	Acrobat". A LISTBOX gives `&` no meaning, so it renders literally and the
	row reads as a typo.
*/
TEST(reliability_rows, a_mnemonic_marker_is_not_shown)
{
	using Nilesoft::Shell::Reliability::without_mnemonics;

	CHECK(without_mnemonics(L"E&dit with Adobe Acrobat") == L"Edit with Adobe Acrobat");
	CHECK(format_row(named(L"E&dit with Adobe Acrobat", 87700)).find(L"E&dit")
		  == std::wstring::npos);
}

// The Win32 rule, not an invented one: a doubled ampersand is a literal.
TEST(reliability_rows, a_doubled_ampersand_is_one_ampersand)
{
	using Nilesoft::Shell::Reliability::without_mnemonics;

	CHECK(without_mnemonics(L"Rock && Roll") == L"Rock & Roll");
	CHECK(without_mnemonics(L"&&") == L"&");

	// A trailing single marker marks nothing and is simply dropped, rather
	// than reading one character past the end.
	CHECK(without_mnemonics(L"Trailing&") == L"Trailing");
	CHECK(without_mnemonics(L"") == L"");
}

// And one nothing has asked yet is distinguished from one that answered
// quickly - both would otherwise read as 0.0 ms.
TEST(reliability_rows, an_unasked_row_is_not_reported_as_ok)
{
	ProviderRow row;
	row.name = L"Something";
	row.samples = 0;

	auto text = format_row(row);
	CHECK(text.find(L"not asked yet") != std::wstring::npos);
}

// A budget deferral is not a verdict about this extension. The menu ran out of
// its allowance before reaching it; quarantining it would punish it for its
// neighbours. Before the two deferrals were told apart, this row read as
// "0.0 ms  ok" - measured, fast and fine - having never been called at all.
// docs/refactor/09-remediation-plan.md finding D.
TEST(reliability_rows, a_row_the_menu_never_reached_says_so)
{
	ProviderRow row;
	row.name = L"Edit in Notepad";
	row.samples = 0;
	row.budget_deferrals = 3;

	auto text = format_row(row);
	CHECK(text.find(L"menu ran out of budget") != std::wstring::npos);
	CHECK(text.find(L"not asked yet") == std::wstring::npos);
}

TEST(reliability_rows, a_row_shell_judged_slow_says_that_instead)
{
	ProviderRow row;
	row.name = L"Create with Designer";
	row.samples = 0;
	row.slow_deferrals = 5;

	auto text = format_row(row);
	CHECK(text.find(L"deferred as slow") != std::wstring::npos);
	CHECK(text.find(L"menu ran out of budget") == std::wstring::npos);
}

// One real measurement outranks any number of deferrals: the row has a timing
// to show, so it shows it.
TEST(reliability_rows, a_measured_row_is_not_relabelled_by_a_later_deferral)
{
	ProviderRow row;
	row.name = L"Rename with PowerRename";
	row.samples = 2;
	row.worst_us = 1600;
	row.budget_deferrals = 4;

	auto text = format_row(row);
	CHECK(text.find(L"ok") != std::wstring::npos);
	CHECK(text.find(L"menu ran out of budget") == std::wstring::npos);
}

TEST(reliability_rows, quarantine_still_outranks_everything)
{
	ProviderRow row;
	row.name = L"Something";
	row.quarantined = true;
	row.slow_deferrals = 9;
	row.budget_deferrals = 9;

	CHECK(format_row(row).find(L"quarantined") != std::wstring::npos);
}
