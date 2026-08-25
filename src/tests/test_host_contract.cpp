#include "test.h"

#include <windows.h>
#include "Include/HostContract.h"

// The translation between the flags a host passed and the flags Shell tracks
// with, plus what the host is owed once the menu has closed.
//
// The defect these pin is not hypothetical and is not a matter of taste. Against
// a host that does not pass TPM_RETURNCMD, Shell used to track its composed menu
// without it too - so the call returned TRUE, ContextMenu::InvokeCommand(1)
// matched nothing, and the command the user clicked did not run. At the same
// time Windows posted WM_COMMAND to that host carrying the chosen item's wID,
// which for one of Shell's own items is a synthetic identifier at or above
// 0x0fffffff and means nothing to anybody.
//
// The return values asserted below are not invented either. They are what the
// real API did when the trace harness asked it:
//   TPM_RETURNCMD + selection -> the identifier   (select.returncmd.*.trace)
//   TPM_RETURNCMD + cancel    -> 0                (cancel.returncmd.trace)
//   no TPM_RETURNCMD          -> TRUE, either way (select.plain.*, cancel.plain)

using namespace Nilesoft::Shell;

namespace
{
	// What Explorer passes for a shell context menu.
	constexpr uint32_t EXPLORER_LIKE = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN;

	// A host that lets the menu notify it instead - the case that was broken.
	constexpr uint32_t NOTIFY_LIKE = TPM_RIGHTBUTTON | TPM_LEFTALIGN;

	// Any identifier Shell hands out for one of its own items.
	constexpr int SYNTHETIC = 0x0fffffff + 7;

	// A mirrored native item keeps the host's own identifier.
	constexpr int NATIVE = 0x7003;
}

TEST(host_contract, returncmd_is_added_when_the_host_did_not_ask_for_it)
{
	auto plan = plan_host_track(NOTIFY_LIKE);

	CHECK(plan.added_returncmd);
	CHECK((plan.flags & TPM_RETURNCMD) != 0);
}

TEST(host_contract, returncmd_is_left_alone_when_the_host_already_asked)
{
	auto plan = plan_host_track(EXPLORER_LIKE);

	CHECK(!plan.added_returncmd);
	CHECK((plan.flags & TPM_RETURNCMD) != 0);
}

TEST(host_contract, nonotify_is_removed)
{
	// Measured, not assumed: TPM_NONOTIFY suppresses the menu lifecycle -
	// WM_INITMENUPOPUP included - and not WM_COMMAND. Shell's popups are driven
	// through the host window, so the flag is dropped from what it tracks with.
	auto plan = plan_host_track(NOTIFY_LIKE | TPM_NONOTIFY);

	CHECK((plan.flags & TPM_NONOTIFY) == 0);
}

TEST(host_contract, every_other_flag_the_host_passed_survives)
{
	// Alignment, button and layout are the host's presentation decisions and
	// Shell has no business editing them here.
	auto flags = TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON |
				 TPM_LAYOUTRTL | TPM_NOANIMATION;
	auto plan = plan_host_track(static_cast<uint32_t>(flags));

	CHECK((plan.flags & TPM_RIGHTALIGN) != 0);
	CHECK((plan.flags & TPM_BOTTOMALIGN) != 0);
	CHECK((plan.flags & TPM_RIGHTBUTTON) != 0);
	CHECK((plan.flags & TPM_LAYOUTRTL) != 0);
	CHECK((plan.flags & TPM_NOANIMATION) != 0);
}

TEST(host_contract, a_returncmd_host_gets_the_native_identifier_back)
{
	// Shell did not recognise the item, so it is the host's to run.
	auto done = complete_host_contract(EXPLORER_LIKE, NATIVE, NATIVE);

	CHECK_EQ(done.result, NATIVE);
	CHECK(done.notify == HostNotification::None);
}

TEST(host_contract, a_returncmd_host_gets_zero_for_an_item_shell_ran)
{
	// InvokeCommand returns 0 for anything in Shell's own identifier range.
	// There is nothing left for the host to do, and it must not be handed an
	// identifier it cannot map.
	auto done = complete_host_contract(EXPLORER_LIKE, SYNTHETIC, 0);

	CHECK_EQ(done.result, 0);
	CHECK(done.notify == HostNotification::None);
}

TEST(host_contract, a_returncmd_host_gets_zero_when_the_user_cancelled)
{
	auto done = complete_host_contract(EXPLORER_LIKE, 0, 0);

	CHECK_EQ(done.result, 0);
	CHECK(done.notify == HostNotification::None);
}

TEST(host_contract, a_notifying_host_is_told_about_a_native_item)
{
	// This is the message Windows itself used to post while Shell tracked
	// without TPM_RETURNCMD. Adding the flag stops Windows sending it, so Shell
	// has to.
	auto done = complete_host_contract(NOTIFY_LIKE, NATIVE, NATIVE);

	CHECK_EQ(done.result, TRUE);
	CHECK(done.notify == HostNotification::Command);
	CHECK_EQ(done.notify_id, static_cast<uint32_t>(NATIVE));
}

TEST(host_contract, a_synthetic_identifier_never_reaches_a_notifying_host)
{
	// The regression that mattered. Shell ran the item, so InvokeCommand
	// returned 0 and there is nothing to notify - and in particular the
	// 0x0fffffff-range identifier the user's click really carried must not be
	// posted to a host that has no idea what it means.
	auto done = complete_host_contract(NOTIFY_LIKE, SYNTHETIC, 0);

	CHECK_EQ(done.result, TRUE);
	CHECK(done.notify == HostNotification::None);
	CHECK_EQ(done.notify_id, 0u);
}

TEST(host_contract, a_notifying_host_still_gets_true_when_the_user_cancelled)
{
	// cancel.plain.trace: the real API returns 1 here. "Nonzero if the function
	// succeeds" is about the function, not about the user changing their mind.
	auto done = complete_host_contract(NOTIFY_LIKE, 0, 0);

	CHECK_EQ(done.result, TRUE);
	CHECK(done.notify == HostNotification::None);
}

TEST(host_contract, a_nonotify_host_is_not_notified)
{
	// It asked for a quiet menu. Shell strips the flag from what it tracks with,
	// for reasons that have nothing to do with the host's wishes, so honouring
	// them here is the whole of the honouring.
	auto done = complete_host_contract(NOTIFY_LIKE | TPM_NONOTIFY, NATIVE, NATIVE);

	CHECK_EQ(done.result, TRUE);
	CHECK(done.notify == HostNotification::None);
}

TEST(host_contract, a_nonotify_returncmd_host_still_gets_its_identifier)
{
	// TPM_NONOTIFY says nothing about the return value.
	auto done = complete_host_contract(EXPLORER_LIKE | TPM_NONOTIFY, NATIVE, NATIVE);

	CHECK_EQ(done.result, NATIVE);
	CHECK(done.notify == HostNotification::None);
}

TEST(host_contract, a_synthetic_identifier_is_refused_even_if_it_gets_this_far)
{
	// Defence in depth. InvokeCommand zeroes anything in Shell's own range
	// before this function ever sees it, so this cannot happen today - which is
	// exactly why it is worth a test: the rule belongs to this function, and the
	// thing enforcing it is one function away in another file.
	auto done = complete_host_contract(NOTIFY_LIKE, SYNTHETIC, SYNTHETIC);

	CHECK_EQ(done.result, TRUE);
	CHECK(done.notify == HostNotification::None);
}

TEST(host_contract, the_synthetic_range_matches_the_one_contextmenu_hands_out)
{
	// ContextMenu.h: ident::start_id 0x0fffffff, ident::start_sys 0x5fffffff,
	// and equals() is [start_id, start_sys). If either constant moves, the
	// duplicate here has to move with it.
	CHECK(is_synthetic_id(0x0fffffff));
	CHECK(is_synthetic_id(0x5ffffffe));
	CHECK(!is_synthetic_id(0x0ffffffe));
	CHECK(!is_synthetic_id(0x5fffffff));

	// A plausible native identifier from a shell context menu is nowhere near it.
	CHECK(!is_synthetic_id(static_cast<uint32_t>(NATIVE)));
}

TEST(host_contract, a_failed_track_is_zero_for_every_host)
{
	// Once TPM_RETURNCMD is always set, "the user cancelled" and "the call never
	// showed a menu" both come back as 0, and a notifying host would otherwise
	// be told TRUE for a menu that never appeared. What tells them apart is the
	// last-error code: 0 for a cancel, ERROR_INVALID_MENU_HANDLE for a failure.
	CHECK_EQ(complete_host_contract(NOTIFY_LIKE, 0, 0, true).result, 0);
	CHECK_EQ(complete_host_contract(EXPLORER_LIKE, 0, 0, true).result, 0);
	CHECK(complete_host_contract(NOTIFY_LIKE, 0, 0, true).notify == HostNotification::None);

	// And the cancel case is still TRUE for a notifying host, which is the whole
	// reason the two have to be told apart.
	CHECK_EQ(complete_host_contract(NOTIFY_LIKE, 0, 0, false).result, TRUE);
}

TEST(host_contract, nothing_is_notified_for_a_cancelled_menu_that_left_a_stale_identifier)
{
	// Defensive: selected == 0 means the user cancelled, whatever else is on the
	// second argument. A notification here would fire a command nobody chose.
	auto done = complete_host_contract(NOTIFY_LIKE, 0, NATIVE);

	CHECK_EQ(done.result, TRUE);
	CHECK(done.notify == HostNotification::None);
}

// ---- by-position replay -------------------------------------------------
//
// docs/refactor/01-takeover-contract.md section 3's replay table has always
// required this and nothing implemented it. A host that set MNS_NOTIFYBYPOS on
// the menu it handed Shell is owed a different message carrying a different
// thing:
//
//     "MNS_NOTIFYBYPOS ... Menu owner receives a WM_MENUCOMMAND message instead
//      of a WM_COMMAND message when the user makes a selection."
//     https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-menuinfo
//
//     "wParam - The zero-based index of the item selected.
//      lParam - A handle to the menu for the item selected."
//     https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menucommand
//
// Before this, complete_host_contract could only answer Command - and since
// such a host has no reason to give its items identifiers, `unhandled` was
// usually 0, which reads as "the user cancelled". The click did nothing and
// nothing said so.

namespace
{
	HostSelection by_position(int selected, int unhandled, uint32_t position,
							  void *menu, bool known = true)
	{
		HostSelection sel;
		sel.selected = selected;
		sel.unhandled = unhandled;
		sel.by_position = true;
		sel.position_known = known;
		sel.position = position;
		sel.containing_menu = menu;
		return sel;
	}

	// Stands in for a borrowed HMENU. Never dereferenced by anything here.
	void *const ROOT_MENU = reinterpret_cast<void *>(0xAB01);
	void *const SUB_MENU = reinterpret_cast<void *>(0xAB02);

	// The tracking identifier Shell gives a mirrored native item in this mode.
	// ContextMenu::ID::start_native; outside the synthetic range on purpose.
	constexpr int TRACKING = 0x60000000;
}

TEST(host_contract, a_by_position_host_is_told_which_position)
{
	auto done = complete_host_contract(NOTIFY_LIKE, by_position(TRACKING, TRACKING, 3, ROOT_MENU));

	CHECK_EQ(done.result, TRUE);
	CHECK(done.notify == HostNotification::MenuCommand);
	CHECK_EQ(done.notify_position, (uint32_t)3);
	CHECK(done.notify_menu == ROOT_MENU);
}

// lParam is "A handle to the menu for the item selected" - the submenu that
// owns it, not the root. A host that looks the item up in the menu it is given
// finds nothing if the root is passed for a nested selection.
TEST(host_contract, a_nested_selection_names_its_own_submenu)
{
	auto done = complete_host_contract(NOTIFY_LIKE, by_position(TRACKING, TRACKING, 1, SUB_MENU));

	CHECK(done.notify == HostNotification::MenuCommand);
	CHECK_EQ(done.notify_position, (uint32_t)1);
	CHECK(done.notify_menu == SUB_MENU);
}

// Position zero is a real position, and the first item of a menu is the one
// most likely to be picked. A "0 means nothing was chosen" reading here is the
// same class of defect as the identifier it replaces.
TEST(host_contract, position_zero_is_a_position_and_not_a_cancellation)
{
	auto done = complete_host_contract(NOTIFY_LIKE, by_position(TRACKING, TRACKING, 0, ROOT_MENU));

	CHECK(done.notify == HostNotification::MenuCommand);
	CHECK_EQ(done.notify_position, (uint32_t)0);
}

TEST(host_contract, a_by_position_host_hears_nothing_for_an_item_shell_ran)
{
	// unhandled == 0: Shell recognised the item and ran it. Nothing is owed.
	auto done = complete_host_contract(NOTIFY_LIKE, by_position(SYNTHETIC, 0, 2, ROOT_MENU, false));

	CHECK_EQ(done.result, TRUE);
	CHECK(done.notify == HostNotification::None);
}

TEST(host_contract, a_by_position_host_hears_nothing_when_it_cancels)
{
	auto done = complete_host_contract(NOTIFY_LIKE, by_position(0, 0, 0, ROOT_MENU, false));

	CHECK_EQ(done.result, TRUE);
	CHECK(done.notify == HostNotification::None);
}

// If Shell could not place the identifier, silence is the only safe answer: the
// host is not listening for WM_COMMAND, and the value it would carry is a
// tracking identifier that exists only inside this process.
TEST(host_contract, an_unplaceable_selection_notifies_nothing_rather_than_falling_back)
{
	auto done = complete_host_contract(NOTIFY_LIKE,
									   by_position(TRACKING, TRACKING, 0, nullptr, false));

	CHECK_EQ(done.result, TRUE);
	CHECK(done.notify == HostNotification::None);
}

TEST(host_contract, a_quiet_by_position_menu_is_still_quiet)
{
	// TPM_NONOTIFY is the host asking for no message at all, and that does not
	// change because the message would have been a different one.
	auto sel = by_position(TRACKING, TRACKING, 3, ROOT_MENU);
	auto done = complete_host_contract(NOTIFY_LIKE | TPM_NONOTIFY, sel);

	CHECK_EQ(done.result, TRUE);
	CHECK(done.notify == HostNotification::None);
}

TEST(host_contract, an_ordinary_host_is_untouched_by_any_of_this)
{
	// Not by-position: still WM_COMMAND with the host's own identifier.
	HostSelection sel;
	sel.selected = NATIVE;
	sel.unhandled = NATIVE;
	auto done = complete_host_contract(NOTIFY_LIKE, sel);

	CHECK(done.notify == HostNotification::Command);
	CHECK_EQ(done.notify_id, (uint32_t)NATIVE);
	CHECK_EQ(done.notify_position, (uint32_t)0);
	CHECK(done.notify_menu == nullptr);
}

TEST(host_contract, a_returncmd_host_never_reaches_the_by_position_path)
{
	// The hook only sets by_position for a host without TPM_RETURNCMD, but the
	// function must not depend on the caller for that: a host that asked for an
	// identifier gets its identifier and no message.
	auto sel = by_position(NATIVE, NATIVE, 3, ROOT_MENU);
	auto done = complete_host_contract(EXPLORER_LIKE, sel);

	CHECK_EQ(done.result, NATIVE);
	CHECK(done.notify == HostNotification::None);
}

TEST(host_contract, a_failed_by_position_track_is_still_zero_and_silent)
{
	auto sel = by_position(0, 0, 0, ROOT_MENU, false);
	sel.tracking_failed = true;
	auto done = complete_host_contract(NOTIFY_LIKE, sel);

	CHECK_EQ(done.result, 0);
	CHECK(done.notify == HostNotification::None);
}

// The four-argument form the hook used before this work, and the older tests
// still use. It must keep meaning exactly what it meant.
TEST(host_contract, the_short_form_still_describes_an_ordinary_host)
{
	auto done = complete_host_contract(NOTIFY_LIKE, NATIVE, NATIVE);

	CHECK(done.notify == HostNotification::Command);
	CHECK_EQ(done.notify_id, (uint32_t)NATIVE);
}

TEST(host_contract, a_native_tracking_identifier_is_not_a_synthetic_one)
{
	// ContextMenu.h: ident::start_native 0x60000000, ident::end_native
	// 0x6fffffff. The range sits above start_sys deliberately, so
	// ContextMenu::ID::equals() answers false for it and InvokeCommand leaves a
	// mirrored native item to the host - which is the whole point of giving it
	// an identifier of Shell's own in the first place.
	//
	// It must also stay clear of is_synthetic_id, or the guard in
	// complete_host_contract would treat a placed host item as one of Shell's
	// and swallow the notification.
	CHECK(!is_synthetic_id(0x60000000));
	CHECK(!is_synthetic_id(0x6ffffffe));
	CHECK(is_synthetic_id(0x5ffffffe));
}
