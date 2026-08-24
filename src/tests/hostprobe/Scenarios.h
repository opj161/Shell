#pragma once

#include <string>
#include <vector>

#include "Probe.h"

namespace hostprobe
{
	// Invalid is not a shape so much as the absence of one: it tracks a handle
	// that is not a menu, to find out how a *failed* call differs from a
	// cancelled one.
	enum class MenuShape { Flat, WithSubmenu, WithOwnerDraw, Invalid };

	enum class ScriptKind { SelectSecond, SelectInSubmenu, Cancel, UnmatchedChar,
							UnmatchedCharAfterNavigating };

	// What a question scenario asserts. Matrix scenarios use Record and assert
	// nothing - their output is the baseline, not a verdict.
	enum class Expect
	{
		Record,
		ReturnEquals,
		ReturnDiffers,
		NoCommandMessage,
		OwnerDrawReachesTheOwner,
		MenuCommandPositionEquals,
		FailedWithLastError,
	};

	struct Scenario
	{
		std::wstring name;
		UINT flags{ TPM_LEFTALIGN | TPM_RIGHTBUTTON };
		bool use_ex{};
		bool notify_by_pos{};
		MenuShape shape{ MenuShape::Flat };
		ScriptKind script{ ScriptKind::SelectSecond };

		bool handle_menuchar{};
		UINT menuchar_action{ MNC_IGNORE };
		UINT menuchar_operand{};

		Expect expectation{ Expect::Record };
		UINT expected{};
		const wchar_t *why{};
	};

	struct Result
	{
		std::wstring name;
		std::wstring trace;
		int returned{};
		DWORD last_error{};

		size_t command_ids{};
		size_t menu_commands{};
		size_t measure_items{};
		size_t draw_items{};
		size_t menu_selects{};
		size_t init_popups{};
		size_t uninit_popups{};

		UINT command_id{};
		UINT command_position{};

		// The driver could not put the highlight where the script asked. Always
		// a harness fault, never a finding about Windows, so it is reported
		// separately rather than folded into the scenario's own verdict.
		bool navigation_failed{};
	};

	const std::vector<Scenario> &scenarios();
	Result run_scenario(const Scenario &scenario);
	std::wstring flag_names(UINT flags);
}
