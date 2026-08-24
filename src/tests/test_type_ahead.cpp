// Typing a word in the menu.
//
// docs/refactor/05-capabilities.md section 4, Stage 2. Stage 1 made a menu
// keyboard-drivable by mnemonic; this makes it drivable by name, which is what
// a menu built from packaged verbs and NSS rules actually needs - most of those
// titles declare no mnemonic at all.
//
// Three decisions carry the design and none is forced by documentation: a
// prefix selects rather than executes, mnemonics keep precedence on the first
// character, and a character matching nothing is not added to the buffer. Each
// has a test that fails if it is reversed.

#include "test.h"

#include "..\dll\src\Include\TypeAhead.h"

#include <vector>

using namespace Nilesoft::Shell;

namespace
{
	bool starts(const wchar_t *title, const wchar_t *prefix)
	{
		return title_starts_with(title, prefix, ::wcslen(prefix));
	}

	MnemonicReply by_prefix(const wchar_t *prefix, const std::vector<TypeAheadItem> &items)
	{
		return choose_by_prefix(prefix, ::wcslen(prefix), items.data(), items.size());
	}

	TypeAheadItem item(int position, const wchar_t *title, bool selectable = true)
	{
		return TypeAheadItem{ position, title, selectable };
	}
}

// ---- matching a prefix against a title ----------------------------------

TEST(type_ahead, a_plain_prefix_matches)
{
	CHECK(starts(L"Properties", L"Pro"));
	CHECK(starts(L"Properties", L"Properties"));
	CHECK(!starts(L"Properties", L"Prot"));
}

TEST(type_ahead, a_prefix_longer_than_the_title_does_not_match)
{
	CHECK(!starts(L"Cut", L"Cutting"));
	CHECK(!starts(L"", L"C"));
}

TEST(type_ahead, matching_is_case_insensitive_in_both_directions)
{
	CHECK(starts(L"Properties", L"pro"));
	CHECK(starts(L"properties", L"PRO"));
	CHECK(starts(L"PROPERTIES", L"Pro"));
}

// "&Open" displays as "Open", so the marker must not have to be typed.
TEST(type_ahead, a_mnemonic_marker_is_not_part_of_the_label)
{
	CHECK(starts(L"&Open", L"Op"));
	CHECK(starts(L"E&dit with Adobe Acrobat", L"Edit"));
	CHECK(starts(L"E&dit with Adobe Acrobat", L"Edit with"));
	CHECK(!starts(L"&Open", L"&O"));
}

// "&&" displays one ampersand, and that ampersand can be typed.
TEST(type_ahead, a_doubled_ampersand_is_a_literal_that_can_be_matched)
{
	CHECK(starts(L"Search && Replace", L"Search & Re"));
	CHECK(!starts(L"Search && Replace", L"Search && Re"));
}

// "Rename\tF2" displays "Rename". Matching into the accelerator would let
// somebody type a shortcut key and land somewhere they did not name.
TEST(type_ahead, the_accelerator_column_is_not_part_of_the_label)
{
	CHECK(starts(L"Rename\tF2", L"Rena"));
	CHECK(starts(L"Rename\tF2", L"Rename"));
	CHECK(!starts(L"Rename\tF2", L"Rename\tF"));
	CHECK(!starts(L"Rename\tF2", L"RenameF"));
}

TEST(type_ahead, an_empty_prefix_or_a_missing_title_matches_nothing)
{
	CHECK(!starts(L"Properties", L""));
	CHECK(!title_starts_with(nullptr, L"P", 1));
	CHECK(!title_starts_with(L"Properties", nullptr, 1));
}

// ---- choosing an item ---------------------------------------------------

TEST(type_ahead, the_first_item_in_display_order_wins)
{
	std::vector<TypeAheadItem> items{
		item(0, L"Copy"), item(1, L"Copy as path"), item(2, L"Cut")
	};

	auto reply = by_prefix(L"Co", items);
	CHECK_EQ(reply.action, (UINT)MNC_SELECT);
	CHECK_EQ(reply.position, (UINT)0);
}

// The whole point of the design: menus contain Delete, and somebody typing "de"
// looking for "Deselect" must not have executed it before they saw it.
TEST(type_ahead, a_unique_prefix_selects_rather_than_executing)
{
	std::vector<TypeAheadItem> items{ item(0, L"Copy"), item(1, L"Delete") };

	auto reply = by_prefix(L"Del", items);
	CHECK_EQ(reply.action, (UINT)MNC_SELECT);
	CHECK_MSG(reply.action != MNC_EXECUTE, "type-ahead must never execute");
	CHECK_EQ(reply.position, (UINT)1);
}

TEST(type_ahead, a_longer_prefix_narrows_to_a_later_item)
{
	std::vector<TypeAheadItem> items{
		item(0, L"Copy"), item(1, L"Copy as path"), item(2, L"Cut")
	};

	CHECK_EQ(by_prefix(L"Copy a", items).position, (UINT)1);
	CHECK_EQ(by_prefix(L"Cu", items).position, (UINT)2);
}

TEST(type_ahead, a_prefix_that_names_nothing_is_ignored)
{
	std::vector<TypeAheadItem> items{ item(0, L"Copy"), item(1, L"Cut") };

	auto reply = by_prefix(L"Z", items);
	CHECK_EQ(reply.action, (UINT)MNC_IGNORE);
	CHECK_MSG(reply.action != MNC_CLOSE,
			  "a mistyped letter must not dismiss the menu the user is reading");
}

TEST(type_ahead, a_disabled_item_is_skipped_rather_than_selected)
{
	std::vector<TypeAheadItem> items{
		item(0, L"Paste", false), item(1, L"Paste shortcut")
	};

	CHECK_EQ(by_prefix(L"Pas", items).position, (UINT)1);
}

TEST(type_ahead, an_empty_popup_answers_ignore)
{
	CHECK_EQ(choose_by_prefix(L"a", 1, nullptr, 0).action, (UINT)MNC_IGNORE);

	std::vector<TypeAheadItem> none;
	CHECK_EQ(by_prefix(L"a", none).action, (UINT)MNC_IGNORE);
}

// The position is an index into the popup, not a command identifier - the same
// rule Stage 1 measured, and getting it wrong here would be just as silent.
TEST(type_ahead, the_reply_carries_the_position_and_packs_it_low)
{
	std::vector<TypeAheadItem> items{ item(4, L"Zoom") };

	auto reply = by_prefix(L"Zo", items);
	CHECK_EQ(reply.position, (UINT)4);
	CHECK_EQ(HIWORD(reply.to_lresult()), (WORD)MNC_SELECT);
	CHECK_EQ(LOWORD(reply.to_lresult()), (WORD)4);
}

// ---- the buffer ---------------------------------------------------------

TEST(type_ahead, a_fresh_buffer_is_empty)
{
	TypeAheadBuffer buffer;
	CHECK(buffer.empty());
	CHECK_EQ(buffer.length(), (size_t)0);
}

TEST(type_ahead, characters_accumulate_into_a_prefix)
{
	TypeAheadBuffer buffer;
	auto *popup = reinterpret_cast<const void *>(1);

	wchar_t prefix[64]{};
	size_t length = 0;

	CHECK(buffer.would_be(L'p', prefix, ARRAYSIZE(prefix), length));
	CHECK_EQ(length, (size_t)1);
	buffer.accept(prefix, length, 1000, popup);

	CHECK(buffer.would_be(L'r', prefix, ARRAYSIZE(prefix), length));
	CHECK_EQ(length, (size_t)2);
	buffer.accept(prefix, length, 1000, popup);

	CHECK(::wcscmp(buffer.text(), L"pr") == 0);
}

// One second of idle and the word is over - the same rule Explorer's own list
// views use.
TEST(type_ahead, the_buffer_clears_after_its_timeout)
{
	TypeAheadBuffer buffer;
	auto *popup = reinterpret_cast<const void *>(1);

	buffer.accept(L"pro", 3, 1000, popup);

	buffer.refresh(1000 + TypeAheadBuffer::TIMEOUT_MS, popup);
	CHECK_MSG(!buffer.empty(), "exactly at the timeout is still current");

	buffer.refresh(1001 + TypeAheadBuffer::TIMEOUT_MS, popup);
	CHECK_MSG(buffer.empty(), "past the timeout the word is over");
}

// A submenu is a new list. Carrying "de" into it would select something the
// user never typed towards.
TEST(type_ahead, the_buffer_clears_when_the_popup_changes)
{
	TypeAheadBuffer buffer;
	auto *first = reinterpret_cast<const void *>(1);
	auto *second = reinterpret_cast<const void *>(2);

	buffer.accept(L"de", 2, 1000, first);
	buffer.refresh(1010, second);
	CHECK(buffer.empty());
}

// A clock that went backwards - a machine waking from sleep - must clear rather
// than compute a huge elapsed time or a negative one.
TEST(type_ahead, a_clock_that_went_backwards_clears_the_buffer)
{
	TypeAheadBuffer buffer;
	auto *popup = reinterpret_cast<const void *>(1);

	buffer.accept(L"de", 2, 5000, popup);
	buffer.refresh(4000, popup);
	CHECK(buffer.empty());
}

TEST(type_ahead, the_buffer_stops_growing_rather_than_overflowing)
{
	TypeAheadBuffer buffer;
	auto *popup = reinterpret_cast<const void *>(1);

	std::wstring full(TypeAheadBuffer::CAPACITY, L'a');
	buffer.accept(full.c_str(), full.size(), 1000, popup);
	CHECK_EQ(buffer.length(), TypeAheadBuffer::CAPACITY);

	wchar_t prefix[TypeAheadBuffer::CAPACITY + 2]{};
	size_t length = 0;
	CHECK_MSG(!buffer.would_be(L'b', prefix, ARRAYSIZE(prefix), length),
			  "a full buffer refuses rather than wrapping or truncating");
	CHECK_EQ(buffer.length(), TypeAheadBuffer::CAPACITY);
}

TEST(type_ahead, clearing_forgets_the_popup_as_well_as_the_text)
{
	TypeAheadBuffer buffer;
	auto *popup = reinterpret_cast<const void *>(1);

	buffer.accept(L"de", 2, 1000, popup);
	buffer.clear();
	CHECK(buffer.empty());

	// And refreshing against the same popup does not resurrect anything.
	buffer.refresh(1010, popup);
	CHECK(buffer.empty());
}

/*
	The sequence that makes the third design decision visible.

	Typing "c", "u", then a "z" that names nothing, then "t": the z must not
	poison the buffer. If it were appended, "cuz" would match nothing and so
	would "cuzt", and the word the user was typing would be unreachable for a
	whole second.
*/
TEST(type_ahead, a_character_that_matches_nothing_does_not_poison_the_word)
{
	std::vector<TypeAheadItem> items{ item(0, L"Copy"), item(1, L"Cut") };

	TypeAheadBuffer buffer;
	auto *popup = reinterpret_cast<const void *>(1);

	wchar_t prefix[64]{};
	size_t length = 0;

	// c
	CHECK(buffer.would_be(L'c', prefix, ARRAYSIZE(prefix), length));
	CHECK_EQ(choose_by_prefix(prefix, length, items.data(), items.size()).position, (UINT)0);
	buffer.accept(prefix, length, 1000, popup);

	// u -> Cut
	CHECK(buffer.would_be(L'u', prefix, ARRAYSIZE(prefix), length));
	CHECK_EQ(choose_by_prefix(prefix, length, items.data(), items.size()).position, (UINT)1);
	buffer.accept(prefix, length, 1000, popup);

	// z -> nothing, and so not accepted
	CHECK(buffer.would_be(L'z', prefix, ARRAYSIZE(prefix), length));
	CHECK_EQ(choose_by_prefix(prefix, length, items.data(), items.size()).action, (UINT)MNC_IGNORE);
	CHECK_MSG(::wcscmp(buffer.text(), L"cu") == 0,
			  "the rejected character is not in the buffer");

	// t -> still Cut, because the buffer is still "cu"
	CHECK(buffer.would_be(L't', prefix, ARRAYSIZE(prefix), length));
	CHECK(::wcscmp(prefix, L"cut") == 0);
	CHECK_EQ(choose_by_prefix(prefix, length, items.data(), items.size()).position, (UINT)1);
}
