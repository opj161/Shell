#include "test.h"

#include <windows.h>
#include "Include/Mnemonics.h"

#include <vector>

// Mnemonics: what a typed character does in a menu Windows cannot read.
//
// Every item Shell renders is owner-drawn, so there is no text in the HMENU for
// Windows to match against and WM_MENUCHAR is the only route a keystroke has.
// Nothing answered it, so a keypress beeped - in menus whose titles are full of
// mnemonics, because Explorer's own verbs and packaged verb handlers both carry
// them ("E&dit with Adobe Acrobat", "&Move to OneDrive").
//
// The low word of the reply is a zero-based *index*, not an identifier. That is
// stated in Using Menus and measured in
// src/tests/hostprobe/fixtures/question.menuchar_low_word_is_*: replying with an
// index picks the right item, replying with an identifier picks nothing at all.
// Shell's identifiers start at 0x0fffffff, so getting it backwards would be a
// silently dead keyboard rather than a visible bug.

using namespace Nilesoft::Shell;

namespace
{
	MnemonicItem item(int position, wchar_t mnemonic, bool selectable = true)
	{
		MnemonicItem i;
		i.position = position;
		i.mnemonic = mnemonic;
		i.selectable = selectable;
		return i;
	}
}

TEST(mnemonics, a_marked_character_is_the_mnemonic)
{
	CHECK(mnemonic_of(L"&Open") == L'O');
	CHECK(mnemonic_of(L"E&dit with Adobe Acrobat") == L'D');
	CHECK(mnemonic_of(L"Open in &Terminal") == L'T');
}

TEST(mnemonics, matching_is_case_insensitive_in_both_directions)
{
	// The stored marker and the typed key can each be either case.
	CHECK(mnemonic_of(L"&open") == mnemonic_of(L"&Open"));
	CHECK(upper_char(L'o') == upper_char(L'O'));
}

TEST(mnemonics, a_doubled_ampersand_is_a_literal_and_marks_nothing)
{
	// "Search && Replace" displays one ampersand and has no mnemonic.
	CHECK(mnemonic_of(L"Search && Replace") == 0);

	// ...but a real marker after one still counts.
	CHECK(mnemonic_of(L"Search && &Replace") == L'R');
}

TEST(mnemonics, only_the_first_marker_counts)
{
	CHECK(mnemonic_of(L"&Copy &Path") == L'C');
}

TEST(mnemonics, a_marker_in_the_accelerator_column_is_not_a_mnemonic)
{
	// Everything after a tab is the shortcut text, not the label.
	CHECK(mnemonic_of(L"Rename\t&F2") == 0);
	CHECK(mnemonic_of(L"&Rename\tF2") == L'R');
}

TEST(mnemonics, a_title_with_no_marker_has_no_mnemonic)
{
	CHECK(mnemonic_of(L"Open") == 0);
	CHECK(mnemonic_of(L"") == 0);
	CHECK(mnemonic_of(nullptr) == 0);
	CHECK(mnemonic_of(L"Trailing&") == 0);
}

TEST(mnemonics, an_unmatched_key_is_ignored_rather_than_closing_the_menu)
{
	// MNC_CLOSE would dismiss what the user was reading because they mistyped.
	std::vector<MnemonicItem> items{ item(0, L'O'), item(1, L'C') };
	auto reply = choose_mnemonic(L'Z', items.data(), items.size(), -1);

	CHECK_EQ(reply.action, static_cast<UINT>(MNC_IGNORE));
}

TEST(mnemonics, a_unique_match_is_executed)
{
	std::vector<MnemonicItem> items{ item(0, L'O'), item(2, L'C'), item(3, L'D') };
	auto reply = choose_mnemonic(L'c', items.data(), items.size(), -1);

	CHECK_EQ(reply.action, static_cast<UINT>(MNC_EXECUTE));
	CHECK_EQ(reply.position, 2u);
}

TEST(mnemonics, the_reply_carries_the_position_not_the_index_into_the_candidate_list)
{
	// The distinction that matters once separators are in the menu: the third
	// item with a mnemonic can be at position 5.
	std::vector<MnemonicItem> items{ item(0, L'A'), item(3, L'B'), item(5, L'C') };
	auto reply = choose_mnemonic(L'C', items.data(), items.size(), -1);

	CHECK_EQ(reply.position, 5u);
}

TEST(mnemonics, duplicated_mnemonics_select_rather_than_execute)
{
	// Windows' own behaviour: with more than one candidate the first press moves
	// the highlight and does not commit, so the user can see which one they got.
	std::vector<MnemonicItem> items{ item(1, L'O'), item(4, L'O') };
	auto reply = choose_mnemonic(L'O', items.data(), items.size(), -1);

	CHECK_EQ(reply.action, static_cast<UINT>(MNC_SELECT));
	CHECK_EQ(reply.position, 1u);
}

TEST(mnemonics, pressing_the_same_key_again_moves_to_the_next_match)
{
	std::vector<MnemonicItem> items{ item(1, L'O'), item(4, L'O'), item(7, L'O') };

	CHECK_EQ(choose_mnemonic(L'O', items.data(), items.size(), 1).position, 4u);
	CHECK_EQ(choose_mnemonic(L'O', items.data(), items.size(), 4).position, 7u);
}

TEST(mnemonics, the_cycle_wraps_round_to_the_first_match)
{
	// Otherwise the last duplicate is a dead end and the user has to reach for
	// the arrow keys.
	std::vector<MnemonicItem> items{ item(1, L'O'), item(4, L'O') };
	auto reply = choose_mnemonic(L'O', items.data(), items.size(), 4);

	CHECK_EQ(reply.action, static_cast<UINT>(MNC_SELECT));
	CHECK_EQ(reply.position, 1u);
}

TEST(mnemonics, a_highlight_somewhere_unrelated_still_finds_the_next_match)
{
	std::vector<MnemonicItem> items{ item(1, L'O'), item(6, L'O') };
	auto reply = choose_mnemonic(L'O', items.data(), items.size(), 3);

	CHECK_EQ(reply.position, 6u);
}

TEST(mnemonics, a_disabled_item_does_not_swallow_the_keystroke)
{
	// It is still drawn and still shows its underline, but choosing it would do
	// nothing - so the enabled duplicate below it should get the key instead of
	// the press appearing to be ignored.
	std::vector<MnemonicItem> items{ item(1, L'O', false), item(4, L'O') };
	auto reply = choose_mnemonic(L'O', items.data(), items.size(), -1);

	CHECK_EQ(reply.action, static_cast<UINT>(MNC_EXECUTE));
	CHECK_EQ(reply.position, 4u);
}

TEST(mnemonics, an_empty_popup_answers_ignore_rather_than_reaching_into_nothing)
{
	CHECK_EQ(choose_mnemonic(L'O', nullptr, 0, -1).action, static_cast<UINT>(MNC_IGNORE));

	std::vector<MnemonicItem> none;
	CHECK_EQ(choose_mnemonic(L'O', none.data(), none.size(), -1).action,
			 static_cast<UINT>(MNC_IGNORE));
}

TEST(mnemonics, the_reply_packs_the_action_high_and_the_position_low)
{
	// The shape Windows reads: MAKELRESULT(position, action). Packing them the
	// other way round would make MNC_EXECUTE(2) read as position 2 and an item
	// index read as an action nobody defined.
	MnemonicReply reply;
	reply.action = MNC_EXECUTE;
	reply.position = 5;

	auto packed = reply.to_lresult();
	CHECK_EQ(HIWORD(packed), static_cast<UINT>(MNC_EXECUTE));
	CHECK_EQ(LOWORD(packed), 5u);
}

TEST(mnemonics, an_ignore_reply_is_exactly_what_defwindowproc_would_have_returned)
{
	// So handling the message costs nothing when there is nothing to match: the
	// system beeps and discards the character, as it did before.
	MnemonicReply reply;
	CHECK_EQ(reply.to_lresult(), MAKELRESULT(0, MNC_IGNORE));
}
