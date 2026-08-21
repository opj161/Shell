// LoadStringW with a non-zero cchBufferMax truncates; cchBufferMax 0 is the
// documented length query. LoadStringW_full copies that many characters from
// the resource pointer, which is not promised to be null-terminated.
//
//   https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-loadstringw

#include "test.h"

#include <windows.h>
#include "System.h"

using Nilesoft::Text::string;

#define IDS_LONG_STRING 401

TEST(loadstring, a_resource_longer_than_max_path_arrives_whole)
{
	auto module = ::GetModuleHandleW(nullptr);
	CHECK(module != nullptr);

	wchar_t truncated[MAX_PATH]{};
	int n = ::LoadStringW(module, IDS_LONG_STRING, truncated, MAX_PATH);
	CHECK_EQ(n, MAX_PATH - 1);

	auto full = string::LoadStringW_full(module, IDS_LONG_STRING);
	CHECK_EQ((int)full.length(), 300);
	CHECK_EQ((int)::wcslen(full.c_str()), 300);

	for(int i = 0; i < 300; ++i)
		CHECK(full.c_str()[i] == L'A');
}

TEST(loadstring, cch_zero_length_matches_the_copied_string)
{
	auto module = ::GetModuleHandleW(nullptr);
	wchar_t *resource = nullptr;
	int n = ::LoadStringW(module, IDS_LONG_STRING,
		reinterpret_cast<LPWSTR>(&resource), 0);
	CHECK_EQ(n, 300);
	CHECK(resource != nullptr);

	auto full = string::LoadStringW_full(module, IDS_LONG_STRING);
	CHECK_EQ((int)full.length(), n);
}

TEST(loadstring, a_missing_id_is_empty)
{
	auto module = ::GetModuleHandleW(nullptr);
	auto missing = string::LoadStringW_full(module, 0xFFF0);
	CHECK(missing.empty());
}
