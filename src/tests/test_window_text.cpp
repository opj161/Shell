// Window titles, at and beyond the buffer that used to be hard-coded.
//
// window.title and the parent/owner variants read into wchar_t[250] and passed
// 250 to GetWindowTextW, so a title was silently cut at 249 characters. Window
// titles get long for ordinary reasons - an editor showing a document path, a
// browser showing a URL - and the script had no way to tell a truncated answer
// from a complete one.
//
// These drive a real window rather than a mock, because the truncation was the
// system's behaviour and not the tree's.

#include "test.h"

#include <windows.h>
#include <string>

#include "System.h"

using namespace Nilesoft;
using Nilesoft::Windows::Window;

namespace
{
	// c_str() is null for an invalid string, and wcscmp would fault on it.
	bool same(const Nilesoft::Text::string &s, const wchar_t *expected)
	{
		return ::wcscmp(s.c_str() ? s.c_str() : L"", expected) == 0;
	}
}

namespace
{
	struct TitledWindow
	{
		HWND handle = nullptr;

		explicit TitledWindow(const std::wstring &title)
		{
			handle = ::CreateWindowExW(0, L"STATIC", title.c_str(), WS_POPUP,
									   0, 0, 10, 10, nullptr, nullptr,
									   ::GetModuleHandleW(nullptr), nullptr);
		}

		~TitledWindow() { if(handle) ::DestroyWindow(handle); }
	};
}

TEST(window_text, an_ordinary_title_reads_back_exactly)
{
	TitledWindow window(L"Nilesoft Shell");
	CHECK(window.handle != nullptr);

	auto title = Window::text(window.handle);
	CHECK(same(title, L"Nilesoft Shell"));
	CHECK_EQ((int)title.length(), 14);
}

// The one that was being cut.
TEST(window_text, a_title_longer_than_the_old_buffer_survives_intact)
{
	std::wstring long_title(1000, L'T');
	long_title[999] = L'!';

	TitledWindow window(long_title);
	CHECK(window.handle != nullptr);

	auto title = Window::text(window.handle);
	CHECK_MSG(title.length() == 1000, "a 1000-character title must not stop at 249");
	CHECK(same(title, long_title.c_str()));
	CHECK_MSG(title.length() == 1000 && title.c_str()[999] == L'!',
			  "including its last character");
}

TEST(window_text, a_title_exactly_at_the_old_boundary)
{
	// 249, 250 and 251 characters: the sizes the old buffer got wrong.
	for(int length : { 249, 250, 251 })
	{
		std::wstring title(static_cast<size_t>(length), L'x');
		TitledWindow window(title);
		CHECK(window.handle != nullptr);

		auto read = Window::text(window.handle);
		CHECK_MSG((int)read.length() == length, "boundary length lost characters");
	}
}

TEST(window_text, an_empty_title_is_empty_rather_than_rubbish)
{
	TitledWindow window(L"");
	CHECK(window.handle != nullptr);

	auto title = Window::text(window.handle);
	CHECK(title.empty());
	CHECK_EQ((int)title.length(), 0);
}

TEST(window_text, a_null_window_reads_as_nothing)
{
	auto title = Window::text(nullptr);
	CHECK(title.empty());
}

TEST(window_text, unicode_beyond_the_bmp_is_not_split)
{
	// A surrogate pair either side of the old boundary.
	std::wstring title(248, L'a');
	title += L"\xD83D\xDE80";   // U+1F680
	title += L"tail";

	TitledWindow window(title);
	CHECK(window.handle != nullptr);

	auto read = Window::text(window.handle);
	CHECK_EQ((int)read.length(), (int)title.length());
	CHECK(same(read, title.c_str()));
}
