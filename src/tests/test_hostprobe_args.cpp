#include "test.h"

#include "hostprobe/Arguments.h"

// The trace harness is the only thing in this tree that can see a takeover
// regression, and for three sessions it could be asked to see nothing while
// reporting success. `hostprobe.exe --verify` - the operand forgotten - matched
// `arg == L"--verify" && i + 1 < argc`, failed the second half, fell through
// every branch and landed in the final `else`, which assigned it to `filter`.
// No scenario name contains "--verify", so nothing ran, nothing failed, and the
// process exited 0. Two handoffs recorded 23/31 scenario counts beside commands
// that could not have produced them.
//
// These pin both halves of the fix: the parser refuses the shapes that produce
// an empty run, and the caller refuses an empty run outright.

using namespace hostprobe;

namespace
{
	Arguments parse(std::initializer_list<const wchar_t *> words)
	{
		std::vector<std::wstring> args;
		for(auto w : words)
			args.emplace_back(w);
		return parse_arguments(args);
	}
}

TEST(hostprobe_args, the_canonical_native_gate_command_parses)
{
	auto a = parse({ L"--verify", L"src\\tests\\hostprobe\\fixtures" });
	CHECK(!a.failed);
	CHECK(a.verifying());
	CHECK(a.verify_dir == L"src\\tests\\hostprobe\\fixtures");
	CHECK(a.filter.empty());
	CHECK(!a.takeover);
}

TEST(hostprobe_args, the_canonical_takeover_gate_command_parses)
{
	auto a = parse({ L"--takeover", L"--verify", L"src\\tests\\hostprobe\\fixtures" });
	CHECK(!a.failed);
	CHECK(a.takeover);
	CHECK(a.verify_dir == L"src\\tests\\hostprobe\\fixtures");
}

// The exact defect. Before the fix this produced filter == L"--verify".
TEST(hostprobe_args, verify_without_a_directory_is_a_usage_error)
{
	auto a = parse({ L"--verify" });
	CHECK(a.failed);
	CHECK_EQ(a.exit_code, kUsageExitCode);
	CHECK(a.filter.empty());
	CHECK(!a.verifying());
}

TEST(hostprobe_args, record_without_a_directory_is_a_usage_error)
{
	auto a = parse({ L"--record" });
	CHECK(a.failed);
	CHECK_EQ(a.exit_code, kUsageExitCode);
	CHECK(!a.recording());
}

TEST(hostprobe_args, shell_without_a_path_is_a_usage_error)
{
	auto a = parse({ L"--takeover", L"--shell" });
	CHECK(a.failed);
	CHECK_EQ(a.exit_code, kUsageExitCode);
}

// The same mistake wearing a different hat: this used to verify against a
// directory named "--takeover", which does not exist, so every scenario failed
// with "no baseline" - loud, but for the wrong reason.
TEST(hostprobe_args, an_option_cannot_be_swallowed_as_another_options_operand)
{
	auto a = parse({ L"--verify", L"--takeover" });
	CHECK(a.failed);
	CHECK_EQ(a.exit_code, kUsageExitCode);
	CHECK(!a.takeover);
	CHECK(!a.verifying());
}

TEST(hostprobe_args, an_unknown_option_is_an_error_not_a_filter)
{
	auto a = parse({ L"--verfiy", L"src\\tests\\hostprobe\\fixtures" });
	CHECK(a.failed);
	CHECK_EQ(a.exit_code, kUsageExitCode);
	CHECK(a.filter.empty());
}

// The documented `hostprobe question` workflow keeps working: a plain word is
// still a substring filter, and only a --word is an option.
TEST(hostprobe_args, a_plain_word_is_still_a_substring_filter)
{
	auto a = parse({ L"question" });
	CHECK(!a.failed);
	CHECK(a.filter == L"question");
}

TEST(hostprobe_args, a_filter_combines_with_verification)
{
	auto a = parse({ L"--verify", L"fixtures", L"takeover" });
	CHECK(!a.failed);
	CHECK(a.verify_dir == L"fixtures");
	CHECK(a.filter == L"takeover");
}

TEST(hostprobe_args, shell_without_takeover_keeps_its_own_exit_code)
{
	auto a = parse({ L"--shell", L"x64\\shell.dll" });
	CHECK(a.failed);
	CHECK_EQ(a.exit_code, kShellWithoutTakeoverExitCode);
}

TEST(hostprobe_args, the_first_error_is_the_one_reported)
{
	auto a = parse({ L"--verify", L"--record" });
	CHECK(a.failed);
	CHECK(a.error.find(L"--verify") != std::wstring::npos);
}

// A single dash was never a shape this tool accepted, and a bare "--" is not an
// option name; both stay filters rather than becoming errors, so the parser
// cannot start rejecting a command somebody already has in a script.
TEST(hostprobe_args, only_a_double_dash_word_counts_as_an_option)
{
	CHECK(!argument_is_option(L"--"));
	CHECK(!argument_is_option(L"-v"));
	CHECK(!argument_is_option(L"question"));
	CHECK(argument_is_option(L"--verify"));
}

TEST(hostprobe_args, the_argv_overload_skips_the_program_name)
{
	const wchar_t *argv[] = { L"hostprobe.exe", L"--verify", L"fixtures" };
	auto a = parse_arguments(3, argv);
	CHECK(!a.failed);
	CHECK(a.verify_dir == L"fixtures");
}
