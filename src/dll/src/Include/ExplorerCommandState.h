#pragma once

/*
	What GetState answered, and what it means for the menu.

	Four outcomes from one HRESULT plus one flags word, separated out here so
	the rule can be tested without a handler, a selection or a menu.

	The contract, which the code did not implement
	----------------------------------------------

	`fOkToBeSlow` is FALSE on this path and stays FALSE, because the alternative
	is a third-party call with no bound on it between the user's right-click and
	the first menu pixel:

		"the verb object should not perform any memory intensive computations
		 that could cause the UI thread to stop responding. The verb object
		 should return E_PENDING in that case"
		https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getstate

	docs/refactor/02-first-paint-latency.md section 2 turns that into a rule:

		hr = GetState(FALSE)
		E_PENDING -> provisional EXPCMDSTATE_ENABLED, mark provider state_pending,
		failure   -> omit item this menu; record failure

	Half of it was built. E_PENDING did produce a provisionally enabled item -
	and rightly: a handler that reports pending has said only that it cannot
	answer cheaply, not that the verb is unavailable, and hiding a working
	command is worse than offering one that turns out to be a no-op. But nothing
	was ever recorded as pending, and *any other* failure fell straight through
	to GetTitle carrying the caller's initialised `ECS_ENABLED`, so a handler
	whose state call failed outright still got an enabled item in the menu and
	was recorded as having succeeded.

	docs/refactor/09-remediation-plan.md finding P.
*/

#include <windows.h>
#include <shobjidl_core.h>

namespace Nilesoft
{
	namespace Shell
	{
		enum class CommandStateVerdict
		{
			// Ask the rest of the metadata questions and show the item.
			Show,

			// A live provider that has nothing to offer for this selection. Very
			// common - about half the handlers on the reference machine return
			// no state or no title for any given selection - and not a fault, so
			// it must not evict the cached COM object.
			Hidden,

			// The handler asked for more time than this path has. The item is
			// offered provisionally enabled and Invoke finds out the truth.
			Pending,

			// The call did not succeed. The item is omitted from this menu and
			// the provider stops being reused, because a call that used to work
			// and now does not is a different thing from a decline.
			Failed,
		};

		struct CommandState
		{
			CommandStateVerdict verdict{ CommandStateVerdict::Failed };

			// Only meaningful for Show and Pending. For Pending it is
			// ECS_ENABLED regardless of what the handler left behind: the
			// out-parameter of a call that returned E_PENDING is not a value the
			// caller may read.
			EXPCMDSTATE state{ ECS_ENABLED };
		};

		inline CommandState classify_command_state(HRESULT hr, EXPCMDSTATE state) noexcept
		{
			CommandState out;

			if(hr == E_PENDING)
			{
				out.verdict = CommandStateVerdict::Pending;
				out.state = ECS_ENABLED;
				return out;
			}

			if(FAILED(hr))
			{
				out.verdict = CommandStateVerdict::Failed;
				out.state = ECS_ENABLED;
				return out;
			}

			// ECS_HIDDEN is checked before anything else in the flags word: a
			// hidden item is not shown whatever else it claims to be.
			if(state & ECS_HIDDEN)
			{
				out.verdict = CommandStateVerdict::Hidden;
				out.state = state;
				return out;
			}

			out.verdict = CommandStateVerdict::Show;
			out.state = state;
			return out;
		}
	}
}
