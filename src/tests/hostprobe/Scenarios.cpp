/*
	The scenario matrix from docs/refactor/06-phases-and-tests.md section 2.

	Two kinds of scenario live here, and the distinction matters:

	  - Matrix scenarios walk {TrackPopupMenu, Ex} x {RETURNCMD +/-} x
	    {NONOTIFY +/-} x {MNS_NOTIFYBYPOS +/-} x {select, cancel, submenu}.
	    Their traces are the baseline that Shell's replay must reproduce. They
	    assert nothing by themselves; they record.

	  - Question scenarios each exist to settle one open design decision that
	    the plan could not settle from documentation. Those carry an explicit
	    expectation and report pass or fail, because a design rule is waiting on
	    the answer.

	The four questions, and what turns on each:

	  Q1 menuchar_index_or_id - "Using Menus" says the low word of an
	     MNC_EXECUTE reply is "the zero-based index of the menu item"; the
	     WM_MENUCHAR page says only "the item specified in the low-order word".
	     docs/refactor/05-capabilities.md section 4 warns that returning a
	     command identifier instead would make Windows execute an unrelated
	     item, and Shell's synthetic identifiers are >= 0x0FFFFFFF. Gates
	     mnemonics.

	  Q2 returncmd_duplicate_command - the plan's amendment A4 made
	     TPM_RETURNCMD the only flag Shell adds, leaving the host's notification
	     behaviour alone, on the reasoning that nothing documents NONOTIFY as
	     required alongside it. That holds only if a RETURNCMD track does not
	     *also* notify the owner. Gates docs/refactor/01 section 3.

	  Q3 nonotify_suppression_set - the pages say only "does not send
	     notification messages", without enumerating which. Whether
	     WM_MEASUREITEM and WM_DRAWITEM survive decides whether an owner-drawn
	     menu can be tracked with NONOTIFY at all.

	  Q4 notifybypos_replay_shape - MNS_NOTIFYBYPOS is documented as a header
	     style with "no effect when applied to individual sub menus", and it
	     swaps WM_COMMAND for WM_MENUCOMMAND. Shell has to replay whichever the
	     host chose, so both the position basis and the submenu behaviour need
	     recording.
*/

#include "Scenarios.h"
#include "ShellMenu.h"

#include <cstdio>
#include <cstdlib>
#include <map>

namespace hostprobe
{
	namespace
	{
		// Identifiers deliberately unlike positions, so a result can never be
		// read as both. Item 0 is 5001, item 1 is 5002, and so on.
		constexpr UINT FIRST_ID = 5001;

		HMENU build(MenuShape shape, bool notify_by_pos)
		{
			if(shape == MenuShape::Invalid)
				return reinterpret_cast<HMENU>(static_cast<ULONG_PTR>(0xDEAD0001));

			auto root = ::CreatePopupMenu();

			::AppendMenuW(root, MF_STRING, FIRST_ID + 0, L"&Alpha");
			::AppendMenuW(root, MF_STRING, FIRST_ID + 1, L"&Bravo");

			if(shape == MenuShape::WithOwnerDraw)
			{
				// dwItemData is what an owner-drawn host hangs its own record
				// off, and what section 3 of docs/refactor/05 proposes to wrap
				// with MSAAMENUINFO. Recording it proves the message carried it.
				::AppendMenuW(root, MF_OWNERDRAW, FIRST_ID + 2,
							  reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(0xC0DE)));
			}
			else
				::AppendMenuW(root, MF_STRING, FIRST_ID + 2, L"&Charlie");

			if(shape == MenuShape::WithSubmenu)
			{
				auto child = ::CreatePopupMenu();
				::AppendMenuW(child, MF_STRING, FIRST_ID + 10, L"Child &One");
				::AppendMenuW(child, MF_STRING, FIRST_ID + 11, L"Child &Two");
				::AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(child), L"&Delta");
			}
			else
				::AppendMenuW(root, MF_STRING, FIRST_ID + 3, L"&Delta");

			::AppendMenuW(root, MF_SEPARATOR, 0, nullptr);
			::AppendMenuW(root, MF_STRING, FIRST_ID + 4, L"&Echo");

			if(notify_by_pos)
			{
				// "This style is a header style; it has no effect when applied
				// to individual sub menus."
				// https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-menuinfo
				MENUINFO mi{};
				mi.cbSize = sizeof(mi);
				mi.fMask = MIM_STYLE | MIM_APPLYTOSUBMENUS;
				mi.dwStyle = MNS_NOTIFYBYPOS;
				::SetMenuInfo(root, &mi);
			}

			return root;
		}

		// Highlight the second item - identifier 5002, position 1 - and commit.
		Probe::Script select_second()
		{
			Probe::Script s;
			s.root = Target::command(FIRST_ID + 1);
			s.keys = { Key::vk(VK_RETURN) };
			return s;
		}

		// Into the submenu at position 3, then its first child. The submenu is
		// named by position because that is what WM_MENUSELECT reports for an
		// item that opens one.
		Probe::Script select_in_submenu()
		{
			Probe::Script s;
			s.root = Target::popup(3);
			s.open_submenu = true;
			s.child = Target::command(FIRST_ID + 10);
			s.keys = { Key::vk(VK_RETURN) };
			return s;
		}

		Probe::Script cancel()
		{
			Probe::Script s;
			s.root = Target::command(FIRST_ID + 0);
			s.keys = { Key::vk(VK_ESCAPE) };
			return s;
		}

		// A character matching no mnemonic in the menu above, so WM_MENUCHAR is
		// the only way the owner hears about it. Deliberately typed with nothing
		// highlighted, which is the state a mnemonic press really starts from.
		Probe::Script unmatched_character()
		{
			Probe::Script s;
			s.root = Target::none();
			s.keys = { Key::ch(L'Z') };
			return s;
		}

		// The same, but with the highlight already somewhere - the state a
		// *second* press of a duplicated mnemonic starts from, and the one where
		// the reply has to depend on where the highlight currently is.
		Probe::Script unmatched_character_after_navigating()
		{
			Probe::Script s;
			s.root = Target::command(FIRST_ID + 1);		// position 1
			s.keys = { Key::ch(L'Z') };
			return s;
		}

		// A shell menu's identifiers are whatever QueryContextMenu assigned, so
		// the destination is discovered rather than written down. Shell mirrors
		// a native item's wID unchanged (docs/refactor/01 section 4, "native
		// wID (identity preserved today)"), which is exactly why steering to one
		// through Shell's composed menu is a test of that claim and not just of
		// the driver.
		Probe::Script select_drivable(UINT id)
		{
			Probe::Script s;
			s.root = Target::command(id);
			s.keys = { Key::vk(VK_RETURN) };
			return s;
		}

		// Steers by what the item says instead of by its identifier.
		//
		// The only way to reach a mirrored native item in a by-position menu:
		// Shell tracks those under identifiers of its own, so the host's wID names
		// nothing in the menu the driver is looking at. See Target::titled.
		Probe::Script select_titled(const wchar_t *title)
		{
			Probe::Script s;
			s.root = Target::titled(title);
			s.keys = { Key::vk(VK_RETURN) };
			return s;
		}

		Probe::Script cancel_whatever()
		{
			Probe::Script s;
			s.root = Target::none();
			s.keys = { Key::vk(VK_ESCAPE) };
			return s;
		}

		// Chooses nothing. The driver reads the menu back through MSAA and
		// opens whichever item turns out to have a submenu, so the destination
		// is discovered on the machine rather than written down here.
		Probe::Script read_composed_menu(bool with_submenu)
		{
			Probe::Script s;
			s.root = Target::none();
			s.read_menu = true;
			s.read_submenu = with_submenu;
			s.keys = { Key::vk(VK_ESCAPE) };
			return s;
		}

		// Does every popup the owner was told about get told it is finished
		// with? WM_INITMENUPOPUP and WM_UNINITMENUPOPUP both carry the HMENU in
		// wParam, so the pairing is readable straight off the trace.
		//
		// "If an application receives WM_INITMENUPOPUP, it will receive
		// WM_UNINITMENUPOPUP."
		// https://learn.microsoft.com/en-us/windows/win32/menurc/wm-uninitmenupopup
		size_t count_unpaired_popups(const Trace &trace)
		{
			std::map<HMENU, int> balance;
			for(auto &e : trace.events())
			{
				if(e.message == WM_INITMENUPOPUP)
					balance[reinterpret_cast<HMENU>(e.wparam)]++;
				else if(e.message == WM_UNINITMENUPOPUP)
					balance[reinterpret_cast<HMENU>(e.wparam)]--;
			}

			size_t unpaired = 0;
			for(auto &entry : balance)
			{
				if(entry.second != 0)
					unpaired++;
			}
			return unpaired;
		}

		// Whether anything other than the handle the host created was
		// initialised. That is the visible signature of takeover: Shell tracks a
		// menu it composed, and the host's own menu becomes the borrowed one.
		bool saw_a_menu_other_than(const Trace &trace, HMENU host_menu)
		{
			for(auto &e : trace.events())
			{
				if(e.message != WM_INITMENUPOPUP && e.message != WM_MENUSELECT)
					continue;

				auto handle = e.message == WM_INITMENUPOPUP
					? reinterpret_cast<HMENU>(e.wparam)
					: reinterpret_cast<HMENU>(e.lparam);

				if(handle && handle != host_menu)
					return true;
			}
			return false;
		}

		// ---- rendering verdicts ------------------------------------------
		//
		// Computed here, where the snapshot is still in hand, so main.cpp only
		// has to report. Each fills a detail string on failure: "the order did
		// not match" is not actionable without saying at which position and
		// with what on either side.

		// Every item Shell composed is legible to a screen reader, and a
		// separator is legible as a separator rather than as a nameless item.
		// This is section 05.3's result, which until now was proved by a probe
		// that no longer exists.
		void judge_readable(const MenuSnapshot &snap, Result *out)
		{
			for(size_t i = 0; i < snap.root.items.size(); i++)
			{
				auto &item = snap.root.items[i];
				wchar_t detail[256];

				if(item.is_separator())
				{
					if(item.has_name)
					{
						::swprintf_s(detail, L"position %zu is a separator and "
									 L"reports the name [%s]", i, item.name.c_str());
						out->render_detail = detail;
						return;
					}
					continue;
				}

				if(!item.has_name)
				{
					::swprintf_s(detail, L"position %zu has no name and is not a "
								 L"separator (role %ld)", i, item.role);
					out->render_detail = detail;
					return;
				}
			}
			out->render_readable = true;
		}

		// What MSAA reports, position by position, is what the composed HMENU
		// holds. The popup-menu page is what makes this a contract: child IDs
		// are "numbered sequentially from top to bottom starting with one" and
		// the count includes separators, so MSAA child i is HMENU position
		// i - 1 with nothing left to interpret.
		//
		// This is the assertion `MenuModel` is most likely to disturb, because
		// a reordering is invisible to every other test in the tree.
		void judge_order(const MenuSnapshot &snap, Result *out)
		{
			wchar_t detail[512];

			if(snap.rows.size() != snap.root.items.size())
			{
				::swprintf_s(detail, L"the composed menu holds %zu item(s) and "
							 L"MSAA reports %zu", snap.rows.size(),
							 snap.root.items.size());
				out->render_detail = detail;
				return;
			}

			for(size_t i = 0; i < snap.rows.size(); i++)
			{
				auto &row = snap.rows[i];
				auto &item = snap.root.items[i];

				if(row.separator != item.is_separator())
				{
					::swprintf_s(detail, L"position %zu: the menu says %s, MSAA "
								 L"says %s", i,
								 row.separator ? L"separator" : L"item",
								 item.is_separator() ? L"separator" : L"item");
					out->render_detail = detail;
					return;
				}

				if(row.submenu != item.has_popup())
				{
					::swprintf_s(detail, L"position %zu [%s]: the menu %s a "
								 L"submenu, MSAA %s HASPOPUP", i, row.title.c_str(),
								 row.submenu ? L"has" : L"has no",
								 item.has_popup() ? L"reports" : L"does not report");
					out->render_detail = detail;
					return;
				}

				// Separators are skipped: measured, a separator reports
				// STATE_SYSTEM_UNAVAILABLE whatever the HMENU says about it, so
				// comparing the two there would assert a fact about Windows'
				// rendering rather than about Shell's composition.
				if(!row.separator && row.disabled != item.unavailable())
				{
					::swprintf_s(detail, L"position %zu [%s]: the menu says %s, "
								 L"MSAA says %s", i, row.title.c_str(),
								 row.disabled ? L"disabled" : L"enabled",
								 item.unavailable() ? L"unavailable" : L"available");
					out->render_detail = detail;
					return;
				}

				// The one check that catches a pure reorder of plain items,
				// where every structural property still agrees. Measured: MSAA
				// strips the '&' marker and keeps everything after a tab, so
				// the title is stripped and nothing else is touched.
				if(!row.separator)
				{
					auto expected = strip_mnemonics(row.title);
					if(expected != item.name)
					{
						::swprintf_s(detail, L"position %zu: the menu holds [%s], "
									 L"MSAA reports [%s]", i, expected.c_str(),
									 item.name.c_str());
						out->render_detail = detail;
						return;
					}
				}
			}

			out->render_order_matches = true;
		}

		// The measure pass put every item inside the window it sized, and laid
		// them out in reading order. Deliberately expressed as relationships
		// rather than as pixel counts: a composed menu's height depends on
		// which handlers the machine has, so a recorded size would be a fixture
		// this file has already established it cannot have.
		//
		// Column-aware, because `settings columns = N` exists (section 05.5a):
		// items continue down a column and then start a new one, so the
		// ordering rule applies within a column and a change of left edge is a
		// new column rather than a violation.
		void judge_geometry(const MenuSnapshot &snap, Result *out)
		{
			wchar_t detail[512];

			if(!snap.root.has_rect)
			{
				out->render_detail = L"the popup reported no rectangle";
				return;
			}
			if(snap.root.items.empty())
			{
				out->render_detail = L"the popup reported no items";
				return;
			}

			const ReadItem *previous = nullptr;
			for(size_t i = 0; i < snap.root.items.size(); i++)
			{
				auto &item = snap.root.items[i];

				if(!item.has_rect)
				{
					::swprintf_s(detail, L"position %zu reported no rectangle", i);
					out->render_detail = detail;
					return;
				}

				if(item.rect.right <= item.rect.left || item.rect.bottom <= item.rect.top)
				{
					::swprintf_s(detail, L"position %zu measured empty: (%ld,%ld)-(%ld,%ld)",
								 i, item.rect.left, item.rect.top,
								 item.rect.right, item.rect.bottom);
					out->render_detail = detail;
					return;
				}

				if(item.rect.left < snap.root.rect.left
				   || item.rect.top < snap.root.rect.top
				   || item.rect.right > snap.root.rect.right
				   || item.rect.bottom > snap.root.rect.bottom)
				{
					::swprintf_s(detail, L"position %zu at (%ld,%ld)-(%ld,%ld) is "
								 L"outside the popup (%ld,%ld)-(%ld,%ld)", i,
								 item.rect.left, item.rect.top,
								 item.rect.right, item.rect.bottom,
								 snap.root.rect.left, snap.root.rect.top,
								 snap.root.rect.right, snap.root.rect.bottom);
					out->render_detail = detail;
					return;
				}

				if(previous && previous->rect.left == item.rect.left
				   && item.rect.top < previous->rect.bottom)
				{
					::swprintf_s(detail, L"position %zu starts at y=%ld, above the "
								 L"bottom of the item before it (y=%ld)", i,
								 item.rect.top, previous->rect.bottom);
					out->render_detail = detail;
					return;
				}

				previous = &item;
			}

			out->render_geometry_ok = true;
		}

		// A submenu is placed against the item it belongs to.
		//
		// Two relationships, both chosen so the answer does not depend on where
		// on the screen the menu happened to open. Windows flips a submenu to
		// the *left* of its parent when there is no room on the right, and this
		// harness deliberately places its popup in whichever screen corner is
		// furthest from the cursor - so a rule that demanded "child.left equals
		// parent.right" would fail on half of the runs for a reason that has
		// nothing to do with Shell.
		//
		// What this does NOT pin, stated rather than implied: the multi-level
		// property behind parent_of_top() - that a third-level popup is placed
		// against its own parent rather than against the root. With one level,
		// the parent *is* the root and the two are indistinguishable. A
		// composed menu on an arbitrary machine is not guaranteed to have a
		// three-level cascade, so that stays where section 01.6a left it:
		// verified by hand, structural in PopupStack rather than tested for.
		void judge_placement(const MenuSnapshot &snap, Result *out)
		{
			wchar_t detail[512];
			constexpr long ADJACENT = 8;	// a border's worth of slack

			if(snap.parent_item_index == 0
			   || snap.parent_item_index > snap.parent.items.size())
			{
				out->render_detail = L"the parent item could not be read back";
				return;
			}
			if(!snap.child.has_rect || !snap.parent.has_rect)
			{
				out->render_detail = L"a popup reported no rectangle";
				return;
			}

			auto &parent_item = snap.parent.items[snap.parent_item_index - 1];
			if(!parent_item.has_rect)
			{
				out->render_detail = L"the parent item reported no rectangle";
				return;
			}

			auto rightwards = std::labs(snap.child.rect.left - snap.parent.rect.right);
			auto leftwards = std::labs(snap.child.rect.right - snap.parent.rect.left);
			if(rightwards > ADJACENT && leftwards > ADJACENT)
			{
				::swprintf_s(detail, L"the submenu at x=(%ld..%ld) sits against "
							 L"neither edge of its parent (x=%ld..%ld)",
							 snap.child.rect.left, snap.child.rect.right,
							 snap.parent.rect.left, snap.parent.rect.right);
				out->render_detail = detail;
				return;
			}

			// Vertical overlap rather than equality, for the same reason:
			// Windows lifts a submenu that would run off the bottom. What is
			// being pinned is that it belongs to *this* row - a submenu placed
			// against the top of the menu while its item is at the bottom does
			// not overlap it.
			if(snap.child.rect.top >= parent_item.rect.bottom
			   || snap.child.rect.bottom <= parent_item.rect.top)
			{
				::swprintf_s(detail, L"the submenu at y=(%ld..%ld) does not "
							 L"overlap the row that opens it (y=%ld..%ld)",
							 snap.child.rect.top, snap.child.rect.bottom,
							 parent_item.rect.top, parent_item.rect.bottom);
				out->render_detail = detail;
				return;
			}

			out->render_submenu_placed = true;
		}

		bool menu_contains(HMENU menu, UINT id)
		{
			if(!menu || !id)
				return false;

			auto count = ::GetMenuItemCount(menu);
			for(int i = 0; i < count; i++)
			{
				MENUITEMINFOW mii{};
				mii.cbSize = sizeof(mii);
				mii.fMask = MIIM_ID | MIIM_SUBMENU;
				if(!::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii))
					continue;
				if(!mii.hSubMenu && mii.wID == id)
					return true;
				if(mii.hSubMenu && menu_contains(mii.hSubMenu, id))
					return true;
			}
			return false;
		}
	}

	std::wstring flag_names(UINT flags)
	{
		std::wstring out;
		auto add = [&out](const wchar_t *n)
		{
			if(!out.empty()) out += L'|';
			out += n;
		};
		if(flags & TPM_RETURNCMD)  add(L"RETURNCMD");
		if(flags & TPM_NONOTIFY)   add(L"NONOTIFY");
		if(flags & TPM_RIGHTBUTTON)add(L"RIGHTBUTTON");
		if(flags & TPM_VERTICAL)   add(L"VERTICAL");
		if(flags & TPM_RIGHTALIGN) add(L"RIGHTALIGN");
		if(flags & TPM_BOTTOMALIGN)add(L"BOTTOMALIGN");
		if(out.empty()) out = L"LEFTALIGN|TOPALIGN";
		return out;
	}

	Result run_scenario(const Scenario &s)
	{
		auto &probe = Probe::instance();

		probe.policy() = ReplyPolicy{};
		probe.policy().handle_menuchar = s.handle_menuchar;
		probe.policy().menuchar_action = s.menuchar_action;
		probe.policy().menuchar_operand = s.menuchar_operand;

		// Lives until the end of the function: the handlers that filled it may
		// hold state keyed on the HMENU, and Shell certainly does.
		ShellMenu shell_menu;

		HMENU menu = nullptr;
		if(s.shape == MenuShape::ShellItem)
		{
			if(!shell_menu.create(probe.owner()))
			{
				Result failed;
				failed.name = s.name;
				failed.setup_failed = true;
				failed.setup_detail = shell_menu.why();
				return failed;
			}
			menu = shell_menu.menu();

			// The style goes on the menu the *host* owns, which is this one -
			// Shell composes its own and must never carry it. Applied after the
			// handlers have filled the menu so the appended probe items sit at
			// the end and cannot move anything the scan already found.
			if(s.notify_by_pos && !shell_menu.apply_notify_by_position())
			{
				Result failed;
				failed.name = s.name;
				failed.setup_failed = true;
				failed.setup_detail = L"could not apply MNS_NOTIFYBYPOS to the host menu";
				return failed;
			}
		}
		else
			menu = build(s.shape, s.notify_by_pos);

		Probe::Script script;
		switch(s.script)
		{
		case ScriptKind::SelectSecond:    script = select_second(); break;
		case ScriptKind::SelectInSubmenu: script = select_in_submenu(); break;
		case ScriptKind::Cancel:          script = cancel(); break;
		case ScriptKind::UnmatchedChar:   script = unmatched_character(); break;
		case ScriptKind::UnmatchedCharAfterNavigating:
			script = unmatched_character_after_navigating(); break;
		case ScriptKind::SelectDrivableCommand:
			script = select_drivable(shell_menu.drivable_command()); break;
		case ScriptKind::SelectByPositionTarget:
			script = select_titled(ShellMenu::TARGET_TITLE); break;
		case ScriptKind::CancelWhatever:  script = cancel_whatever(); break;
		case ScriptKind::ReadComposedMenu:
			script = read_composed_menu(
				s.expectation == Expect::ASubmenuOpensAgainstItsParent); break;
		}

		probe.clear_navigation_failure();

		Result result;
		result.name = s.name;
		result.returned = probe.track(menu, s.flags, s.use_ex, script);
		result.navigation_failed = probe.navigation_failed();
		result.last_error = probe.last_error();
		result.trace = probe.trace().render();
		result.draw_items = probe.draw_items();

		// The trace on its own does not pin the thing that most often matters.
		// cancel.returncmd and cancel.plain produce identical message streams and
		// return 0 and 1 respectively; without this line a fixture would not
		// notice if those swapped. The last-error code is reduced to set/none
		// because its exact value after a *successful* call is not something to
		// hold Windows to, while "a failure sets one" is the whole distinction
		// HostContract.h depends on.
		{
			wchar_t summary[160];
			::swprintf_s(summary, L"= returned %d, lasterror %s, ownerdrawn %s\n",
						 result.returned,
						 result.last_error == ERROR_SUCCESS ? L"none" : L"set",
						 result.draw_items ? L"drawn" : L"not drawn");
			result.trace += summary;
		}

		result.command_ids = probe.trace().count(WM_COMMAND);
		result.menu_commands = probe.trace().count(WM_MENUCOMMAND);
		result.measure_items = probe.trace().count(WM_MEASUREITEM);
		result.menu_selects = probe.trace().count(WM_MENUSELECT);
		result.init_popups = probe.trace().count(WM_INITMENUPOPUP);
		result.uninit_popups = probe.trace().count(WM_UNINITMENUPOPUP);

		Event command;
		if(probe.trace().first(WM_COMMAND, &command))
			result.command_id = LOWORD(command.wparam);
		Event by_pos;
		if(probe.trace().first(WM_MENUCOMMAND, &by_pos))
		{
			// "wParam - The zero-based index of the item selected. lParam - A
			// handle to the menu for the item selected."
			// https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menucommand
			result.command_position = static_cast<UINT>(by_pos.wparam);
			result.command_menu = reinterpret_cast<HMENU>(by_pos.lparam);
		}

		// What the host built, for the by-position expectation to compare
		// against. Meaningless for the shapes this file builds itself, which
		// assert against recorded traces instead.
		if(s.shape == MenuShape::ShellItem)
		{
			result.expected_position =
				s.script == ScriptKind::SelectByPositionTarget
					? shell_menu.target_position()
					: shell_menu.drivable_position();
			result.host_root_menu = shell_menu.menu();
			result.expected_title = shell_menu.title_at(result.expected_position);
			result.replayed_title = shell_menu.title_at(result.command_position);
		}

		// The rendering verdicts, while the snapshot the driver took is still
		// the current one. Every judge_* leaves its own detail behind on
		// failure; a scenario that did not ask for a read leaves them all
		// false, and `render_attempted` is what tells the two apart.
		{
			auto &snap = probe.snapshot();
			result.render_attempted = snap.attempted;
			result.render_popups_seen = snap.popups_before;
			result.render_items = snap.root.items.size();
			result.render_submenu_attempted = snap.submenu_attempted;
			result.render_submenu_opened = snap.submenu_opened;
			result.render_popup_rect = snap.root.rect;

			// One popup and no more. Two means another menu was open on this
			// desktop, and then none of these readings is about Shell - see
			// AGENTS.md on running the harness on a quiet desktop.
			// Only the judge this scenario asked for. They share one detail
			// string, so running all four would leave whichever spoke last
			// explaining a failure reported by another - which is exactly what
			// the first version did, and it reported the ordering judge's
			// words under the readability scenario's name.
			if(snap.attempted && snap.popups_before == 1)
			{
				switch(s.expectation)
				{
				case Expect::EveryComposedItemIsReadable:
					judge_readable(snap, &result); break;
				case Expect::ComposedOrderSurvivesToTheScreen:
					judge_order(snap, &result); break;
				case Expect::ThePopupContainsTheItemsItMeasured:
					judge_geometry(snap, &result); break;
				case Expect::ASubmenuOpensAgainstItsParent:
					if(snap.submenu_opened)
						judge_placement(snap, &result);
					break;
				default:
					break;
				}
			}
		}

		result.unpaired_popups = count_unpaired_popups(probe.trace());
		result.init_uninit_paired = result.unpaired_popups == 0;
		result.tracked_a_different_menu = saw_a_menu_other_than(probe.trace(), menu);
		result.command_is_native = result.command_ids > 0
			&& menu_contains(menu, result.command_id);

		// ShellMenu owns its own handle; destroying it here would leave the
		// destructor a dangling one, and the IContextMenu that filled it is
		// still alive at this point.
		if(s.shape != MenuShape::Invalid && s.shape != MenuShape::ShellItem)
			::DestroyMenu(menu);
		return result;
	}

	const std::vector<Scenario> &scenarios()
	{
		static std::vector<Scenario> all = []
		{
			std::vector<Scenario> v;

			// ---- matrix -------------------------------------------------
			// The four TPM_* combinations that decide how a selection is
			// reported, each for both tracking entry points.
			struct Combo { const wchar_t *tag; UINT flags; };
			const Combo combos[] = {
				{ L"plain",              TPM_LEFTALIGN | TPM_RIGHTBUTTON },
				{ L"returncmd",          TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD },
				{ L"nonotify",           TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_NONOTIFY },
				{ L"returncmd_nonotify", TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY },
			};

			for(auto &combo : combos)
			{
				for(int ex = 0; ex < 2; ex++)
				{
					Scenario s;
					s.name = std::wstring(L"select.") + combo.tag + (ex ? L".ex" : L".classic");
					s.flags = combo.flags;
					s.use_ex = ex != 0;
					s.script = ScriptKind::SelectSecond;
					v.push_back(s);
				}
			}

			{
				Scenario s;
				s.name = L"cancel.returncmd";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
				s.script = ScriptKind::Cancel;
				v.push_back(s);
			}
			{
				Scenario s;
				s.name = L"cancel.plain";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON;
				s.script = ScriptKind::Cancel;
				v.push_back(s);
			}
			{
				// The lazy-submenu case: what a host really receives when a
				// child popup opens, which is the contract Shell's
				// NativeMenuBridge synthesises.
				Scenario s;
				s.name = L"submenu.returncmd";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
				s.shape = MenuShape::WithSubmenu;
				s.script = ScriptKind::SelectInSubmenu;
				v.push_back(s);
			}
			{
				Scenario s;
				s.name = L"submenu.plain";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON;
				s.shape = MenuShape::WithSubmenu;
				s.script = ScriptKind::SelectInSubmenu;
				v.push_back(s);
			}

			// ---- questions ----------------------------------------------
			{
				// Q1. Reply MNC_EXECUTE with low word 2. If the low word is a
				// zero-based index the chosen item is identifier 5003; if it
				// were an identifier, nothing in this menu has id 2 and the
				// result would be something else entirely.
				Scenario s;
				s.name = L"question.menuchar_low_word_is_an_index";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
				s.script = ScriptKind::UnmatchedChar;
				s.handle_menuchar = true;
				s.menuchar_action = MNC_EXECUTE;
				s.menuchar_operand = 2;
				s.expectation = Expect::ReturnEquals;
				s.expected = FIRST_ID + 2;
				s.why = L"Using Menus: the low word of an MNC_EXECUTE reply is "
						L"the zero-based index of the item";
				v.push_back(s);
			}
			{
				// Q1b. The same reply built from an identifier instead - what a
				// naive implementation would send. It must not choose the item
				// whose identifier that is.
				Scenario s;
				s.name = L"question.menuchar_low_word_is_not_an_identifier";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
				s.script = ScriptKind::UnmatchedChar;
				s.handle_menuchar = true;
				s.menuchar_action = MNC_EXECUTE;
				s.menuchar_operand = FIRST_ID + 2;
				s.expectation = Expect::ReturnDiffers;
				s.expected = FIRST_ID + 2;
				s.why = L"returning an identifier where an index is expected must "
						L"not select that identifier's item";
				v.push_back(s);
			}
			{
				// Q2. TPM_RETURNCMD without TPM_NONOTIFY: does the owner also
				// get a WM_COMMAND? Amendment A4 in docs/refactor/01 rests on
				// the answer being no.
				Scenario s;
				s.name = L"question.returncmd_alone_sends_no_wm_command";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
				s.script = ScriptKind::SelectSecond;
				s.expectation = Expect::NoCommandMessage;
				s.why = L"docs/refactor/01 section 3: TPM_RETURNCMD alone is "
						L"enough, so exactly one component observes the selection";
				v.push_back(s);
			}
			{
				// Q3. What NONOTIFY actually suppresses, for an owner-drawn
				// menu that cannot be measured without WM_MEASUREITEM.
				Scenario s;
				s.name = L"question.nonotify_still_measures_ownerdraw";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD;
				s.shape = MenuShape::WithOwnerDraw;
				s.script = ScriptKind::SelectSecond;
				s.expectation = Expect::OwnerDrawReachesTheOwner;
				s.why = L"the pages say only 'does not send notification "
						L"messages' without enumerating which";
				v.push_back(s);
			}
			{
				// Q3 control: the same menu without NONOTIFY, so the diff shows
				// exactly what the flag removed.
				Scenario s;
				s.name = L"question.ownerdraw_control_without_nonotify";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
				s.shape = MenuShape::WithOwnerDraw;
				s.script = ScriptKind::SelectSecond;
				s.expectation = Expect::OwnerDrawReachesTheOwner;
				s.why = L"control for the NONOTIFY comparison";
				v.push_back(s);
			}
			{
				// Q4. MNS_NOTIFYBYPOS swaps WM_COMMAND for WM_MENUCOMMAND, and
				// the payload becomes a position. Item 1 is identifier 5002.
				Scenario s;
				s.name = L"question.notifybypos_reports_a_position";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON;
				s.notify_by_pos = true;
				s.script = ScriptKind::SelectSecond;
				s.expectation = Expect::MenuCommandPositionEquals;
				s.expected = 1;
				s.why = L"WM_MENUCOMMAND wParam is the zero-based index of the "
						L"item selected";
				v.push_back(s);
			}
			{
				// Q4b. With MNS_NOTIFYBYPOS *and* TPM_RETURNCMD, what does the
				// tracking call return - an identifier, or nothing? Shell has to
				// answer the host in whichever currency it asked for.
				Scenario s;
				s.name = L"question.notifybypos_with_returncmd";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
				s.notify_by_pos = true;
				s.script = ScriptKind::SelectSecond;
				s.expectation = Expect::Record;
				s.why = L"undecided in the plan; recorded so the replay rule can "
						L"be written against what Windows does";
				v.push_back(s);
			}

			{
				// Q6. The same MNC_EXECUTE reply, but against an *owner-drawn*
				// item - which is the only kind Shell ever renders, and the
				// reason WM_MENUCHAR reaches the owner at all. An owner-drawn
				// item has no text in the HMENU, so this asks whether Windows
				// still honours a position-based reply when it cannot itself see
				// what is at that position. Gates Include/Mnemonics.h.
				//
				// Position 2 is the owner-drawn item in this shape, so a correct
				// answer is identifier 5003.
				Scenario s;
				s.name = L"question.menuchar_executes_an_ownerdrawn_item";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
				s.shape = MenuShape::WithOwnerDraw;
				s.script = ScriptKind::UnmatchedChar;
				s.handle_menuchar = true;
				s.menuchar_action = MNC_EXECUTE;
				s.menuchar_operand = 2;
				s.expectation = Expect::ReturnEquals;
				s.expected = FIRST_ID + 2;
				s.why = L"Shell renders every item owner-drawn, so this is the "
						L"configuration mnemonics actually run in";
				v.push_back(s);
			}
			{
				// Q7. MNC_SELECT moves the highlight instead of committing,
				// which is what a duplicated mnemonic does on the first press.
				// The trace records where the highlight ended up.
				Scenario s;
				s.name = L"question.menuchar_select_moves_the_highlight";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
				s.script = ScriptKind::UnmatchedChar;
				s.handle_menuchar = true;
				s.menuchar_action = MNC_SELECT;
				s.menuchar_operand = 3;
				s.expectation = Expect::Record;
				s.why = L"MNC_SELECT is what a duplicated mnemonic returns on its "
						L"first press; the menu must stay open";
				v.push_back(s);
			}
			{
				// Q8. Can the owner see where the highlight is when it is asked?
				// Include/Mnemonics.h cycles through duplicated mnemonics by
				// starting from the current selection, and MFS_HILITE is the only
				// place that fact lives. Every other scenario types with nothing
				// highlighted, so this is the one that says the readback works.
				Scenario s;
				s.name = L"question.menuchar_sees_the_current_highlight";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
				s.script = ScriptKind::UnmatchedCharAfterNavigating;
				s.handle_menuchar = true;
				s.menuchar_action = MNC_IGNORE;
				s.expectation = Expect::Record;
				s.why = L"the reply for a duplicated mnemonic depends on where the "
						L"highlight already is";
				v.push_back(s);
			}
			{
				// Q5. "If the user cancels the menu without making a selection,
				// or if an error occurs, the return value is zero" - so under
				// TPM_RETURNCMD the two are indistinguishable by return value,
				// and Shell has to tell them apart to know whether to answer a
				// notifying host TRUE or FALSE. Does the last-error code
				// separate them? Cancel is recorded as leaving it at 0
				// (cancel.returncmd), so this asks what a real failure leaves.
				Scenario s;
				s.name = L"question.a_failed_track_sets_a_last_error";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
				s.shape = MenuShape::Invalid;
				s.script = ScriptKind::Cancel;
				s.expectation = Expect::FailedWithLastError;
				s.why = L"Include/HostContract.h has to answer a notifying host "
						L"TRUE for a cancel and FALSE for a failure";
				v.push_back(s);
			}

			// ---- takeover ------------------------------------------------
			// The only scenarios Shell actually takes over, because they are
			// the only ones that reach it the way a file manager does. See
			// ShellMenu.h. Their traces are printed but never recorded: the
			// items depend on which handlers this machine has installed.
			{
				// Does Shell substitute its own menu at all? Everything below
				// is meaningless if it declined, and a decline looks exactly
				// like success from the return value alone.
				Scenario s;
				s.name = L"takeover.shell_composes_its_own_menu";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON;
				s.shape = MenuShape::ShellItem;
				s.script = ScriptKind::CancelWhatever;
				s.needs = Requires::Takeover;
				s.machine_specific = true;
				s.expectation = Expect::ShellTrackedItsOwnMenu;
				s.why = L"a shell-namespace menu is the one shape QueryShellWindow "
						L"accepts from a host that is not Explorer";
				v.push_back(s);
			}
			{
				// a634ab6. The borrowed popup is a real one this time, filled
				// by whatever handlers the machine has, rather than the fake in
				// test_native_menu_lazy.cpp.
				Scenario s;
				s.name = L"takeover.every_borrowed_popup_is_told_it_is_finished";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON;
				s.shape = MenuShape::ShellItem;
				s.script = ScriptKind::CancelWhatever;
				s.needs = Requires::Takeover;
				s.machine_specific = true;
				s.expectation = Expect::EveryInitPopupHasOneUninit;
				s.why = L"\"If an application receives WM_INITMENUPOPUP, it will "
						L"receive WM_UNINITMENUPOPUP\"";
				v.push_back(s);
			}
			{
				// b63fdc2, against a host that does not ask for identifiers.
				// The item is chosen from the menu the host built, so the
				// assertion is that the identifier survived being mirrored into
				// Shell's menu and replayed back out - not merely that some
				// WM_COMMAND arrived.
				Scenario s;
				s.name = L"takeover.a_native_item_replays_its_own_identifier";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON;
				s.shape = MenuShape::ShellItem;
				s.script = ScriptKind::SelectDrivableCommand;
				s.needs = Requires::Takeover;
				s.machine_specific = true;
				s.expectation = Expect::CommandCarriesTheNativeIdentifier;
				s.why = L"a host that did not ask for TPM_RETURNCMD must still be "
						L"told which of *its* items was chosen";
				v.push_back(s);
			}
			{
				// The other half of the same contract: a host that did ask for
				// identifiers gets one back and is not notified as well.
				Scenario s;
				s.name = L"takeover.a_returncmd_host_is_not_notified_as_well";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
				s.shape = MenuShape::ShellItem;
				s.script = ScriptKind::SelectDrivableCommand;
				s.needs = Requires::Takeover;
				s.machine_specific = true;
				s.expectation = Expect::NoCommandMessage;
				s.why = L"synthetic identifiers must never reach a host, and a "
						L"RETURNCMD host is notified by the return value alone";
				v.push_back(s);
			}
			{
				/*
					docs/refactor/09-remediation-plan.md R2, and the half of
					section 3's replay table that was never built.

					question.notifybypos_reports_a_position already records what
					*Windows* does with MNS_NOTIFYBYPOS. Nothing asserted what
					*Shell* does, and the answer was: nothing at all. Such a host
					has no reason to give its items identifiers, so `unhandled`
					came back 0, which reads as a cancellation, and the user's
					click did nothing with no message and no log line.

					The host menu here carries a duplicate of the identifier the
					script steers to, plus two items with no identifier. Both are
					ordinary in a menu addressed by position and both are what a
					tracking table keyed on the host's own wID gets wrong - see
					ShellMenu::apply_notify_by_position.
				*/
				Scenario s;
				s.name = L"takeover.a_by_position_host_is_told_which_position";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON;
				s.notify_by_pos = true;
				s.shape = MenuShape::ShellItem;
				s.script = ScriptKind::SelectByPositionTarget;
				s.needs = Requires::Takeover;
				s.machine_specific = true;
				s.expectation = Expect::MenuCommandNamesTheHostPosition;
				s.why = L"\"Menu owner receives a WM_MENUCOMMAND message instead "
						L"of a WM_COMMAND message\", carrying \"the zero-based "
						L"index of the item selected\"";
				v.push_back(s);
			}

			// ---- rendering ------------------------------------------------
			// docs/refactor/08-handoff.md section 3.8. Everything above reads
			// the message stream; these four read the menu that is on screen.
			// That is the difference that matters for seam steps 6 and 7,
			// whose regressions are a menu that draws slightly wrong rather
			// than a message that stops arriving - and ContextMenu.cpp, where
			// both of them live, is not linked by the unit test project.
			//
			// They share one script and differ only in what they assert, so a
			// failure names the property rather than the run.
			{
				Scenario s;
				s.name = L"render.every_composed_item_is_readable";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON;
				s.shape = MenuShape::ShellItem;
				s.script = ScriptKind::ReadComposedMenu;
				s.needs = Requires::Takeover;
				s.machine_specific = true;
				s.expectation = Expect::EveryComposedItemIsReadable;
				s.why = L"section 05.3's result, which was proved once by a probe "
						L"that no longer exists";
				v.push_back(s);
			}
			{
				Scenario s;
				s.name = L"render.the_composed_order_reaches_the_screen";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON;
				s.shape = MenuShape::ShellItem;
				s.script = ScriptKind::ReadComposedMenu;
				s.needs = Requires::Takeover;
				s.machine_specific = true;
				s.expectation = Expect::ComposedOrderSurvivesToTheScreen;
				s.why = L"a reordering is what MenuModel is most likely to "
						L"disturb, and nothing else in the tree would notice";
				v.push_back(s);
			}
			{
				Scenario s;
				s.name = L"render.the_popup_contains_the_items_it_measured";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON;
				s.shape = MenuShape::ShellItem;
				s.script = ScriptKind::ReadComposedMenu;
				s.needs = Requires::Takeover;
				s.machine_specific = true;
				s.expectation = Expect::ThePopupContainsTheItemsItMeasured;
				s.why = L"a measure pass that silently changed is what section "
						L"05.5a's 239x1031 against 938x990 caught by hand";
				v.push_back(s);
			}
			{
				Scenario s;
				s.name = L"render.a_submenu_opens_against_its_parent";
				s.flags = TPM_LEFTALIGN | TPM_RIGHTBUTTON;
				s.shape = MenuShape::ShellItem;
				s.script = ScriptKind::ReadComposedMenu;
				s.needs = Requires::Takeover;
				s.machine_specific = true;
				s.expectation = Expect::ASubmenuOpensAgainstItsParent;
				s.why = L"placement is presenter work, and section 07 will move "
						L"the presenter";
				v.push_back(s);
			}

			return v;
		}();
		return all;
	}
}
