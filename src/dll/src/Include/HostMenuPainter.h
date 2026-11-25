#pragma once

/*
	Has the host already replaced this menu window's procedure?

	Shell draws a menu by owner-drawing every item into a #32768 window it
	subclasses, made transparent over its own backdrop windows. A host that
	replaces the procedure of every menu window itself paints that window
	instead, so Shell's items are never drawn and the user gets a blank
	rectangle. Delphi's VCL Styles does exactly that (TSysPopupStyleHook, on
	every #32768 in the process); WizTree is such a host. Main.cpp hands those
	hosts their own menus.

	Measured on Windows 11 26300 x64 (scratch probe, 2026-11-25). An untouched
	#32768 reports the same procedure as its class, inside ntdll.dll. One
	subclassed with SetWindowLongPtr at HCBT_CREATEWND - VCL's moment - already
	reports the subclasser's own code by EVENT_OBJECT_CREATE. One subclassed with
	SetWindowSubclass reports comctl32.dll, whose chain still reaches Shell's own
	subclass. So the test is: a procedure that is not the class's and lives in no
	system module. A Delphi object-instance thunk lives in no module at all,
	which counts.

	In WizTree the backdrop also outlived the menu: no #32768 was left, yet
	Shell's subclass never saw WM_NCDESTROY. An old-style replacement explains it
	- "It is difficult to remove a subclass after it is created", and restoring
	the saved procedure cuts comctl32's chain out - but that is inferred, not
	traced. ContextMenu::Uninitialize() destroys a leftover backdrop either way.
	https://learn.microsoft.com/en-us/windows/win32/controls/subclassing-overview
	https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandleexw
*/

#include <windows.h>

namespace Nilesoft
{
	namespace Shell
	{
		inline bool menu_window_replaced_by_host(HWND hWnd)
		{
			auto proc = ::GetWindowLongPtrW(hWnd, GWLP_WNDPROC);
			if(!proc || static_cast<ULONG_PTR>(proc) == ::GetClassLongPtrW(hWnd, GCLP_WNDPROC))
				return false;

			HMODULE owner{};
			if(!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
									 reinterpret_cast<LPCWSTR>(proc), &owner))
				return true;

			wchar_t path[MAX_PATH]{};
			auto length = ::GetModuleFileNameW(owner, path, MAX_PATH);
			if(length == 0 || length >= MAX_PATH)
				return true;

			const wchar_t *name = path;
			for(auto p = path; *p; p++)
				if(*p == L'\\' || *p == L'/')
					name = p + 1;

			for(auto system : { L"comctl32.dll", L"user32.dll", L"ntdll.dll" })
				if(::CompareStringOrdinal(name, -1, system, -1, TRUE) == CSTR_EQUAL)
					return false;

			return true;
		}
	}
}
