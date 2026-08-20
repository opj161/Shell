// Reading string values out of the registry.
//
// RegQueryValueEx carries a warning that the tree used to ignore:
//
//   "If the value being queried is a string (REG_SZ, REG_MULTI_SZ, and
//   REG_EXPAND_SZ) the value returned is NOT guaranteed to be null-terminated.
//   ... even if the function returns ERROR_SUCCESS, the application should ensure
//   that the string is properly terminated before using it"
//
//   https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regqueryvalueexw
//
// Taking the length as datasize/2 - 1 regardless gave two defects:
//
//   * an empty REG_SZ underflowed to SIZE_MAX characters, which string::terminate
//     clamped to the capacity - the value came back as a run of NULs whose
//     length() was 15 and whose first character was '\0', so empty() was false;
//   * a value stored without its terminator lost its last character. Not
//     hypothetical: see the comment in exe/src/Main.cpp about the TreatAs
//     redirect that Unregister could never recognise.
//
// Separately, REG_EXPAND_SZ values were read and then expanded into a temporary
// that was thrown away, so callers got the raw %VARIABLES% back.
//
// These write real values through the raw API - including the malformed shapes,
// which no wrapper would let us produce - and read them back through the real
// RegistryKey.

#include "test.h"

#include <windows.h>
#include "System.h"

using namespace Nilesoft;
using Nilesoft::Text::string;

namespace
{
	constexpr auto TESTKEY = LR"(Software\Nilesoft\ShellSelfTest)";

	// string has several operator== overloads that a bare literal cannot pick
	// between, so comparisons go through this.
	bool same(const string &a, const wchar_t *b)
	{
		return ::wcscmp(a.c_str(), b ? b : L"") == 0;
	}

	struct ScopedTestKey
	{
		HKEY handle = nullptr;

		ScopedTestKey()
		{
			::RegCreateKeyExW(HKEY_CURRENT_USER, TESTKEY, 0, nullptr, 0,
							  KEY_READ | KEY_WRITE, nullptr, &handle, nullptr);
		}

		~ScopedTestKey()
		{
			if(handle)
				::RegCloseKey(handle);
			::RegDeleteKeyExW(HKEY_CURRENT_USER, TESTKEY, KEY_WOW64_64KEY, 0);
		}

		// Writes exactly the bytes given - no terminator is added.
		void write_raw(const wchar_t *name, DWORD type, const wchar_t *text, size_t chars) const
		{
			::RegSetValueExW(handle, name, 0, type,
							 reinterpret_cast<const BYTE *>(text),
							 static_cast<DWORD>(chars * sizeof(wchar_t)));
		}

		void write(const wchar_t *name, DWORD type, const wchar_t *text) const
		{
			write_raw(name, type, text, ::wcslen(text) + 1);
		}

		RegistryKey open() const
		{
			return Registry::CurrentUser.OpenSubKey(TESTKEY, false, false);
		}
	};
}

TEST(registry, a_normal_string_reads_back_exactly)
{
	ScopedTestKey key;
	CHECK(key.handle != nullptr);
	key.write(L"plain", REG_SZ, L"Nilesoft Shell");

	auto k = key.open();
	CHECK(k);

	auto direct = k.ReadString(L"plain");
	CHECK(same(direct, L"Nilesoft Shell"));
	CHECK_EQ((int)direct.length(), 14);

	string into;
	CHECK(k.ReadString(L"plain", into));
	CHECK(same(into, L"Nilesoft Shell"));
	CHECK_EQ((int)into.length(), 14);

	CHECK(same(k.GetString(L"plain"), L"Nilesoft Shell"));
}

// The underflow. An empty value is a perfectly ordinary thing to find in the
// registry - a cleared default value, for one.
TEST(registry, an_empty_value_is_empty_rather_than_a_run_of_nuls)
{
	ScopedTestKey key;
	key.write_raw(L"empty", REG_SZ, L"", 0);   // zero bytes, not even a terminator

	auto k = key.open();
	CHECK(k);

	auto direct = k.ReadString(L"empty");
	CHECK_MSG(direct.length() == 0, "length must be 0, not the buffer capacity");
	CHECK(direct.empty());

	string into;
	CHECK(k.ReadString(L"empty", into));
	CHECK_EQ((int)into.length(), 0);
	CHECK(into.empty());
}

// A value written with a terminator and one written without must read back the
// same. This is the shape that broke the TreatAs ownership check.
TEST(registry, a_value_stored_without_its_terminator_keeps_its_last_character)
{
	ScopedTestKey key;
	const wchar_t *clsid = L"{BAE3934B-3F8B-4E3F-9F4E-000000000000}";
	auto chars = ::wcslen(clsid);

	key.write_raw(L"terminated", REG_SZ, clsid, chars + 1);
	key.write_raw(L"unterminated", REG_SZ, clsid, chars);

	auto k = key.open();
	CHECK(k);

	auto with = k.ReadString(L"terminated");
	auto without = k.ReadString(L"unterminated");

	CHECK_EQ((int)with.length(), (int)chars);
	CHECK_MSG((int)without.length() == (int)chars,
			  "the last character used to be dropped as an assumed terminator");
	CHECK(same(with, without.c_str()));
	CHECK(same(without, clsid));
}

// A single character with no terminator is the smallest form of the same thing,
// and the one most likely to fall off the end of an off-by-one.
TEST(registry, a_one_character_unterminated_value_reads_back_whole)
{
	ScopedTestKey key;
	key.write_raw(L"single", REG_SZ, L"x", 1);

	auto k = key.open();
	CHECK(k);

	auto got = k.ReadString(L"single");
	CHECK_EQ((int)got.length(), 1);
	CHECK(same(got, L"x"));
}

TEST(registry, expandable_values_come_back_expanded)
{
	ScopedTestKey key;
	key.write(L"expandable", REG_EXPAND_SZ, L"%SystemRoot%\\explorer.exe");

	wchar_t expected[MAX_PATH]{};
	auto n = ::ExpandEnvironmentStringsW(L"%SystemRoot%\\explorer.exe", expected, MAX_PATH);
	CHECK(n > 1);

	auto k = key.open();
	CHECK(k);

	auto direct = k.ReadString(L"expandable");
	CHECK_MSG(same(direct, expected), "ReadString already expanded, and still must");

	// This overload computed the expansion and threw it away.
	string into;
	CHECK(k.ReadString(L"expandable", into));
	CHECK_MSG(same(into, expected), "the %VARIABLES% used to survive into the caller");

	CHECK(same(k.GetString(L"expandable"), expected));
}

// The config override path (HKCU\...\Shell\config -> Parser::load) reads through
// this overload, so an expandable path there has to resolve to a real file name.
TEST(registry, an_expandable_value_without_variables_is_unchanged)
{
	ScopedTestKey key;
	key.write(L"literal", REG_EXPAND_SZ, L"D:\\configs\\shell.nss");

	auto k = key.open();
	string into;
	CHECK(k.ReadString(L"literal", into));
	CHECK(same(into, L"D:\\configs\\shell.nss"));
}

TEST(registry, a_value_that_is_not_a_string_is_refused)
{
	ScopedTestKey key;
	DWORD number = 42;
	::RegSetValueExW(key.handle, L"number", 0, REG_DWORD,
					 reinterpret_cast<const BYTE *>(&number), sizeof(number));

	auto k = key.open();
	CHECK(k);

	string into;
	CHECK(!k.ReadString(L"number", into));
	CHECK(into.empty());
	CHECK(k.ReadString(L"number").empty());
	CHECK(k.GetString(L"number").empty());
}

TEST(registry, a_missing_value_reads_as_nothing)
{
	ScopedTestKey key;
	auto k = key.open();
	CHECK(k);

	string into;
	CHECK(!k.ReadString(L"absent", into));
	CHECK(into.empty());
	CHECK(k.ReadString(L"absent").empty());
}

// The fixed-buffer overload, which RegisterContextMenuHandler uses to read a
// ProgID out of HKCR\.ext. Same terminator rules.
TEST(registry, the_fixed_buffer_overload_terminates_what_it_returns)
{
	ScopedTestKey key;
	const wchar_t *progid = L"Nilesoft.Shell.Test";
	key.write_raw(L"progid", REG_SZ, progid, ::wcslen(progid));  // no terminator

	auto k = key.open();
	CHECK(k);

	wchar_t buffer[64];
	for(auto &c : buffer) c = L'#';

	auto len = k.GetString(L"progid", static_cast<wchar_t *>(buffer), 64u);
	CHECK_EQ((int)len, (int)::wcslen(progid));
	CHECK_MSG(len < 64 && buffer[len] == L'\0',
			  "the caller's buffer must come back terminated");

	// The buffer was pre-filled with a non-terminator, so an implementation that
	// forgets to terminate leaves no NUL anywhere in it. Put one at the end
	// before comparing - the assertion above is what reports that defect, and
	// running off the end of the array is not a useful way to fail.
	buffer[63] = L'\0';
	CHECK(::wcscmp(buffer, progid) == 0);
}

TEST(registry, the_fixed_buffer_overload_refuses_a_buffer_it_cannot_use)
{
	ScopedTestKey key;
	key.write(L"plain", REG_SZ, L"something");

	auto k = key.open();
	CHECK_EQ((int)k.GetString(L"plain", static_cast<wchar_t *>(nullptr), 64u), 0);

	wchar_t one[1]{ L'#' };
	CHECK_EQ((int)k.GetString(L"plain", static_cast<wchar_t *>(one), 0u), 0);
}
