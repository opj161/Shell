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
