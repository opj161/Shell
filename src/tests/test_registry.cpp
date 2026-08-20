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
#include <string>
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

// The write side of the same contract. Registry::SetKeyValue used to write
// REG_SZ / REG_EXPAND_SZ without a terminator (length * sizeof(wchar_t), where
// length excludes the null), which is exactly the shape RegQueryValueEx warns
// other readers cannot rely on. reg.set() in scripts and the ContextMenuHandler
// registration both went through it, so their values arrived unterminated.
// The bytes in the hive must now include the terminator, like RegistryKey::
// SetString always did.
TEST(registry, values_written_through_set_key_value_are_terminated)
{
	ScopedTestKey key;
	CHECK(Registry::SetKeyValue(HKEY_CURRENT_USER, TESTKEY, L"term",
								L"written", false));
	CHECK(Registry::SetKeyValue(HKEY_CURRENT_USER, TESTKEY, L"expterm",
								L"%SystemRoot%", true));

	wchar_t raw[64]{};
	DWORD cb = sizeof(raw);
	DWORD type = 0;
	CHECK(ERROR_SUCCESS == ::RegQueryValueExW(key.handle, L"term", nullptr,
											  &type,
											  reinterpret_cast<LPBYTE>(raw), &cb));
	CHECK_EQ((int)type, (int)REG_SZ);
	// Size includes the terminator the writer must have stored.
	CHECK_MSG(cb == (::wcslen(L"written") + 1) * sizeof(wchar_t),
			  "the stored size must include the terminating null");
	CHECK(::wcscmp(raw, L"written") == 0);

	cb = sizeof(raw);
	CHECK(ERROR_SUCCESS == ::RegQueryValueExW(key.handle, L"expterm", nullptr,
											  &type,
											  reinterpret_cast<LPBYTE>(raw), &cb));
	CHECK_EQ((int)type, (int)REG_EXPAND_SZ);
	CHECK_MSG(cb == (::wcslen(L"%SystemRoot%") + 1) * sizeof(wchar_t),
			  "an expandable value must keep its terminator too");
	CHECK(::wcscmp(raw, L"%SystemRoot%") == 0);
}

// The write side of the *empty* string, which the terminator fix above missed.
// Both writers guarded on `value && *value && length > 0`, so L"" fell through
// with cbData 0 - a REG_SZ containing no terminator at all. RegSetValueEx
// separates the two cases explicitly:
//
//     For string-based types, such as REG_SZ, the string must be
//     null-terminated. [...] cbData must include the size of the terminating
//     null character or characters.
//
//     lpData indicating a null value is valid, however, if this is the case,
//     cbData must be set to '0'.
//
//   https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regsetvalueexw
//
// An empty string is one terminator. A null pointer is zero bytes. They are
// different values. These read the raw bytes back rather than going through
// this project's readers, which were deliberately hardened to forgive exactly
// the shape being tested for.
namespace
{
	// Returns the stored byte count, or -1 if the value is missing.
	int stored_bytes(HKEY key, const wchar_t *name, DWORD *type = nullptr)
	{
		DWORD cb = 0;
		DWORD t = 0;
		if(::RegQueryValueExW(key, name, nullptr, &t, nullptr, &cb) != ERROR_SUCCESS)
			return -1;
		if(type)
			*type = t;
		return static_cast<int>(cb);
	}
}

TEST(registry, an_empty_string_is_written_as_one_terminator)
{
	ScopedTestKey key;
	auto k = Registry::CurrentUser.OpenSubKey(TESTKEY, false, true);
	CHECK(k);

	CHECK(k.SetString(L"empty_sz", L"", false));

	DWORD type = 0;
	CHECK_MSG(stored_bytes(key.handle, L"empty_sz", &type) == (int)sizeof(wchar_t),
			  "an empty REG_SZ is two bytes: the terminator");
	CHECK_EQ((int)type, (int)REG_SZ);

	wchar_t raw[4]{ L'#', L'#', L'#', L'#' };
	DWORD cb = sizeof(raw);
	CHECK(ERROR_SUCCESS == ::RegQueryValueExW(key.handle, L"empty_sz", nullptr, nullptr,
											  reinterpret_cast<LPBYTE>(raw), &cb));
	CHECK_EQ((int)raw[0], 0);
}

TEST(registry, an_empty_expandable_string_is_written_as_one_terminator)
{
	ScopedTestKey key;
	auto k = Registry::CurrentUser.OpenSubKey(TESTKEY, false, true);
	CHECK(k);

	CHECK(k.SetString(L"empty_exp", L"", true));

	DWORD type = 0;
	CHECK_EQ(stored_bytes(key.handle, L"empty_exp", &type), (int)sizeof(wchar_t));
	CHECK_EQ((int)type, (int)REG_EXPAND_SZ);
}

// reg.set(key, name, "", reg.sz) in a script reaches the hive through this one.
TEST(registry, an_empty_string_through_set_key_value_is_terminated)
{
	ScopedTestKey key;
	CHECK(Registry::SetKeyValue(HKEY_CURRENT_USER, TESTKEY, L"empty_skv", L"", false));

	DWORD type = 0;
	CHECK_EQ(stored_bytes(key.handle, L"empty_skv", &type), (int)sizeof(wchar_t));
	CHECK_EQ((int)type, (int)REG_SZ);
}

// The other half of the documented pair: a null pointer really is zero bytes,
// and must not acquire a terminator it has no buffer for.
TEST(registry, a_null_string_is_written_as_no_data)
{
	ScopedTestKey key;
	auto k = Registry::CurrentUser.OpenSubKey(TESTKEY, false, true);
	CHECK(k);

	CHECK(k.SetString(L"null_sz", nullptr, false));
	CHECK_EQ(stored_bytes(key.handle, L"null_sz"), 0);
}

// An empty value must still read back as empty afterwards - the point of the
// fix is the bytes in the hive, not a change in what callers see.
TEST(registry, an_empty_written_value_reads_back_empty)
{
	ScopedTestKey key;
	auto k = Registry::CurrentUser.OpenSubKey(TESTKEY, false, true);
	CHECK(k);
	CHECK(k.SetString(L"round_trip", L"", false));

	auto r = Registry::CurrentUser.OpenSubKey(TESTKEY, false, false);
	CHECK(r.ReadString(L"round_trip").empty());
	CHECK_EQ((int)r.ReadString(L"round_trip").length(), 0);
}

// cbData is a DWORD. A length that cannot fit must be refused rather than
// narrowed into some other, shorter string - no allocation needed to test it,
// because the guard runs before the pointer is touched.
TEST(registry, a_length_too_large_for_cbdata_is_refused)
{
	ScopedTestKey key;
	auto k = Registry::CurrentUser.OpenSubKey(TESTKEY, false, true);
	CHECK(k);

	CHECK(!k.SetString(L"overflow", L"x", SIZE_MAX, false));
	CHECK_MSG(stored_bytes(key.handle, L"overflow") == -1,
			  "a refused write must not leave a value behind");

	CHECK(!Registry::SetKeyValue(HKEY_CURRENT_USER, TESTKEY, L"overflow2",
								 L"x", SIZE_MAX, false));
	CHECK_EQ(stored_bytes(key.handle, L"overflow2"), -1);
}

// Enumeration, with names longer than the buffer that used to be hard-coded.
//
// reg.keys and reg.values allocated 260 characters and looped while the result
// was ERROR_SUCCESS. A value name longer than that answers ERROR_MORE_DATA,
// which is neither success nor ERROR_NO_MORE_ITEMS, so the loop stopped there -
// hiding that name and every name after it, with nothing reported. Value names
// go up to 16,383 characters:
//
//   https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-element-size-limits
TEST(registry, enumeration_survives_a_name_longer_than_the_old_buffer)
{
	ScopedTestKey key;
	CHECK(key.handle != nullptr);

	// Deliberately either side of the old 260-character buffer, and written in
	// an order that puts the long one first so a truncating enumerator loses
	// the short ones too.
	std::wstring long_name(400, L'L');
	std::wstring longer_name(1200, L'M');

	CHECK(ERROR_SUCCESS == ::RegSetValueExW(key.handle, long_name.c_str(), 0, REG_SZ,
											reinterpret_cast<const BYTE *>(L"1"), sizeof(wchar_t) * 2));
	CHECK(ERROR_SUCCESS == ::RegSetValueExW(key.handle, longer_name.c_str(), 0, REG_SZ,
											reinterpret_cast<const BYTE *>(L"2"), sizeof(wchar_t) * 2));
	CHECK(ERROR_SUCCESS == ::RegSetValueExW(key.handle, L"short", 0, REG_SZ,
											reinterpret_cast<const BYTE *>(L"3"), sizeof(wchar_t) * 2));

	auto names = Registry::EnumNames(key.handle, false);
	CHECK_MSG(names.size() == 3, "every value must be listed, whatever its name is");

	bool saw_long = false, saw_longer = false, saw_short = false;
	for(auto &n : names)
	{
		if(n.length() == 400) saw_long = true;
		else if(n.length() == 1200) saw_longer = true;
		else if(same(n, L"short")) saw_short = true;
	}
	CHECK_MSG(saw_long, "a 400-character value name");
	CHECK_MSG(saw_longer, "a 1200-character value name");
	CHECK_MSG(saw_short, "and the ordinary one that used to be lost with them");
}

TEST(registry, subkey_enumeration_lists_them_all)
{
	ScopedTestKey key;

	for(int i = 0; i < 5; i++)
	{
		HKEY sub = nullptr;
		std::wstring name = L"sub" + std::to_wstring(i);
		if(ERROR_SUCCESS == ::RegCreateKeyExW(key.handle, name.c_str(), 0, nullptr, 0,
											  KEY_WRITE, nullptr, &sub, nullptr))
			::RegCloseKey(sub);
	}

	auto names = Registry::EnumNames(key.handle, true);
	CHECK_EQ((int)names.size(), 5);

	for(int i = 0; i < 5; i++)
		::RegDeleteKeyExW(key.handle, (L"sub" + std::to_wstring(i)).c_str(), KEY_WOW64_64KEY, 0);
}

TEST(registry, enumerating_an_empty_key_is_an_empty_list)
{
	ScopedTestKey key;
	CHECK_EQ((int)Registry::EnumNames(key.handle, true).size(), 0);
	CHECK_EQ((int)Registry::EnumNames(key.handle, false).size(), 0);
	CHECK_EQ((int)Registry::EnumNames(nullptr, false).size(), 0);
}
