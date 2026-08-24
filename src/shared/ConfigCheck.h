#pragma once

/*
	`shell.exe -check` - parse a configuration, report, publish nothing.

	docs/refactor/03-config-safety.md section 1b step 4: "the cheapest possible
	prevention and the thing a user editing .nss will actually run". The same
	section records why it was not the XS the plan first called it: shell.exe is
	a single Main.cpp that does not link the parser, and it is a
	Windows-subsystem binary with no console of its own.

	So this is the boundary between the two. shell.dll owns the parser and
	exports one function; shell.exe loads the DLL sitting next to it - not
	whatever copy is registered on the machine, which is the mistake
	AGENTS.md records the installer's custom action making - calls that
	function, and prints what comes back.

	The result is a fixed-size POD rather than a caller-sized buffer on purpose.
	Required-size protocols are the shape this codebase keeps getting wrong (see
	AGENTS.md on `release(n - 1)` and on GetMenuItemInfo), and there is nothing
	here that needs one: a parse produces exactly one error, in one file, at one
	position. `cbSize` at offset 0 lets a newer DLL recognise an older caller's
	struct, which is the same versioning idiom MENUITEMINFO and this codebase's
	own MenuItemInfo::Signed() use.
*/

#include <cstdint>
#include <cstddef>

namespace Nilesoft
{
	namespace Shell
	{
		// Fixed caps. A path longer than this is truncated rather than refused -
		// the point of the report is to name the file, and a truncated name
		// still does that better than no report at all.
		inline constexpr size_t CONFIG_CHECK_PATH = 1024;
		inline constexpr size_t CONFIG_CHECK_MESSAGE = 128;

		struct ConfigCheckResult
		{
			// sizeof(ConfigCheckResult) as the caller compiled it.
			uint32_t cbSize;

			// TokenError as an integer, so the ABI does not depend on a header
			// that lives inside the DLL. Zero is TokenError::None.
			int32_t error;

			// 1-based, as the parser reports them and as an editor expects.
			// Both zero when there is no error.
			uint32_t line;
			uint32_t column;

			// How many files the parse opened, root included, and how many
			// entries it produced. An entry is anything the parser counts
			// towards TotalMenuCount - a menu, an item or a separator - so the
			// word is "entries" rather than "menus", which it is not.
			// Only meaningful on success.
			uint32_t files;
			uint32_t entries;

			// The file the error is in - which is not necessarily the file that
			// was asked about, because an import fails inside itself.
			wchar_t path[CONFIG_CHECK_PATH];

			// ParserException::errortostr for `error`.
			wchar_t message[CONFIG_CHECK_MESSAGE];
		};

		// Exit codes, and the function's return value. These are what a script
		// sees, so they are part of the contract.
		enum ConfigCheckCode : int
		{
			CONFIG_CHECK_OK = 0,		// parsed
			CONFIG_CHECK_FAILED = 1,	// did not parse; the result says where
			CONFIG_CHECK_UNUSABLE = 2,	// bad arguments, or the DLL could not be asked
		};

		inline constexpr char CONFIG_CHECK_EXPORT[] = "ShellCheckConfig";

		// A null or empty path means "the configuration this machine would
		// actually load", resolved exactly as the DLL resolves it.
		using ConfigCheckFn = int(__stdcall *)(const wchar_t *path, ConfigCheckResult *result);

		/*
			One line, in the shape compilers and editors already parse:

				C:\path\shell.nss(12,7): error: String terminated expected
				C:\path\shell.nss: ok - 4 files, 137 entries

			Always null-terminates, never writes past `capacity`, and returns the
			number of characters written excluding the terminator. A zero
			capacity writes nothing and returns 0 rather than indexing into a
			buffer it was told it does not have.
		*/
		inline size_t format_config_check(const ConfigCheckResult &result, int code,
										  wchar_t *out, size_t capacity)
		{
			if(!out || capacity == 0)
				return 0;

			out[0] = L'\0';

			// A path the DLL never filled in still has to produce a readable
			// line rather than an empty one.
			const wchar_t *path = (result.path[0] != L'\0') ? result.path : L"(configuration)";

			int written = 0;
			if(code == CONFIG_CHECK_OK)
			{
				written = _snwprintf_s(out, capacity, _TRUNCATE,
									   L"%s: ok - %u file%s, %u entr%s",
									   path,
									   result.files, result.files == 1 ? L"" : L"s",
									   result.entries, result.entries == 1 ? L"y" : L"ies");
			}
			else
			{
				const wchar_t *message = (result.message[0] != L'\0') ? result.message : L"unknown error";

				// A parse that failed before it reached a position - a file that
				// does not exist, say - has nothing useful to put in the
				// parentheses, and "(0,0)" reads as a location rather than as
				// the absence of one.
				if(result.line == 0 && result.column == 0)
				{
					written = _snwprintf_s(out, capacity, _TRUNCATE,
										   L"%s: error: %s", path, message);
				}
				else
				{
					written = _snwprintf_s(out, capacity, _TRUNCATE,
										   L"%s(%u,%u): error: %s",
										   path, result.line, result.column, message);
				}
			}

			// _TRUNCATE returns -1 when it truncated, having still terminated
			// the buffer. That is a complete answer for a report line, so it is
			// reported as "filled the buffer" rather than as a failure.
			if(written < 0)
				return capacity - 1;

			return static_cast<size_t>(written);
		}
	}
}
