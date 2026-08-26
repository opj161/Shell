#pragma once

/*
	Translating between two contracts: the one the host asked for, and the one
	Shell needs in order to know what the user picked.

	The problem, stated exactly. A host calls TrackPopupMenu with its own flags.
	Shell intercepts, builds a different menu, and tracks that instead. Whatever
	the host would have observed, it must still observe - and Shell must also
	learn which item was chosen, because for its own items nobody else will run
	them.

	Those two needs collide in TPM_RETURNCMD, and until now the collision was
	losing:

	  - With TPM_RETURNCMD the call "returns the menu item identifier of the item
	    that the user selected". Without it the call returns TRUE and the owner
	    is notified instead.
	    https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenuex
	  - Shell adds neither flag today. So against a host that did not pass
	    TPM_RETURNCMD, tracking returns 1, ContextMenu::InvokeCommand(1) matches
	    no item, and the command the user chose does not run. Meanwhile Windows
	    posts WM_COMMAND to the host carrying whatever wID that item had - for
	    one of Shell's own items, a synthetic identifier at or above 0x0fffffff,
	    which means nothing to the host.

	So: two failures from one missing flag, and both silent.

	The fix is to add TPM_RETURNCMD always, learn the identifier, run what
	belongs to Shell, and then give the host back exactly what the real API
	would have given it. That is safe because of something the trace harness
	measured rather than the documentation stating it
	(src/tests/hostprobe/fixtures/README.md, Windows 11 26200.8875 x64):

	    TPM_RETURNCMD is what suppresses WM_COMMAND. TPM_NONOTIFY is not - it
	    suppresses the menu lifecycle instead, and WM_COMMAND survives it.

	Adding TPM_RETURNCMD therefore stops the stray synthetic identifier reaching
	the host as a side effect of fixing the invocation, with no second flag and
	no host-class opt-in. What the host loses by it, Shell posts back.

	Posted, not sent. "The window does not receive a WM_COMMAND message from the
	menu until the function returns" was read in the plan as placing delivery
	before the return; it says the opposite. Sending synchronously from inside
	the hook would put the notification in front of the host's own tracking call
	returning, which is a sequence untouched Windows never produces.

	This used to claim the trace harness measured that, and the harness cannot:
	Probe::track drains its own queue (src/tests/hostprobe/Probe.h) before the
	summary is printed, so a posted message and a sent one leave identical
	fixtures. See fixtures/README.md. It was settled instead by a direct probe,
	which records whether the window procedure ran *inside* TrackPopupMenu or
	only once the caller pumped its queue afterwards - Windows 11 26200 x64,
	MSVC 14.44.35207, three runs:

	    WM_COMMAND                       during-track 0   after-return 1
	    WM_MENUCOMMAND (MNS_NOTIFYBYPOS) during-track 0   after-return 1
	                                     IsMenu(lParam) true at delivery

	So both are posted, and the handle-carrying one is posted too - despite its
	page opening with "Sent when the user invokes a command from a menu"
	(https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menucommand).
	Shell's PostMessageW is what untouched Windows does. Do not "fix" it to a
	send on the strength of that sentence; this is the measurement that says
	otherwise.

	Everything here is pure arithmetic over flags, so it is tested directly:
	src/tests/test_host_contract.cpp.
*/

#include <cstdint>

// TPM_* and TRUE/FALSE. The DLL always has windows.h by way of pch; the test
// includes it too, and this include makes the header standalone either way.
#include <windows.h>

namespace Nilesoft
{
	namespace Shell
	{
		// What Shell tracks its own composed menu with, given what the host
		// asked for.
		struct HostTrackPlan
		{
			uint32_t flags{};
			bool added_returncmd{};
		};

		// TPM_RETURNCMD in; TPM_NONOTIFY out.
		//
		// The removal is not new and is deliberately kept: NONOTIFY suppresses
		// WM_INITMENUPOPUP among the rest of the lifecycle, and Shell's own
		// popups are driven through the host window. Recorded as a decision in
		// docs/refactor/01-takeover-contract.md section 3a rather than left as
		// an unexplained line.
		inline HostTrackPlan plan_host_track(uint32_t flags_in)
		{
			HostTrackPlan plan;
			plan.added_returncmd = (flags_in & TPM_RETURNCMD) == 0;
			plan.flags = (flags_in | TPM_RETURNCMD) & ~static_cast<uint32_t>(TPM_NONOTIFY);
			return plan;
		}

		enum class HostNotification
		{
			None,
			Command,		// WM_COMMAND, MAKEWPARAM(id, 0), lParam 0

			/*
				WM_MENUCOMMAND, wParam = position, lParam = the containing menu.

				For a host that set MNS_NOTIFYBYPOS on the menu it handed Shell.
				That style changes which message the owner is told with, and it
				is a property of the menu's *header*:

					"MNS_NOTIFYBYPOS ... Menu owner receives a WM_MENUCOMMAND
					 message instead of a WM_COMMAND message when the user makes
					 a selection. MNS_NOTIFYBYPOS is a menu header style and has
					 no effect when applied to individual sub menus."
					 https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-menuinfo

					"wParam - The zero-based index of the item selected.
					 lParam - A handle to the menu for the item selected."
					 https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menucommand

				Such a host has no reason to give its items meaningful
				identifiers, so before this existed the completion answered
				`None` for most of them - wID 0 reads as "nothing was chosen" -
				and the user's click did nothing at all. That is the half of
				docs/refactor/01-takeover-contract.md section 3's replay table
				that was never built; the harness proved what *Windows* does with
				the style and nothing asserted what *Shell* does.
			*/
			MenuCommand,
		};

		// The identifier range ContextMenu hands out for items of its own
		// (ContextMenu.h, ident::start_id and start_sys). Duplicated here rather
		// than reached for, so this header stays free of the menu engine and can
		// be tested without it - and pinned by a test that fails if either
		// constant moves.
		//
		// It is duplicated for a second reason. Today an identifier in this range
		// cannot reach complete_host_contract, because InvokeCommand recognises
		// it and returns 0. That is one function away, in another file, and the
		// rule it enforces - a synthetic identifier must never be posted to a
		// host - is this function's rule. So this function enforces it too.
		inline constexpr uint32_t SYNTHETIC_ID_FIRST = 0x0fffffff;
		inline constexpr uint32_t SYNTHETIC_ID_LAST = 0x5fffffff;

		inline constexpr bool is_synthetic_id(uint32_t id)
		{
			return id >= SYNTHETIC_ID_FIRST && id < SYNTHETIC_ID_LAST;
		}

		// What the hook does once the menu has closed and Shell has run whatever
		// was its own.
		struct HostCompletion
		{
			int result{};				// the hook's return value
			HostNotification notify{ HostNotification::None };
			uint32_t notify_id{};		// Command

			// MenuCommand. `notify_menu` is the menu that *owns* the item, not
			// the root - a nested selection names its own submenu.
			uint32_t notify_position{};
			void *notify_menu{};
		};

		/*
			Everything the completion needs to know about how the menu ended,
			gathered while the ContextMenu is still alive.

			A struct rather than six positional parameters, because two of the
			six are only meaningful together and the function has to stay pure
			arithmetic - that is what makes test_host_contract.cpp possible
			without a menu, a window or a host.
		*/
		struct HostSelection
		{
			// The identifier the tracked menu returned; 0 if the user cancelled.
			int selected{};

			// What ContextMenu::InvokeCommand made of it: 0 when Shell
			// recognised the item and ran it, otherwise the identifier that
			// belongs to the host.
			int unhandled{};

			// The call did not show a menu at all, as opposed to showing one the
			// user dismissed.
			bool tracking_failed{};

			// The borrowed root carried MNS_NOTIFYBYPOS and the host did not ask
			// for TPM_RETURNCMD, so this menu is replayed by position.
			bool by_position{};

			// Where `unhandled` came from in the host's own menu. Resolved by
			// ContextMenu before it is destroyed; `position_known` is false when
			// the identifier was not a mirrored native item - a Shell item the
			// user picked, or one Shell could not place.
			bool position_known{};
			uint32_t position{};
			void *containing_menu{};
		};

		/*
			selected  - the identifier the tracked menu returned; 0 if the user
			            cancelled.
			unhandled - what ContextMenu::InvokeCommand made of it: 0 when Shell
			            recognised the item and ran it, otherwise the mirrored
			            native identifier that belongs to the host.

			The return values match what the real API does, which the harness
			recorded rather than this code guessing:

			  TPM_RETURNCMD, item chosen  -> the identifier   (select.returncmd.*)
			  TPM_RETURNCMD, cancelled    -> 0                (cancel.returncmd)
			  no TPM_RETURNCMD, either    -> TRUE             (select.plain.*, cancel.plain)

			Note the last row: a cancelled track still returns TRUE without
			TPM_RETURNCMD. "The return value is nonzero if the function succeeds"
			is about the function, not about the user changing their mind.

			tracking_failed - the call did not show a menu at all, as opposed to
			            showing one the user dismissed. The two are documented as
			            indistinguishable by return value ("If the user cancels
			            the menu without making a selection, or if an error
			            occurs, the return value is zero"), and once TPM_RETURNCMD
			            is always set they collapse to the same 0 - so without
			            this a notifying host would be told TRUE for a call that
			            never ran.

			            What separates them is the last-error code, which is
			            measured rather than documented: a cancelled track leaves
			            it at 0 (cancel.returncmd.trace) and a failed one sets
			            ERROR_INVALID_MENU_HANDLE
			            (question.a_failed_track_sets_a_last_error.trace).
		*/
		inline HostCompletion complete_host_contract(uint32_t flags_in, const HostSelection &sel)
		{
			HostCompletion completion;

			const int selected = sel.selected;
			const int unhandled = sel.unhandled;
			const bool tracking_failed = sel.tracking_failed;

			if(tracking_failed)
			{
				// Zero either way: it is what TPM_RETURNCMD returns on error, and
				// it is FALSE without it.
				completion.result = 0;
				return completion;
			}

			if(flags_in & TPM_RETURNCMD)
			{
				// The host asked for an identifier and will invoke it itself.
				// Shell hands back 0 for anything it ran, which is also what a
				// cancelled menu gives - correct in both cases, because there is
				// nothing left for the host to do.
				completion.result = unhandled;
				return completion;
			}

			completion.result = TRUE;

			// Nothing to tell the host about: either the user cancelled, or the
			// item was Shell's and Shell ran it. A synthetic identifier must
			// never reach a host under any flag combination.
			if(unhandled == 0 || selected == 0)
				return completion;

			// Belt and braces, per the note on is_synthetic_id: whatever else
			// went wrong upstream, one of Shell's own identifiers is not
			// something a host can be told about.
			if(is_synthetic_id(static_cast<uint32_t>(unhandled)))
				return completion;

			// The host asked for a quiet menu. Honour that: it gets its return
			// value and no message, exactly as the real API would leave it.
			if(flags_in & TPM_NONOTIFY)
				return completion;

			if(sel.by_position)
			{
				// The item's own identifier is not what this host is owed, and
				// for most of its items there is not a useful one to owe: a menu
				// addressed by position commonly leaves wID at 0 and may repeat
				// an identifier across items. Only the position it came from,
				// and the menu that holds it, mean anything here.
				//
				// If Shell could not place it, say nothing rather than fall back
				// to WM_COMMAND. A by-position host is not listening for that
				// message, and the identifier it carried would be an internal
				// tracking value that exists only inside Shell.
				if(!sel.position_known)
					return completion;

				completion.notify = HostNotification::MenuCommand;
				completion.notify_position = sel.position;
				completion.notify_menu = sel.containing_menu;
				return completion;
			}

			completion.notify = HostNotification::Command;
			completion.notify_id = static_cast<uint32_t>(unhandled);
			return completion;
		}

		// The four-argument form the hook and the older tests use. Kept so this
		// header's original callers are unaffected by the by-position work.
		inline HostCompletion complete_host_contract(uint32_t flags_in, int selected,
													 int unhandled, bool tracking_failed = false)
		{
			HostSelection sel;
			sel.selected = selected;
			sel.unhandled = unhandled;
			sel.tracking_failed = tracking_failed;
			return complete_host_contract(flags_in, sel);
		}
	}
}
