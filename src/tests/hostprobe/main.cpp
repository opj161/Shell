/*
	hostprobe.exe - records what a menu's owner window observes.

		hostprobe.exe                       run every scenario, print traces
		hostprobe.exe <substring>           run the scenarios whose name matches
		hostprobe.exe --record <dir>        write each trace to <dir>\<name>.trace
		hostprobe.exe --verify <dir>        diff each trace against that fixture

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
		}
		return 0;
	}
}

int __cdecl wmain(int argc, wchar_t **argv)
{
	std::wstring filter;
	std::wstring record_dir;
	std::wstring verify_dir;

	for(int i = 1; i < argc; i++)
	{
		std::wstring arg = argv[i];
		if(arg == L"--record" && i + 1 < argc)
			record_dir = argv[++i];
		else if(arg == L"--verify" && i + 1 < argc)
			verify_dir = argv[++i];
		else
			filter = arg;
	}

	auto &probe = Probe::instance();
	if(!probe.create())
	{
		::wprintf(L"could not create the probe window: %lu\n", ::GetLastError());
		return 125;
	}

	int failures = 0;
	int ran = 0;

	for(auto &s : scenarios())
	{
		if(!filter.empty() && s.name.find(filter) == std::wstring::npos)
			continue;
		ran++;

		::wprintf(L"\n[%s]\n", s.name.c_str());
		::wprintf(L"  flags %s%s%s\n", flag_names(s.flags).c_str(),
				  s.use_ex ? L", TrackPopupMenuEx" : L", TrackPopupMenu",
				  s.notify_by_pos ? L", MNS_NOTIFYBYPOS" : L"");
		if(s.why)
			::wprintf(L"  asking: %s\n", s.why);
		::fflush(stdout);

		auto r = run_scenario(s);

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

		if(!record_dir.empty())
		{
			auto path = record_dir + L"\\" + s.name + L".trace";
			if(!write_file(path, r.trace))
			{
				::wprintf(L"    FAIL could not write %s\n", path.c_str());
				failures++;
			}
		}

		if(!verify_dir.empty())
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

	::wprintf(L"\n%d scenario(s), %d failure(s)\n", ran, failures);
	return failures > 125 ? 125 : failures;
}
