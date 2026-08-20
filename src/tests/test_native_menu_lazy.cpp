// Lazy initialisation of host-owned menu popups.
//
// The invariant these protect: building the root Shell menu must not initialise
// a single host submenu the user has not opened. Before this, Shell walked the
// whole native tree up front, so every submenu of every installed shell
// extension was paid for on every right-click.
//
// NativeMenuLazy.h is deliberately free of Shell dependencies so the state
// machine can be driven directly, and the second half of this file drives it
// against a real owner window and real HMENUs.

#include "test.h"

#include <windows.h>
#include "..\dll\src\Include\NativeMenuLazy.h"

#include <chrono>
#include <vector>

using namespace Nilesoft::Shell;

namespace
{
	struct Notification
	{
		HMENU menu;
		LPARAM lparam;
	};

	std::vector<Notification> &log()
	{
		static std::vector<Notification> v;
		return v;
	}

	auto recorder()
	{
		return [](HMENU h, LPARAM lp) { log().push_back({ h, lp }); };
	}

	HMENU fake(uintptr_t n) { return reinterpret_cast<HMENU>(n); }
}

TEST(native_menu_lazy, a_popup_is_initialised_exactly_once)
{
	log().clear();

	NativePopupState state;
	state.handle = fake(0x100);
	state.parent_position = 3;

	CHECK(initialize_native_popup(state, recorder()));
	CHECK(state.initialized);
	CHECK(!state.initializing);
	CHECK_EQ(log().size(), size_t(1));

	// Opening the same submenu again must not re-run the host's handler.
	CHECK(!initialize_native_popup(state, recorder()));
	CHECK_EQ(log().size(), size_t(1));
}

TEST(native_menu_lazy, lparam_carries_the_position_and_is_not_a_window_menu)
{
	log().clear();

	NativePopupState state;
	state.handle = fake(0x200);
	state.parent_position = 7;

	initialize_native_popup(state, recorder());

	CHECK_EQ(log().size(), size_t(1));
	CHECK_EQ(LOWORD(log()[0].lparam), 7);
	CHECK_EQ(HIWORD(log()[0].lparam), FALSE);

	// The value the old code sent, which claimed position 65535 in a window menu.
	CHECK(log()[0].lparam != static_cast<LPARAM>(0xFFFFFFFF));
}

TEST(native_menu_lazy, the_root_notification_matches_what_windows_sends)
{
	log().clear();

	// Measured against a real TrackPopupMenu: Windows sends lParam 0 for the
	// tracked root popup, and LOWORD = the opening item's position with
	// HIWORD = FALSE for a submenu. Shell's synthesised notification for the
	// host's root menu has to be indistinguishable from that.
	NativePopupState root;
	root.handle = fake(0x500);
	root.parent_position = 0;

	initialize_native_popup(root, recorder());

	CHECK_EQ(log().size(), size_t(1));
	CHECK_EQ(log()[0].lparam, LPARAM(0));
}

TEST(native_menu_lazy, a_popup_without_a_handle_is_never_notified)
{
	log().clear();

	NativePopupState state;
	CHECK(!initialize_native_popup(state, recorder()));
	CHECK_EQ(log().size(), size_t(0));
	CHECK(!state.initialized);
}

TEST(native_menu_lazy, reentrant_initialisation_is_refused)
{
	log().clear();

	NativePopupState state;
	state.handle = fake(0x300);

	// The notification runs host code synchronously, and that code can come
	// straight back through the same popup.
	int depth = 0;
	initialize_native_popup(state, [&](HMENU h, LPARAM lp)
	{
		log().push_back({ h, lp });
		depth++;
		initialize_native_popup(state, recorder());
	});

	CHECK_EQ(depth, 1);
	CHECK_EQ(log().size(), size_t(1));
	CHECK(state.initialized);
	CHECK(!state.initializing);
}

TEST(native_menu_lazy, a_throwing_host_does_not_wedge_the_guard)
{
	log().clear();

	NativePopupState state;
	state.handle = fake(0x400);

	bool threw = false;
	// Volatile so the compiler cannot prove the notification always throws and
	// fold away the rest of initialize_native_popup.
	volatile bool always = true;
	try
	{
		initialize_native_popup(state, [&](HMENU h, LPARAM lp)
		{
			if(always) throw 1;
			log().push_back({ h, lp });
		});
	}
	catch(int)
	{
		threw = true;
	}

	CHECK(threw);
	// Not marked initialised - the host never populated it - but the popup must
	// still be retryable rather than stuck half-initialised.
	CHECK(!state.initializing);
	CHECK(!state.initialized);
	CHECK(initialize_native_popup(state, recorder()));
	CHECK_EQ(log().size(), size_t(1));
}

// ---------------------------------------------------------------------------
// Real menus, real owner window, real message dispatch.
// ---------------------------------------------------------------------------

namespace
{
	// Each entry is a submenu whose owner deliberately takes time to populate,
	// standing in for a slow third-party shell extension.
	struct SlowChild
	{
		HMENU menu{};
		int initialised{};
	};

	std::vector<SlowChild> &children()
	{
		static std::vector<SlowChild> v;
		return v;
	}

	constexpr DWORD CHILD_DELAY_MS = 60;
	int root_initialised = 0;
	HMENU root_menu = nullptr;

	LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if(msg == WM_INITMENUPOPUP)
		{
			auto h = reinterpret_cast<HMENU>(wp);
			if(h == root_menu)
			{
				root_initialised++;
			}
			else
			{
				for(auto &c : children())
				{
					if(c.menu == h)
					{
						c.initialised++;
						::Sleep(CHILD_DELAY_MS);
						::AppendMenuW(h, MF_STRING, 1000, L"populated");
						break;
					}
				}
			}
			return 0;
		}
		return ::DefWindowProcW(hwnd, msg, wp, lp);
	}

	double ms_since(std::chrono::steady_clock::time_point t0)
	{
		return std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
	}
}

TEST(native_menu_lazy, opening_the_root_does_not_pay_for_unopened_submenus)
{
	WNDCLASSW wc{};
	wc.lpfnWndProc = OwnerProc;
	wc.hInstance = ::GetModuleHandleW(nullptr);
	wc.lpszClassName = L"NssLazyMenuOwner";
	::RegisterClassW(&wc);

	HWND owner = ::CreateWindowExW(0, L"NssLazyMenuOwner", L"", WS_OVERLAPPED,
								   0, 0, 0, 0, HWND_MESSAGE, nullptr,
								   wc.hInstance, nullptr);
	CHECK(owner != nullptr);
	if(!owner)
		return;

	root_menu = ::CreatePopupMenu();
	root_initialised = 0;
	children().clear();
	children().resize(3);

	for(int i = 0; i < 3; i++)
	{
		children()[i].menu = ::CreatePopupMenu();
		::AppendMenuW(root_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(children()[i].menu),
					  L"child");
	}

	NativePopupState root;
	root.handle = root_menu;

	std::vector<NativePopupState> child_state(3);
	for(int i = 0; i < 3; i++)
	{
		child_state[i].handle = children()[i].menu;
		child_state[i].parent_position = static_cast<UINT>(i);
	}

	auto notify = [owner](HMENU h, LPARAM lp)
	{
		::SendMessageW(owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(h), lp);
	};

	// Building the root menu: root only.
	auto t0 = std::chrono::steady_clock::now();
	initialize_native_popup(root, notify);
	auto root_ms = ms_since(t0);

	CHECK_EQ(root_initialised, 1);
	CHECK_EQ(children()[0].initialised, 0);
	CHECK_EQ(children()[1].initialised, 0);
	CHECK_EQ(children()[2].initialised, 0);

	// The old whole-tree walk paid 3 x CHILD_DELAY_MS here.
	CHECK(root_ms < CHILD_DELAY_MS);

	// The user opens the second submenu.
	t0 = std::chrono::steady_clock::now();
	initialize_native_popup(child_state[1], notify);
	auto child_ms = ms_since(t0);

	CHECK_EQ(children()[1].initialised, 1);
	CHECK_EQ(children()[0].initialised, 0);
	CHECK_EQ(children()[2].initialised, 0);
	CHECK(child_ms >= CHILD_DELAY_MS);
	CHECK_EQ(::GetMenuItemCount(children()[1].menu), 1);

	// Re-opening it costs nothing.
	t0 = std::chrono::steady_clock::now();
	initialize_native_popup(child_state[1], notify);
	CHECK_EQ(children()[1].initialised, 1);
	CHECK(ms_since(t0) < CHILD_DELAY_MS);

	::DestroyMenu(root_menu);
	root_menu = nullptr;
	::DestroyWindow(owner);
	::UnregisterClassW(L"NssLazyMenuOwner", wc.hInstance);
}
