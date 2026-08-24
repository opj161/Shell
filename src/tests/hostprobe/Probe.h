#pragma once

/*
	A real owner window, a real HMENU, and a scripted user.

	TrackPopupMenu is modal, so a scenario needs something to act while the
	tracking call is blocked. Three mechanics make that reliable, and each was
	established by probing rather than assumed - the first attempt at this got
	all three wrong and hung:

	1. Delivering keys. The obvious route, SendInput, injects real keystrokes
	   into whatever holds the foreground; on a developer's own desktop that is
	   not acceptable, and it makes the harness a hazard rather than a tool.
	   Measured instead (Windows 11 26200.8875 x64, 2026-08-24): the menu's
	   modal loop takes keyboard messages off the *thread* queue, so all three
	   of PostMessage-to-the-#32768-window, PostMessage-to-the-owner and
	   PostThreadMessage-to-the-owner-thread drive it identically. This uses
	   PostThreadMessage: it needs no window handle at all, and so depends on
	   nothing undocumented - not even the menu window class.

	2. Ending a run that the script failed to end. A menu left standing blocks
	   the tracking call forever. EndMenu ends "the calling thread's active
	   menu" and the driver is not that thread, so the watchdog uses the
	   documented alternative: "If a platform does not support EndMenu, send the
	   owner of the active menu a WM_CANCELMODE message."
	   https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-endmenu

	3. Keeping the mouse out of it. The first run of the probe drew the popup
	   under the live cursor and the trace filled with MF_MOUSESELECT hover
	   notifications nothing had asked for. The popup is placed in the screen
	   quadrant furthest from the cursor.

	The owner window is real and visible because a menu needs one, and because a
	hidden owner is a different configuration from the one hosts actually use.
*/

#include <windows.h>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "Trace.h"

namespace hostprobe
{
	// One scripted keystroke. Character keys arrive as WM_CHAR and virtual keys
	// as WM_KEYDOWN/WM_KEYUP; the menu loop reads both.
	struct Key
	{
		UINT code{};
		bool character{};

		static Key vk(UINT v) { return { v, false }; }
		static Key ch(wchar_t c) { return { static_cast<UINT>(c), true }; }
	};

	// Where a script wants the highlight before it commits.
	//
	// Expressed as a destination rather than as a number of key presses, because
	// counting presses is wrong in three separate ways: a separator is skipped
	// silently, the first press may land before the menu loop is reading (which
	// is how the first version of this harness produced a scenario with no
	// selection at all), and TPM_NONOTIFY menus give no sign of life until
	// something is selected. The driver instead steps and watches, so it knows
	// where it is rather than assuming.
	struct Target
	{
		// WM_MENUSELECT reports an identifier for a command item and a position
		// for one that opens a submenu, distinguished by MF_POPUP. Both are
		// needed to name a destination unambiguously.
		UINT item{};
		bool is_popup{};
		bool valid{};

		static Target command(UINT id) { return { id, false, true }; }
		static Target popup(UINT position) { return { position, true, true }; }
		static Target none() { return {}; }
	};

	// What the probe answers when Windows asks it something. A scenario sets
	// this before tracking; the default is the behaviour of a host that does not
	// handle the message.
	struct ReplyPolicy
	{
		// MNC_IGNORE / MNC_CLOSE / MNC_EXECUTE / MNC_SELECT in the high word.
		UINT menuchar_action{ MNC_IGNORE };
		// The low word. Documented as "the zero-based index of the menu item"
		// for MNC_EXECUTE and MNC_SELECT; whether Windows really treats it as an
		// index rather than an identifier is what scenario menuchar_* asks.
		// https://learn.microsoft.com/en-us/windows/win32/menurc/using-menus
		UINT menuchar_operand{ 0 };
		bool handle_menuchar{ false };
	};

	class Probe
	{
	public:
		static Probe &instance()
		{
			static Probe *p = new Probe();
			return *p;
		}

		Trace &trace() { return _trace; }
		ReplyPolicy &policy() { return _policy; }
		HWND owner() const { return _owner; }

		bool create()
		{
			WNDCLASSEXW wc{};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = &Probe::window_proc;
			wc.hInstance = ::GetModuleHandleW(nullptr);
			wc.lpszClassName = L"NilesoftShellHostProbe";
			wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
			wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
			if(!::RegisterClassExW(&wc))
				return false;

			_owner = ::CreateWindowExW(0, wc.lpszClassName, L"Nilesoft Shell host probe",
									   WS_OVERLAPPEDWINDOW, 60, 60, 420, 220,
									   nullptr, nullptr, wc.hInstance, nullptr);
			if(!_owner)
				return false;

			_owner_thread = ::GetCurrentThreadId();
			::ShowWindow(_owner, SW_SHOWNORMAL);
			::UpdateWindow(_owner);
			::SetForegroundWindow(_owner);

			_menu_up = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
			return _menu_up != nullptr;
		}

		void destroy()
		{
			if(_owner)
			{
				::DestroyWindow(_owner);
				_owner = nullptr;
			}
			if(_menu_up)
			{
				::CloseHandle(_menu_up);
				_menu_up = nullptr;
			}
		}

		// What the driver does while the tracking call is blocked. Navigation
		// steps are closed-loop; the trailing keys are posted as written.
		struct Script
		{
			Target root;			// highlight this in the root menu
			bool open_submenu{};	// then press Right to open it
			Target child;			// then highlight this in the submenu
			std::vector<Key> keys;	// then post these
		};

		// Runs one tracking call with the given script, and returns whatever the
		// tracking function returned.
		int track(HMENU menu, UINT flags, bool use_ex, const Script &script)
		{
			_trace.clear();
			_last_error = 0;
			_select_count.store(0);
			_popup_count.store(0);
			_draw_items.store(0);
			_selected_item.store(0);
			_selected_is_popup.store(false);
			::ResetEvent(_menu_up);

			// The alias table assigns numbers in first-seen order, so claiming
			// the owner and the root menu up front keeps them stable across
			// scenarios that differ in which message arrives first.
			_trace.window_alias(_owner);
			_trace.menu_alias(menu);

			_script = script;
			auto thread = ::CreateThread(nullptr, 0, &Probe::driver_main, this, 0, nullptr);

			auto pt = away_from_cursor();

			::SetLastError(ERROR_SUCCESS);
			int result = 0;
			if(use_ex)
				result = ::TrackPopupMenuEx(menu, flags, pt.x, pt.y, _owner, nullptr);
			else
				result = ::TrackPopupMenu(menu, flags, pt.x, pt.y, 0, _owner, nullptr);
			_last_error = ::GetLastError();

			if(thread)
			{
				::WaitForSingleObject(thread, 10000);
				::CloseHandle(thread);
			}

			// "The window does not receive a WM_COMMAND message from the menu
			// until the function returns" - so anything the menu notified with
			// is still queued here, and draining it is part of observing the
			// contract rather than cleanup.
			// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenuex
			MSG msg;
			for(int i = 0; i < 200 && ::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); i++)
			{
				::TranslateMessage(&msg);
				::DispatchMessageW(&msg);
			}

			return result;
		}

		DWORD last_error() const { return _last_error; }

	private:
		Probe() = default;

		// The furthest corner from the live cursor, so a stationary mouse never
		// lands on the popup and generates MF_MOUSESELECT notifications.
		static POINT away_from_cursor()
		{
			POINT cursor{};
			::GetCursorPos(&cursor);
			auto cx = ::GetSystemMetrics(SM_CXSCREEN);
			auto cy = ::GetSystemMetrics(SM_CYSCREEN);
			return { cursor.x > cx / 2 ? 40 : cx - 260,
					 cursor.y > cy / 2 ? 40 : cy - 260 };
		}

		static DWORD WINAPI driver_main(void *param)
		{
			static_cast<Probe *>(param)->drive();
			return 0;
		}

		void post(const Key &key)
		{
			if(key.character)
				::PostThreadMessageW(_owner_thread, WM_CHAR, key.code, 0);
			else
			{
				::PostThreadMessageW(_owner_thread, WM_KEYDOWN, key.code, 0);
				::PostThreadMessageW(_owner_thread, WM_KEYUP, key.code, 0);
			}
		}

		// Presses a key and waits for the selection to move. Returns false if it
		// did not, which is the signal to press again rather than to carry on
		// counting presses that may never have been read.
		bool step(UINT vk, DWORD budget_ms = 400)
		{
			auto before = _select_count.load();
			post(Key::vk(vk));

			for(DWORD waited = 0; waited < budget_ms; waited += 20)
			{
				if(_select_count.load() != before)
					return true;
				::Sleep(20);
			}
			return false;
		}

		// Steps with VK_DOWN until the highlight is where the script asked.
		//
		// The cap used to be forty presses, on the reasoning that it was twice
		// round a six-item menu. That held only while every menu in the harness
		// was one this file built. A menu Shell composed from the shell
		// namespace has thirty-odd items at its root, and forty presses is not
		// reliably one lap of it - so a destination that was there all along
		// looked like a destination that did not exist, and only when the run
		// happened to be slow enough for the opening highlight to arrive before
		// this thread first looked.
		//
		// Stopping is now decided by the menu rather than by a number: once the
		// highlight returns to somewhere it has already been, the whole list has
		// been walked. The check runs *before* the first step for the same
		// reason - the menu may already be sitting on the destination, and
		// whether it got there before or after this thread started watching is a
		// race no test should depend on.
		bool navigate_to(const Target &target)
		{
			if(!target.valid)
				return true;

			std::vector<std::pair<UINT, bool>> visited;
			auto matches = [&target](UINT item, bool popup)
			{
				return item == target.item && popup == target.is_popup;
			};

			// Two laps' worth of presses is the backstop for a menu whose
			// highlight never settles; the visited set is what normally ends it.
			for(int attempt = 0; attempt < 400; attempt++)
			{
				if(_select_count.load() != 0)
				{
					auto item = _selected_item.load();
					auto popup = _selected_is_popup.load();
					if(matches(item, popup))
						return true;

					std::pair<UINT, bool> here{ item, popup };
					if(attempt > 0)
					{
						bool seen = false;
						for(auto &v : visited)
						{
							if(v == here) { seen = true; break; }
						}
						if(seen)
							return false;
					}
					visited.push_back(here);
				}

				// A step that produced no notification was not read by the menu
				// loop, so it costs an attempt and not a position.
				step(VK_DOWN);
			}
			return false;
		}

		void drive()
		{
			// A first sign of life, if there is one to wait for. Under
			// TPM_NONOTIFY there is not: measured on this machine, that flag
			// suppresses WM_ENTERMENULOOP, WM_INITMENU, WM_INITMENUPOPUP,
			// WM_UNINITMENUPOPUP and WM_EXITMENULOOP, and the first thing the
			// owner hears is a WM_MENUSELECT caused by a key this driver has not
			// pressed yet. So the wait is short and the navigation below is what
			// actually establishes that the menu is up.
			::WaitForSingleObject(_menu_up, 800);

			if(!navigate_to(_script.root))
				_navigation_failed = true;

			if(_script.open_submenu)
			{
				auto before = _popup_count.load();
				for(int attempt = 0; attempt < 10; attempt++)
				{
					post(Key::vk(VK_RIGHT));
					::Sleep(80);
					if(_popup_count.load() != before || _selected_is_popup.load() == false)
						break;
				}
				if(!navigate_to(_script.child))
					_navigation_failed = true;
			}

			for(auto &key : _script.keys)
			{
				post(key);
				::Sleep(140);
			}

			// Watchdog, always armed. A scenario that deliberately leaves the
			// menu open - a cancel case, or navigation that failed - relies on
			// it, and without it a failed run hangs the whole harness.
			::Sleep(300);
			::PostMessageW(_owner, WM_CANCELMODE, 0, 0);
		}

		static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
		{
			return instance().on_message(hwnd, msg, wp, lp);
		}

		LRESULT on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
		{
			auto name = message_name(msg);

			// WM_DRAWITEM is counted but kept out of the trace text. It is a
			// paint: how many arrive and where they interleave with selection is
			// a function of repaints, not of the menu contract, and recording it
			// inline made two scenarios fail their own baseline on a re-run. What
			// is worth pinning - that it survives TPM_NONOTIFY at all - is in the
			// summary line every trace ends with.
			if(msg == WM_DRAWITEM)
			{
				_draw_items.fetch_add(1);
				name = nullptr;
			}

			if(name)
				record(name, msg, wp, lp);

			// Any of these means the menu is up. WM_ENTERMENULOOP alone is not
			// enough: TPM_NONOTIFY suppresses it, and the first version of this
			// harness then waited out its whole budget on every NONOTIFY run.
			if(msg == WM_ENTERMENULOOP || msg == WM_INITMENUPOPUP ||
			   msg == WM_MEASUREITEM || msg == WM_MENUSELECT)
				::SetEvent(_menu_up);

			if(msg == WM_INITMENUPOPUP)
				_popup_count.fetch_add(1);

			if(msg == WM_MENUSELECT && HIWORD(wp) != 0xFFFF)
			{
				// "If the selected item is a command item, this parameter
				// contains the identifier of the menu item. If the selected item
				// opens a drop-down menu or submenu, this parameter contains the
				// index of the drop-down menu or submenu in the main menu."
				// https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menuselect
				_selected_item.store(LOWORD(wp));
				_selected_is_popup.store((HIWORD(wp) & MF_POPUP) != 0);
				_select_count.fetch_add(1);
			}

			if(msg == WM_MENUCHAR && _policy.handle_menuchar)
			{
				auto reply = MAKELRESULT(_policy.menuchar_operand, _policy.menuchar_action);
				_trace.set_reply(reply);
				return reply;
			}

			// An owner-drawn item must be measured or it has no height, and the
			// draw is answered so Windows does not fall back.
			if(msg == WM_MEASUREITEM)
			{
				auto mis = reinterpret_cast<MEASUREITEMSTRUCT *>(lp);
				if(mis && mis->CtlType == ODT_MENU)
				{
					mis->itemWidth = 120;
					mis->itemHeight = 20;
					return TRUE;
				}
			}
			if(msg == WM_DRAWITEM)
			{
				auto dis = reinterpret_cast<DRAWITEMSTRUCT *>(lp);
				if(dis && dis->CtlType == ODT_MENU)
					return TRUE;
			}

			return ::DefWindowProcW(hwnd, msg, wp, lp);
		}

		// Which item the menu says is highlighted, or -1. MFS_HILITE is where
		// that fact lives - there is nowhere else to ask.
		// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmenuiteminfow
		static int highlighted_position(HMENU menu)
		{
			if(!menu)
				return -1;
			auto count = ::GetMenuItemCount(menu);
			for(int i = 0; i < count; i++)
			{
				MENUITEMINFOW mii{};
				mii.cbSize = sizeof(mii);
				mii.fMask = MIIM_STATE;
				if(::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii) &&
				   (mii.fState & MFS_HILITE))
					return i;
			}
			return -1;
		}

		void record(const wchar_t *name, UINT msg, WPARAM wp, LPARAM lp)
		{
			wchar_t buf[320];

			switch(msg)
			{
			case WM_INITMENU:
				::swprintf_s(buf, L"%-20s menu=%s", name,
							 _trace.menu_alias(reinterpret_cast<HMENU>(wp)).c_str());
				break;

			case WM_INITMENUPOPUP:
				// "The low-order word specifies the zero-based relative position
				// of the menu item that opens the drop-down menu or submenu. The
				// high-order word indicates whether the drop-down menu is the
				// window menu."
				// https://learn.microsoft.com/en-us/windows/win32/menurc/wm-initmenupopup
				::swprintf_s(buf, L"%-20s menu=%s position=%u windowmenu=%u", name,
							 _trace.menu_alias(reinterpret_cast<HMENU>(wp)).c_str(),
							 static_cast<unsigned>(LOWORD(lp)),
							 static_cast<unsigned>(HIWORD(lp)));
				break;

			case WM_UNINITMENUPOPUP:
				// lParam's high word "can only be MF_SYSMENU".
				// https://learn.microsoft.com/en-us/windows/win32/menurc/wm-uninitmenupopup
				::swprintf_s(buf, L"%-20s menu=%s sysmenu=%u", name,
							 _trace.menu_alias(reinterpret_cast<HMENU>(wp)).c_str(),
							 static_cast<unsigned>(HIWORD(lp) == MF_SYSMENU ? 1 : 0));
				break;

			case WM_MENUSELECT:
				::swprintf_s(buf, L"%-20s item=%u flags=%s menu=%s", name,
							 static_cast<unsigned>(LOWORD(wp)),
							 menu_flags(static_cast<UINT>(HIWORD(wp))).c_str(),
							 _trace.menu_alias(reinterpret_cast<HMENU>(lp)).c_str());
				break;

			case WM_MENUCHAR:
			{
				// The highlighted position, read back from the menu the way
				// Shell's own handler has to read it. Two things depend on that
				// working: knowing where the highlight is at all, and whether it
				// is legible for an *owner-drawn* item, which is the only kind
				// Shell ever renders.
				::swprintf_s(buf, L"%-20s char=%u type=%s menu=%s hilite=%d", name,
							 static_cast<unsigned>(LOWORD(wp)),
							 HIWORD(wp) == MF_SYSMENU ? L"SYSMENU" : L"POPUP",
							 _trace.menu_alias(reinterpret_cast<HMENU>(lp)).c_str(),
							 highlighted_position(reinterpret_cast<HMENU>(lp)));
				break;
			}

			case WM_COMMAND:
				::swprintf_s(buf, L"%-20s id=%u notify=%u from=%s", name,
							 static_cast<unsigned>(LOWORD(wp)),
							 static_cast<unsigned>(HIWORD(wp)),
							 lp ? L"control" : L"menu");
				break;

			case WM_MENUCOMMAND:
				// "wParam: the zero-based index of the item selected."
				// https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menucommand
				::swprintf_s(buf, L"%-20s position=%u menu=%s", name,
							 static_cast<unsigned>(wp),
							 _trace.menu_alias(reinterpret_cast<HMENU>(lp)).c_str());
				break;

			case WM_MEASUREITEM:
			{
				auto mis = reinterpret_cast<MEASUREITEMSTRUCT *>(lp);
				::swprintf_s(buf, L"%-20s ctl=%u id=%u data=%llu", name,
							 mis ? static_cast<unsigned>(mis->CtlType) : 0u,
							 mis ? static_cast<unsigned>(mis->itemID) : 0u,
							 mis ? static_cast<unsigned long long>(mis->itemData) : 0ull);
				break;
			}

			case WM_DRAWITEM:
			{
				auto dis = reinterpret_cast<DRAWITEMSTRUCT *>(lp);
				::swprintf_s(buf, L"%-20s ctl=%u id=%u", name,
							 dis ? static_cast<unsigned>(dis->CtlType) : 0u,
							 dis ? static_cast<unsigned>(dis->itemID) : 0u);
				break;
			}

			default:
				::swprintf_s(buf, L"%s", name);
				break;
			}

			_trace.add(msg, wp, lp, buf);
		}

		Trace _trace;
		ReplyPolicy _policy;
		HWND _owner{};
		DWORD _owner_thread{};
		HANDLE _menu_up{};
		DWORD _last_error{};
		Script _script;

		// Written on the owner's thread inside the tracking call, read on the
		// driver thread. Atomics rather than a lock: the driver only ever needs
		// the latest value, and taking a lock inside a window procedure that
		// runs during a menu modal loop is the kind of thing this project's own
		// re-entrancy rules exist to forbid.
		std::atomic<size_t> _select_count{ 0 };
		std::atomic<size_t> _popup_count{ 0 };
		std::atomic<size_t> _draw_items{ 0 };
		std::atomic<UINT> _selected_item{ 0 };
		std::atomic<bool> _selected_is_popup{ false };
		bool _navigation_failed{};

	public:
		bool navigation_failed() const { return _navigation_failed; }
		void clear_navigation_failure() { _navigation_failed = false; }
		size_t draw_items() const { return _draw_items.load(); }
	};
}
