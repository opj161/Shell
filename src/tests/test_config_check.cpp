// The report line `shell.exe -check` prints.
//
// The parse itself is already covered by test_parser and test_parser_imports;
// what is new here is the boundary between shell.dll and shell.exe - a
// fixed-size POD and one formatting function - and the formatting is the half
// that can go wrong silently. A truncated line, an off-by-one at the end of the
// buffer, or "(0,0)" printed as though it were a real location all produce
// something that looks like a report and misleads whoever reads it.
//
// docs/refactor/03-config-safety.md section 1b step 4.

#include "test.h"

#include "..\shared\ConfigCheck.h"

#include <string>

using namespace Nilesoft::Shell;

namespace
{
	// Fills a result the way Initializer::check does, so the tests read like
	// the thing they describe.
	ConfigCheckResult ok_result(const wchar_t *path, uint32_t files, uint32_t entries)
	{
		ConfigCheckResult r{};
		r.cbSize = sizeof(r);
		r.files = files;
		r.entries = entries;
		if(path)
			::wcscpy_s(r.path, path);
		return r;
	}

	ConfigCheckResult error_result(const wchar_t *path, uint32_t line, uint32_t column,
								   const wchar_t *message)
	{
		ConfigCheckResult r{};
		r.cbSize = sizeof(r);
		r.error = 42;
		r.line = line;
		r.column = column;
		if(path)
			::wcscpy_s(r.path, path);
		if(message)
			::wcscpy_s(r.message, message);
		return r;
	}

	std::wstring rendered(const ConfigCheckResult &r, int code, size_t capacity = 512)
	{
		std::wstring buffer(capacity, L'\0');
		auto written = format_config_check(r, code, buffer.data(), capacity);
		buffer.resize(::wcslen(buffer.c_str()));
		CHECK_EQ(written, buffer.size());
		return buffer;
	}
}

TEST(config_check, a_clean_parse_names_the_file_and_what_it_found)
{
	auto r = ok_result(L"C:\\Program Files\\Nilesoft Shell\\shell.nss", 4, 137);
	CHECK(rendered(r, CONFIG_CHECK_OK) ==
		  L"C:\\Program Files\\Nilesoft Shell\\shell.nss: ok - 4 files, 137 entries");
}

// "1 files" reads as a bug in the tool rather than as a count.
TEST(config_check, a_single_file_and_a_single_entry_are_not_plural)
{
	auto r = ok_result(L"shell.nss", 1, 1);
	CHECK(rendered(r, CONFIG_CHECK_OK) == L"shell.nss: ok - 1 file, 1 entry");
}

TEST(config_check, zero_entries_is_still_plural)
{
	auto r = ok_result(L"shell.nss", 1, 0);
	CHECK(rendered(r, CONFIG_CHECK_OK) == L"shell.nss: ok - 1 file, 0 entries");
}

// file(line,column): error: message - the shape a compiler emits, so an editor
// that already knows how to jump to a build error knows how to jump to this.
TEST(config_check, an_error_is_reported_where_an_editor_can_find_it)
{
	auto r = error_result(L"C:\\config\\shell.nss", 12, 7, L"String terminated expected");
	CHECK(rendered(r, CONFIG_CHECK_FAILED) ==
		  L"C:\\config\\shell.nss(12,7): error: String terminated expected");
}

// A file that does not exist fails before the parser has a position. Printing
// "(0,0)" would claim a location that does not exist.
TEST(config_check, an_error_with_no_position_omits_the_parentheses)
{
	auto r = error_result(L"C:\\config\\missing.nss", 0, 0, L"Cannot found config file");
	CHECK(rendered(r, CONFIG_CHECK_FAILED) ==
		  L"C:\\config\\missing.nss: error: Cannot found config file");
}

// Column 0 on a real line is a position, not the absence of one.
TEST(config_check, a_line_with_no_column_still_reports_its_position)
{
	auto r = error_result(L"shell.nss", 9, 0, L"Syntax");
	CHECK(rendered(r, CONFIG_CHECK_FAILED) == L"shell.nss(9,0): error: Syntax");
}

TEST(config_check, a_result_with_no_path_still_produces_a_readable_line)
{
	auto r = error_result(nullptr, 3, 4, L"Syntax");
	CHECK(rendered(r, CONFIG_CHECK_FAILED) == L"(configuration)(3,4): error: Syntax");
}

TEST(config_check, a_result_with_no_message_says_so_rather_than_trailing_off)
{
	auto r = error_result(L"shell.nss", 3, 4, nullptr);
	CHECK(rendered(r, CONFIG_CHECK_FAILED) == L"shell.nss(3,4): error: unknown error");
}

// The one that matters for safety: a buffer too small must be filled,
// terminated, and reported as full - never written past.
TEST(config_check, a_short_buffer_truncates_and_still_terminates)
{
	auto r = ok_result(L"C:\\a\\very\\long\\path\\to\\shell.nss", 4, 137);

	constexpr size_t CAPACITY = 12;
	wchar_t buffer[CAPACITY + 4];
	for(auto &c : buffer)
		c = L'\xFFFF';

	auto written = format_config_check(r, CONFIG_CHECK_OK, buffer, CAPACITY);

	CHECK_EQ(written, CAPACITY - 1);
	CHECK_EQ(buffer[CAPACITY - 1], L'\0');
	CHECK_EQ(::wcslen(buffer), CAPACITY - 1);

	// Nothing beyond the capacity it was given was touched.
	for(size_t i = CAPACITY; i < CAPACITY + 4; i++)
		CHECK_EQ(buffer[i], L'\xFFFF');
}

TEST(config_check, a_capacity_of_zero_writes_nothing_at_all)
{
	auto r = ok_result(L"shell.nss", 1, 1);

	wchar_t buffer[2] = { L'\xFFFF', L'\xFFFF' };
	auto written = format_config_check(r, CONFIG_CHECK_OK, buffer, 0);

	CHECK_EQ(written, 0u);
	CHECK_EQ(buffer[0], L'\xFFFF');
}

TEST(config_check, a_null_buffer_is_refused_rather_than_dereferenced)
{
	auto r = ok_result(L"shell.nss", 1, 1);
	CHECK_EQ(format_config_check(r, CONFIG_CHECK_OK, nullptr, 128), 0u);
}

// The exit codes are the contract a script sees, so they are pinned too.
TEST(config_check, the_exit_codes_are_what_a_script_expects)
{
	CHECK_EQ(static_cast<int>(CONFIG_CHECK_OK), 0);
	CHECK_EQ(static_cast<int>(CONFIG_CHECK_FAILED), 1);
	CHECK_EQ(static_cast<int>(CONFIG_CHECK_UNUSABLE), 2);
}

// cbSize is how a newer shell.dll recognises an older caller's struct. If the
// layout is changed without thinking, this is what says so.
TEST(config_check, the_struct_starts_with_its_own_size)
{
	ConfigCheckResult r{};
	r.cbSize = sizeof(r);

	CHECK_EQ(offsetof(ConfigCheckResult, cbSize), 0u);
	CHECK_EQ(*reinterpret_cast<const uint32_t *>(&r), sizeof(ConfigCheckResult));
}
