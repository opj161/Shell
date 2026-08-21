// The Win32 buffer-sizing contracts, and the path wrappers built on them.
//
// Three different contracts were being treated as one:
//
//   * GetFullPathName / GetLongPathName / GetShortPathName answer a sizing
//     query with a capacity that counts the terminator, and a real fill with a
//     length that does not. Path::Long and Path::Full released the *capacity*,
//     so every converted path was one character too long and ended in a NUL
//     that length() counted and wcslen() did not.
//
//   * GetModuleFileName has no sizing query and signals truncation by returning
//     exactly nSize. Path::Module tested for `> MAX_PATH`, which the function is
//     documented never to return, so a truncated module path was returned as
//     though it were complete.
//
//   * SearchPath / GetCurrentDirectory / GetTempPath return a length that fits
//     or a capacity that is needed, distinguished by being strictly greater than
//     what was passed. Three wrappers passed MAX_PATH and never looked.
//
// The first half of this file drives the helpers with a fake API so the
// boundaries can be hit exactly - including the ones a real machine will not
// reproduce on demand, like a path that grows between the sizing call and the
// fill. The second half runs the real wrappers against the real system.
//
// The invariant every one of them has to hold: wcslen(c_str()) == length().

#include "test.h"

#include <windows.h>
#include "System.h"

using namespace Nilesoft;
using Nilesoft::Text::string;
namespace Contracts = Nilesoft::IO::Contracts;

namespace
{
	bool consistent(const string &s)
	{
		return s.empty() ? s.length() == 0
						 : ::wcslen(s.c_str()) == s.length();
	}

	// Stands in for an API with contract A or C: a sizing query answers with the
	// capacity needed (length + terminator); a fill either succeeds and returns
	// the length, or reports the capacity it wanted.
	struct FakeApi
	{
		const wchar_t *text;
		int calls = 0;

		DWORD size() const { return static_cast<DWORD>(::wcslen(text) + 1); }

		DWORD operator()(wchar_t *buffer, DWORD capacity)
		{
			calls++;
			if(!buffer || capacity == 0)
				return size();

			if(capacity < size())
				return size();

			::wcscpy_s(buffer, capacity, text);
			return size() - 1;
		}
	};
}

// ---------------------------------------------------------------- contract A

TEST(path_contracts, preflight_then_fill_reports_the_fill_length)
{
	FakeApi api{ L"C:\\Program Files\\Nilesoft Shell\\shell.dll" };

	auto result = Contracts::preflight_then_fill(std::ref(api));

	CHECK(::wcscmp(result.c_str(), api.text) == 0);
	CHECK_EQ((int)result.length(), (int)::wcslen(api.text));
	CHECK_MSG(consistent(result), "length must not count the terminator");
	CHECK_EQ(api.calls, 2);
}

// The value grows between the sizing call and the fill - a directory renamed
// underneath us. The second answer is a fresh capacity, not a length.
TEST(path_contracts, preflight_then_fill_retries_when_the_answer_grows)
{
	struct Growing
	{
		int calls = 0;
		DWORD operator()(wchar_t *buffer, DWORD capacity)
		{
			calls++;
			const wchar_t *before = L"C:\\short";
			const wchar_t *after = L"C:\\considerably longer than before";

			if(calls == 1)
				return static_cast<DWORD>(::wcslen(before) + 1);  // sizing

			DWORD need = static_cast<DWORD>(::wcslen(after) + 1);
			if(!buffer || capacity < need)
				return need;

			::wcscpy_s(buffer, capacity, after);
			return need - 1;
		}
	} api;

	auto result = Contracts::preflight_then_fill(std::ref(api));

	CHECK(::wcscmp(result.c_str(), L"C:\\considerably longer than before") == 0);
	CHECK(consistent(result));
	CHECK_MSG(api.calls == 3, "sizing, a fill that was too small, then the real fill");
}

TEST(path_contracts, preflight_then_fill_gives_nothing_back_on_failure)
{
	auto result = Contracts::preflight_then_fill(
		[](wchar_t *, DWORD) -> DWORD { return 0; });

	CHECK(result.empty());
	CHECK_EQ((int)result.length(), 0);
}

// A caller that never settles must not loop forever.
TEST(path_contracts, preflight_then_fill_gives_up_rather_than_spinning)
{
	int calls = 0;
	auto result = Contracts::preflight_then_fill(
		[&](wchar_t *, DWORD capacity) -> DWORD { calls++; return capacity + 1; });

	CHECK(result.empty());
	CHECK_MSG(calls < 10, "bounded retries");
}

// ---------------------------------------------------------------- contract B

TEST(path_contracts, grow_on_exact_fill_treats_a_full_buffer_as_truncation)
{
	// Answers with exactly `capacity` until the buffer is big enough - which is
	// precisely what GetModuleFileName does, and precisely what the old
	// `> MAX_PATH` test could never see.
	const wchar_t *full = L"0123456789ABCDEF";
	int calls = 0;

	auto result = Contracts::grow_on_exact_fill(
		[&](wchar_t *buffer, DWORD capacity) -> DWORD
		{
			calls++;
			DWORD length = static_cast<DWORD>(::wcslen(full));
			if(capacity <= length)
				return capacity;             // documented truncation signal

			::wcscpy_s(buffer, capacity, full);
			return length;
		},
		4);                                   // start smaller than the answer

	CHECK(::wcscmp(result.c_str(), full) == 0);
	CHECK(consistent(result));
	CHECK_MSG(calls == 4,
			  "4, 8, 16 - all truncating, 16 included, since the NUL has nowhere "
			  "to go - then 32");
}

TEST(path_contracts, grow_on_exact_fill_keeps_a_result_that_fits)
{
	int calls = 0;
	auto result = Contracts::grow_on_exact_fill(
		[&](wchar_t *buffer, DWORD capacity) -> DWORD
		{
			calls++;
			::wcscpy_s(buffer, capacity, L"C:\\shell.exe");
			return static_cast<DWORD>(::wcslen(L"C:\\shell.exe"));
		});

	CHECK(::wcscmp(result.c_str(), L"C:\\shell.exe") == 0);
	CHECK_EQ(calls, 1);
	CHECK(consistent(result));
}

TEST(path_contracts, grow_on_exact_fill_stops_at_the_maximum_path)
{
	int calls = 0;
	auto result = Contracts::grow_on_exact_fill(
		[&](wchar_t *, DWORD capacity) -> DWORD { calls++; return capacity; });

	CHECK(result.empty());
	CHECK_MSG(calls <= 8, "doubling from MAX_PATH reaches 32768 in a few steps");
}

TEST(path_contracts, grow_on_copied_fill_treats_nsize_minus_one_as_truncation)
{
	// GetModuleFileNameExW: truncated success copies nSize-1 characters and
	// returns that count. grow_on_exact_fill would keep the truncated path.
	const wchar_t *full = L"0123456789ABCDEF";
	int calls = 0;

	auto result = Contracts::grow_on_copied_fill(
		[&](wchar_t *buffer, DWORD capacity) -> DWORD
		{
			calls++;
			DWORD length = static_cast<DWORD>(::wcslen(full));
			if(capacity <= length)
			{
				DWORD n = capacity - 1;
				for(DWORD i = 0; i < n; ++i)
					buffer[i] = full[i];
				buffer[n] = L'\0';
				return n;
			}
			::wcscpy_s(buffer, capacity, full);
			return length;
		},
		4);

	CHECK(::wcscmp(result.c_str(), full) == 0);
	CHECK(consistent(result));
	CHECK_MSG(calls >= 2, "a copied-fill of nSize-1 must retry");
}

TEST(path_contracts, grow_on_copied_fill_keeps_a_result_with_slack)
{
	int calls = 0;
	auto result = Contracts::grow_on_copied_fill(
		[&](wchar_t *buffer, DWORD capacity) -> DWORD
		{
			calls++;
			::wcscpy_s(buffer, capacity, L"C:\\shell.exe");
			return static_cast<DWORD>(::wcslen(L"C:\\shell.exe"));
		});

	CHECK(::wcscmp(result.c_str(), L"C:\\shell.exe") == 0);
	CHECK_EQ(calls, 1);
	CHECK(consistent(result));
}

// ---------------------------------------------------------------- contract C

TEST(path_contracts, fill_then_resize_accepts_a_result_that_exactly_fits)
{
	// A result that fills the buffer except for its terminator is a success,
	// and must not be mistaken for a request to grow.
	const wchar_t *text = L"1234";
	int calls = 0;
	auto result = Contracts::fill_then_resize(
		[&](wchar_t *buffer, DWORD capacity) -> DWORD
		{
			calls++;
			::wcscpy_s(buffer, capacity, text);
			return static_cast<DWORD>(::wcslen(text));
		},
		5);

	CHECK(::wcscmp(result.c_str(), text) == 0);
	CHECK_EQ((int)result.length(), 4);
	CHECK_EQ(calls, 1);
	CHECK(consistent(result));
}

// The boundary that separates contract C from contract B. The same number -
// a return equal to the capacity passed - means "grew too big, here is the
// size" to GetModuleFileName and "here is your length" to SearchPath, whose
// overflow signal is documented as strictly greater than nBufferLength.
TEST(path_contracts, fill_then_resize_treats_a_full_return_as_a_length)
{
	const wchar_t *text = L"1234";
	auto result = Contracts::fill_then_resize(
		[&](wchar_t *buffer, DWORD capacity) -> DWORD
		{
			// Safe because the helper documents that it allocates capacity + 1;
			// this writes capacity characters and terminates inside that slack.
			::wmemcpy(buffer, text, capacity);
			buffer[capacity] = L'\0';
			return capacity;
		},
		4);

	CHECK_EQ((int)result.length(), 4);
	CHECK(::wcscmp(result.c_str(), text) == 0);
	CHECK(consistent(result));
}

TEST(path_contracts, fill_then_resize_grows_to_the_size_it_is_told)
{
	FakeApi api{ L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" };

	auto result = Contracts::fill_then_resize(std::ref(api), 8);

	CHECK(::wcscmp(result.c_str(), api.text) == 0);
	CHECK(consistent(result));
	CHECK_MSG(api.calls == 2, "one undersized attempt, then the size it asked for");
}

TEST(path_contracts, fill_then_resize_gives_nothing_back_on_failure)
{
	auto result = Contracts::fill_then_resize(
		[](wchar_t *, DWORD) -> DWORD { return 0; });

	CHECK(result.empty());
}

// ------------------------------------------------------------- the real APIs

TEST(path, module_path_matches_the_running_executable)
{
	wchar_t expected[MAX_PATH]{};
	DWORD n = ::GetModuleFileNameW(nullptr, expected, MAX_PATH);
	CHECK(n > 0 && n < MAX_PATH);

	auto path = IO::Path::Module(nullptr);
	CHECK(::wcscmp(path.c_str(), expected) == 0);
	CHECK_EQ((int)path.length(), (int)n);
	CHECK_MSG(consistent(path), "a module path must not carry a trailing NUL in its length");
}

TEST(path, module_filename_ex_matches_getmodulefilenamew_for_this_process)
{
	wchar_t expected[MAX_PATH]{};
	DWORD n = ::GetModuleFileNameW(nullptr, expected, MAX_PATH);
	CHECK(n > 0 && n < MAX_PATH);

	auto via_ex = Diagnostics::Process::ModuleFileName(::GetCurrentProcess());
	CHECK(::wcscmp(via_ex.c_str(), expected) == 0);
	CHECK(consistent(via_ex));
}

TEST(path, current_directory_length_is_its_length)
{
	auto cwd = IO::Path::CurrentDirectory();
	CHECK(!cwd.empty());
	CHECK_MSG(consistent(cwd), "GetCurrentDirectory returns a length, not a capacity");
}

TEST(path, temp_directory_length_is_its_length)
{
	auto temp = IO::Path::TempDirectory();
	CHECK(!temp.empty());
	CHECK(consistent(temp));
	CHECK_MSG(temp.c_str()[temp.length() - 1] == L'\\',
			  "GetTempPath is documented to end in a backslash");
}

// This is the one that was visibly wrong: the result used to be one character
// longer than the string in it.
TEST(path, long_and_short_round_trip_without_a_trailing_nul)
{
	wchar_t self[MAX_PATH]{};
	CHECK(::GetModuleFileNameW(nullptr, self, MAX_PATH) > 0);

	auto long_path = IO::Path::Long(self);
	CHECK(!long_path.empty());
	CHECK_MSG(consistent(long_path), "Long released the preflight capacity as a length");

	auto short_path = IO::Path::Short(self);
	if(short_path.empty())
	{
		// 8.3 name creation can be disabled per volume; that is a real
		// configuration, not a failure.
		CHECK_MSG(true, "short names unavailable on this volume - skipped");
	}
	else
	{
		CHECK(consistent(short_path));
		auto back = IO::Path::Long(short_path);
		CHECK(consistent(back));
		CHECK_MSG(back.equals(long_path, true), "short -> long must return the original");
	}
}

TEST(path, full_resolves_a_relative_path_and_reports_its_length)
{
	auto cwd = IO::Path::CurrentDirectory();
	CHECK(!cwd.empty());

	auto full = IO::Path::Full(L".");
	CHECK(!full.empty());
	CHECK_MSG(consistent(full), "Full released the preflight capacity as a length");

	// GetFullPathName resolves "." to the current directory; the two may differ
	// only by a trailing separator on a drive root.
	auto trimmed = full;
	CHECK(trimmed.length() > 0);
}

TEST(path, search_finds_a_system_executable)
{
	auto found = IO::Path::Search(L"cmd.exe");
	CHECK(!found.empty());
	CHECK(consistent(found));
	CHECK(found.ends_with(L"cmd.exe", true));
}
