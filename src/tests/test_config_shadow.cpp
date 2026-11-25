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

	// What the parser hands save(): the stamp of each file, taken before it
	// was read. The tests take them just before saving, which is the same
	// thing when nothing is writing.
	std::vector<uint64_t> stamps_of(const std::vector<std::wstring> &files)
	{
		std::vector<uint64_t> out;
		for(const auto &f : files)
			out.push_back(Nilesoft::Shell::config_file_stamp(f));
		return out;
	}

	bool save_now(const std::wstring &store, const std::wstring &root,
				  const std::vector<std::wstring> &loaded)
	{
		return shadow::save(store, root, loaded, stamps_of(loaded));
	}

	shadow::Manifest manifest_of(const std::wstring &store)
	{
		shadow::Manifest manifest;
		shadow::parse_manifest(read(shadow::join(store, shadow::ManifestName)), manifest);
		return manifest;
	}

	// A file inside the generation the manifest currently names.
	std::wstring shadowed(const TempTree &store, const std::wstring &relative)
	{
		return shadow::join(shadow::join(store.path, manifest_of(store.path).generation), relative);
	}

	std::vector<std::wstring> names_in(const std::wstring &directory)
	{
		std::vector<std::wstring> names;
		WIN32_FIND_DATAW find{};
		auto handle = ::FindFirstFileW(shadow::join(directory, L"*").c_str(), &find);
		if(handle == INVALID_HANDLE_VALUE)
			return names;
		do
		{
			std::wstring name = find.cFileName;
			if(name != L"." && name != L"..")
				names.push_back(name);
		}
		while(::FindNextFileW(handle, &find));
		::FindClose(handle);
		return names;
	}

	// Backdates a file, directory or link itself (not its target).
	bool set_age(const std::wstring &path, unsigned hours)
	{
		auto handle = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
									FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
									nullptr, OPEN_EXISTING,
									FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if(handle == INVALID_HANDLE_VALUE)
			return false;

		FILETIME now{};
		::GetSystemTimeAsFileTime(&now);
		ULARGE_INTEGER value{};
		value.LowPart = now.dwLowDateTime;
		value.HighPart = now.dwHighDateTime;
		value.QuadPart -= static_cast<unsigned long long>(hours) * 3600ULL * 10000000ULL;
		FILETIME then{ value.LowPart, value.HighPart };

		auto ok = ::SetFileTime(handle, nullptr, nullptr, &then) != 0;
		::CloseHandle(handle);
		return ok;
	}

	// A v1 manifest line for a file already in place under `store`.
	std::wstring legacy_line(const std::wstring &path, const std::wstring &relative)
	{
		auto file = shadow::open_for_read(path);
		shadow::Digest digest{};
		unsigned long long size = 0;
		shadow::digest_of(file, digest, size);
		::CloseHandle(file);
		return L"file\t" + relative + L"\t" + std::to_wstring(size) + L"\t" + shadow::to_hex(digest) + L"\n";
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

	CHECK(save_now(store.path, root, { root, theme }));

	auto resolved = shadow::resolve(store.path);
	CHECK(!resolved.empty());
	CHECK_MSG(resolved == shadowed(store, L"shell.nss"), "the root keeps its own name");
	CHECK_MSG(exists(shadowed(store, L"imports\\theme.nss")),
			  "and an imported file keeps its position relative to the root");
	CHECK(read(shadowed(store, L"imports\\theme.nss")) == L"theme { }\n");
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
	CHECK(save_now(store.path, root, { root }));
	CHECK(!shadow::resolve(store.path).empty());

	shadow::write_all(shadowed(store, L"shell.nss"), L"menu { tampered }\n");
	CHECK_MSG(shadow::resolve(store.path).empty(), "content no longer matches its digest");
}

TEST(config_shadow, a_shadow_missing_a_file_is_refused)
{
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"import 'a.nss'\n");
	auto other = config.write(L"a.nss", L"item { }\n");
	CHECK(save_now(store.path, root, { root, other }));
	CHECK(!shadow::resolve(store.path).empty());

	::DeleteFileW(shadowed(store, L"a.nss").c_str());
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
	CHECK(save_now(store.path, root, { root }));

	auto copy = shadowed(store, L"shell.nss");
	::DeleteFileW(store.at(shadow::ManifestName).c_str());
	CHECK(shadow::resolve(store.path).empty());
	CHECK_MSG(exists(copy), "the copy is still there, it just is not claimed");
}

TEST(config_shadow, a_manifest_this_build_does_not_understand_is_refused)
{
	// Against parse_manifest directly. Going through resolve() would prove
	// nothing: it refuses a manifest with no files either way, so the test
	// would pass whether the unknown line was rejected or silently skipped.
	shadow::Manifest manifest;

	const std::wstring good = std::wstring(shadow::ManifestHeader)
		+ L"\ngeneration\t0123456789abcdef\nroot\tshell.nss\nfile\tshell.nss\t9\t0123456789abcdef\n";
	CHECK_MSG(shadow::parse_manifest(good, manifest), "control: this one is understood");
	CHECK(manifest.generation == L"0123456789abcdef");

	const std::wstring legacy = std::wstring(shadow::LegacyManifestHeader)
		+ L"\nroot\tshell.nss\nfile\tshell.nss\t9\t0123456789abcdef\n";
	CHECK_MSG(shadow::parse_manifest(legacy, manifest), "v1 is still understood");
	CHECK_MSG(manifest.generation.empty(), "and has no generation: its files sit in the store itself");
	CHECK_MSG(!shadow::parse_manifest(legacy + L"generation\t0123456789abcdef\n", manifest),
			  "a generation line in a v1 manifest is not v1");

	CHECK_MSG(!shadow::parse_manifest(std::wstring(shadow::ManifestHeader)
									  + L"\nroot\tshell.nss\nfile\tshell.nss\t9\t0123456789abcdef\n", manifest),
			  "v2 without a generation names nowhere to look");
	CHECK_MSG(!shadow::parse_manifest(std::wstring(shadow::ManifestHeader)
									  + L"\ngeneration\tNOTHEX\nroot\tshell.nss\nfile\tshell.nss\t9\t0123456789abcdef\n", manifest),
			  "a generation that is not a 16-digit name");

	// The manifest decides what is read and, for v1 cleanup, deleted, so it
	// must not be able to point outside the store.
	for(auto bad : { L"..\\evil.nss", L"a\\..\\..\\evil.nss", L"C:\\evil.nss", L"\\evil.nss", L"a\\\\b.nss", L"." })
		CHECK_MSG(!shadow::parse_manifest(L"nilesoft-shell-lkg\t1\nroot\tshell.nss\nfile\t" + std::wstring(bad)
										  + L"\t9\t0123456789abcdef\n", manifest),
				  "a file path that leaves the store is refused");

	CHECK_MSG(!shadow::parse_manifest(L"nilesoft-shell-lkg\t99\n" + good.substr(good.find(L'\n') + 1), manifest),
			  "a version this build does not know");

	CHECK_MSG(!shadow::parse_manifest(good + L"something-new\tx\n", manifest),
			  "an unknown line is refused, not skipped - it may be the one that mattered");

	CHECK_MSG(!shadow::parse_manifest(good + L"file\ta.nss\tnotanumber\t0123456789abcdef\n", manifest),
			  "a size that is not a number");
	CHECK_MSG(!shadow::parse_manifest(good + L"file\ta.nss\t3\tzz\n", manifest),
			  "a digest that is not a digest");
	CHECK_MSG(!shadow::parse_manifest(std::wstring(shadow::ManifestHeader)
									  + L"\ngeneration\t0123456789abcdef\nroot\tshell.nss\n", manifest),
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
	CHECK(save_now(store.path, root, { root }));

	auto copy = shadowed(store, L"shell.nss");
	CHECK(stamp(copy));
	CHECK(save_now(store.path, root, { root }));
	CHECK_MSG(still_stamped(copy), "unchanged input, untouched copy");
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
	CHECK(save_now(store.path, root, { root, other }));
	CHECK(!shadow::resolve(store.path).empty());

	::DeleteFileW(shadowed(store, L"imports\\a.nss").c_str());
	CHECK(shadow::resolve(store.path).empty());

	CHECK_MSG(save_now(store.path, root, { root, other }), "same input, damaged store");
	CHECK_MSG(!shadow::resolve(store.path).empty(), "the store is rebuilt rather than skipped over");
}

TEST(config_shadow, an_edited_configuration_replaces_the_previous_shadow)
{
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"menu { first }\n");
	CHECK(save_now(store.path, root, { root }));
	CHECK(read(shadowed(store, L"shell.nss")) == L"menu { first }\n");

	config.write(L"shell.nss", L"menu { second }\n");
	CHECK(save_now(store.path, root, { root }));
	CHECK(read(shadowed(store, L"shell.nss")) == L"menu { second }\n");
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

	CHECK(save_now(store.path, root, { root, outside }));
	CHECK(!shadow::resolve(store.path).empty());
	CHECK(!exists(shadowed(store, L"outside.nss")));

	shadow::Manifest manifest;
	CHECK(shadow::parse_manifest(read(store.at(shadow::ManifestName)), manifest));
	CHECK_MSG(manifest.partial, "and the manifest says so rather than pretending it is complete");
	CHECK_EQ(manifest.files.size(), size_t(1));
}

TEST(config_shadow, nothing_worth_shadowing_is_not_an_error_that_writes_a_broken_store)
{
	TempTree store(L"store");

	CHECK(!shadow::save(store.path, L"", {}, {}));
	CHECK(!save_now(store.path, L"C:\\nowhere\\shell.nss", { L"C:\\nowhere\\shell.nss" }));
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
	CHECK_MSG(save_now(store.path, std::wstring(shortened), loaded),
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

// ---- only what was parsed is saved ------------------------------------------

TEST(config_shadow, a_file_that_changed_after_it_was_stamped_is_not_shadowed)
{
	// ~24 hosts reload after an editor save. One that stamped the file, then
	// found it rewritten by the time it saved, must not publish the newer
	// bytes as "known good": it never parsed them. The previous shadow stays.
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"menu { good }\n");
	CHECK(save_now(store.path, root, { root }));
	auto before = manifest_of(store.path).generation;

	auto parsed_stamp = stamps_of({ root });
	config.write(L"shell.nss", L"menu { half-typed, and longer\n");

	CHECK_MSG(!shadow::save(store.path, root, { root }, parsed_stamp),
			  "the file is not the one that was parsed");
	CHECK(manifest_of(store.path).generation == before);
	auto resolved = shadow::resolve(store.path);
	CHECK(!resolved.empty());
	CHECK_MSG(read(resolved) == L"menu { good }\n", "the previous shadow still serves");

	CHECK_MSG(!shadow::save(store.path, root, { root }, {}), "one stamp per file");
}

// ---- immutable generations ---------------------------------------------------

TEST(config_shadow, a_new_save_leaves_the_previous_generation_intact_until_pruned)
{
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"menu { first }\n");
	CHECK(save_now(store.path, root, { root }));
	auto first = manifest_of(store.path).generation;

	config.write(L"shell.nss", L"menu { second, longer }\n");
	CHECK(save_now(store.path, root, { root }));
	auto second = manifest_of(store.path).generation;

	CHECK_MSG(first != second, "different content, different generation");
	CHECK(shadow::resolve(store.path) == shadow::join(shadow::join(store.path, second), L"shell.nss"));
	CHECK_MSG(read(shadow::join(shadow::join(store.path, first), L"shell.nss")) == L"menu { first }\n",
			  "a process still reading the old generation is not pulled out from under");

	// Once it is old enough that no saver can be about to name it, the next
	// save collects it.
	CHECK(set_age(shadow::join(store.path, first), 1));
	config.write(L"shell.nss", L"menu { third, longer still }\n");
	CHECK(save_now(store.path, root, { root }));
	CHECK(!exists(shadow::join(store.path, first)));
	CHECK_MSG(exists(shadow::join(store.path, second)), "the one just superseded is still young");
	CHECK(!shadow::resolve(store.path).empty());
}

TEST(config_shadow, identical_content_saved_twice_is_one_generation_and_no_leftovers)
{
	// Two hosts that parsed the same configuration derive the same name, so
	// the second finds the first's generation and leaves it alone.
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"import 'a.nss'\n");
	auto other = config.write(L"a.nss", L"item { }\n");

	CHECK(save_now(store.path, root, { root, other }));
	CHECK(save_now(store.path, root, { root, other }));

	auto names = names_in(store.path);
	CHECK_MSG(names.size() == 2, "the manifest and one generation; no .tmp, no .staging");
	CHECK(!shadow::resolve(store.path).empty());
}

TEST(config_shadow, a_save_that_cannot_complete_keeps_the_previous_shadow)
{
	// Something that is not a directory sits where the new generation has to
	// go, so the set cannot be put in place. The old manifest must still name
	// an intact generation, and the failed attempt must not leave a staging
	// tree behind.
	TempTree config(L"cfg");
	TempTree store(L"store");
	TempTree scratch(L"scratch");

	auto root = config.write(L"shell.nss", L"menu { first }\n");
	CHECK(save_now(store.path, root, { root }));
	auto first = manifest_of(store.path).generation;

	// Learn the name the second content will get by saving it elsewhere.
	config.write(L"shell.nss", L"menu { second, longer }\n");
	CHECK(save_now(scratch.path, root, { root }));
	auto second = manifest_of(scratch.path).generation;
	CHECK(first != second);

	shadow::write_all(shadow::join(store.path, second), L"in the way");

	CHECK(!save_now(store.path, root, { root }));
	CHECK(manifest_of(store.path).generation == first);
	CHECK_MSG(!shadow::resolve(store.path).empty(), "the previous shadow still resolves");
	CHECK_MSG(read(shadow::join(store.path, second)) == L"in the way", "and what was in the way is left alone");
	CHECK_EQ(names_in(store.path).size(), size_t(3));		// manifest, first, the obstacle
}

// ---- manifest v1 -------------------------------------------------------------

TEST(config_shadow, a_v1_shadow_still_resolves_and_is_removed_by_the_first_v2_save)
{
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"import 'imports/a.nss'\n");
	auto other = config.write(L"imports\\a.nss", L"item { }\n");

	// What the previous build left behind: files directly under the store.
	auto v1_root = store.write(L"shell.nss", L"import 'imports/a.nss'\n");
	auto v1_other = store.write(L"imports\\a.nss", L"item { }\n");
	shadow::write_all(store.at(shadow::ManifestName),
					  std::wstring(shadow::LegacyManifestHeader) + L"\nroot\tshell.nss\n"
					  + legacy_line(v1_root, L"shell.nss") + legacy_line(v1_other, L"imports\\a.nss"));

	CHECK_MSG(shadow::resolve(store.path) == v1_root, "an upgraded machine keeps its fallback");

	CHECK(save_now(store.path, root, { root, other }));

	CHECK_MSG(!exists(v1_root) && !exists(v1_other), "the v1 files are gone");
	CHECK_MSG(!exists(store.at(L"imports")), "and so is the directory they emptied");
	auto resolved = shadow::resolve(store.path);
	CHECK(!resolved.empty());
	CHECK_MSG(resolved == shadowed(store, L"shell.nss"), "v2 serves now");
	CHECK(exists(store.path));
}

// ---- pruning -----------------------------------------------------------------

TEST(config_shadow, pruning_deletes_only_what_is_old_and_not_named)
{
	TempTree config(L"cfg");
	TempTree store(L"store");

	auto root = config.write(L"shell.nss", L"menu { }\n");
	CHECK(save_now(store.path, root, { root }));
	auto named = manifest_of(store.path).generation;

	// Entries pruning is allowed to take once they are old.
	auto old_generation = shadow::join(store.path, L"aaaaaaaaaaaaaaaa");
	auto old_staging = shadow::join(store.path, L"aaaaaaaaaaaaaaaa.1-2.tmp");
	auto old_manifest = shadow::join(store.path, L"manifest.1-2.staging");
	// ... the same, but young.
	auto young_generation = shadow::join(store.path, L"bbbbbbbbbbbbbbbb");
	auto young_staging = shadow::join(store.path, L"bbbbbbbbbbbbbbbb.1-2.tmp");
	auto young_manifest = shadow::join(store.path, L"manifest.3-4.staging");
	// ... and things that are not ours at any age.
	auto user_file = shadow::join(store.path, L"notes.txt");
	auto user_directory = shadow::join(store.path, L"imports");
	auto not_a_generation = shadow::join(store.path, L"gggggggggggggggg");

	for(auto directory : { old_generation, old_staging, young_generation, young_staging,
						   user_directory, not_a_generation })
	{
		shadow::ensure_directory(directory);
		shadow::write_all(shadow::join(directory, L"x.nss"), L"x");
	}
	shadow::write_all(old_manifest, L"x");
	shadow::write_all(young_manifest, L"x");
	shadow::write_all(user_file, L"x");

	for(auto path : { old_generation, old_staging, old_manifest, user_file, user_directory,
					  not_a_generation, shadow::join(store.path, named) })
		CHECK(set_age(path, 2));

	shadow::prune(store.path, L"cccccccccccccccc");

	CHECK_MSG(!exists(old_generation), "an old unreferenced generation goes");
	CHECK_MSG(!exists(old_staging), "so does an old staging directory, contents and all");
	CHECK_MSG(!exists(old_manifest), "and an old staging manifest");
	CHECK_MSG(exists(young_generation), "nothing younger than the threshold is touched");
	CHECK(exists(young_staging));
	CHECK(exists(young_manifest));
	CHECK_MSG(exists(shadow::join(store.path, named)), "the generation the manifest names stays however old");
	CHECK_MSG(exists(user_file) && exists(user_directory) && exists(not_a_generation),
			  "names this code does not create are not its to delete");

	// And a caller's own commit is kept even when the manifest names another.
	shadow::ensure_directory(shadow::join(store.path, L"dddddddddddddddd"));
	CHECK(set_age(shadow::join(store.path, L"dddddddddddddddd"), 2));
	shadow::prune(store.path, L"dddddddddddddddd");
	CHECK(exists(shadow::join(store.path, L"dddddddddddddddd")));

	// With no readable manifest nothing is known to be unnamed.
	::DeleteFileW(store.at(shadow::ManifestName).c_str());
	shadow::prune(store.path, L"cccccccccccccccc");
	CHECK(exists(shadow::join(store.path, named)));
}

TEST(config_shadow, pruning_removes_a_link_without_following_it)
{
	// A link in the store, named like a generation, must be deleted as the
	// entry it is. Following it would delete something outside the store.
	// Creating a symbolic link needs developer mode or a privilege; where it
	// cannot be made, the property cannot be exercised and the test says so
	// rather than failing.
	TempTree config(L"cfg");
	TempTree store(L"store");
	TempTree outside(L"outside");

	auto root = config.write(L"shell.nss", L"menu { }\n");
	CHECK(save_now(store.path, root, { root }));

	auto sentinel = outside.write(L"keep.nss", L"must survive");
	auto link = shadow::join(store.path, L"eeeeeeeeeeeeeeee");
	if(!::CreateSymbolicLinkW(link.c_str(), outside.path.c_str(),
							  SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
		return;

	CHECK(set_age(link, 2));
	shadow::prune(store.path, L"cccccccccccccccc");

	CHECK_MSG(::GetFileAttributesW(link.c_str()) == INVALID_FILE_ATTRIBUTES, "the link is gone");
	CHECK_MSG(exists(sentinel), "and what it pointed at is not");
	::RemoveDirectoryW(link.c_str());
}
