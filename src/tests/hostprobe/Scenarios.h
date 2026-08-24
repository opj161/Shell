#pragma once

#include <string>
#include <vector>

#include "Probe.h"

namespace hostprobe
{
	// Invalid is not a shape so much as the absence of one: it tracks a handle
	// that is not a menu, to find out how a *failed* call differs from a
	// cancelled one.
	//
	// ShellItem is the odd one out and the only shape Shell will take over: the
	// menu is filled by the shell namespace rather than by AppendMenu, which is
	// how a file manager builds one. See ShellMenu.h.
	enum class MenuShape { Flat, WithSubmenu, WithOwnerDraw, Invalid, ShellItem };

	enum class ScriptKind { SelectSecond, SelectInSubmenu, Cancel, UnmatchedChar,
							UnmatchedCharAfterNavigating,
							// For ShellItem, whose item identifiers are not
							// known until the handlers have filled the menu.
							SelectDrivableCommand, CancelWhatever };

	// Whether a scenario needs Shell in the process. Requiring the flag rather
	// than detecting the mode is deliberate: QueryContextMenu loads Shell
	// through COM whether or not anybody asked for it, so a "native" run of a
	// ShellItem scenario would quietly be a takeover run.
	enum class Requires { Any, Takeover };

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

		// Takeover-only. Each is a property rather than a recorded stream,
		// because a shell menu's contents depend on the machine.
		ShellTrackedItsOwnMenu,
		EveryInitPopupHasOneUninit,
		CommandCarriesTheNativeIdentifier,
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

		Requires needs{ Requires::Any };

		// Its trace depends on what this machine has installed, so --record and
		// --verify skip it. The expectation is the whole test.
		bool machine_specific{};
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

		// The scenario could not be set up at all - no shell menu, no drivable
		// item. Reported like a navigation failure, and for the same reason.
		bool setup_failed{};
		std::wstring setup_detail;

		// Which menu the host handed over, and whether anything other than that
		// one was initialised - the difference between Shell tracking the host's
		// menu and Shell tracking one it composed.
		bool tracked_a_different_menu{};

		// Every popup that received a WM_INITMENUPOPUP got exactly one
		// WM_UNINITMENUPOPUP. The pairing commit a634ab6 landed, asserted
		// against a real borrowed menu rather than against a unit fake.
		bool init_uninit_paired{};
		size_t unpaired_popups{};

		// The identifier the host was told to run, and whether the menu it
		// created actually contains it.
		bool command_is_native{};
	};

	const std::vector<Scenario> &scenarios();
	Result run_scenario(const Scenario &scenario);
	std::wstring flag_names(UINT flags);
}
