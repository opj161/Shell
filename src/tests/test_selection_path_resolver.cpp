#include "test.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>

#include "System.h"
#include "../dll/src/Expression/SelectionPathResolver.h"

using Nilesoft::Shell::SelectionPaths::SelectionPathResolver;
using Nilesoft::Shell::SelectionPaths::WindowsPathForm;
using Nilesoft::IO::Path;

namespace
{
	namespace fs = std::filesystem;

	bool equals(std::wstring_view left, std::wstring_view right)
	{
		return left == right;
	}

	bool equals_path(const fs::path &left, const fs::path &right)
	{
		return ::_wcsicmp(left.lexically_normal().c_str(), right.lexically_normal().c_str()) == 0;
	}

	std::wstring full_once(std::wstring_view path)
	{
		std::wstring source(path);
		std::wstring result(32768, L'\0');
		auto length = ::GetFullPathNameW(source.c_str(), static_cast<DWORD>(result.size()),
			result.data(), nullptr);
		if(length == 0 || length >= result.size()) return source;
		result.resize(length);
		return result;
	}

	struct TemporaryTree
	{
		fs::path root;
		fs::path selection;
		fs::path host;

		TemporaryTree()
		{
			GUID id{};
			::CoCreateGuid(&id);
			wchar_t name[64]{};
			::StringFromGUID2(id, name, static_cast<int>(std::size(name)));
			root = fs::temp_directory_path() / (std::wstring(L"Shell-R6-") + name);
			selection = root / L"selection-A";
			host = root / L"host-B";
			std::error_code error;
			fs::create_directories(selection, error);
			fs::create_directories(host, error);
		}

		~TemporaryTree()
		{
			std::error_code error;
			fs::remove_all(root, error);
		}
	};

	struct ScopedCurrentDirectory
	{
		fs::path original = fs::current_path();

		explicit ScopedCurrentDirectory(const fs::path &path)
		{
			fs::current_path(path);
		}

		~ScopedCurrentDirectory()
		{
			std::error_code error;
			fs::current_path(original, error);
		}
	};

	void write_text(const fs::path &path, const char *text)
	{
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		stream << text;
	}

	std::string read_text(const fs::path &path)
	{
		std::ifstream stream(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(stream), {});
	}
}

TEST(selection_paths, classifies_every_documented_windows_prefix_form)
{
	CHECK_EQ(int(SelectionPathResolver::classify(L"")), int(WindowsPathForm::Empty));
	CHECK_EQ(int(SelectionPathResolver::classify(L"C:\\file.txt")), int(WindowsPathForm::FullyQualified));
	CHECK_EQ(int(SelectionPathResolver::classify(L"C:/file.txt")), int(WindowsPathForm::FullyQualified));
	CHECK_EQ(int(SelectionPathResolver::classify(L"\\\\server\\share\\file.txt")), int(WindowsPathForm::FullyQualified));
	CHECK_EQ(int(SelectionPathResolver::classify(L"\\\\?\\C:\\file.txt")), int(WindowsPathForm::FullyQualified));
	CHECK_EQ(int(SelectionPathResolver::classify(L"\\\\.\\C:\\file.txt")), int(WindowsPathForm::FullyQualified));
	CHECK_EQ(int(SelectionPathResolver::classify(L"\\rooted.txt")), int(WindowsPathForm::RootRelative));
	CHECK_EQ(int(SelectionPathResolver::classify(L"/rooted.txt")), int(WindowsPathForm::RootRelative));
	CHECK_EQ(int(SelectionPathResolver::classify(L"C:file.txt")), int(WindowsPathForm::DriveRelative));
	CHECK_EQ(int(SelectionPathResolver::classify(L"file.txt")), int(WindowsPathForm::Relative));
	CHECK_EQ(int(SelectionPathResolver::classify(L".\\file.txt")), int(WindowsPathForm::Relative));
	CHECK_EQ(int(SelectionPathResolver::classify(L"..\\file.txt")), int(WindowsPathForm::Relative));
}

TEST(selection_paths, resolves_ordinary_and_root_relative_forms_against_the_selection)
{
	SelectionPathResolver drive(L"C:\\selection\\child", L"C:\\ignored");
	CHECK(equals(drive.resolve(L"file.txt"), L"C:\\selection\\child\\file.txt"));
	CHECK(equals(drive.resolve(L".\\file.txt"), L"C:\\selection\\child\\.\\file.txt"));
	CHECK(equals(drive.resolve(L"..\\file.txt"), L"C:\\selection\\child\\..\\file.txt"));
	CHECK(equals(drive.resolve(L"\\rooted.txt"), L"C:\\rooted.txt"));
	CHECK(equals(drive.resolve(L"/rooted.txt"), L"C:\\rooted.txt"));

	SelectionPathResolver unc(L"\\\\server\\share\\selection", {});
	CHECK(equals(unc.resolve(L"\\rooted.txt"), L"\\\\server\\share\\rooted.txt"));

	SelectionPathResolver extended_drive(L"\\\\?\\C:\\selection", {});
	CHECK(equals(extended_drive.resolve(L"\\rooted.txt"), L"\\\\?\\C:\\rooted.txt"));

	SelectionPathResolver extended_unc(L"\\\\?\\UNC\\server\\share\\selection", {});
	CHECK(equals(extended_unc.resolve(L"\\rooted.txt"),
		L"\\\\?\\UNC\\server\\share\\rooted.txt"));

	SelectionPathResolver volume(L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\selection", {});
	CHECK(equals(volume.resolve(L"\\rooted.txt"),
		L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\rooted.txt"));
}

TEST(selection_paths, drive_relative_policy_is_explicit_for_same_and_other_drives)
{
	SelectionPathResolver resolver(L"C:\\selection\\child", {});
	CHECK(equals(resolver.resolve(L"C:file.txt"), L"C:\\selection\\child\\file.txt"));
	CHECK(equals(resolver.resolve(L"c:file.txt"), L"C:\\selection\\child\\file.txt"));
	CHECK(equals(resolver.resolve(L"C:"), L"C:\\selection\\child"));

	// A different drive retains D:foo's documented per-drive-current-directory
	// meaning and is captured once as an absolute path; it is not rewritten as
	// D:\foo by string concatenation.
	auto expected = full_once(L"Z:other.txt");
	CHECK(equals(resolver.resolve(L"Z:other.txt"), expected));
	CHECK(SelectionPathResolver::classify(expected) == WindowsPathForm::FullyQualified);
}

TEST(selection_paths, directory_precedes_parent_and_no_selection_preserves_input)
{
	SelectionPathResolver directory(L"C:\\directory", L"C:\\parent");
	CHECK(equals(directory.base(), L"C:\\directory"));
	CHECK(equals(directory.resolve(L"file.txt"), L"C:\\directory\\file.txt"));

	SelectionPathResolver parent({}, L"C:\\parent");
	CHECK(equals(parent.base(), L"C:\\parent"));
	CHECK(equals(parent.resolve(L"file.txt"), L"C:\\parent\\file.txt"));

	SelectionPathResolver none({}, {});
	CHECK(equals(none.resolve(L"file.txt"), L"file.txt"));
	CHECK(equals(none.resolve(L"C:file.txt"), L"C:file.txt"));
}

TEST(selection_paths, filesystem_consumers_ignore_an_unrelated_host_cwd)
{
	TemporaryTree tree;
	CHECK(fs::exists(tree.selection));
	CHECK(fs::exists(tree.host));
	if(!fs::exists(tree.selection) || !fs::exists(tree.host)) return;

	write_text(tree.selection / L"file.txt", "selection");
	write_text(tree.host / L"file.txt", "host");
	fs::create_directory(tree.selection / L"empty");

	ScopedCurrentDirectory cwd(tree.host);
	SelectionPathResolver resolver(tree.selection.native(), {});

	auto selected_file = fs::path(resolver.resolve(L"file.txt"));
	auto selected_empty = fs::path(resolver.resolve(L"empty"));
	CHECK(equals_path(selected_file, tree.selection / L"file.txt"));
	CHECK(read_text(selected_file) == "selection");
	CHECK(read_text(fs::path(L"file.txt")) == "host");
	CHECK(Path::Exists(selected_file.native()));
	CHECK(Path::IsFileExists(selected_file.native()));
	CHECK(Path::IsDirectoryExists(selected_empty.native()));
	CHECK(Path::IsDirectoryEmpty(selected_empty.native()));

	auto full = Path::Full(selected_file.native());
	CHECK(equals_path(fs::path(full.c_str()), selected_file));
	auto short_name = Path::Short(selected_file.native());
	CHECK(!short_name.empty());

	WIN32_FIND_DATAW found{};
	auto pattern = fs::path(resolver.resolve(L"*.txt"));
	auto find = ::FindFirstFileW(pattern.c_str(), &found);
	CHECK(find != INVALID_HANDLE_VALUE);
	if(find != INVALID_HANDLE_VALUE)
	{
		CHECK(::_wcsicmp(found.cFileName, L"file.txt") == 0);
		::FindClose(find);
	}

	auto link = fs::path(resolver.resolve(L"selection.lnk"));
	auto working = fs::path(resolver.resolve(L"."));
	CHECK(Path::CreateLnk(link.c_str(), selected_file.c_str(), nullptr,
		selected_file.c_str(), 0, working.c_str()));
	Nilesoft::Text::string link_target;
	Nilesoft::Text::string link_working;
	CHECK(Path::GetLinkInfo(link.native(), &link_target, &link_working));
	CHECK(equals_path(fs::path(link_target.c_str()), selected_file));
	CHECK(equals_path(fs::path(Path::Full(link_working).c_str()), tree.selection));
}
