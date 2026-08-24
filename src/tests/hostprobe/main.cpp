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
	gate once a baseline is committed.
*/

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
		}
		return 0;
	}
}

int __cdecl wmain(int argc, wchar_t **argv)
{
	std::wstring filter;
	std::wstring record_dir;
	std::wstring verify_dir;
	std::wstring shell_dll;
	bool takeover = false;

	for(int i = 1; i < argc; i++)
	{
		std::wstring arg = argv[i];
		if(arg == L"--record" && i + 1 < argc)
			record_dir = argv[++i];
		else if(arg == L"--verify" && i + 1 < argc)
			verify_dir = argv[++i];
		else if(arg == L"--shell" && i + 1 < argc)
			shell_dll = argv[++i];
		else if(arg == L"--takeover")
			takeover = true;
		else
			filter = arg;
	}

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
	else if(!shell_dll.empty())
	{
		::wprintf(L"--shell has no meaning without --takeover\n");
		return 123;
	}

	auto &probe = Probe::instance();
	if(!probe.create())
	{
		::wprintf(L"could not create the probe window: %lu\n", ::GetLastError());
		return 125;
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
				failures++;
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

	if(skipped)
		::wprintf(L"\n%d scenario(s), %d failure(s), %d skipped "
				  L"(need --takeover against the registered copy)\n",
				  ran, failures, skipped);
	else
		::wprintf(L"\n%d scenario(s), %d failure(s)\n", ran, failures);
	return failures > 125 ? 125 : failures;
}
