#include "test.h"

#include "..\shared\ConfigShadow.h"

#include <string>
#include <vector>

// The persisted half of last-known-good. The in-memory half only helps a
// process that already had a generation loaded; a process that *starts* while
// shell.nss is broken has nothing, and that is the case a user actually hits -
// they save a typo, and the next application they right-click in has no
// context menu. docs/refactor/03-config-safety.md section 1b.
//
// Real files in a real temporary directory throughout: this code is about what
// survives on disk, and a mock of the file system would only assert that the
// mock works.

namespace shadow = Nilesoft::ConfigShadow;

namespace
{
	std::wstring temp_root()
	{
		wchar_t buffer[MAX_PATH]{};
		auto n = ::GetTempPathW(MAX_PATH, buffer);
		if(n == 0 || n >= MAX_PATH)
			return {};
		return std::wstring(buffer, n);
	}

	// A throwaway directory tree, removed on the way out.
	struct TempTree
	{
		std::wstring path;

		explicit TempTree(const wchar_t *tag)
		{
			static unsigned counter = 0;
			path = temp_root() + L"nss_shadow_" + std::to_wstring(::GetCurrentProcessId())
				 + L"_" + tag + L"_" + std::to_wstring(++counter);
			shadow::ensure_directory(path);
		}

		~TempTree() { remove(path); }

		TempTree(const TempTree &) = delete;
		TempTree &operator=(const TempTree &) = delete;

		std::wstring at(const std::wstring &relative) const
		{
			return shadow::join(path, relative);
		}

		// Writes `text` to `relative`, creating parent directories. Returns the
		// full path.
		std::wstring write(const std::wstring &relative, const std::wstring &text) const
		{
			auto full = at(relative);
			auto cut = full.find_last_of(L"\\/");
			if(cut != std::wstring::npos)
				shadow::ensure_directory(full.substr(0, cut));
			shadow::write_all(full, text);
			return full;
		}

		static void remove(const std::wstring &directory)
		{
			WIN32_FIND_DATAW find{};
			auto handle = ::FindFirstFileW(shadow::join(directory, L"*").c_str(), &find);
			if(handle == INVALID_HANDLE_VALUE)
				return;
			do
			{
				std::wstring name = find.cFileName;
				if(name == L"." || name == L"..")
					continue;
				auto full = shadow::join(directory, name);
				if(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					remove(full);
				else
					::DeleteFileW(full.c_str());
			}
			while(::FindNextFileW(handle, &find));
			::FindClose(handle);
			::RemoveDirectoryW(directory.c_str());
		}
	};

	bool exists(const std::wstring &path)
	{
		return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	std::wstring read(const std::wstring &path)
	{
		std::wstring text;
		shadow::read_all(path, text);
		return text;
	}
}

TEST(config_shadow, a_saved_set_comes_back_with_its_layout_intact)
{
	// Relative imports are rooted against the importing file's own directory,
	// so the shadow has to be a mirror rather than a bag of blobs - otherwise
	// the copied root would import the original, broken files.
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"import 'imports/theme.nss'\n");
	auto theme = config.write(L"imports\\theme.nss", L"theme { }\n");

	CHECK(shadow::save(store.path, root, { root, theme }));

	auto resolved = shadow::resolve(store.path);
	CHECK(!resolved.empty());
	CHECK_MSG(resolved == store.at(L"shell.nss"), "the root keeps its own name");
	CHECK_MSG(exists(store.at(L"imports\\theme.nss")),
			  "and an imported file keeps its position relative to the root");
	CHECK(read(store.at(L"imports\\theme.nss")) == L"theme { }\n");
}

TEST(config_shadow, an_empty_store_offers_nothing)
{
	TempTree store(L"empty");
	CHECK(shadow::resolve(store.path).empty());
	CHECK(shadow::resolve(L"").empty());
}

TEST(config_shadow, a_shadow_whose_content_changed_is_refused)
{
	// The hashes are integrity, not authenticity: they are here to catch a
	// damaged or partly replaced shadow, which must be refused rather than
	// parsed. A process then falls back to the ordinary "never loaded"
	// refusal, which is the old behaviour and the safe one.
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"menu { }\n");
	CHECK(shadow::save(store.path, root, { root }));
	CHECK(!shadow::resolve(store.path).empty());

	shadow::write_all(store.at(L"shell.nss"), L"menu { tampered }\n");
	CHECK_MSG(shadow::resolve(store.path).empty(), "content no longer matches its digest");
}

TEST(config_shadow, a_shadow_missing_a_file_is_refused)
{
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"import 'a.nss'\n");
	auto other = config.write(L"a.nss", L"item { }\n");
	CHECK(shadow::save(store.path, root, { root, other }));
	CHECK(!shadow::resolve(store.path).empty());

	::DeleteFileW(store.at(L"a.nss").c_str());
	CHECK_MSG(shadow::resolve(store.path).empty(),
			  "a partial set must not be parsed as if it were whole");
}

TEST(config_shadow, a_shadow_with_no_manifest_is_refused)
{
	// The manifest is the commit point. Copies that nothing names are not a
	// shadow, which is what makes a torn write harmless rather than dangerous.
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"menu { }\n");
	CHECK(shadow::save(store.path, root, { root }));

	::DeleteFileW(store.at(shadow::ManifestName).c_str());
	CHECK(shadow::resolve(store.path).empty());
	CHECK_MSG(exists(store.at(L"shell.nss")), "the copy is still there, it just is not claimed");
}

TEST(config_shadow, a_manifest_this_build_does_not_understand_is_refused)
{
	// Against parse_manifest directly. Going through resolve() would prove
	// nothing: it refuses a manifest with no files either way, so the test
	// would pass whether the unknown line was rejected or silently skipped.
	shadow::Manifest manifest;

	const std::wstring good = std::wstring(shadow::ManifestHeader)
		+ L"\nroot\tshell.nss\nfile\tshell.nss\t9\t0123456789abcdef\n";
	CHECK_MSG(shadow::parse_manifest(good, manifest), "control: this one is understood");

	CHECK_MSG(!shadow::parse_manifest(L"nilesoft-shell-lkg\t99\n" + good.substr(good.find(L'\n') + 1), manifest),
			  "a version this build does not know");

	CHECK_MSG(!shadow::parse_manifest(good + L"something-new\tx\n", manifest),
			  "an unknown line is refused, not skipped - it may be the one that mattered");

	CHECK_MSG(!shadow::parse_manifest(good + L"file\ta.nss\tnotanumber\t0123456789abcdef\n", manifest),
			  "a size that is not a number");
	CHECK_MSG(!shadow::parse_manifest(good + L"file\ta.nss\t3\tzz\n", manifest),
			  "a digest that is not a digest");
	CHECK_MSG(!shadow::parse_manifest(std::wstring(shadow::ManifestHeader) + L"\nroot\tshell.nss\n", manifest),
			  "a root that names no files");
}

namespace
{
	// CopyFileW carries the source's timestamps across, so comparing before and
	// after a save cannot tell a skipped copy from a repeated one. Stamping the
	// shadow with a time the source does not have can: if the copy is rewritten
	// the stamp is replaced by the source's, and if it is skipped the stamp
	// survives.
	const FILETIME Marker{ 0x12345678, 0x01D00000 };

	bool stamp(const std::wstring &path)
	{
		auto handle = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
									nullptr, OPEN_EXISTING, 0, nullptr);
		if(handle == INVALID_HANDLE_VALUE)
			return false;
		auto ok = ::SetFileTime(handle, nullptr, nullptr, &Marker) != 0;
		::CloseHandle(handle);
		return ok;
	}

	bool still_stamped(const std::wstring &path)
	{
		auto handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
									nullptr, OPEN_EXISTING, 0, nullptr);
		if(handle == INVALID_HANDLE_VALUE)
			return false;
		FILETIME written{};
		auto ok = ::GetFileTime(handle, nullptr, nullptr, &written) != 0;
		::CloseHandle(handle);
		return ok && ::CompareFileTime(&written, &Marker) == 0;
	}
}

TEST(config_shadow, saving_the_same_configuration_twice_rewrites_nothing)
{
	// Every process that loads Shell parses the configuration, so without this
	// every one of them would rewrite the same bytes into the same place.
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"menu { }\n");
	CHECK(shadow::save(store.path, root, { root }));

	CHECK(stamp(store.at(L"shell.nss")));
	CHECK(shadow::save(store.path, root, { root }));
	CHECK_MSG(still_stamped(store.at(L"shell.nss")), "unchanged input, untouched copy");
}

TEST(config_shadow, a_damaged_shadow_is_repaired_by_the_next_save)
{
	// The skip above is keyed on the input not having changed, which says
	// nothing about whether the store is still intact. Without re-reading it,
	// a shadow whose copies were deleted would stay broken for exactly as long
	// as the user left their configuration alone - which is exactly as long as
	// there would be nothing to recover from.
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"menu { }\n");
	auto other = config.write(L"imports\\a.nss", L"item { }\n");
	CHECK(shadow::save(store.path, root, { root, other }));
	CHECK(!shadow::resolve(store.path).empty());

	::DeleteFileW(store.at(L"imports\\a.nss").c_str());
	CHECK(shadow::resolve(store.path).empty());

	CHECK_MSG(shadow::save(store.path, root, { root, other }), "same input, damaged store");
	CHECK_MSG(!shadow::resolve(store.path).empty(), "the store is rebuilt rather than skipped over");
}

TEST(config_shadow, an_edited_configuration_replaces_the_previous_shadow)
{
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"menu { first }\n");
	CHECK(shadow::save(store.path, root, { root }));
	CHECK(read(store.at(L"shell.nss")) == L"menu { first }\n");

	config.write(L"shell.nss", L"menu { second }\n");
	CHECK(shadow::save(store.path, root, { root }));
	CHECK(read(store.at(L"shell.nss")) == L"menu { second }\n");
	CHECK(!shadow::resolve(store.path).empty());
}

TEST(config_shadow, a_file_outside_the_configuration_directory_makes_the_shadow_partial)
{
	// It cannot be mirrored without a second root. The rest is still worth
	// keeping - an absolute import usually resolves the same way wherever it is
	// read from - and if that outside file is the broken one, parsing the
	// shadow fails too and the caller falls back to refusing.
	TempTree config(L"cfg");
	TempTree elsewhere(L"other");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"menu { }\n");
	auto outside = elsewhere.write(L"outside.nss", L"item { }\n");

	CHECK(shadow::save(store.path, root, { root, outside }));
	CHECK(!shadow::resolve(store.path).empty());
	CHECK(!exists(store.at(L"outside.nss")));

	shadow::Manifest manifest;
	CHECK(shadow::parse_manifest(read(store.at(shadow::ManifestName)), manifest));
	CHECK_MSG(manifest.partial, "and the manifest says so rather than pretending it is complete");
	CHECK_EQ(manifest.files.size(), size_t(1));
}

TEST(config_shadow, nothing_worth_shadowing_is_not_an_error_that_writes_a_broken_store)
{
	TempTree store(L"store");

	CHECK(!shadow::save(store.path, L"", {}));
	CHECK(!shadow::save(store.path, L"C:\\nowhere\\shell.nss", { L"C:\\nowhere\\shell.nss" }));
	CHECK_MSG(shadow::resolve(store.path).empty(),
			  "a failed save must not leave something that resolves");
}

TEST(config_shadow, relative_paths_are_recognised_case_insensitively)
{
	// The file system treats these as the same path, and a parse can reach the
	// same file through either spelling.
	CHECK(shadow::relative_to(L"C:\\Config", L"C:\\config\\shell.nss") == L"shell.nss");
	CHECK(shadow::relative_to(L"C:\\Config", L"C:\\Config\\imports\\a.nss") == L"imports\\a.nss");

	// Not below it at all.
	CHECK(shadow::relative_to(L"C:\\Config", L"C:\\ConfigOther\\a.nss").empty());
	CHECK(shadow::relative_to(L"C:\\Config", L"C:\\Conf\\a.nss").empty());
	CHECK(shadow::relative_to(L"C:\\Config", L"C:\\Config").empty());
	CHECK(shadow::relative_to(L"", L"C:\\Config\\a.nss").empty());
}

TEST(config_shadow, the_default_directory_is_under_local_appdata)
{
	auto directory = shadow::default_directory();
	CHECK(!directory.empty());
	if(directory.empty())
		return;

	CHECK(directory.find(L"\\Nilesoft\\Shell\\lkg") != std::wstring::npos);
	CHECK_MSG(directory.find(L"\\\\") == std::wstring::npos,
			  "SHGetKnownFolderPath returns no trailing backslash, so joining must not double one");
}

// ---- one path, two spellings ----------------------------------------------

TEST(config_shadow, a_short_form_root_still_finds_its_own_files)
{
	// The defect the first CI run this branch ever had found, in production
	// code rather than in a test.
	//
	// save() decided whether a loaded file lives under the configuration
	// directory by comparing their leading characters, and it took `root` from
	// the caller while `loaded` came from the parser. Those are different
	// sources and need not agree on 8.3 versus long form. When they disagreed,
	// relative_to() matched nothing, every file was skipped, `pending` came out
	// empty and save() returned false - so the shadow was silently never
	// written, which for a last-known-good mechanism means it has quietly
	// stopped existing.
	//
	// GetShortPathName is used to *construct* the disagreement rather than to
	// assert anything: if this volume has 8.3 generation disabled it hands back
	// the long path, both spellings match, and the test still passes for the
	// ordinary reason. That is the honest way round - it can never fail
	// spuriously on a machine that cannot reproduce the condition.
	TempTree tree(L"shortform");
	auto root = tree.write(L"root.nss", L"item(title='Root')");
	tree.write(L"imports\\extra.nss", L"item(title='Extra')");

	std::vector<std::wstring> loaded{ root, tree.at(L"imports\\extra.nss") };

	wchar_t shortened[MAX_PATH]{};
	auto n = ::GetShortPathNameW(root.c_str(), shortened, MAX_PATH);
	CHECK_MSG(n > 0 && n < MAX_PATH, "GetShortPathName should answer for a file that exists");
	if(n == 0 || n >= MAX_PATH)
		return;

	TempTree store(L"shortform_store");

	// `loaded` is long-form throughout, exactly as the parser reports it; only
	// the root is handed over short.
	CHECK_MSG(shadow::save(store.path, std::wstring(shortened), loaded),
			  "a root spelled differently from the files under it is still that "
			  "directory, and the shadow has to be written");

	auto resolved = shadow::resolve(store.path);
	CHECK_MSG(!resolved.empty(), "and it has to verify and come back");
}

TEST(config_shadow, long_form_leaves_an_ordinary_path_alone)
{
	// The over-correction guard. long_form() falls back to the caller's string
	// whenever GetLongPathName cannot answer - a path that does not exist, or a
	// parent nobody may list - so it must not mangle or empty a plain one.
	TempTree tree(L"longform");
	auto file = tree.write(L"plain.nss", L"item(title='Plain')");

	CHECK(shadow::long_form(file) == shadow::long_form(file));
	CHECK(!shadow::long_form(file).empty());
	CHECK(shadow::long_form(L"") == L"");

	// Does not exist: the documented zero return, and the caller's string back.
	auto missing = tree.at(L"no-such-file-here.nss");
	CHECK(shadow::long_form(missing) == missing);
}
