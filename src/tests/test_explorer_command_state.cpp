#include "test.h"

#include <windows.h>
#include "..\dll\src\Include\ExplorerCommandState.h"

// What GetState answered, and what the menu does about it.
//
// docs/refactor/02-first-paint-latency.md section 2 has always specified this:
//
//     hr = GetState(FALSE)
//     E_PENDING -> provisional EXPCMDSTATE_ENABLED, mark provider state_pending,
//     failure   -> omit item this menu; record failure
//
// Half of it was built. E_PENDING produced a provisionally enabled item, but
// nothing was ever marked pending; and any *other* failure fell through to
// GetTitle carrying the caller's ECS_ENABLED initialiser, so a handler whose
// state call failed outright still got an enabled item in the menu and was
// recorded as a success. docs/refactor/09-remediation-plan.md finding P.

using namespace Nilesoft::Shell;

TEST(explorer_command_state, an_enabled_command_is_shown)
{
	auto s = classify_command_state(S_OK, ECS_ENABLED);
	CHECK(s.verdict == CommandStateVerdict::Show);
	CHECK_EQ((int)s.state, (int)ECS_ENABLED);
}

TEST(explorer_command_state, a_disabled_command_is_still_shown)
{
	// Disabled is a state, not a refusal: the item appears greyed. Only
	// ECS_HIDDEN takes it out of the menu.
	auto s = classify_command_state(S_OK, ECS_DISABLED);
	CHECK(s.verdict == CommandStateVerdict::Show);
	CHECK((s.state & ECS_DISABLED) != 0);
}

TEST(explorer_command_state, a_hidden_command_is_not_shown)
{
	auto s = classify_command_state(S_OK, ECS_HIDDEN);
	CHECK(s.verdict == CommandStateVerdict::Hidden);
}

TEST(explorer_command_state, hidden_wins_over_anything_else_in_the_flags)
{
	auto s = classify_command_state(S_OK, ECS_HIDDEN | ECS_ENABLED | ECS_CHECKED);
	CHECK(s.verdict == CommandStateVerdict::Hidden);
}

// "the verb object should not perform any memory intensive computations that
// could cause the UI thread to stop responding. The verb object should return
// E_PENDING in that case"
// https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getstate
//
// A handler that says pending has told us only that it cannot answer cheaply,
// not that the verb is unavailable. Hiding a working command is worse than
// offering one that turns out to be a no-op, so it is shown - and said so.
TEST(explorer_command_state, pending_is_shown_provisionally_enabled_and_reported)
{
	auto s = classify_command_state(E_PENDING, 0);
	CHECK(s.verdict == CommandStateVerdict::Pending);
	CHECK_EQ((int)s.state, (int)ECS_ENABLED);
}

// The out-parameter of a call that did not return S_OK is not a value the
// caller may read. Whatever a pending handler left in it, the item is enabled.
TEST(explorer_command_state, a_pending_handlers_leftover_flags_are_not_believed)
{
	auto s = classify_command_state(E_PENDING, ECS_HIDDEN | ECS_DISABLED);
	CHECK(s.verdict == CommandStateVerdict::Pending);
	CHECK_EQ((int)s.state, (int)ECS_ENABLED);
}

TEST(explorer_command_state, any_other_failure_omits_the_item)
{
	CHECK(classify_command_state(E_FAIL, ECS_ENABLED).verdict
		  == CommandStateVerdict::Failed);
	CHECK(classify_command_state(E_NOTIMPL, ECS_ENABLED).verdict
		  == CommandStateVerdict::Failed);
	CHECK(classify_command_state(RPC_E_DISCONNECTED, ECS_ENABLED).verdict
		  == CommandStateVerdict::Failed);
	CHECK(classify_command_state(E_OUTOFMEMORY, 0).verdict
		  == CommandStateVerdict::Failed);
}

// The exact regression. Before the fix, a failed GetState left `state` at the
// caller's ECS_ENABLED initialiser and the function carried on to GetTitle, so
// this produced a normal, enabled, clickable menu item.
TEST(explorer_command_state, a_failed_state_call_never_produces_an_enabled_item)
{
	auto s = classify_command_state(E_FAIL, ECS_ENABLED);
	CHECK(s.verdict != CommandStateVerdict::Show);
	CHECK(s.verdict != CommandStateVerdict::Pending);
}

// S_FALSE is a success. Nothing in the reference says a handler returns it
// here, but the classification must follow SUCCEEDED rather than == S_OK, or an
// unusual-but-legal answer would be read as a failure and drop the item.
TEST(explorer_command_state, success_is_succeeded_and_not_equality_with_s_ok)
{
	auto s = classify_command_state(S_FALSE, ECS_ENABLED);
	CHECK(s.verdict == CommandStateVerdict::Show);
}
