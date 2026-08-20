// Path predicates at their boundaries.
//
// Four of these read past the end of their argument for inputs that reach them
// in ordinary use:
//
//   EndsWithSlash  called .back() on a view that may be empty;
//   GetRoot        indexed [1] and [2] having proved nothing about the length;
//   IsCLSID        indexed [0..2] the same way;
//   path.wsl       indexed [1] after checking only that the string was non-empty.
//
// IsCLSID was also wrong in a way that has nothing to do with memory. It read
//
//     if(path.length() >= 40) return false;
//     return path[0] == ':' && path[1] == ':' && path[2] == '{';
//
// and the canonical shell-namespace CLSID - "::" plus a braced GUID - is
// exactly 40 characters. So the single input the predicate exists to recognise
// was the single input it rejected, while "::{" on its own was accepted.
//
// The table below is the one the implementation plan asks for.

#include "test.h"

#include <windows.h>
#include "System.h"

using namespace Nilesoft;
using Nilesoft::IO::Path;

namespace
{
	// A real one: FOLDERID_ControlPanelFolder's shell CLSID.
	constexpr auto ControlPanel = L"::{26EE0668-A00A-44D7-9371-BEB064C98683}";
	constexpr auto NullGuid = L"::{00000000-0000-0000-0000-000000000000}";
}

TEST(path_bounds, ends_with_slash_survives_an_empty_path)
{
	CHECK_MSG(!Path::EndsWithSlash(L""), "back() on an empty view is undefined");
	CHECK(!Path::EndsWithSlash(L"C:"));
	CHECK(Path::EndsWithSlash(L"C:\\"));
	CHECK(Path::EndsWithSlash(L"C:/"));
	CHECK(!Path::EndsWithSlash(L"C:\\file.txt"));
}

TEST(path_bounds, get_root_needs_three_characters_before_it_reads_three)
{
	CHECK(Path::GetRoot(L"").empty());
	CHECK(Path::GetRoot(L"C").empty());
	CHECK(Path::GetRoot(L"C:").empty());

	CHECK(Path::GetRoot(L"C:\\") == std::wstring_view(L"C:\\"));
	CHECK(Path::GetRoot(L"C:\\Windows\\System32") == std::wstring_view(L"C:\\"));
	CHECK(Path::GetRoot(L"C:/Windows") == std::wstring_view(L"C:/"));

	// A UNC path has no drive root of this shape.
	CHECK(Path::GetRoot(L"\\\\server\\share").empty());
}

TEST(path_bounds, is_clsid_accepts_the_canonical_form_it_used_to_reject)
{
	CHECK_MSG(Path::IsCLSID(ControlPanel),
			  "the canonical 40-character CLSID is the whole point of this predicate");
	CHECK(Path::IsCLSID(NullGuid));
	CHECK_EQ((int)::wcslen(ControlPanel), 40);
}

TEST(path_bounds, is_clsid_refuses_everything_short_of_one)
{
	CHECK(!Path::IsCLSID(L""));
	CHECK(!Path::IsCLSID(L":"));
	CHECK(!Path::IsCLSID(L"::"));
	CHECK_MSG(!Path::IsCLSID(L"::{"), "this used to be accepted");
	CHECK(!Path::IsCLSID(L"C:\\"));
	CHECK(!Path::IsCLSID(L"{26EE0668-A00A-44D7-9371-BEB064C98683}"));   // no ::
}

TEST(path_bounds, is_clsid_parses_the_guid_rather_than_the_punctuation)
{
	// Right length, right brackets, not a GUID.
	CHECK(!Path::IsCLSID(L"::{ZZZZZZZZ-A00A-44D7-9371-BEB064C98683}"));
	CHECK(!Path::IsCLSID(L"::{26EE0668+A00A-44D7-9371-BEB064C98683}"));
	// Right shape, one character short, so not the canonical form.
	CHECK(!Path::IsCLSID(L"::{26EE0668-A00A-44D7-9371-BEB064C9868}"));
	// Missing the closing brace.
	CHECK(!Path::IsCLSID(L"::{26EE0668-A00A-44D7-9371-BEB064C98683 "));
}

TEST(path_bounds, is_clsid_is_exact_and_is_namespace_is_not)
{
	std::wstring child = std::wstring(ControlPanel) + L"\\Sub";

	CHECK_MSG(!Path::IsCLSID(child), "IsCLSID answers one question only");
	CHECK_MSG(Path::IsNameSpace(child), "a CLSID with a child path still lives in the namespace");
	CHECK(Path::IsNameSpace(ControlPanel));

	// Not a namespace path: a CLSID with something glued to it.
	CHECK(!Path::IsNameSpace(std::wstring(ControlPanel) + L"garbage"));
	CHECK(!Path::IsNameSpace(L""));
	CHECK(!Path::IsNameSpace(L"::{"));
	CHECK(!Path::IsNameSpace(L"C:\\Windows"));
}

TEST(path_bounds, is_root_only_accepts_a_drive_root)
{
	CHECK(!Path::IsRoot(L""));
	CHECK(!Path::IsRoot(L"C"));
	CHECK(!Path::IsRoot(L"C:"));
	CHECK(Path::IsRoot(L"C:\\"));
	CHECK(!Path::IsRoot(L"C:\\x"));
}

TEST(path_bounds, drive_prefix_checks_its_length_when_it_is_given_one)
{
	CHECK(!Path::IsDrivePrefix(std::wstring_view(L"")));
	CHECK(!Path::IsDrivePrefix(std::wstring_view(L"C")));
	CHECK(Path::IsDrivePrefix(std::wstring_view(L"C:")));
	CHECK(Path::IsDrivePrefix(std::wstring_view(L"c:\\Windows")));
	CHECK(!Path::IsDrivePrefix(std::wstring_view(L"1:")));
	CHECK(!Path::IsDrivePrefix(std::wstring_view(L"\\\\server")));
}
