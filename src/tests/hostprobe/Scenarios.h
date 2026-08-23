#pragma once

#include <string>
#include <vector>

#include "Probe.h"

namespace hostprobe
{
	enum class MenuShape { Flat, WithSubmenu, WithOwnerDraw };

	enum class ScriptKind { SelectSecond, SelectInSubmenu, Cancel, UnmatchedChar };

	// What a question scenario asserts. Matrix scenarios use Record and assert
	// nothing - their output is the baseline, not a verdict.
	enum class Expect
	{
		Record,
		ReturnEquals,
		ReturnDiffers,
		NoCommandMessage,
		MeasureItemArrives,
		MenuCommandPositionEquals,
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
