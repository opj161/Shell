#pragma once

/*
	Lazy initialisation state for host-owned ("native") menu popups.

	Windows treats WM_INITMENUPOPUP as a just-in-time notification: it is sent
	when a drop-down is about to become active, so the owner can populate that
	one popup immediately before it is displayed.

		https://learn.microsoft.com/en-us/windows/win32/menurc/wm-initmenupopup

	Shell used to defeat that model - before the first menu was shown it walked
	the entire host menu tree and sent the notification for the root and every
	descendant, so the user paid for every submenu of every installed shell
	extension whether or not they ever opened one. Microsoft's own guidance for
	shortcut-menu handlers is the opposite: keep the UI thread conservative.

		https://learn.microsoft.com/en-us/windows/win32/shell/verbs-best-practices

	This header holds the per-popup state and the one-shot guard, kept free of
	any Shell dependency so the invariants can be tested directly.
*/

#include <windows.h>

namespace Nilesoft
{
	namespace Shell
	{
		enum class NativeTreePolicy
		{
			// Root level only; a submenu is initialised when it is opened.
			Lazy,
			// Whole tree up front, the pre-1.9.20 behaviour. Diagnostic escape
			// hatch for configurations or hosts that turn out to need it.
			LegacyEager
		};

		// Ordinary configurations stay just-in-time. Parent movement is the one
		// operation that needs descendant topology before a submenu is opened, so
		// an applicable moveto rule reuses the pre-lazy whole-tree behavior. The
		// registry flag remains a diagnostic override, not a user-visible repair.
		inline NativeTreePolicy choose_native_tree_policy(bool modify_enabled,
			bool parent_movement_enabled, bool has_applicable_moveto,
			bool diagnostic_eager_override) noexcept
		{
			return diagnostic_eager_override
				|| (modify_enabled && parent_movement_enabled && has_applicable_moveto)
				? NativeTreePolicy::LegacyEager
				: NativeTreePolicy::Lazy;
		}

		// A moveto rule without a location selector is root-only: the production
		// rule walker explicitly skips non-root levels when location is absent.
		// Such a rule can operate on the already materialized root and must not
		// forfeit lazy submenu initialization.
		inline bool native_moveto_requires_descendant_discovery(bool has_moveto,
			bool has_location_selector) noexcept
		{
			return has_moveto && has_location_selector;
		}

		struct NativePopupState
		{
			// Borrowed. The host owns this menu and destroys it; Shell never does.
			HMENU handle{};

			// Zero-based position of the item that opens this popup in its parent,
			// which is what Windows puts in LOWORD(lParam).
			UINT parent_position{};

			// The host has been told to populate this popup.
			bool initialized{};
			// Guards against re-entering initialisation: the notification runs
			// arbitrary host and third-party extension code synchronously.
			bool initializing{};
			// This level has been enumerated into menuitem_t children.
			bool materialized{};
			// Configured modify() rules have been applied to this level.
			bool rules_applied{};
		};

		/*
			Sends the just-in-time notification for one popup, at most once.

			`notify(HMENU, LPARAM)` performs the actual SendMessage. Returns true
			if this call is the one that initialised the popup.

			The lParam layout is Windows' own: the low word is the position of the
			item that opens the popup, and the high word is nonzero only for a
			window menu. The old code passed 0xFFFFFFFF, which claims position
			65535 in a window menu - neither of which is true here.
		*/
		template<typename Notify>
		inline bool initialize_native_popup(NativePopupState &state, Notify &&notify)
		{
			if(!state.handle || state.initialized || state.initializing)
				return false;

			// Scope guard: the host may throw or longjmp out of the notification,
			// and a stuck `initializing` would wedge that popup permanently empty.
			struct guard
			{
				NativePopupState *s;
				~guard() { s->initializing = false; }
			} g{ &state };

			state.initializing = true;

			notify(state.handle,
				   static_cast<LPARAM>(MAKELPARAM(state.parent_position, FALSE)));

			state.initialized = true;
			return true;
		}
	}
}
