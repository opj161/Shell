#pragma once

/*
	hostprobe's command line, as pure data.

	This is a header rather than a few lines inside wmain because of what the
	old few lines did when they were wrong. `--verify` and `--record` were
	matched as `arg == L"--verify" && i + 1 < argc`, so an invocation that
	forgot the directory fell through the whole chain to the final `else` and
	became a *substring filter* named "--verify". No scenario contains that
	string, so the run printed

		0 scenario(s), 0 failure(s)

	and exited 0. Two sessions recorded 23/31 scenario counts that had not been
	produced by the commands they were recorded next to. A gate that can
	exercise nothing and still report success gates nothing, so the parser now
	refuses the shapes that produce that outcome, and the caller refuses a run
	that selected no scenario at all.

	Kept free of <windows.h> and of any process state so the unit suite can
	drive it directly - src/tests/test_hostprobe_args.cpp.
*/

#include <string>
#include <vector>

namespace hostprobe
{
	struct Arguments
	{
		std::wstring filter;
		std::wstring record_dir;
		std::wstring verify_dir;
		std::wstring shell_dll;
		bool takeover = false;

		// Set together. `error` is one line, already worded for stdout; the
		// caller prints it, prints usage and exits with `exit_code`.
		bool failed = false;
		std::wstring error;
		int exit_code = 0;

		bool verifying() const { return !verify_dir.empty(); }
		bool recording() const { return !record_dir.empty(); }
	};

	// Distinct from the exit codes wmain already uses for the states it can
	// reach after parsing - 123 --shell without --takeover, 124 Shell would not
	// load, 125 no window - so a caller can tell a malformed command line from
	// a machine that could not run the probe.
	inline constexpr int kUsageExitCode = 122;
	inline constexpr int kShellWithoutTakeoverExitCode = 123;

	// A run that selected nothing. Separate from a failed expectation because
	// it is a fault in the command, not a finding about Windows or Shell.
	inline constexpr int kNothingRanExitCode = 121;

	inline bool argument_is_option(const std::wstring &arg)
	{
		// Two dashes, then at least one more character. A bare "--" and a plain
		// word are both filters; "-x" is not a shape this tool has ever used.
		return arg.size() > 2 && arg[0] == L'-' && arg[1] == L'-';
	}

	inline Arguments parse_arguments(const std::vector<std::wstring> &args)
	{
		Arguments out;

		auto fail = [&out](std::wstring message, int code)
		{
			if(out.failed)
				return;			// first error wins; the rest is noise
			out.failed = true;
			out.error = std::move(message);
			out.exit_code = code;
		};

		for(size_t i = 0; i < args.size(); i++)
		{
			const auto &arg = args[i];

			// Every option that takes a directory or a path is handled by the
			// same helper, so none of them can grow the old fall-through.
			auto take_operand = [&](const wchar_t *name, std::wstring &into) -> bool
			{
				if(i + 1 >= args.size())
				{
					fail(std::wstring(name) + L" needs a directory or path after it",
						 kUsageExitCode);
					return false;
				}
				const auto &operand = args[i + 1];
				if(argument_is_option(operand))
				{
					// `--verify --takeover` is the same mistake wearing a
					// different hat: it would silently verify against a
					// directory called "--takeover".
					fail(std::wstring(name) + L" needs a directory or path, not "
						 + operand, kUsageExitCode);
					return false;
				}
				into = operand;
				i++;
				return true;
			};

			if(arg == L"--record")
			{
				if(!take_operand(L"--record", out.record_dir))
					break;
			}
			else if(arg == L"--verify")
			{
				if(!take_operand(L"--verify", out.verify_dir))
					break;
			}
			else if(arg == L"--shell")
			{
				if(!take_operand(L"--shell", out.shell_dll))
					break;
			}
			else if(arg == L"--takeover")
				out.takeover = true;
			else if(argument_is_option(arg))
			{
				// Was a filter. An unrecognised option is a typo, and a typo
				// that matches no scenario name used to be a clean run.
				fail(L"unknown option " + arg, kUsageExitCode);
				break;
			}
			else
				out.filter = arg;
		}

		if(!out.failed && !out.shell_dll.empty() && !out.takeover)
			fail(L"--shell has no meaning without --takeover",
				 kShellWithoutTakeoverExitCode);

		return out;
	}

	inline Arguments parse_arguments(int argc, const wchar_t *const *argv)
	{
		std::vector<std::wstring> args;
		for(int i = 1; i < argc; i++)
			args.emplace_back(argv[i]);
		return parse_arguments(args);
	}
}
