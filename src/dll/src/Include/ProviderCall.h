#pragma once

/*
	Where one provider's cost stops being counted, as a rule rather than as an
	ordering that happens to be written correctly today.

	The defect
	----------

	`GetCanonicalName` used to be called *after* `health.record` and
	`Diagnostics::session_provider`, so every provider's remembered cost and
	every reported cost excluded it. The whole-menu `ProviderBudget` charged the
	call either way, which is what made the gap visible rather than free:
	docs/refactor/09-remediation-plan.md section 2.1 reads the difference
	between the 36.6 ms `explorer.commands` phase and the ~35 ms of
	per-provider records as "Shell's own pre-paint work is ~1.1 ms ... the phase
	is honestly attributed". Part of that gap was this call.

	There is no basis for treating it as outside the provider's cost. It is an
	IExplorerCommand method like the ones beside it, answered by the same
	third-party handler, on the same thread, before this provider's item is
	published:

		"None of the methods of this interface should communicate with network
		 resources. These methods are called on the UI thread"
		https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorercommand

	Why it is a function
	--------------------

	Because otherwise the rule is a comment, and a comment cannot fail. The
	ordering is four statements that each look correct in isolation, and the
	defect was produced once already by writing them in the order that reads
	most naturally. Include/ExplorerCommandState.h exists for the same reason
	and in the same shape: the rule separated out so it can be tested without a
	handler, a selection or a menu.

	What it does not do
	-------------------

	It does not sample the *start* of the span. Activation is inside the
	measured cost - `CoCreateInstance` is ~2 ms per provider even fully warm,
	and a provider that is slow to create is slow for the menu - so the caller
	samples before it borrows from the cache, and hands that reading in. Moving
	the first sample in here would silently drop the activation from every
	provider's cost, which is the same defect this file exists to prevent,
	pointing the other way.

	The out-parameter is written only when the call succeeds, which is what the
	method documents: "A pointer to a value that, when this method returns
	successfully, receives the command's GUID".
	https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getcanonicalname
*/

#include <windows.h>
#include <shobjidl_core.h>

#include <cstdint>

namespace Nilesoft
{
	namespace Shell
	{
		struct ProviderCallResult
		{
			// Everything this provider cost the menu, from the caller's
			// `spent_before` reading to after the last call made on it.
			uint32_t cost_us{};

			// Whether the fill produced an item, which is the only case that
			// asks for a canonical name.
			bool shown{};
		};

		/*
			One provider's whole synchronous, pre-publication cost, as one span.

			`spent_us` reads the menu's elapsed budget; `fill` performs the
			GetState/GetTitle/GetIcon sequence and answers whether an item
			resulted. Both are injected so the rule can be exercised against a
			counted fake rather than against a live handler.
		*/
		template<typename Clock, typename Fill>
		inline ProviderCallResult measure_provider_call(uint32_t spent_before,
													    Clock &&spent_us,
													    IExplorerCommand *cmd,
													    Fill &&fill,
													    GUID *canonical)
		{
			ProviderCallResult out;
			out.shown = fill();

			// Inside the span, deliberately. Only on the shown path, because
			// that is the only path that publishes an item and therefore the
			// only one that makes the call at all.
			if(out.shown && cmd && canonical)
			{
				if(FAILED(cmd->GetCanonicalName(canonical)))
					*canonical = GUID_NULL;
			}

			auto after = spent_us();
			out.cost_us = after > spent_before ? after - spent_before : 0u;
			return out;
		}
	}
}
