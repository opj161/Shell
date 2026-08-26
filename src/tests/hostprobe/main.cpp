/*
	hostprobe.exe - records what a menu's owner window observes.

		hostprobe.exe                       run every scenario, print traces
		hostprobe.exe <substring>           run the scenarios whose name matches
		hostprobe.exe --record <dir>        write each trace to <dir>\<name>.trace
		hostprobe.exe --verify <dir>        diff each trace against that fixture
		hostprobe.exe --takeover            run them through Shell's hook instead
		hostprobe.exe --shell <dll>         which Shell to load for --takeover

	Without --takeover every trace records what *untouched Windows* does, which
	is the baseline. With it, Shell is loaded into this process and its hook
	intercepts the same calls, so the two sets of traces can be diffed - see
	Takeover.h for why that needs no injection and no deployed build. Takeover
	traces belong in their own fixture directory: the whole point is that they
	differ from the baseline, and the review is of *how*.

	Execution needs an interactive desktop: it creates a real window and tracks
	real menus. It does *not* inject desktop-wide input - keystrokes are posted
	to its own thread queue - so it cannot type into anything else, but a small
	window and a series of popups will appear while it runs. That is why the
	build produces it and does not run it, and why
	docs/refactor/06-phases-and-tests.md puts its execution in the scheduled VM
	job rather than PR CI.

	Exit code is the number of failed expectations, so --verify is usable as a
	gate once a baseline is committed. Codes above that range mean the run never
	got as far as an expectation: 121 nothing ran, 122 malformed command line,
	123 --shell without --takeover, 124 Shell would not load, 125 no window.

	--record, --verify and --shell each require their operand, and a run that
	selects no scenario fails. Both rules exist because the abbreviated form
	`hostprobe.exe --verify` used to become a substring *filter* named
	"--verify", match nothing, and print "0 scenario(s), 0 failure(s)" with exit
	code 0. See Arguments.h.
*/

#include "Arguments.h"
#include "Scenarios.h"
#include "Takeover.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace hostprobe;

namespace
{
	// `found` is separate from emptiness on purpose: a scenario whose owner
	// window observes nothing at all - a call that failed before showing a menu -
	// has an empty trace, and that is the observation rather than a missing
	// baseline.
	std::wstring read_file(const std::wstring &path, bool *found)
	{
		if(found)
			*found = false;

		auto h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
							   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if(h == INVALID_HANDLE_VALUE)
			return std::wstring();

		if(found)
			*found = true;

		std::string bytes;
		char buf[4096];
		DWORD read = 0;
		while(::ReadFile(h, buf, sizeof(buf), &read, nullptr) && read)
			bytes.append(buf, read);
		::CloseHandle(h);

		// The traces are ASCII by construction - message names, decimal numbers
		// and aliases - so a byte-per-character widening is exact and avoids
		// dragging a codepage into a comparison.
		std::wstring out;
		out.reserve(bytes.size());
		for(auto c : bytes)
		{
			if(c != '\r')
				out += static_cast<wchar_t>(static_cast<unsigned char>(c));
		}
		return out;
	}

	bool write_file(const std::wstring &path, const std::wstring &text)
	{
		auto h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
							   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if(h == INVALID_HANDLE_VALUE)
			return false;

		std::string bytes;
		for(auto c : text)
		{
			// .gitattributes checks these out as CRLF like every other text file
			// in the tree, so write them that way rather than leaving a
			// whole-file diff for the next person who touches one.
			if(c == L'\n')
				bytes += '\r';
			bytes += static_cast<char>(c < 128 ? c : '?');
		}

		DWORD written = 0;
		auto ok = ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
		::CloseHandle(h);
		return ok && written == bytes.size();
	}

	// Reports the first line that differs, which is the one worth reading.
	bool diff(const std::wstring &expected, const std::wstring &actual)
	{
		size_t a = 0, b = 0;
		int line = 1;
		while(a < expected.size() || b < actual.size())
		{
			auto ea = expected.find(L'\n', a);
			auto eb = actual.find(L'\n', b);
			auto la = expected.substr(a, ea == std::wstring::npos ? std::wstring::npos : ea - a);
			auto lb = actual.substr(b, eb == std::wstring::npos ? std::wstring::npos : eb - b);
			if(la != lb)
			{
				::wprintf(L"      line %d\n        expected: %s\n        actual:   %s\n",
						  line, la.c_str(), lb.c_str());
				return false;
			}
			if(ea == std::wstring::npos && eb == std::wstring::npos)
				break;
			a = ea == std::wstring::npos ? expected.size() : ea + 1;
			b = eb == std::wstring::npos ? actual.size() : eb + 1;
			line++;
		}
		return true;
	}

	// Whether a rendering scenario is in a position to assert anything at all.
	// Both failures are about the run rather than about Shell, so they are
	// reported in their own words instead of as a property that did not hold.
	bool rendering_was_readable(const Result &r)
	{
		if(!r.render_attempted)
		{
			::wprintf(L"    FAIL the driver never read the menu\n");
			return false;
		}
		if(r.render_popups_seen != 1)
		{
			// AGENTS.md, "Run the harness on a quiet desktop": a second popup
			// belongs to something else, and then none of these readings is
			// about Shell.
			::wprintf(L"    FAIL %zu popup window(s) were visible, so the "
					  L"reading is not about this menu\n", r.render_popups_seen);
			return false;
		}
		return true;
	}

	int check(const Scenario &s, const Result &r)
	{
		switch(s.expectation)
		{
		case Expect::Record:
			return 0;

		case Expect::ReturnEquals:
			if(static_cast<UINT>(r.returned) == s.expected)
				return 0;
			::wprintf(L"    FAIL expected the call to return %u, got %d\n", s.expected, r.returned);
			return 1;

		case Expect::ReturnDiffers:
			if(static_cast<UINT>(r.returned) != s.expected)
				return 0;
			::wprintf(L"    FAIL the call returned %u, which it must not\n", s.expected);
			return 1;

		case Expect::NoCommandMessage:
			if(r.command_ids == 0 && r.menu_commands == 0)
				return 0;
			::wprintf(L"    FAIL the owner was notified as well: %zu WM_COMMAND, "
					  L"%zu WM_MENUCOMMAND\n", r.command_ids, r.menu_commands);
			return 1;

		case Expect::OwnerDrawReachesTheOwner:
			// Both halves, because the NONOTIFY question is whether an
			// owner-drawn menu can be tracked with that flag at all - and it
			// cannot if either the measure or the draw is suppressed.
			if(r.measure_items > 0 && r.draw_items > 0)
				return 0;
			::wprintf(L"    FAIL owner-draw did not reach the owner: %zu "
					  L"WM_MEASUREITEM, %zu WM_DRAWITEM\n",
					  r.measure_items, r.draw_items);
			return 1;

		case Expect::FailedWithLastError:
			// The distinction Shell needs: a cancelled track returns 0 and
			// leaves the last error alone; a failed one returns 0 and sets it.
			if(r.returned == 0 && r.last_error != ERROR_SUCCESS)
				return 0;
			::wprintf(L"    FAIL expected a failed call with a last error, got "
					  L"%d / %lu\n", r.returned, r.last_error);
			return 1;

		case Expect::MenuCommandPositionEquals:
			if(r.menu_commands > 0 && r.command_position == s.expected)
				return 0;
			::wprintf(L"    FAIL expected WM_MENUCOMMAND at position %u, got %zu "
					  L"message(s), position %u\n",
					  s.expected, r.menu_commands, r.command_position);
			return 1;

		case Expect::ShellTrackedItsOwnMenu:
			// A decline is indistinguishable from success by return value, so
			// this is what separates "takeover works" from "takeover was never
			// attempted" - and every other takeover assertion depends on it.
			if(r.tracked_a_different_menu)
				return 0;
			::wprintf(L"    FAIL the only menu the owner heard about was its own: "
					  L"Shell declined, or the breaker is open\n");
			return 1;

		case Expect::EveryInitPopupHasOneUninit:
			if(r.init_uninit_paired)
				return 0;
			::wprintf(L"    FAIL %zu popup(s) received an unbalanced number of "
					  L"INIT/UNINIT notifications (%zu INIT, %zu UNINIT)\n",
					  r.unpaired_popups, r.init_popups, r.uninit_popups);
			return 1;

		case Expect::CommandCarriesTheNativeIdentifier:
			if(r.command_ids == 1 && r.command_is_native)
				return 0;
			::wprintf(L"    FAIL expected one WM_COMMAND carrying an identifier "
					  L"from the host's own menu, got %zu message(s), id %u, "
					  L"%s\n",
					  r.command_ids, r.command_id,
					  r.command_is_native ? L"which is native"
										  : L"which the host's menu does not contain");
			return 1;

		case Expect::MenuCommandNamesTheHostPosition:
			// Three things at once, because any one of them alone is passable
			// by an implementation that has not done the work: exactly one
			// WM_MENUCOMMAND (not zero, which is what a by-position host got
			// before this existed), no WM_COMMAND at all (the message such a
			// host is not listening for), and the position and menu naming
			// where the item actually lives in the host's own menu.
			if(r.menu_commands == 1 && r.command_ids == 0
			   && r.expected_position != 0xFFFFFFFF
			   && r.command_position == r.expected_position
			   && r.command_menu == r.host_expected_menu
			   && !r.expected_title.empty()
			   && r.replayed_title == r.expected_title)
				return 0;
			::wprintf(L"    FAIL expected one WM_MENUCOMMAND at position %u "
					  L"(\"%s\") in menu %p and no WM_COMMAND; got %zu "
					  L"WM_MENUCOMMAND (position %u, \"%s\", menu %p) and %zu "
					  L"WM_COMMAND\n",
					  r.expected_position, r.expected_title.c_str(),
					  (void *)r.host_expected_menu,
					  r.menu_commands, r.command_position,
					  r.replayed_title.c_str(), (void *)r.command_menu,
					  r.command_ids);
			return 1;

		case Expect::EveryComposedItemIsReadable:
			if(!rendering_was_readable(r)) return 1;
			if(r.render_readable) return 0;
			::wprintf(L"    FAIL an item Shell composed is not legible through "
					  L"MSAA: %s\n", r.render_detail.c_str());
			return 1;

		case Expect::ComposedOrderSurvivesToTheScreen:
			if(!rendering_was_readable(r)) return 1;
			if(r.render_order_matches) return 0;
			::wprintf(L"    FAIL what is on screen is not what was composed: %s\n",
					  r.render_detail.c_str());
			return 1;

		case Expect::ThePopupContainsTheItemsItMeasured:
			if(!rendering_was_readable(r)) return 1;
			if(r.render_geometry_ok) return 0;
			::wprintf(L"    FAIL the popup and its items disagree about layout: "
					  L"%s\n", r.render_detail.c_str());
			return 1;

		case Expect::ASubmenuOpensAgainstItsParent:
			if(!rendering_was_readable(r)) return 1;

			// A composed menu only has a submenu if this machine's handlers
			// gave it one. Saying so beats passing silently, which would let
			// the assertion rot into one that never runs.
			if(!r.render_submenu_attempted)
			{
				::wprintf(L"    SKIP the composed menu has no submenu on this "
						  L"machine\n");
				return 0;
			}
			if(!r.render_submenu_opened)
			{
				::wprintf(L"    FAIL the item reporting HASPOPUP never opened a "
						  L"second popup\n");
				return 1;
			}
			if(r.render_submenu_placed) return 0;
			::wprintf(L"    FAIL %s\n", r.render_detail.c_str());
			return 1;
		}
		return 0;
	}
}

namespace
{
	void print_usage()
	{
		::wprintf(L"usage:\n"
				  L"  hostprobe.exe                        run every scenario, print traces\n"
				  L"  hostprobe.exe <substring>            run the scenarios whose name matches\n"
				  L"  hostprobe.exe --record <dir>         write each trace to <dir>\\<name>.trace\n"
				  L"  hostprobe.exe --verify <dir>         diff each trace against that fixture\n"
				  L"  hostprobe.exe --takeover             run them through Shell's hook instead\n"
				  L"  hostprobe.exe --shell <dll>          which Shell to load for --takeover\n"
				  L"\n"
				  L"the two gate commands, both of which need the fixture directory:\n"
				  L"  hostprobe.exe --verify src\\tests\\hostprobe\\fixtures\n"
				  L"  hostprobe.exe --takeover --verify src\\tests\\hostprobe\\fixtures\n");
	}
}

int __cdecl wmain(int argc, wchar_t **argv)
{
	auto args = parse_arguments(argc, argv);
	if(args.failed)
	{
		// Said with the word every other failure uses, so a run whose output is
		// filtered for "FAIL" cannot miss it.
		::wprintf(L"FAIL %s\n\n", args.error.c_str());
		print_usage();
		return args.exit_code;
	}

	const std::wstring &filter = args.filter;
	const std::wstring &record_dir = args.record_dir;
	const std::wstring &verify_dir = args.verify_dir;
	const std::wstring &shell_dll = args.shell_dll;
	const bool takeover = args.takeover;

	// Before the window exists, so nothing this process owns has been shown to
	// a hook that is about to be installed. Shell pins itself once its hooks
	// are in, so this is one-way: a run is either native or takeover.
	bool the_registered_shell = false;
	if(takeover)
	{
		auto load = load_shell(shell_dll);
		::wprintf(L"takeover: %s\n         %s\n", load.path.c_str(), load.detail.c_str());
		if(!load.loaded || !load.bootstrapped)
		{
			::wprintf(L"could not put Shell into this process\n");
			return 124;
		}
		the_registered_shell = load.is_the_registered_copy;
		::wprintf(L"         every trace below is what a host observes through "
				  L"Shell, not through Windows\n");

		if(!the_registered_shell)
		{
			// This was a real false pass before it was caught: two knowingly
			// broken builds were loaded through --shell and every takeover
			// assertion still passed, because the scenarios that matter reach
			// Shell through COM and COM loads the copy named in the registry.
			::wprintf(L"         NOTE this is not the registered copy (%s),\n"
					  L"              so the shell-namespace scenarios would "
					  L"exercise that one instead and are skipped.\n"
					  L"              Deploy the build you want to test.\n",
					  load.registered.empty() ? L"none" : load.registered.c_str());
		}
	}

	auto &probe = Probe::instance();
	if(!probe.create())
	{
		::wprintf(L"could not create the probe window: %lu\n", ::GetLastError());
		return 125;
	}

	// The table itself, before anything runs. Independent of any filter and of
	// which mode this is, so a scenario deleted or accidentally not registered
	// is reported once, plainly, rather than as a smaller-but-still-passing
	// run.
	if(static_cast<int>(scenarios().size()) != kTakeoverScenarios)
	{
		::wprintf(L"FAIL the scenario table holds %d scenario(s), "
				  L"kTakeoverScenarios says %d - update Scenarios.h and the "
				  L"documents that cite it\n",
				  static_cast<int>(scenarios().size()), kTakeoverScenarios);
		return kUnexpectedCardinalityExitCode;
	}

	int failures = 0;
	int ran = 0;
	int skipped = 0;

	for(auto &s : scenarios())
	{
		if(!filter.empty() && s.name.find(filter) == std::wstring::npos)
			continue;

		// Not a failure and not silence: a run that skipped the scenarios which
		// exercise takeover should say so, or "23 scenarios, 0 failures" reads
		// as more coverage than it is.
		if(s.needs == Requires::Takeover && !(takeover && the_registered_shell))
		{
			skipped++;
			continue;
		}
		ran++;

		::wprintf(L"\n[%s]\n", s.name.c_str());
		::wprintf(L"  flags %s%s%s\n", flag_names(s.flags).c_str(),
				  s.use_ex ? L", TrackPopupMenuEx" : L", TrackPopupMenu",
				  s.notify_by_pos ? L", MNS_NOTIFYBYPOS" : L"");
		if(s.why)
			::wprintf(L"  asking: %s\n", s.why);
		::fflush(stdout);

		// Between menus, not before the first: the previous one's teardown is
		// what has to finish, and there is no previous one yet. See
		// Probe::settle for the measurement that put this here.
		if(ran > 1)
			probe.settle(120);

		auto r = run_scenario(s);

		if(r.setup_failed)
		{
			// Same class as a navigation failure: the harness could not put the
			// system into the state the scenario is about, which is a fault
			// here rather than a finding about Shell.
			::wprintf(L"    FAIL could not build the scenario: %s\n",
					  r.setup_detail.c_str());
			failures++;
			continue;
		}

		::wprintf(L"  returned %d (GetLastError %lu)\n", r.returned, r.last_error);
		::wprintf(L"%s", r.trace.c_str());

		// The measurement, for the scenarios that took one. Not a verdict:
		// see Result::render_popup_rect for why a size has to be read by a
		// person rather than asserted against a number this file made up.
		if(r.render_attempted && r.render_popups_seen == 1)
			::wprintf(L"  measured  popup %ldx%ld at (%ld,%ld), %zu item(s)%s\n",
					  r.render_popup_rect.right - r.render_popup_rect.left,
					  r.render_popup_rect.bottom - r.render_popup_rect.top,
					  r.render_popup_rect.left, r.render_popup_rect.top,
					  r.render_items,
					  r.render_submenu_opened ? L", submenu opened" : L"");

		if(r.navigation_failed)
		{
			// A harness fault, not a finding. Reporting it as a failure is the
			// point: a scenario whose selection never happened would otherwise
			// record an empty trace and look like a discovery about Windows.
			::wprintf(L"    FAIL the driver could not reach the scripted item\n");
			failures++;
		}

		failures += check(s, r);

		// A machine-specific trace is printed but never stored or compared: its
		// contents depend on which context-menu handlers are installed, so a
		// fixture would be a record of one desktop rather than of Windows.
		if(!record_dir.empty() && !s.machine_specific)
		{
			auto path = record_dir + L"\\" + s.name + L".trace";
			if(!write_file(path, r.trace))
			{
				::wprintf(L"    FAIL could not write %s\n", path.c_str());
				failures++;
			}
		}

		if(!verify_dir.empty() && !s.machine_specific)
		{
			auto path = verify_dir + L"\\" + s.name + L".trace";
			bool found = false;
			auto expected = read_file(path, &found);
			if(!found)
			{
				::wprintf(L"    FAIL no baseline at %s\n", path.c_str());
				failures++;
			}
			else if(!diff(expected, r.trace))
			{
				// Said in the same words as every other failure, and naming the
				// scenario, because this one is the reason a real transient has
				// escaped capture twice. `diff` prints the offending line and
				// nothing else, so a run that filtered its output for "FAIL"
				// saw a failure *count* with no failure in it - which reads as
				// a harness bug rather than as the thing it is. The scenario
				// name matters too: the `[name]` header can be a screenful
				// above by the time a trace has been printed.
				::wprintf(L"    FAIL %s does not match its recorded baseline\n",
						  s.name.c_str());
				failures++;
			}
		}

		::fflush(stdout);
	}

	probe.destroy();

	if(takeover)
	{
		auto shells = loaded_shells();
		::wprintf(L"\nshell.dll mapped in this process:\n");
		for(auto &s : shells)
			::wprintf(L"  %s\n", s.c_str());
		if(shells.size() > 1)
			::wprintf(L"  (more than one - the shell-namespace scenarios run "
					  L"against whichever COM activated)\n");
	}

	// What the run was measured against, in the summary rather than only in the
	// command that started it: a pasted tail of this output is what gets
	// recorded in a handoff, and "23 scenarios, 0 failures" says nothing about
	// which fixtures - or whether any were consulted at all.
	if(!verify_dir.empty())
		::wprintf(L"\nverified against %s\n", verify_dir.c_str());
	if(!record_dir.empty())
		::wprintf(L"\nrecorded into %s\n", record_dir.c_str());

	if(skipped)
		::wprintf(L"\n%d scenario(s), %d failure(s), %d skipped "
				  L"(need --takeover against the registered copy)\n",
				  ran, failures, skipped);
	else
		::wprintf(L"\n%d scenario(s), %d failure(s)\n", ran, failures);

	// A run that exercised nothing is a fault in the command, not a pass. This
	// is the second half of what let `--verify` with no operand report success:
	// the filter matched no scenario name, and zero of zero failed.
	if(ran == 0)
	{
		if(!filter.empty())
			::wprintf(L"FAIL no scenario name contains \"%s\" - nothing ran\n",
					  filter.c_str());
		else if(skipped)
			::wprintf(L"FAIL every selected scenario was skipped - nothing ran\n");
		else
			::wprintf(L"FAIL no scenarios ran\n");
		return kNothingRanExitCode;
	}

	// An unfiltered run has exactly one right answer, and it is not "more than
	// zero". A filter is a request for a subset, so it is exempt - and a filter
	// that selected nothing is already the case above.
	//
	// The skipped count is asserted too, not just `ran`. Without it, one
	// scenario losing its Requires::Takeover tag would move a scenario from
	// skipped to ran in a native run and the totals would still add up to 32.
	if(filter.empty())
	{
		const bool through_shell = takeover && the_registered_shell;
		const int expected_ran =
			through_shell ? kTakeoverScenarios : kNativeScenarios;
		const int expected_skipped =
			through_shell ? 0 : kSkippedWithoutTakeover;

		if(ran != expected_ran || skipped != expected_skipped)
		{
			::wprintf(L"FAIL expected %d scenario(s) and %d skipped in %s mode, "
					  L"got %d and %d\n",
					  expected_ran, expected_skipped,
					  through_shell ? L"takeover" : L"native", ran, skipped);
			return kUnexpectedCardinalityExitCode;
		}
	}

	return failures > 125 ? 125 : failures;
}
