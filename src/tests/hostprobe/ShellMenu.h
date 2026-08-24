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

			return true;
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
		std::wstring _file;
		std::wstring _why;
	};
}
