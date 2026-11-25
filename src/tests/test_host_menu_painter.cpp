#include "test.h"

#include <windows.h>
#include <commctrl.h>
#include "Include/HostMenuPainter.h"

using Nilesoft::Shell::menu_window_replaced_by_host;

// Real windows, so the module lookup runs against real procedures: a system
// class untouched, chained by comctl32 the way Shell subclasses, and replaced
// outright the way Delphi VCL Styles does to every menu window.
namespace
{
	WNDPROC g_original;

	LRESULT CALLBACK replacement(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		return ::CallWindowProcW(g_original, h, m, w, l);
	}

	LRESULT CALLBACK chained(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
	{
		return ::DefSubclassProc(h, m, w, l);
	}

	HWND make_static()
	{
		return ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1,
								 nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	}
}

TEST(host_menu_painter, an_untouched_window_is_shells_to_paint)
{
	auto h = make_static();
	CHECK(h != nullptr);
	CHECK(!menu_window_replaced_by_host(h));
	::DestroyWindow(h);
}

TEST(host_menu_painter, a_comctl32_subclass_still_chains_to_shell)
{
	auto h = make_static();
	CHECK(::SetWindowSubclass(h, chained, 1, 0));
	CHECK(!menu_window_replaced_by_host(h));
	::RemoveWindowSubclass(h, chained, 1);
	::DestroyWindow(h);
}

TEST(host_menu_painter, a_replaced_procedure_means_the_host_paints)
{
	auto h = make_static();
	g_original = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(h, GWLP_WNDPROC,
																reinterpret_cast<LONG_PTR>(&replacement)));
	CHECK(g_original != nullptr);
	CHECK(menu_window_replaced_by_host(h));
	::SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original));
	CHECK(!menu_window_replaced_by_host(h));
	::DestroyWindow(h);
}
