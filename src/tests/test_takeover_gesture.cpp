// Which gesture a right-click carries.
//
// The plan's QA-04 requires proving that the bypass gesture and the
// config-reload gesture "can never both fire from one click". That was not
// provable while each read the keyboard for itself, microseconds apart, from
// inside the same hook body - so the rules were made a pure function of one
// snapshot instead, and the property became structural: a function returns one
// value.
//
// What is left for a test is that the table is the table, and that the
// combination chosen for bypass is not one of the combinations that already
// meant something. The exhaustive case below is the one that matters: it walks
// every reachable combination of the four modifiers and asserts that no state
// classified as BypassOnce is also classified as anything else - which is
// trivially true of a function, and is exactly why the classifier is one.
//
// docs/refactor/01-takeover-contract.md section 7,
// docs/refactor/05-capabilities.md section 2.

#include "test.h"

#include "..\dll\src\Include\TakeoverGesture.h"

#include <vector>

using namespace Nilesoft::Shell;

namespace
{
	GestureState keys(int held, bool ctrl = false, bool shift = false,
					  bool win = false, bool alt = false)
	{
		GestureState s;
		s.held = held;
		s.ctrl = ctrl;
		s.shift = shift;
		s.win = win;
		s.alt = alt;
		return s;
	}
}

TEST(takeover_gesture, an_ordinary_right_click_carries_no_gesture)
{
	CHECK(classify_gesture(keys(0)) == Gesture::None);
}

// More than two modifiers is a user resting a hand on the keyboard, not a
// combination anybody meant.
TEST(takeover_gesture, more_than_two_keys_is_not_a_gesture)
{
	CHECK(classify_gesture(keys(3, true, true, true)) == Gesture::None);
	CHECK(classify_gesture(keys(4, true, true, true, true)) == Gesture::None);
}

// A negative count is reachable: the caller subtracts the buttons that caused
// the click, and Shift+F10 takes two off a count that may already be one.
TEST(takeover_gesture, a_negative_count_is_not_a_gesture)
{
	CHECK(classify_gesture(keys(-1, true)) == Gesture::None);
	CHECK(classify_gesture(keys(-2)) == Gesture::None);
}

TEST(takeover_gesture, ctrl_alone_reloads_the_configuration)
{
	CHECK(classify_gesture(keys(1, true)) == Gesture::ReloadConfig);
}

TEST(takeover_gesture, f5_and_the_left_button_also_reload)
{
	auto f5 = keys(1);
	f5.f5 = true;
	CHECK(classify_gesture(f5) == Gesture::ReloadConfig);

	auto left = keys(1);
	left.lbutton = true;
	CHECK(classify_gesture(left) == Gesture::ReloadConfig);
}

TEST(takeover_gesture, win_alone_leaves_the_windows_menu_in_charge)
{
	CHECK(classify_gesture(keys(1, false, false, true)) == Gesture::PreferModern);
}

TEST(takeover_gesture, shift_alone_is_not_a_gesture)
{
	CHECK(classify_gesture(keys(1, false, true)) == Gesture::None);
}

// Alt alone must stay free: it is what CoCreateInstanceHook times activations
// with, and giving it a meaning here would make that diagnostic change the
// behaviour it is diagnosing.
TEST(takeover_gesture, alt_alone_is_not_a_gesture)
{
	CHECK(classify_gesture(keys(1, false, false, false, true)) == Gesture::None);
}

TEST(takeover_gesture, ctrl_and_win_disable_the_shell)
{
	CHECK(classify_gesture(keys(2, true, false, true)) == Gesture::DisableShell);
}

TEST(takeover_gesture, ctrl_and_shift_reload_the_configuration)
{
	CHECK(classify_gesture(keys(2, true, true)) == Gesture::ReloadConfig);
}

TEST(takeover_gesture, ctrl_and_alt_bypass_shell_for_this_click)
{
	CHECK(classify_gesture(keys(2, true, false, false, true)) == Gesture::BypassOnce);
}

// The three pairs the plan says the bypass must not be confused with.
TEST(takeover_gesture, the_bypass_pair_is_none_of_the_pairs_already_taken)
{
	auto bypass = classify_gesture(keys(2, true, false, false, true));

	CHECK(bypass != classify_gesture(keys(2, true, true)));         // Ctrl+Shift
	CHECK(bypass != classify_gesture(keys(2, true, false, true)));  // Ctrl+Win
	CHECK(bypass != classify_gesture(keys(1, true)));               // Ctrl alone
}

TEST(takeover_gesture, shift_and_alt_inspect_the_menu)
{
	// docs/refactor/05-capabilities.md section 7, Include/MenuInspector.h. It
	// is the one two-key combination that was left: every other pair below
	// involves Ctrl.
	CHECK(classify_gesture(keys(2, false, true, false, true)) == Gesture::Inspect);
}

TEST(takeover_gesture, the_inspect_pair_is_none_of_the_pairs_already_taken)
{
	auto inspect = classify_gesture(keys(2, false, true, false, true));

	CHECK(inspect != classify_gesture(keys(2, true, true)));               // Ctrl+Shift
	CHECK(inspect != classify_gesture(keys(2, true, false, true)));        // Ctrl+Win
	CHECK(inspect != classify_gesture(keys(2, true, false, false, true))); // Ctrl+Alt
	CHECK(inspect != classify_gesture(keys(1, true)));                     // Ctrl alone
}

TEST(takeover_gesture, the_remaining_pairs_are_not_gestures)
{
	// Shift+Alt used to be here and is now Inspect. The other two are still
	// unclaimed, and a later gesture taking one of them has to come past this
	// test and past the whole-table walk below.
	CHECK(classify_gesture(keys(2, false, true, true)) == Gesture::None);        // Shift+Win
	CHECK(classify_gesture(keys(2, false, false, true, true)) == Gesture::None); // Win+Alt
}

// A left-click on the taskbar is somebody using the taskbar. It must not
// disable the shell or reload the configuration however many keys are held.
TEST(takeover_gesture, a_taskbar_left_click_is_never_a_gesture)
{
	for(int held = 0; held <= 3; held++)
	{
		auto s = keys(held, true, true, true, true);
		s.lbutton = true;
		s.taskbar_lbutton = true;
		CHECK(classify_gesture(s) == Gesture::None);
	}
}

/*
	The whole reachable table, walked.

	Every combination of the four modifiers at every plausible held-count, with
	one assertion that is the point of the file: whatever a state classifies as,
	it classifies as exactly one thing, and a state that bypasses is therefore
	not a state that also reloads. This is what QA-04 asks for, and it holds
	because there is one classifier rather than two readers of the keyboard.
*/
TEST(takeover_gesture, no_click_can_mean_two_things_at_once)
{
	size_t bypass_states = 0;
	size_t inspect_states = 0;
	size_t reload_states = 0;

	for(int held = -2; held <= 4; held++)
	{
		for(int bits = 0; bits < 16; bits++)
		{
			auto s = keys(held,
						  (bits & 1) != 0,
						  (bits & 2) != 0,
						  (bits & 4) != 0,
						  (bits & 8) != 0);

			auto first = classify_gesture(s);
			auto again = classify_gesture(s);

			// Pure: the same snapshot always classifies the same way. A live
			// keyboard read twice does not have this property, which is the
			// whole reason the classifier takes a snapshot.
			CHECK(first == again);

			if(first == Gesture::BypassOnce)
			{
				bypass_states++;
				CHECK(first != Gesture::ReloadConfig);
				CHECK(first != Gesture::DisableShell);
				CHECK(first != Gesture::PreferModern);
				CHECK(first != Gesture::Inspect);
			}
			else if(first == Gesture::Inspect)
			{
				inspect_states++;
				CHECK(first != Gesture::ReloadConfig);
				CHECK(first != Gesture::DisableShell);
				CHECK(first != Gesture::PreferModern);
				CHECK(first != Gesture::BypassOnce);
			}
			else if(first == Gesture::ReloadConfig)
			{
				reload_states++;
			}
		}
	}

	// Exactly the states that should bypass: held == 2 with Ctrl+Alt, whether
	// or not the classifier was also told about keys it ignores at that count.
	CHECK_MSG(bypass_states == 1, "exactly one modifier combination bypasses");
	CHECK_MSG(inspect_states == 1, "exactly one modifier combination inspects");
	CHECK_MSG(reload_states > 0, "and the reload combinations still exist");
}
