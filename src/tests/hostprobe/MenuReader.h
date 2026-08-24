#pragma once

/*
	What a screen reader sees, read back while the menu is still up.

	docs/refactor/08-handoff.md section 3.8 asks for this and says why: seam
	steps 6 and 7 of section 04.4 move `MenuModel` and then the presenter out of
	ContextMenu.cpp, and their regressions do not look like a failing test. They
	look like a menu that draws slightly wrong - an item in the wrong place, a
	submenu against the wrong edge, a measure pass that silently changed. The
	test project does not link ContextMenu.cpp, so the only instrument that can
	see any of that is a real menu, read back.

	Two things make this possible where a unit test cannot go. The owner thread
	is blocked inside its tracking call while the menu is up, so the reading
	happens on the driver thread that is already there. And a composed menu's
	*contents* depend on which handlers the machine has installed, so nothing
	here can be a recorded fixture: every scenario built on this asserts a
	property instead, exactly as the takeover.* scenarios already do.

	Four facts this file depends on. The first two are documented; the last two
	were measured on this machine (Windows 11 26200.8875 x64, 2026-08-25),
	because the documentation does not state them and one of them contradicts
	the obvious reading of the page.

	  - Child IDs run 1..N top to bottom, and the count includes separators:
	    "Retrieves the IDispatch for the specified menu item. The child IDs for
	    the menu items are numbered sequentially from top to bottom starting
	    with one" / "The ChildCount property is the number of menu items in the
	    menu, including menu separators."
	    https://learn.microsoft.com/windows/win32/winauto/pop-up-menu
	    That is what makes "MSAA child i is HMENU position i-1" a contract
	    rather than a guess, and it is the whole basis of the ordering
	    assertion.

	  - accLocation is a screen rectangle given as origin plus size: "This
	    method returns width and height. If you want the right and bottom
	    coordinates, calculate them using right = left + width, and bottom =
	    top + height."
	    https://learn.microsoft.com/windows/win32/api/oleacc/nf-oleacc-iaccessible-acclocation

	  - **get_accName strips the mnemonic marker and keeps the accelerator.**
	    Measured: "&Alpha" reads back as "Alpha", and "&Bravo\tCtrl+B" as
	    "Bravo\tCtrl+B" with the tab intact. The access key is separately
	    available as get_accKeyboardShortcut ("a"). So comparing an MSAA name
	    against an HMENU title means stripping '&' from the title and leaving
	    everything after a tab alone - see strip_mnemonics.

	  - **A separator reports role ROLE_SYSTEM_SEPARATOR, state
	    STATE_SYSTEM_UNAVAILABLE, and a NULL name with S_FALSE** - not S_OK
	    with an empty string. A reader that only checks the BSTR for null and
	    ignores the HRESULT happens to work; one that only checks the HRESULT
	    for FAILED does not, because S_FALSE is a success code. `has_name`
	    below is written against the measurement rather than against either
	    habit.

	One thing deliberately *not* used, because measuring it is what stopped a
	test that would have passed for the wrong reason. The Menu Item page says
	get_accChild "Retrieves the IDispatch interface to the pop-up menu object
	for this item", and descending that way does give the right object - role
	MENUPOPUP with the right child count. But **its accLocation is (0,0 0x0)**,
	before and after the submenu is opened. A placement assertion written on top
	of it would have compared a submenu at the origin against its parent and
	reported whatever the comparison's sense happened to be. Geometry comes from
	the popup's own #32768 window, and parent and child are told apart by which
	window appeared - see Probe's two-phase read.

	This file depends on nothing in the DLL, which is the rule the rest of the
	harness follows: it records what Windows says, so a defect shared with the
	code under test cannot hide in both.
*/

#include <windows.h>
#include <oleacc.h>

#include <string>
#include <vector>

namespace hostprobe
{
	struct ReadItem
	{
		std::wstring name;
		// get_accName answered S_OK with a non-empty string. False for a
		// separator, which answers S_FALSE with a null BSTR.
		bool has_name{};
		long role{};
		long state{};
		RECT rect{};
		bool has_rect{};

		bool is_separator() const { return role == ROLE_SYSTEM_SEPARATOR; }
		bool has_popup() const { return (state & STATE_SYSTEM_HASPOPUP) != 0; }
		bool unavailable() const { return (state & STATE_SYSTEM_UNAVAILABLE) != 0; }
	};

	struct ReadPopup
	{
		HWND window{};
		std::wstring name;
		RECT rect{};
		bool has_rect{};
		std::vector<ReadItem> items;
	};

	// The mnemonic markers Win32 defines, removed the way Windows removes them
	// for accessibility: "&&" is a literal ampersand, a single "&" marks the
	// next character. Everything else, the accelerator after a tab included, is
	// left exactly as it was - measured, see the header comment.
	inline std::wstring strip_mnemonics(const std::wstring &title)
	{
		std::wstring out;
		out.reserve(title.size());
		for(size_t i = 0; i < title.size(); i++)
		{
			if(title[i] == L'&')
			{
				if(i + 1 < title.size() && title[i + 1] == L'&')
				{
					out += L'&';
					i++;
				}
				// A trailing lone '&' marks nothing and disappears, which is
				// what Windows does with it.
				continue;
			}
			out += title[i];
		}
		return out;
	}

	namespace detail
	{
		inline VARIANT child_id(long id)
		{
			VARIANT v{};
			v.vt = VT_I4;
			v.lVal = id;
			return v;
		}

		inline bool location(IAccessible *acc, long id, RECT *out)
		{
			long left = 0, top = 0, width = 0, height = 0;
			auto v = child_id(id);
			if(FAILED(acc->accLocation(&left, &top, &width, &height, v)))
				return false;

			// "right = left + width, and bottom = top + height"
			out->left = left;
			out->top = top;
			out->right = left + width;
			out->bottom = top + height;
			return true;
		}

		inline bool name_of(IAccessible *acc, long id, std::wstring *out)
		{
			BSTR bstr = nullptr;
			auto v = child_id(id);
			auto hr = acc->get_accName(v, &bstr);

			// S_FALSE is a success code and is what a separator answers with a
			// null BSTR, so both halves have to be checked. Getting this wrong
			// is how a separator acquires an empty name that reads like a
			// nameless item rather than like a separator.
			bool got = hr == S_OK && bstr != nullptr && ::SysStringLen(bstr) > 0;
			if(got)
				out->assign(bstr, ::SysStringLen(bstr));
			if(bstr)
				::SysFreeString(bstr);
			return got;
		}

		inline long i4_of(const VARIANT &v) { return v.vt == VT_I4 ? v.lVal : 0; }
	}

	// Everything MSAA will say about one live popup window. An empty item list
	// means the window answered nothing, which is a failure to read rather than
	// a menu with no items - `window` is set either way so a caller can say so.
	inline ReadPopup read_popup(HWND menu_window)
	{
		ReadPopup popup;
		popup.window = menu_window;

		// OBJID_CLIENT is defined as ((LONG)0xFFFFFFFC) and the parameter is a
		// DWORD, so the conversion has to be spelled out. This is the same
		// constant docs/refactor/08-handoff.md section 3.7 records biting a
		// PowerShell caller, where 0xFFFFFFFC parses as Int32 -4; here it is a
		// signed/unsigned warning that the harness treats as an error.
		IAccessible *acc = nullptr;
		auto hr = ::AccessibleObjectFromWindow(menu_window,
											   static_cast<DWORD>(OBJID_CLIENT),
											   IID_IAccessible,
											   reinterpret_cast<void **>(&acc));
		if(FAILED(hr) || !acc)
			return popup;

		detail::name_of(acc, CHILDID_SELF, &popup.name);
		popup.has_rect = detail::location(acc, CHILDID_SELF, &popup.rect);

		long count = 0;
		if(FAILED(acc->get_accChildCount(&count)) || count <= 0)
		{
			acc->Release();
			return popup;
		}

		popup.items.reserve(static_cast<size_t>(count));
		for(long i = 1; i <= count; i++)
		{
			ReadItem item;
			item.has_name = detail::name_of(acc, i, &item.name);

			VARIANT role{};
			if(SUCCEEDED(acc->get_accRole(detail::child_id(i), &role)))
				item.role = detail::i4_of(role);

			VARIANT state{};
			if(SUCCEEDED(acc->get_accState(detail::child_id(i), &state)))
				item.state = detail::i4_of(state);

			item.has_rect = detail::location(acc, i, &item.rect);
			popup.items.push_back(item);
		}

		acc->Release();
		return popup;
	}

	// Every visible popup window on the desktop, in whatever order EnumWindows
	// gives. Callers that need to know which is the parent compare two reads
	// rather than trusting this order - see the file comment.
	inline std::vector<HWND> visible_popup_windows()
	{
		std::vector<HWND> found;
		::EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL
		{
			// GetClassNameW needs the wide form throughout; the harness is
			// compiled Unicode, so this is the wide entry point already.
			wchar_t cls[32]{};
			if(::GetClassNameW(hwnd, cls, 32) && ::wcscmp(cls, L"#32768") == 0
			   && ::IsWindowVisible(hwnd))
				reinterpret_cast<std::vector<HWND> *>(lp)->push_back(hwnd);
			return TRUE;
		}, reinterpret_cast<LPARAM>(&found));
		return found;
	}

	// What the composed HMENU says about itself, for the same positions, so the
	// two can be compared. Kept to plain data for the same reason the rest of
	// the harness is: the comparison happens after the menu is gone.
	struct MenuRow
	{
		std::wstring title;
		bool separator{};
		bool submenu{};
		bool disabled{};
	};

	inline std::vector<MenuRow> read_hmenu(HMENU menu)
	{
		std::vector<MenuRow> rows;
		if(!menu)
			return rows;

		auto count = ::GetMenuItemCount(menu);
		for(int i = 0; i < count; i++)
		{
			MenuRow row;

			MENUITEMINFOW mii{};
			mii.cbSize = sizeof(mii);
			mii.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU;
			if(!::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii))
			{
				rows.push_back(row);
				continue;
			}

			row.separator = (mii.fType & MFT_SEPARATOR) != 0;
			row.submenu = mii.hSubMenu != nullptr;
			row.disabled = (mii.fState & (MFS_DISABLED | MFS_GRAYED)) != 0;

			// The documented two-call pattern. GetMenuItemInfo "truncates the
			// item string" to whatever cch was passed and says nothing about
			// it, and third-party extensions cross MAX_PATH routinely - the
			// same trap Include/MenuText.h wraps for the DLL. The harness
			// cannot use that wrapper (it depends on nothing in the DLL), so
			// the pattern is spelled out here.
			// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmenuiteminfow
			if(!row.separator)
			{
				MENUITEMINFOW query{};
				query.cbSize = sizeof(query);
				query.fMask = MIIM_STRING;
				query.dwTypeData = nullptr;
				if(::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &query)
				   && query.cch > 0)
				{
					std::wstring buffer(static_cast<size_t>(query.cch) + 1, L'\0');
					query.dwTypeData = buffer.data();
					query.cch = query.cch + 1;
					if(::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &query))
						row.title.assign(buffer.c_str());
				}
			}

			rows.push_back(row);
		}
		return rows;
	}
}
