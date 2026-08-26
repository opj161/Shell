#pragma once

/*
	A menu built the way a file manager builds one.

	Everything else in this harness creates its menu with AppendMenu, which is
	what an ordinary application does - and Shell declines those, because
	Selections::QueryShellWindow recognises Explorer's window classes, the
	taskbar, and nothing else. That is why `--takeover` reproduces the native
	baselines byte for byte: it exercises the fail-open path, not the replay.

	A third-party file manager reaches Shell down a different road. It asks the
	shell namespace for the item's context menu and lets every registered
	handler fill an HMENU it owns:

		SHParseDisplayName -> SHBindToParent -> IShellFolder::GetUIObjectOf
		-> IContextMenu::QueryContextMenu(hmenu, ...) -> TrackPopupMenu

	Shell is registered as a ContextMenuHandler for "*" and "Directory"
	(src/shared/RegistryConfig.h, RegisterContextMenuHandler), so step four runs
	Shell's own IShellExtInit/IContextMenu implementation, which records the
	selection against that HMENU. `ShellExtCapture::has(hMenu)` is then true when
	the popup hook sees the same handle, `loader.contextmenuhandler` is set, and
	QueryShellWindow takes its `goto ui` branch - so Shell takes the menu over
	whatever window the host owns.

	That is the path docs/refactor/01-takeover-contract.md section 3 is about,
	and the one commit b63fdc2 fixed. Directory Opus and Total Commander get here
	the same way.

	Two consequences for how these scenarios are written:

	  - **Their traces cannot be fixtures.** The items come from whichever
	    handlers the machine has installed, so the message stream is not
	    reproducible anywhere else. They assert properties instead.
	  - **They only make sense under --takeover**, and not because the mode is
	    optional: QueryContextMenu loads Shell into this process through COM
	    whether or not anybody asked, which installs the hook. Requiring the
	    flag makes that explicit rather than surprising.

	The item is a temporary file this harness creates and deletes, not one of
	the user's, and nothing here ever calls InvokeCommand - the trace records
	which command the host was told to run, and stops there.
*/

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <string>

namespace hostprobe
{
	class ShellMenu
	{
	public:
		// QueryContextMenu is given this range. The floor is 1 because zero is
		// documented as "no item": "the value of idCmdFirst ... the menu item
		// identifiers must be in the range idCmdFirst to idCmdLast".
		// https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-icontextmenu-querycontextmenu
		static constexpr UINT ID_FIRST = 1;
		static constexpr UINT ID_LAST = 0x7000;

		~ShellMenu() { destroy(); }

		HMENU menu() const { return _menu; }
		bool valid() const { return _menu != nullptr; }
		const std::wstring &why() const { return _why; }

		// An enabled item that is neither a separator nor a submenu, so a script
		// has somewhere deterministic to steer to. Zero when the menu had none.
		UINT drivable_command() const { return _drivable; }

		bool create(HWND owner)
		{
			destroy();

			if(!make_temp_file())
				return false;

			PIDLIST_ABSOLUTE pidl = nullptr;
			auto hr = ::SHParseDisplayName(_file.c_str(), nullptr, &pidl, 0, nullptr);
			if(FAILED(hr) || !pidl)
				return fail(L"SHParseDisplayName", hr);

			IShellFolder *folder = nullptr;
			PCUITEMID_CHILD child = nullptr;
			hr = ::SHBindToParent(pidl, IID_IShellFolder,
								  reinterpret_cast<void **>(&folder), &child);
			if(FAILED(hr) || !folder)
			{
				::CoTaskMemFree(pidl);
				return fail(L"SHBindToParent", hr);
			}

			hr = folder->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr,
									   reinterpret_cast<void **>(&_cm));
			folder->Release();
			::CoTaskMemFree(pidl);

			if(FAILED(hr) || !_cm)
				return fail(L"GetUIObjectOf", hr);

			_menu = ::CreatePopupMenu();
			if(!_menu)
				return fail(L"CreatePopupMenu", HRESULT_FROM_WIN32(::GetLastError()));

			// CMF_NORMAL is the plain right-click. Anything else would be asking
			// the handlers a different question than the one a file manager asks.
			hr = _cm->QueryContextMenu(_menu, 0, ID_FIRST, ID_LAST, CMF_NORMAL);
			if(FAILED(hr))
				return fail(L"QueryContextMenu", hr);

			_drivable = find_drivable(_menu);
			if(!_drivable)
				return fail(L"no enabled command item in the menu", S_OK);

			_drivable_position = find_position(_menu, _drivable);

			return true;
		}

		/*
			Make this the kind of menu that is addressed by position.

			A menu *header* style, so it is set once on the root and applies to
			the whole tree: "MNS_NOTIFYBYPOS is a menu header style and has no
			effect when applied to individual sub menus."
			https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-menuinfo

			Also appends one item carrying the *same identifier* as the drivable
			one. That is legal in a menu nobody addresses by identifier, and it
			is the case a tracking table keyed on the host's own wID gets wrong:
			the later item overwrites the earlier, the navigation still stops at
			the first, and the position replayed is the wrong one. Without it a
			scenario built on a menu of unique identifiers passes against an
			implementation that never separated tracking identity from host
			identity at all.
		*/
		bool apply_notify_by_position()
		{
			if(!_menu)
				return false;

			::AppendMenuW(_menu, MF_SEPARATOR, 0, nullptr);

			// One item repeating the identifier of a real one. Legal in a menu
			// nobody addresses by identifier, and the case a tracking table keyed
			// on the host's own wID gets wrong: the later entry overwrites the
			// earlier, the driver still stops at the first, and the position
			// replayed is the wrong one.
			::AppendMenuW(_menu, MF_STRING, _drivable, L"Probe Duplicate Identifier");

			// And two with no identifier at all, which is the ordinary shape of
			// an item in such a menu. Composition used to hand these one of
			// *Shell's* identifiers, which made a host item look like one of
			// Shell's own - so the click ran nothing and told the host nothing.
			// The last is the destination, because it is the case this whole
			// workstream exists for.
			::AppendMenuW(_menu, MF_STRING, 0, L"Probe Zero Identifier A");
			::AppendMenuW(_menu, MF_STRING, 0, TARGET_TITLE);
			::AppendMenuW(_menu, MF_STRING, 0, L"Probe Zero Identifier C");
			::AppendMenuW(_menu, MF_STRING, 0, L"Probe Zero Identifier D");

			MENUINFO mi{};
			mi.cbSize = sizeof(mi);
			mi.fMask = MIM_STYLE;
			mi.dwStyle = MNS_NOTIFYBYPOS;
			if(!::SetMenuInfo(_menu, &mi))
				return false;

			// Deliberately not the last item. Shell composes a menu of its own
			// with a different length - it drops some of the host's items and
			// appends packaged verbs of its own - so "last in the host's menu"
			// and "last in Shell's" tend to be the same *index* by accident, and
			// a scenario that steered to it would pass against an implementation
			// replaying its own position rather than the host's.
			auto count = ::GetMenuItemCount(_menu);
			if(count < 3)
				return false;
			_target_position = static_cast<UINT>(count - 3);

			// The appends above do not move the drivable item - it was found by
			// forward scan and they go on the end - but re-reading is one call
			// and removes the assumption.
			_drivable_position = find_position(_menu, _drivable);
			return _drivable_position != NOT_FOUND;
		}

		static constexpr UINT NOT_FOUND = 0xFFFFFFFF;

		// A title no configuration rule matches and no handler produces, so a
		// driver steering to it cannot land on something else. No mnemonic in
		// it: the driver strips '&' before comparing, but a title without one
		// cannot disagree with itself about what it says.
		static constexpr const wchar_t *TARGET_TITLE = L"Probe Zero Identifier B";

		// Where the item the by-position script chooses sits in the *host's* own
		// menu, which is what WM_MENUCOMMAND's wParam has to carry.
		UINT target_position() const { return _target_position; }

		// Where an identifier-driven script's destination sits.
		UINT drivable_position() const { return _drivable_position; }

		/*
			What the host's own menu says at `position`, so a replayed position
			can be checked against the item it claims to name rather than only
			against a number. Empty when there is nothing there.

			The documented two-call pattern: ask with dwTypeData null to learn
			cch, then read into cch + 1.
			https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmenuiteminfow

			This read into wchar_t[256] with a literal cch, which is the exact
			trap AGENTS.md names and the project had already solved twice -
			Probe.h and MenuReader.h both spell out this pattern, and the DLL has
			Include/MenuText.h. It came back in new harness code anyway, which is
			why invariant rule 11 now exists.

			The effect was a weakened assertion rather than a visible failure,
			which is worse: both expected_title and replayed_title truncate the
			same way, so two different long titles sharing a 255-character prefix
			compared equal. And this reads the *host's* real shell menu, which is
			where long third-party titles come from.
		*/
		std::wstring title_at(UINT position) const
		{
			if(!_menu || position >= static_cast<UINT>(::GetMenuItemCount(_menu)))
				return {};

			MENUITEMINFOW query{};
			query.cbSize = sizeof(query);
			query.fMask = MIIM_STRING;
			query.dwTypeData = nullptr;
			if(!::GetMenuItemInfoW(_menu, position, TRUE, &query) || query.cch == 0)
				return {};

			std::wstring buffer(static_cast<size_t>(query.cch) + 1, L'\0');
			query.dwTypeData = buffer.data();
			query.cch = query.cch + 1;
			if(!::GetMenuItemInfoW(_menu, position, TRUE, &query))
				return {};

			return std::wstring(buffer.c_str());
		}

		void destroy()
		{
			if(_menu)
			{
				::DestroyMenu(_menu);
				_menu = nullptr;
			}
			if(_cm)
			{
				_cm->Release();
				_cm = nullptr;
			}
			if(!_file.empty())
			{
				::DeleteFileW(_file.c_str());
				_file.clear();
			}
			_drivable = 0;
			_drivable_position = NOT_FOUND;
			_target_position = NOT_FOUND;
		}

	private:
		bool fail(const wchar_t *what, HRESULT hr)
		{
			wchar_t buffer[256];
			::swprintf_s(buffer, L"%s -> 0x%08lX", what, static_cast<unsigned long>(hr));
			_why = buffer;
			destroy();
			return false;
		}

		bool make_temp_file()
		{
			wchar_t dir[MAX_PATH]{};
			if(!::GetTempPathW(MAX_PATH, dir))
				return fail(L"GetTempPath", HRESULT_FROM_WIN32(::GetLastError()));

			wchar_t path[MAX_PATH]{};
			::swprintf_s(path, L"%shostprobe-%lu.txt", dir, ::GetCurrentProcessId());

			auto h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
								   FILE_ATTRIBUTE_NORMAL, nullptr);
			if(h == INVALID_HANDLE_VALUE)
				return fail(L"CreateFile", HRESULT_FROM_WIN32(::GetLastError()));
			::CloseHandle(h);

			_file = path;
			return true;
		}

		// First position holding this identifier. First, deliberately: the
		// navigation stops at the first match too, so the two agree even when
		// the menu repeats an identifier.
		static UINT find_position(HMENU menu, UINT id)
		{
			auto count = ::GetMenuItemCount(menu);
			for(int i = 0; i < count; i++)
			{
				MENUITEMINFOW mii{};
				mii.cbSize = sizeof(mii);
				mii.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_FTYPE;
				if(!::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii))
					continue;
				if(mii.hSubMenu || (mii.fType & MFT_SEPARATOR))
					continue;
				if(mii.wID == id)
					return static_cast<UINT>(i);
			}
			return NOT_FOUND;
		}

		// First item that a keyboard script can actually land on and commit.
		static UINT find_drivable(HMENU menu)
		{
			auto count = ::GetMenuItemCount(menu);
			for(int i = 0; i < count; i++)
			{
				MENUITEMINFOW mii{};
				mii.cbSize = sizeof(mii);
				mii.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
				if(!::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii))
					continue;
				if(mii.hSubMenu)
					continue;
				if(mii.fType & MFT_SEPARATOR)
					continue;
				if(mii.fState & (MFS_DISABLED | MFS_GRAYED))
					continue;
				if(mii.wID == 0)
					continue;
				return mii.wID;
			}
			return 0;
		}

		HMENU _menu{};
		IContextMenu *_cm{};
		UINT _drivable{};
		UINT _drivable_position{ NOT_FOUND };
		UINT _target_position{ NOT_FOUND };
		std::wstring _file;
		std::wstring _why;
	};
}
