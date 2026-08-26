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
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "MenuReader.h"
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

		/*
			Or name it by what it says.

			An identifier is the natural handle for everything else here, but it
			stops working the moment Shell composes a menu whose identifiers are
			its own. That is not hypothetical: a borrowed root carrying
			MNS_NOTIFYBYPOS makes Shell track every mirrored native item under a
			tracking identifier of its own, precisely because the host's wID is
			not an identity in such a menu - see Include/HostContract.h. The
			driver then steers by an identifier that exists nowhere in the menu
			it is looking at, and reports that the item does not exist.

			Titles survive mirroring. Matched as a substring, case-sensitively,
			after mnemonic markers and any accelerator are removed, so
			"&Probe Zero\tCtrl+P" is found by "Probe Zero".
		*/
		std::wstring title;

		static Target command(UINT id) { return { id, false, true, {} }; }
		static Target popup(UINT position) { return { position, true, true, {} }; }
		static Target titled(std::wstring text) { return { 0, false, true, std::move(text) }; }

		// A submenu named by what it says, for the same reason as titled(): in
		// a by-position menu Shell tracks mirrored native items under
		// identifiers of its own, and the *position* a popup is reported under
		// is a position in Shell's composed menu rather than in the host's - so
		// neither handle survives mirroring, and only the title does.
		static Target titled_popup(std::wstring text) { return { 0, true, true, std::move(text) }; }

		static Target none() { return {}; }

		bool by_title() const { return !title.empty(); }
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

	// What the driver read off the live menu, for the scenarios that assert
	// what was *rendered* rather than what was sent. Filled on the driver
	// thread while the owner is blocked inside its tracking call, and read
	// after that call returns - by which time the menu is destroyed, which is
	// why every field here is plain data rather than a handle to ask later.
	//
	// docs/refactor/08-handoff.md section 3.8: this is the coverage seam steps
	// 6 and 7 are gated on, because their regressions look like a menu that
	// draws slightly wrong rather than like a test that fails.
	struct MenuSnapshot
	{
		bool attempted{};

		// How many popup windows were visible when the read began. Anything
		// other than one means another menu was open on the desktop, and no
		// rendering assertion can be trusted - AGENTS.md, "Run the harness on a
		// quiet desktop". Recorded rather than worked around, so a run that
		// could not assert says so instead of passing.
		size_t popups_before{};

		ReadPopup root;
		std::vector<MenuRow> rows;		// the composed HMENU, same positions

		bool submenu_attempted{};
		bool submenu_opened{};
		size_t parent_item_index{};		// 1-based, into parent.items
		ReadPopup parent;				// re-read once the child was up
		ReadPopup child;
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

			// Read the live menu back through MSAA instead of navigating. The
			// two are alternatives rather than additions: the reader does its
			// own navigation, to whichever item turns out to open a submenu.
			bool read_menu{};

			// Also steer into a submenu and read that. Only the placement
			// scenario asserts on it, and opening one costs a navigation across
			// a thirty-item menu plus the wait for a second popup - so the
			// three scenarios that only read the root do not pay for it. Work a
			// test does not assert on is not free: it is time, and it is one
			// more thing that can perturb the reading.
			bool read_submenu{};
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
			_target_armed.store(false);
			_target_reached.store(false);
			_tracked_menu.store(menu);
			_composed_menu.store(nullptr);
			_snapshot = MenuSnapshot{};
			::ResetEvent(_menu_up);

			// The alias table assigns numbers in first-seen order, so claiming
			// the owner and the root menu up front keeps them stable across
			// scenarios that differ in which message arrives first.
			_trace.window_alias(_owner);
			_trace.menu_alias(menu);

			_script = script;
			auto thread = ::CreateThread(nullptr, 0, &Probe::driver_main, this, 0, nullptr);

			auto pt = away_from_cursor();

			// The documented preamble, and the reason a run used to fail about
			// once in four. TrackPopupMenu's remarks:
			//
			//   "when the current window is the foreground window, the second
			//    time this menu is displayed, it appears and then immediately
			//    disappears. To correct this, you must force a task switch to
			//    the application that called TrackPopupMenu. This is done by
			//    posting a benign message to the window or thread"
			//   https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenu
			//
			// This harness shows twenty-three to thirty-one menus in a row from
			// one window, so every one of them after the first is "the second
			// time". When it bit, the menu vanished before the driver pressed
			// anything: no WM_MENUSELECT at all, a return of 0, and a recorded
			// trace that no longer matched - which read as a finding about
			// Windows rather than as the harness failing to meet a contract.
			// docs/refactor/08-handoff.md recorded that as an unexplained
			// transient for three sessions.
			::SetForegroundWindow(_owner);

			::SetLastError(ERROR_SUCCESS);
			int result = 0;
			if(use_ex)
				result = ::TrackPopupMenuEx(menu, flags, pt.x, pt.y, _owner, nullptr);
			else
				result = ::TrackPopupMenu(menu, flags, pt.x, pt.y, 0, _owner, nullptr);
			_last_error = ::GetLastError();

			// The other half of the same remark, and it has to be after the
			// call: the task switch it forces is what lets the *next* menu
			// appear and stay. Posted, not sent, exactly as the sample shows.
			::PostMessageW(_owner, WM_NULL, 0, 0);

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

		// Lets the previous menu finish going away before the next one opens.
		//
		// Measured, because the alternative was guessing: the recorded traces
		// flake as a function of *how many menus this process has already
		// shown*, not of which scenario. Twenty runs of four scenarios and
		// eight runs of eight produced no mismatch at all; fourteen runs of
		// twenty-three produced two. Nothing about the failing scenarios is
		// special - the ones seen were select.plain.ex, select.nonotify.classic,
		// select.returncmd_nonotify.classic and submenu.returncmd - and each of
		// them passes indefinitely on its own.
		//
		// The harness opened the next menu the instant the previous tracking
		// call returned, which no host does and which leaves teardown racing
		// the next TrackPopupMenu. This pumps rather than sleeps, because the
		// teardown that has to finish is message-driven: the menu window is
		// destroyed on this thread, and a thread that is not pumping is a
		// thread where that has not happened yet.
		void settle(DWORD ms)
		{
			auto until = ::GetTickCount64() + ms;
			for(;;)
			{
				MSG msg;
				while(::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
				{
					::TranslateMessage(&msg);
					::DispatchMessageW(&msg);
				}
				if(::GetTickCount64() >= until)
					return;
				::Sleep(10);
			}
		}

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
		// Arrival is noticed by the *message handler*, not by reading the
		// selection back after each keypress. One press does not always produce
		// one notification - a separator is skipped silently, and a menu Shell
		// composed can move the highlight twice - so a driver that reads once
		// per press steps straight over its destination and keeps walking. That
		// is what made this intermittently claim the item did not exist.
		//
		// Two further rules, both learned the same way:
		//
		//  - The target is armed *before* anything is pressed, because the menu
		//    may already be sitting on it. Whether the opening highlight arrives
		//    before or after this thread starts watching is a race no test
		//    should depend on.
		//  - Walking stops when the highlight has gone a long stretch without
		//    reaching anywhere new - not at the first repeat. WM_MENUSELECT
		//    reports an *identifier*, and a composed menu repeats them: every
		//    item without one reports zero, and mirrored natives from different
		//    subtrees collide. A repeat is not proof of a full lap.
		bool navigate_to(const Target &target)
		{
			if(!target.valid)
				return true;

			arm_target(target);

			std::vector<std::pair<UINT, bool>> visited;
			int stale = 0;
			constexpr int STALE_LIMIT = 60;
			constexpr int STEP_LIMIT = 300;

			for(int attempt = 0; attempt < STEP_LIMIT && stale < STALE_LIMIT; attempt++)
			{
				if(_target_reached.load())
					return true;

				if(_select_count.load() != 0)
				{
					std::pair<UINT, bool> here{ _selected_item.load(),
												_selected_is_popup.load() };
					bool seen = false;
					for(auto &v : visited)
					{
						if(v == here) { seen = true; break; }
					}
					if(seen)
						stale++;
					else
					{
						visited.push_back(here);
						stale = 0;
					}
				}

				// A step that produced no notification was not read by the menu
				// loop, so it costs an attempt and not a position.
				step(VK_DOWN);
			}

			return _target_reached.load();
		}

		void arm_target(const Target &target)
		{
			{
				std::lock_guard<std::mutex> lock(_target_title_mutex);
				_target_title = target.title;
			}
			_target_by_title.store(target.by_title());
			_target_item.store(target.item);
			_target_is_popup.store(target.is_popup);
			_target_reached.store(false);
			_target_armed.store(true);

			// Armed last so the handler cannot match a stale destination, and
			// checked here so a highlight that arrived before arming is not
			// missed either. A title target cannot be re-checked this way - the
			// menu that held the earlier highlight was not recorded - so it
			// relies on the walk, which visits every item anyway.
			if(!target.by_title()
			   && _select_count.load() != 0
			   && _selected_item.load() == target.item
			   && _selected_is_popup.load() == target.is_popup)
				_target_reached.store(true);
		}

		/*
			What an item says, with the parts that are presentation removed.

			The documented two-call pattern: ask with dwTypeData null to learn
			cch, then read into cch + 1. A fixed buffer truncates silently, which
			is the defect src/dll/src/Include/MenuText.h was written to remove.
			https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmenuiteminfow

			Owner-drawn items included: Shell keeps MIIM_STRING on its own items
			deliberately - a screen reader reads the title from there alongside
			MFT_OWNERDRAW - and check-invariants enforces it.
		*/
		static std::wstring item_title(HMENU menu, UINT id_low_word)
		{
			if(!menu)
				return {};

			/*
				Found by scanning positions and comparing the *low word* of each
				identifier, not by GetMenuItemInfo(..., fByPosition = FALSE).

				WM_MENUSELECT carries the identifier in the low-order word of
				wParam, and a menu identifier is a UINT: everything above 16 bits
				is gone by the time it arrives. Both kinds of identifier Shell
				hands out are above that line - its own start at 0x0FFFFFFF and
				the by-position tracking range at 0x60000000 - so a by-command
				lookup with what WM_MENUSELECT reported fails with
				ERROR_MENU_ITEM_NOT_FOUND every time. Measured here on 2026-08-25
				against a composed menu whose items were 0x60000000 upward and
				whose notifications were 0, 1, 2 ...
				https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menuselect

				This costs one walk of one menu per highlight, in a test driver.
				Shell itself is unaffected: it tracks with TPM_RETURNCMD, which
				returns the whole identifier.
			*/
			auto count = ::GetMenuItemCount(menu);
			int found = -1;
			for(int i = 0; i < count && found < 0; i++)
			{
				MENUITEMINFOW probe{};
				probe.cbSize = sizeof(probe);
				probe.fMask = MIIM_ID;
				if(::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &probe)
				   && LOWORD(probe.wID) == LOWORD(id_low_word))
					found = i;
			}
			if(found < 0)
				return {};

			return item_title_at(menu, static_cast<UINT>(found));
		}

		/*
			The same, addressed by position rather than by identifier.

			Needed because WM_MENUSELECT does not report the same kind of
			thing for both kinds of item:

				"If the selected item is a command item, this parameter
				 contains the identifier of the menu item. If the selected
				 item opens a drop-down menu or submenu, this parameter
				 contains the index of the drop-down menu or submenu in the
				 main menu."
				https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menuselect

			item_title above searches for a matching identifier, which for a
			popup means searching for an item whose wID happens to equal a
			*position* - it finds the wrong item or none at all. That is why
			the first attempt at a nested by-position scenario could not steer
			into the host submenu: the driver walked the whole composed root
			past five popups without recognising any of them.
		*/
		static std::wstring item_title_at(HMENU menu, UINT position)
		{
			if(!menu || position >= static_cast<UINT>(::GetMenuItemCount(menu)))
				return {};

			const auto found = position;
			MENUITEMINFOW mii{};
			mii.cbSize = sizeof(mii);
			mii.fMask = MIIM_STRING;
			mii.dwTypeData = nullptr;
			if(!::GetMenuItemInfoW(menu, static_cast<UINT>(found), TRUE, &mii) || mii.cch == 0)
				return {};

			std::wstring text(static_cast<size_t>(mii.cch) + 1, L'\0');
			mii.dwTypeData = text.data();
			mii.cch = mii.cch + 1;
			if(!::GetMenuItemInfoW(menu, static_cast<UINT>(found), TRUE, &mii))
				return {};
			text.resize(mii.cch);

			// Drop the accelerator column and the mnemonic markers, so the
			// caller names an item by what a person reading it would say.
			auto tab = text.find(L'\t');
			if(tab != std::wstring::npos)
				text.erase(tab);
			std::wstring out;
			out.reserve(text.size());
			for(auto c : text)
			{
				if(c != L'&')
					out.push_back(c);
			}
			return out;
		}

		// Waits until this thread actually owns a visible popup window.
		//
		// This replaces a blind 800 ms sleep, and the blind wait was the second
		// half of the transient. `_menu_up` is set by the earliest of several
		// messages - but under TPM_NONOTIFY the only one of those that survives
		// is WM_MENUSELECT, which does not arrive until something is
		// *selected*, and nothing is selected until this driver presses a key.
		// So for every NONOTIFY scenario the event never fired, the driver
		// waited out its whole budget and then started pressing keys at a menu
		// it had never confirmed was there. When the machine was slow enough
		// that it was not, the keystrokes went nowhere, the menu closed
		// untouched, and the recorded trace no longer matched.
		//
		// The window is the signal that needs no notifications at all. Matching
		// on the owner thread rather than on any visible #32768 keeps another
		// application's menu from answering for ours.
		bool wait_for_our_menu(DWORD budget_ms)
		{
			for(DWORD waited = 0; waited < budget_ms; waited += 25)
			{
				for(auto window : visible_popup_windows())
				{
					if(::GetWindowThreadProcessId(window, nullptr) == _owner_thread)
						return true;
				}
				if(::WaitForSingleObject(_menu_up, 25) == WAIT_OBJECT_0)
					return true;
			}
			return false;
		}

		// Polls until a popup window exists, then a little longer, and returns
		// what is on screen.
		//
		// The settling wait is not superstition. A composed menu's window is
		// created before its items are measured, and this project has already
		// been caught once reading a menu that had not finished becoming
		// itself - AGENTS.md, "A context menu read seconds after an Explorer
		// restart measures Explorer settling, not your change". The cheap
		// version of that rule here is to wait for the reading to stop
		// changing: two consecutive polls agreeing on the same window set.
		std::vector<HWND> wait_for_popups(DWORD budget_ms)
		{
			std::vector<HWND> previous;
			for(DWORD waited = 0; waited < budget_ms; waited += 50)
			{
				auto now = visible_popup_windows();
				if(!now.empty() && now == previous)
					return now;
				previous = now;
				::Sleep(50);
			}
			return previous;
		}

		// Reads the menu that is on screen right now, and then - if the machine's
		// handlers gave it one - opens a submenu and reads that too.
		//
		// Two reads rather than one, and the reason is a measurement. The
		// documented descent (a menu item's get_accChild "Retrieves the
		// IDispatch interface to the pop-up menu object for this item") does
		// return the right object, but its accLocation is (0,0 0x0) whether or
		// not the submenu is open - so it cannot answer where anything was
		// placed. Geometry has to come from each popup's own #32768 window, and
		// parent and child are told apart by which window was not there before.
		void read_the_menu()
		{
			_snapshot.attempted = true;

			// _menu_up is not the signal to read on, and taking it for one is
			// what the first version of this did. It is set by the earliest of
			// several messages, and under takeover the earliest is a
			// WM_INITMENUPOPUP that Shell *synthesises* for the borrowed host
			// menu - sent before Shell has composed anything, let alone shown
			// it. Reading then finds no popup window at all, which reads as
			// "Shell drew nothing" rather than as "we looked too early".
			//
			// So the window is what gets waited for, which is also what
			// section 3.8 of the handoff specifies.
			auto before = wait_for_popups(4000);
			_snapshot.popups_before = before.size();
			if(before.size() != 1)
				return;

			_snapshot.root = read_popup(before.front());

			auto composed = _composed_menu.load();
			_snapshot.rows = read_hmenu(composed ? composed : _tracked_menu.load());

			size_t index = 0;
			for(size_t i = 0; i < _snapshot.root.items.size(); i++)
			{
				if(_snapshot.root.items[i].has_popup())
				{
					index = i + 1;
					break;
				}
			}
			if(!index || !_script.read_submenu)
				return;

			_snapshot.submenu_attempted = true;
			_snapshot.parent_item_index = index;

			// WM_MENUSELECT reports a *position* for an item that opens a
			// submenu, and an MSAA child id is its position plus one. This is
			// the one place the two indexings meet, and the popup-menu page is
			// what makes the conversion a contract rather than an assumption:
			// "The child IDs for the menu items are numbered sequentially from
			// top to bottom starting with one."
			if(!navigate_to(Target::popup(static_cast<UINT>(index - 1))))
			{
				_navigation_failed = true;
				return;
			}

			for(int attempt = 0; attempt < 12; attempt++)
			{
				post(Key::vk(VK_RIGHT));
				::Sleep(120);

				auto after = visible_popup_windows();
				for(auto window : after)
				{
					if(std::find(before.begin(), before.end(), window) != before.end())
						continue;

					_snapshot.child = read_popup(window);
					// The parent is re-read now rather than reused, because
					// opening a child changes the parent item's state - the
					// item gains HOTTRACKED and FOCUSED - and an assertion
					// about placement wants both sides read at the same moment.
					_snapshot.parent = read_popup(before.front());
					_snapshot.submenu_opened = true;
					return;
				}
			}
		}

		void drive()
		{
			// Under TPM_NONOTIFY the owner hears nothing until something is
			// selected - measured on this machine, that flag suppresses
			// WM_ENTERMENULOOP, WM_INITMENU, WM_INITMENUPOPUP,
			// WM_UNINITMENUPOPUP and WM_EXITMENULOOP - so there is no message
			// to wait for and this used to wait out a fixed budget instead.
			// Waiting for the window needs no notifications; see
			// wait_for_our_menu for the transient that came of guessing.
			wait_for_our_menu(2000);

			if(_script.read_menu)
			{
				// AccessibleObjectFromWindow unmarshals an interface published
				// by another thread's window, so this thread needs an
				// apartment. S_FALSE means somebody already initialised one and
				// still has to be balanced; RPC_E_CHANGED_MODE means it must
				// not be.
				// https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex
				auto com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
				read_the_menu();
				if(SUCCEEDED(com))
					::CoUninitialize();
			}
			else if(!navigate_to(_script.root))
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
			{
				_popup_count.fetch_add(1);

				// Which HMENU is actually on screen. Shell tracks a menu it
				// composed and hands the host's own menu the borrowed-popup
				// treatment, so the first initialised handle that is not the
				// one passed to track is the composed root - the same signature
				// saw_a_menu_other_than uses to tell takeover from a decline.
				// Captured here rather than derived from the trace afterwards
				// because the driver needs it *while* the menu is up: by the
				// time the trace is read the composed menu is destroyed.
				auto opened = reinterpret_cast<HMENU>(wp);
				if(opened && opened != _tracked_menu.load() && !_composed_menu.load())
					_composed_menu.store(opened);
			}

			if(msg == WM_MENUSELECT && HIWORD(wp) != 0xFFFF)
			{
				// "If the selected item is a command item, this parameter
				// contains the identifier of the menu item. If the selected item
				// opens a drop-down menu or submenu, this parameter contains the
				// index of the drop-down menu or submenu in the main menu."
				// https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menuselect
				auto item = static_cast<UINT>(LOWORD(wp));
				auto popup = (HIWORD(wp) & MF_POPUP) != 0;

				_selected_item.store(item);
				_selected_is_popup.store(popup);
				_select_count.fetch_add(1);

				// The menu holding the highlighted item, which is what a title
				// lookup needs and is not otherwise recoverable: for a nested
				// highlight it is the submenu, not the tracked root.
				auto owning = reinterpret_cast<HMENU>(lp);

				// Latched here rather than sampled by the driver, so a
				// destination the highlight only passed through is still an
				// arrival. See navigate_to.
				if(_target_armed.load())
				{
					if(_target_by_title.load())
					{
						std::wstring text;
						{
							std::lock_guard<std::mutex> lock(_target_title_mutex);
							text = _target_title;
						}
						// Popup-ness is part of the match, not a filter against
						// it. This was `!popup`, which is the same thing for
						// every title target that names a command item - and
						// made it impossible to name a submenu by title at all,
						// which is what steering into one through a composed
						// menu requires.
						//
						// And the lookup differs with it, because wParam does:
						// an identifier for a command item, "the index of the
						// drop-down menu or submenu" for a popup. Asking
						// item_title for a popup searches for an item whose wID
						// equals a position, which finds the wrong item or none.
						if(!text.empty() && popup == _target_is_popup.load())
						{
							auto says = popup ? item_title_at(owning, item)
											  : item_title(owning, item);
							if(says.find(text) != std::wstring::npos)
								_target_reached.store(true);
						}
					}
					else if(item == _target_item.load()
							&& popup == _target_is_popup.load())
						_target_reached.store(true);
				}
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

		// Where the script wants the highlight, and whether it has ever been
		// there. Latched by the message handler; see navigate_to.
		std::atomic<bool> _target_armed{ false };
		std::atomic<UINT> _target_item{ 0 };
		std::atomic<bool> _target_is_popup{ false };
		std::atomic<bool> _target_by_title{ false };
		mutable std::mutex _target_title_mutex;
		std::wstring _target_title;
		std::atomic<bool> _target_reached{ false };
		bool _navigation_failed{};

		// The handle passed to track, and the first different one Windows
		// initialised - which is Shell's composed menu when it took over, and
		// nothing when it declined.
		std::atomic<HMENU> _tracked_menu{ nullptr };
		std::atomic<HMENU> _composed_menu{ nullptr };

		// Written by the driver thread before track returns, read by the caller
		// afterwards. The two never overlap: track waits for the driver.
		MenuSnapshot _snapshot;

	public:
		bool navigation_failed() const { return _navigation_failed; }
		void clear_navigation_failure() { _navigation_failed = false; }
		size_t draw_items() const { return _draw_items.load(); }
		const MenuSnapshot &snapshot() const { return _snapshot; }
	};
}
