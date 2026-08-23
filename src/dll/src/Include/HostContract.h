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

	Posted, not sent, and that is also measured. "The window does not receive a
	WM_COMMAND message from the menu until the function returns" was read in the
	plan as placing delivery before the return; it says the opposite, and the
	traces show Windows posting both WM_COMMAND and WM_MENUCOMMAND after
	WM_EXITMENULOOP. Sending synchronously from inside the hook would put the
	notification in front of the host's own tracking call returning, which is a
	sequence untouched Windows never produces.

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
			uint32_t notify_id{};
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
		inline HostCompletion complete_host_contract(uint32_t flags_in, int selected,
													 int unhandled, bool tracking_failed = false)
		{
			HostCompletion completion;

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

			completion.notify = HostNotification::Command;
			completion.notify_id = static_cast<uint32_t>(unhandled);
			return completion;
		}
	}
}
