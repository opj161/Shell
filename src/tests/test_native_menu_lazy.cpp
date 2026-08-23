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

TEST(native_menu_lazy, only_active_parent_movement_selects_legacy_eager)
{
	CHECK(!native_moveto_requires_descendant_discovery(false, false));
	CHECK(!native_moveto_requires_descendant_discovery(true, false));
	CHECK(native_moveto_requires_descendant_discovery(true, true));

	CHECK(choose_native_tree_policy(true, true, false, false) == NativeTreePolicy::Lazy);
	CHECK(choose_native_tree_policy(true, false, true, false) == NativeTreePolicy::Lazy);
	CHECK(choose_native_tree_policy(false, true, true, false) == NativeTreePolicy::Lazy);
	CHECK(choose_native_tree_policy(true, true, true, false) == NativeTreePolicy::LegacyEager);

	// The hidden registry value is still a diagnostic override, including for a
	// configuration with no moveto rule.
	CHECK(choose_native_tree_policy(true, true, false, true) == NativeTreePolicy::LegacyEager);
}

TEST(native_menu_lazy, unmaterialized_empty_children_are_pending_not_empty)
{
	NativePopupState state;
	state.handle = fake(0x600);

	CHECK(!state.materialized);
	CHECK(!native_popup_contents_known_empty(state, true));

	// Notification without copy is still pending, not known empty.
	state.initialized = true;
	CHECK(!native_popup_contents_known_empty(state, true));
}

TEST(native_menu_lazy, materialized_empty_children_are_known_empty)
{
	NativePopupState state;
	state.handle = fake(0x601);
	state.materialized = true;

	CHECK(native_popup_contents_known_empty(state, true));
}

TEST(native_menu_lazy, materialized_nonempty_children_are_not_empty)
{
	NativePopupState state;
	state.handle = fake(0x602);
	state.materialized = true;

	CHECK(!native_popup_contents_known_empty(state, false));
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

	constexpr UINT REGRESSION_COMMAND = 0x4242;
	HMENU regression_root = nullptr;
	HMENU regression_child = nullptr;
	int regression_root_initialised = 0;
	int regression_child_initialised = 0;
	LPARAM regression_child_lparam = 0;

	LRESULT CALLBACK RegressionOwnerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if(msg == WM_INITMENUPOPUP)
		{
			auto menu = reinterpret_cast<HMENU>(wp);
			if(menu == regression_root)
				regression_root_initialised++;
			else if(menu == regression_child)
			{
				regression_child_initialised++;
				regression_child_lparam = lp;
				if(::GetMenuItemCount(menu) == 0)
					::AppendMenuW(menu, MF_STRING, REGRESSION_COMMAND,
						L"Regression Nested Item");
			}
			return 0;
		}
		return ::DefWindowProcW(hwnd, msg, wp, lp);
	}

	struct MoveScenarioResult
	{
		bool moved{};
		int root_notifications{};
		int child_notifications{};
		LPARAM child_lparam{};
	};

	MoveScenarioResult run_nested_move_scenario(HWND owner, NativeTreePolicy policy)
	{
		regression_root = ::CreatePopupMenu();
		regression_child = ::CreatePopupMenu();
		regression_root_initialised = 0;
		regression_child_initialised = 0;
		regression_child_lparam = 0;
		::AppendMenuW(regression_root, MF_POPUP,
			reinterpret_cast<UINT_PTR>(regression_child), L"Regression Parent");

		auto notify = [owner](HMENU menu, LPARAM lparam)
		{
			::SendMessageW(owner, WM_INITMENUPOPUP,
				reinterpret_cast<WPARAM>(menu), lparam);
		};

		NativePopupState root;
		root.handle = regression_root;
		initialize_native_popup(root, notify);

		NativePopupState child;
		child.handle = regression_child;
		child.parent_position = 0;
		if(policy == NativeTreePolicy::LegacyEager)
			initialize_native_popup(child, notify);

		bool moved = false;
		if(policy == NativeTreePolicy::LegacyEager)
		{
			for(int i = 0; i < ::GetMenuItemCount(regression_child); ++i)
			{
				if(::GetMenuItemID(regression_child, i) == REGRESSION_COMMAND)
				{
					::RemoveMenu(regression_child, static_cast<UINT>(i), MF_BYPOSITION);
					::AppendMenuW(regression_root, MF_STRING, REGRESSION_COMMAND,
						L"Regression Nested Item");
					moved = true;
					break;
				}
			}
		}

		MoveScenarioResult result{
			moved, regression_root_initialised, regression_child_initialised,
			regression_child_lparam
		};
		::DestroyMenu(regression_root);
		regression_root = nullptr;
		regression_child = nullptr;
		return result;
	}
}

TEST(native_menu_lazy, nested_moveto_requires_eager_discovery_and_then_moves)
{
	WNDCLASSW wc{};
	wc.lpfnWndProc = RegressionOwnerProc;
	wc.hInstance = ::GetModuleHandleW(nullptr);
	wc.lpszClassName = L"NssMovetoRegressionOwner";
	::RegisterClassW(&wc);

	auto owner = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
		0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
	CHECK(owner != nullptr);
	if(!owner) return;

	auto lazy = run_nested_move_scenario(owner, NativeTreePolicy::Lazy);
	CHECK(!lazy.moved);
	CHECK_EQ(lazy.root_notifications, 1);
	CHECK_EQ(lazy.child_notifications, 0);

	auto eager = run_nested_move_scenario(owner, NativeTreePolicy::LegacyEager);
	CHECK(eager.moved);
	CHECK_EQ(eager.root_notifications, 1);
	CHECK_EQ(eager.child_notifications, 1);
	CHECK_EQ(LOWORD(eager.child_lparam), 0);
	CHECK_EQ(HIWORD(eager.child_lparam), FALSE);

	::DestroyWindow(owner);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);
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

	// Copied children are still empty; that is pending, not known empty.
	for(int i = 0; i < 3; i++)
	{
		CHECK(!child_state[i].materialized);
		CHECK(!native_popup_contents_known_empty(child_state[i], true));
	}

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

	// Production marks the level materialized after enumerating it.
	child_state[1].materialized = true;
	CHECK(!native_popup_contents_known_empty(child_state[1], false));

	// A deliberately empty materialized popup is known empty; an unopened
	// sibling with the same empty child list is still pending.
	NativePopupState empty_materialized = child_state[2];
	empty_materialized.materialized = true;
	CHECK(native_popup_contents_known_empty(empty_materialized, true));
	CHECK(!native_popup_contents_known_empty(child_state[2], true));

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

// ---------------------------------------------------------------------------
// The other half of the notification: WM_UNINITMENUPOPUP.
//
// "If an application receives a WM_INITMENUPOPUP message, it will receive a
// WM_UNINITMENUPOPUP message."
// https://learn.microsoft.com/en-us/windows/win32/menurc/wm-uninitmenupopup
//
// Shell sent the first and never the second to any borrowed host popup: the
// close path destroys Shell's own synthetic menu and stops there. A host that
// allocated state when it was told to populate a popup was never told it could
// let go of it.
// ---------------------------------------------------------------------------

TEST(native_menu_uninit, a_popup_is_recorded_once_however_often_it_is_offered)
{
	NativePopupNotifications sent;

	CHECK(sent.record_init(fake(0x10)));
	CHECK_MSG(!sent.record_init(fake(0x10)), "one INIT earns one UNINIT, not two");
	CHECK_EQ(sent.pending(), size_t(1));
}

TEST(native_menu_uninit, a_popup_that_was_never_notified_is_owed_nothing)
{
	NativePopupNotifications sent;

	CHECK(!sent.record_init(nullptr));
	CHECK_EQ(sent.pending(), size_t(0));
	CHECK(!sent.init_sent(fake(0x10)));

	sent.record_init(fake(0x10));
	CHECK(sent.init_sent(fake(0x10)));
	CHECK(!sent.init_sent(fake(0x11)));
}

TEST(native_menu_uninit, popups_are_closed_deepest_first)
{
	// The reverse of the order they were opened in, which is the order Windows
	// closes them in: a parent outlives its child.
	NativePopupNotifications sent;
	sent.record_init(fake(0xA));	// root
	sent.record_init(fake(0xB));	// its submenu
	sent.record_init(fake(0xC));	// and that one's submenu

	auto owed = sent.take_for_uninit();
	CHECK_EQ(owed.size(), size_t(3));
	// CHECK does not abort, so the size has to gate the indexing below or a
	// regression here takes the whole suite down with an access violation
	// instead of reporting itself.
	if(owed.size() != 3)
		return;

	CHECK(owed[0] == fake(0xC));
	CHECK(owed[1] == fake(0xB));
	CHECK(owed[2] == fake(0xA));
}

TEST(native_menu_uninit, taking_the_list_empties_it)
{
	// Uninitialize() can be reached twice - InvokeCommand calls it and so does
	// the destructor - and the host must not be told twice.
	NativePopupNotifications sent;
	sent.record_init(fake(0xA));

	CHECK_EQ(sent.take_for_uninit().size(), size_t(1));
	CHECK_EQ(sent.pending(), size_t(0));
	CHECK_EQ(sent.take_for_uninit().size(), size_t(0));
}

// Real owner window, real message dispatch: every popup the owner was told to
// populate is told exactly once that it is finished with.

namespace
{
	struct PopupTraffic
	{
		HMENU menu{};
		int inits{};
		int uninits{};
	};

	std::vector<PopupTraffic> &traffic()
	{
		static std::vector<PopupTraffic> v;
		return v;
	}

	int bad_hiword = 0;

	PopupTraffic *find_traffic(HMENU h)
	{
		for(auto &t : traffic())
		{
			if(t.menu == h)
				return &t;
		}
		return nullptr;
	}

	LRESULT CALLBACK PairingOwnerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if(msg == WM_INITMENUPOPUP)
		{
			if(auto t = find_traffic(reinterpret_cast<HMENU>(wp)); t)
				t->inits++;
			return 0;
		}
		if(msg == WM_UNINITMENUPOPUP)
		{
			if(auto t = find_traffic(reinterpret_cast<HMENU>(wp)); t)
				t->uninits++;
			// The high word "can only be MF_SYSMENU (the window menu)"; none of
			// these is one, so it must arrive as zero. Counted rather than
			// pushed into traffic(), which the test holds pointers into.
			if(HIWORD(lp) != 0)
				bad_hiword++;
			return 0;
		}
		return ::DefWindowProcW(hwnd, msg, wp, lp);
	}
}

TEST(native_menu_uninit, every_notified_popup_is_uninitialised_exactly_once)
{
	WNDCLASSW wc{};
	wc.lpfnWndProc = PairingOwnerProc;
	wc.hInstance = ::GetModuleHandleW(nullptr);
	wc.lpszClassName = L"NssUninitPairingOwner";
	::RegisterClassW(&wc);

	HWND owner = ::CreateWindowExW(0, L"NssUninitPairingOwner", L"", WS_OVERLAPPED,
								   0, 0, 0, 0, HWND_MESSAGE, nullptr,
								   wc.hInstance, nullptr);
	CHECK(owner != nullptr);
	if(!owner)
		return;

	HMENU root = ::CreatePopupMenu();
	HMENU opened = ::CreatePopupMenu();
	HMENU never_opened = ::CreatePopupMenu();
	::AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(opened), L"opened");
	::AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(never_opened), L"not opened");

	traffic().clear();
	bad_hiword = 0;
	traffic().push_back({ root, 0, 0 });
	traffic().push_back({ opened, 0, 0 });
	traffic().push_back({ never_opened, 0, 0 });

	auto notify = [owner](HMENU h, LPARAM lp)
	{
		::SendMessageW(owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(h), lp);
	};

	NativePopupNotifications sent;

	// Shell builds the root, then the user opens one submenu.
	NativePopupState root_state; root_state.handle = root;
	if(initialize_native_popup(root_state, notify))
		sent.record_init(root_state.handle);

	NativePopupState opened_state; opened_state.handle = opened; opened_state.parent_position = 0;
	if(initialize_native_popup(opened_state, notify))
		sent.record_init(opened_state.handle);

	auto root_traffic = find_traffic(root);
	auto opened_traffic = find_traffic(opened);
	auto unopened_traffic = find_traffic(never_opened);
	CHECK(root_traffic && opened_traffic && unopened_traffic);
	if(!root_traffic || !opened_traffic || !unopened_traffic)
		return;

	CHECK_EQ(root_traffic->inits, 1);
	CHECK_EQ(opened_traffic->inits, 1);
	CHECK_EQ(unopened_traffic->inits, 0);

	// Menu closes. This is the loop ContextMenu::uninitialize_native_popups
	// runs, over the same list.
	for(auto hMenu : sent.take_for_uninit())
		::SendMessageW(owner, WM_UNINITMENUPOPUP, reinterpret_cast<WPARAM>(hMenu), 0);

	CHECK_MSG(root_traffic->uninits == 1, "the root was initialised, so it is owed one");
	CHECK_MSG(opened_traffic->uninits == 1, "and so is the submenu the user opened");
	CHECK_MSG(unopened_traffic->uninits == 0,
			  "a popup the host was never asked to populate must not be un-populated");
	CHECK_MSG(bad_hiword == 0, "lParam high word must be zero, not MF_SYSMENU");

	// Every notified popup still exists: they are the host's, and Shell only
	// ever drops the handles.
	CHECK(::GetMenuItemCount(root) == 2);
	CHECK(::IsMenu(opened));

	::DestroyMenu(root);
	::DestroyWindow(owner);
	::UnregisterClassW(L"NssUninitPairingOwner", wc.hInstance);
}
