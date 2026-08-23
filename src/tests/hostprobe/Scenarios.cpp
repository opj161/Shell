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

#include <cstdio>

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

		auto menu = build(s.shape, s.notify_by_pos);

		Probe::Script script;
		switch(s.script)
		{
		case ScriptKind::SelectSecond:    script = select_second(); break;
		case ScriptKind::SelectInSubmenu: script = select_in_submenu(); break;
		case ScriptKind::Cancel:          script = cancel(); break;
		case ScriptKind::UnmatchedChar:   script = unmatched_character(); break;
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
			result.command_position = static_cast<UINT>(by_pos.wparam);

		if(s.shape != MenuShape::Invalid)
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

			return v;
		}();
		return all;
	}
}
