// Reading a menu item's text out of a host menu.
//
// Shell's job on the IShellExtInit path is reproducing somebody else's menu, so
// the item's string is the payload. Every read used to hand GetMenuItemInfoW a
// fixed MAX_PATH buffer with cch = MAX_PATH, which truncates silently at 259
// characters - and third-party extensions cross that line routinely: archivers
// that embed the target name in "Add to ...", "Open with" entries, VCS shells.
//
// These drive real HMENUs rather than a mock. The truncation was the system's
// behaviour, not the tree's, so only the system can show it is gone - and the
// reader under test is the one the DLL actually calls (Include\MenuText.h is
// deliberately self-contained so this file can reach it).

#include "test.h"

#include <windows.h>
#include <string>

#include "Include/MenuText.h"

using Nilesoft::Shell::read_menu_text;

namespace
{
	std::wstring read_item_text(HMENU menu, UINT position)
	{
		std::wstring buffer;
		MENUITEMINFOW mii{ sizeof(mii) };
		mii.fMask = MIIM_STRING;

		if(!read_menu_text(menu, position, TRUE, &mii,
						   [&buffer](UINT count) -> wchar_t *
						   {
							   buffer.assign(static_cast<size_t>(count) + 1, L'\0');
							   return &buffer[0];
						   }))
			return {};

		buffer.resize(mii.cch);
		return buffer;
	}

	// What the tree used to do, kept so the test states what it guards against.
	std::wstring read_item_text_fixed_buffer(HMENU menu, UINT position)
	{
		wchar_t buffer[MAX_PATH]{};
		MENUITEMINFOW mii{ sizeof(mii) };
		mii.fMask = MIIM_STRING;
		mii.dwTypeData = buffer;
		mii.cch = MAX_PATH;
		if(!::GetMenuItemInfoW(menu, position, TRUE, &mii))
			return {};
		return std::wstring(buffer, mii.cch);
	}

	struct ScopedMenu
	{
		HMENU handle = ::CreatePopupMenu();
		~ScopedMenu() { if(handle) ::DestroyMenu(handle); }
		operator HMENU() const { return handle; }
	};
}

TEST(menutext, an_item_longer_than_max_path_survives_intact)
{
	std::wstring text(400, L'x');
	text.front() = L'A';
	text.back() = L'Z';

	ScopedMenu menu;
	CHECK(menu.handle != nullptr);
	CHECK(::AppendMenuW(menu, MF_STRING, 1, text.c_str()) != FALSE);

	auto got = read_item_text(menu, 0);
	CHECK_EQ((int)got.size(), 400);
	CHECK_MSG(got == text, "the whole string, not the first 259 characters");

	auto truncated = read_item_text_fixed_buffer(menu, 0);
	CHECK_MSG(truncated.size() == 259,
			  "a fixed MAX_PATH buffer is what used to lose the tail");
}

TEST(menutext, an_ordinary_item_reads_back_exactly)
{
	ScopedMenu menu;
	CHECK(::AppendMenuW(menu, MF_STRING, 1, L"Open with &Notepad") != FALSE);

	auto got = read_item_text(menu, 0);
	CHECK(got == L"Open with &Notepad");
	CHECK_EQ((int)got.size(), 18);
}

// The sizing call is what makes the pattern safe to use unconditionally: a
// non-string item answers cch == 0, so it costs one extra call and no buffer.
TEST(menutext, a_separator_costs_no_buffer_and_reads_back_empty)
{
	ScopedMenu menu;
	CHECK(::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr) != FALSE);

	bool allocated = false;
	MENUITEMINFOW mii{ sizeof(mii) };
	mii.fMask = MIIM_STRING;
	auto ok = read_menu_text(menu, 0, TRUE, &mii,
							 [&allocated](UINT) -> wchar_t * { allocated = true; return nullptr; });

	CHECK_MSG(ok, "a separator is not a failure to read");
	CHECK_EQ((int)mii.cch, 0);
	CHECK_MSG(!allocated, "no allocation for an item that has no string");
}

TEST(menutext, an_empty_title_is_empty_rather_than_absent)
{
	ScopedMenu menu;
	CHECK(::AppendMenuW(menu, MF_STRING, 1, L"") != FALSE);
	CHECK_EQ((int)read_item_text(menu, 0).size(), 0);
}

TEST(menutext, unicode_beyond_the_bmp_is_not_split)
{
	// U+1F4C1 FILE FOLDER as a surrogate pair, a space, and two CJK
	// ideographs - five UTF-16 units for four glyphs.
	const wchar_t text[] = L"\xD83D\xDCC1 \x4E2D\x6587";

	ScopedMenu menu;
	CHECK(::AppendMenuW(menu, MF_STRING, 1, text) != FALSE);

	auto got = read_item_text(menu, 0);
	CHECK_EQ((int)got.size(), 5);
	CHECK(got == text);
}

TEST(menutext, a_missing_item_fails_rather_than_returning_rubbish)
{
	ScopedMenu menu;
	CHECK(::AppendMenuW(menu, MF_STRING, 1, L"only") != FALSE);

	MENUITEMINFOW mii{ sizeof(mii) };
	mii.fMask = MIIM_STRING;
	CHECK(!read_menu_text(menu, 7, TRUE, &mii, [](UINT) -> wchar_t * { return nullptr; }));
	CHECK_EQ((int)mii.cch, 0);
	CHECK(read_item_text(menu, 7).empty());
}
