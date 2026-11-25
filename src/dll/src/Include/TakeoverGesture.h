#pragma once

/*
	Which gesture a right-click is carrying, decided once from plain data.

	Shell reads modifier keys at the top of the popup hook to decide whether the
	click means something other than "show me a menu": reload the configuration,
	disable the shell, prefer the Windows menu, or - new here - bypass Shell for
	this one click. docs/refactor/01-takeover-contract.md section 7 and
	docs/refactor/05-capabilities.md section 2.

	Those rules used to live inline in Initializer::OnState as a nest of
	conditions over a live Keyboard. That mattered for more than tidiness. The
	plan (QA-04) requires proving that the bypass gesture and the config-reload
	gesture "can never both fire from one click", and the two are evaluated in
	the same hook body, microseconds apart, from two separate reads of the
	keyboard. Proving a property about two independent reads of live global
	state is not something a test can do.

	So the rules are a pure function of a snapshot instead. One call in, one
	Gesture out. Non-interference is then structural - a function returns one
	value - rather than a property somebody has to keep checking, and the whole
	table can be enumerated by a test.

	The default bypass gesture is Ctrl+Alt+right-click, and it is Ctrl+Alt for a
	specific reason: Ctrl+Shift is already the config-reload combo and Ctrl+Win
	already disables the shell, so those are the two it must not be.
*/

#include <cstdint>

namespace Nilesoft
{
	namespace Shell
	{
		enum class Gesture : uint8_t
		{
			// An ordinary right-click. By far the common case.
			None,

			// Re-read the configuration and enable the shell.
			ReloadConfig,

			// Turn Shell off until something turns it back on.
			DisableShell,

			// Stop refreshing; leave the Windows menu in charge.
			PreferModern,

			// Show the host's own menu for this click only, changing nothing.
			// docs/refactor/05-capabilities.md section 2.
			BypassOnce,

			// Compose Shell's menu as usual, but annotate every item with where
			// it came from. docs/refactor/05-capabilities.md section 7,
			// Include/MenuInspector.h.
			Inspect,
		};

		/*
			A snapshot of the keyboard at the moment of the click.

			`held` is the number of keys down after the mouse buttons that
			*caused* this click have been discounted - the caller does that
			subtraction, because only it knows whether the click arrived as a
			right-button press or as the context-menu key. Everything else is a
			plain answer about one key.
		*/
		struct GestureState
		{
			int held{};
			bool ctrl{};
			bool shift{};
			bool win{};
			bool alt{};
			bool lbutton{};
			bool f5{};

			// A left-click on the taskbar is not a gesture at all; it is
			// somebody using the taskbar.
			bool taskbar_lbutton{};
		};

		inline Gesture classify_gesture(const GestureState &s) noexcept
		{
			if(s.taskbar_lbutton)
				return Gesture::None;

			// Nothing held, or too many things held to be a deliberate combo.
			if(s.held <= 0 || s.held > 2)
				return Gesture::None;

			if(s.held == 1)
			{
				if(s.ctrl || s.lbutton || s.f5)
					return Gesture::ReloadConfig;

				if(s.win)
					return Gesture::PreferModern;

				return Gesture::None;
			}

			// Exactly two. The order here is the precedence, and it matters:
			// every combination below is two keys, so a click that satisfied
			// more than one would otherwise depend on which test came first.
			// It cannot - Win, Shift and Alt are mutually exclusive as the
			// second key alongside Ctrl - and the test enumerates the whole
			// table to keep it that way.
			if(s.ctrl && s.win)
				return Gesture::DisableShell;

			if(s.ctrl && s.shift)
				return Gesture::ReloadConfig;

			if(s.ctrl && s.alt)
				return Gesture::BypassOnce;

			// Shift+Alt is the one two-key combination left: every other pair
			// above involves Ctrl.
			//
			// Spelled out in full - including the two keys that must *not* be
			// down - rather than relying on where it sits in this chain. Every
			// rule above is reached only through Ctrl, so each is protected
			// from a contradictory snapshot (three flags set while `held` says
			// two) by an earlier Ctrl rule catching it first. This one is last
			// and has no such protection: written as `shift && alt` it also
			// claims Win+Shift+Alt, which is nobody's gesture. The whole-table
			// walk in test_takeover_gesture is what found that, and it now
			// pins exactly one state per gesture.
			if(s.shift && s.alt && !s.ctrl && !s.win)
				return Gesture::Inspect;

			return Gesture::None;
		}
	}
}
