// Import de-duplication, driven through the real Parser against real files.
//
// The defect: `import` scanned m_imports for the file's path hash, logged
// "already imported" on a match — and then fell through and loaded the file
// anyway, because `break` only left the scan loop, not the function. So a
// diamond
//
//        root
//        /  \
//       a    b
//        \  /
//        common
//
// parsed `common` twice. Every menu, command and variable it declared was
// created twice; whichever definition landed last won. The log line said the
// import had been skipped, which is why this survived for so long: the
// diagnostic claimed the opposite of the behaviour.
//
// test_parser.cpp models import *resolution* (relative paths, cycles, depth)
// without the parser. This drives the parser itself, because the property under
// test is what the real function does after the scan loop, not what a model of
// it says should happen.

#include "test.h"

#include <pch.h>

#include "..\shared\ConfigShadow.h"

#include <string>
#include <vector>

using namespace Nilesoft;
using namespace Nilesoft::Shell;

namespace
{
	// A directory of .nss files that removes itself.
	struct TempConfigDir
	{
		std::wstring dir;
		std::vector<std::wstring> written;

		TempConfigDir()
		{
			wchar_t base[MAX_PATH]{};
			::GetTempPathW(MAX_PATH, base);

			// The long form, because the parser records the files it opened in
			// theirs and this test compares the two. On a machine whose %TEMP%
			// carries an 8.3 component - every GitHub Windows runner has
			// `C:\Users\RUNNER~1\...` - the spellings differ, so
			// `parsed.loaded[0] == root` fails and shadow::save() finds nothing
			// under the configuration directory and refuses. Both failed on the
			// first CI run this branch ever had.
			//
			// The tilde is not a usable shortcut for deciding whether to call:
			// "do not assume that you can skip calling GetLongPathName if the
			// path does not contain a tilde (~) character."
			// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getlongpathnamew
			wchar_t longer[MAX_PATH]{};
			if(auto n = ::GetLongPathNameW(base, longer, MAX_PATH); n > 0 && n < MAX_PATH)
				::wcscpy_s(base, longer);

			wchar_t unique[MAX_PATH]{};
			::swprintf_s(unique, L"%snss_imports_%lu_%llu\\", base,
						 ::GetCurrentProcessId(), (unsigned long long)::GetTickCount64());
			dir = unique;
			::CreateDirectoryW(dir.c_str(), nullptr);
		}

		~TempConfigDir()
		{
			for(const auto &name : written)
				::DeleteFileW((dir + name).c_str());
			::RemoveDirectoryW(dir.c_str());
		}

		std::wstring write(const wchar_t *name, const char *utf8)
		{
			auto full = dir + name;
			HANDLE h = ::CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
									 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if(h != INVALID_HANDLE_VALUE)
			{
				DWORD wrote = 0;
				::WriteFile(h, utf8, (DWORD)::strlen(utf8), &wrote, nullptr);
				::CloseHandle(h);
			}
			written.push_back(name);
			return full;
		}
	};

	// Parser reads its application context off Initializer::instance, which the
	// Initializer constructor publishes. Nothing else about the initializer is
	// used by the file-parsing path.
	//
	// Deliberately leaked, exactly as the DLL treats it: ~Initializer lives in
	// Initializer.cpp, whose link closure is the whole menu engine, and a
	// process-lifetime singleton has nothing to tear down here.
	void ensure_initializer()
	{
		static Initializer *one = new Initializer();
		(void)one;
	}

	// One parse of one root file.
	struct Parsed
	{
		CACHE cache;
		bool ok{};
		int error_code{};
		// Top-level declarations land one entry per declaration in
		// cache.dynamic.items. A file parsed twice contributes its declarations
		// twice, which is exactly what the defect did.
		size_t root_items{};
		// Every file the parse opened, root first. The last-known-good shadow
		// is built from this.
		std::vector<std::wstring> loaded;

		explicit Parsed(const std::wstring &path)
		{
			Parser parser{ string(path.c_str()) };
			parser.context.Cache = &cache;
			parser.context.variables.global = &cache.variables.global;
			parser.context.variables.runtime = &cache.variables.runtime;

			ok = parser.Load();
			error_code = (int)parser.Error();
			root_items = cache.dynamic.items.size();
			loaded = parser.LoadedFiles();
		}
	};
}

TEST(parser_imports, a_diamond_import_loads_the_shared_file_once)
{
	ensure_initializer();
	TempConfigDir tmp;

	tmp.write(L"common.nss", "item(title='DiamondCommon')\r\n");
	tmp.write(L"a.nss", "import 'common.nss'\r\n");
	tmp.write(L"b.nss", "import 'common.nss'\r\n");
	auto root = tmp.write(L"root.nss", "import 'a.nss'\r\nimport 'b.nss'\r\n");

	Parsed parsed(root);

	CHECK_MSG(parsed.ok, "the diamond configuration must parse");
	CHECK_EQ(parsed.error_code, 0);
	CHECK_MSG(parsed.root_items == 1,
			  "the shared file must contribute its item once, not once per path to it");
	CHECK_EQ(parsed.root_items, (size_t)1);
}

TEST(parser_imports, importing_the_same_file_twice_from_one_file_loads_it_once)
{
	ensure_initializer();
	TempConfigDir tmp;

	tmp.write(L"shared.nss", "item(title='TwiceShared')\r\n");
	auto root = tmp.write(L"root.nss", "import 'shared.nss'\r\nimport 'shared.nss'\r\n");

	Parsed parsed(root);

	CHECK_MSG(parsed.ok, "the configuration must parse");
	CHECK_EQ(parsed.error_code, 0);
	CHECK_EQ(parsed.root_items, (size_t)1);
}

TEST(parser_imports, a_deeper_diamond_still_loads_the_shared_file_once)
{
	// Two levels of nesting, so the duplicate is discovered several frames below
	// the root rather than as a sibling.
	ensure_initializer();
	TempConfigDir tmp;

	tmp.write(L"leaf.nss", "item(title='DeepLeaf')\r\n");
	tmp.write(L"mid_a.nss", "import 'leaf.nss'\r\n");
	tmp.write(L"mid_b.nss", "import 'leaf.nss'\r\n");
	tmp.write(L"top_a.nss", "import 'mid_a.nss'\r\n");
	tmp.write(L"top_b.nss", "import 'mid_b.nss'\r\n");
	auto root = tmp.write(L"root.nss", "import 'top_a.nss'\r\nimport 'top_b.nss'\r\n");

	Parsed parsed(root);

	CHECK_MSG(parsed.ok, "the nested diamond must parse");
	CHECK_EQ(parsed.root_items, (size_t)1);
}

TEST(parser_imports, distinct_files_are_all_loaded)
{
	// De-duplication must key on the file. Swallowing every import after the
	// first would also make the tests above pass.
	ensure_initializer();
	TempConfigDir tmp;

	tmp.write(L"one.nss", "item(title='ImportOne')\r\n");
	tmp.write(L"two.nss", "item(title='ImportTwo')\r\n");
	auto root = tmp.write(L"root.nss", "import 'one.nss'\r\nimport 'two.nss'\r\n");

	Parsed parsed(root);

	CHECK_MSG(parsed.ok, "the configuration must parse");
	CHECK_MSG(parsed.root_items == 2, "two distinct imports must both be loaded");
	CHECK_EQ(parsed.root_items, (size_t)2);
}

// ---------------------------------------------------------------------------
// Persisted last-known-good, end to end through the real parser.
//
// The in-memory half of last-known-good only helps a process that already had
// a generation loaded. A process that *starts* while shell.nss is broken has
// nothing in memory, init() fails, and there is no context menu - which is the
// case a user actually hits, because they save a typo and then right-click in
// the next application they open. docs/refactor/03-config-safety.md 1a and 1b.
// ---------------------------------------------------------------------------

TEST(parser_imports, the_parse_reports_every_file_it_opened)
{
	// The shadow is a mirror of this list. m_imports holds only hashes and the
	// lexer stack is unwound as the parse leaves each file, so without this
	// there is no record of what was read.
	ensure_initializer();
	TempConfigDir tmp;

	tmp.write(L"leaf.nss", "item(title='Leaf')\r\n");
	tmp.write(L"branch.nss", "import 'leaf.nss'\r\n");
	auto root = tmp.write(L"root.nss", "import 'branch.nss'\r\n");

	Parsed parsed(root);
	CHECK(parsed.ok);
	CHECK_EQ(parsed.loaded.size(), (size_t)3);
	if(parsed.loaded.size() != 3)
		return;

	CHECK_MSG(Nilesoft::ConfigShadow::equal_ignoring_case(parsed.loaded[0], root),
			  "the root comes first");
	CHECK_MSG(parsed.loaded[1].find(L"branch.nss") != std::wstring::npos,
			  "then each import in the order it was opened");
	CHECK_MSG(parsed.loaded[2].find(L"leaf.nss") != std::wstring::npos, "depth first");
}

TEST(parser_imports, a_file_imported_twice_is_reported_once)
{
	// Otherwise the shadow would copy it twice and the manifest would carry
	// two entries for one file.
	ensure_initializer();
	TempConfigDir tmp;

	tmp.write(L"shared.nss", "item(title='Shared')\r\n");
	auto root = tmp.write(L"root.nss", "import 'shared.nss'\r\nimport 'shared.nss'\r\n");

	Parsed parsed(root);
	CHECK(parsed.ok);
	CHECK_EQ(parsed.loaded.size(), (size_t)2);
}

TEST(parser_imports, a_broken_configuration_still_parses_from_its_shadow)
{
	namespace shadow = Nilesoft::ConfigShadow;

	ensure_initializer();
	TempConfigDir tmp;

	auto store = tmp.dir + L"lkg";

	// TempConfigDir writes flat files; the import lives in a subdirectory
	// on purpose, because that is what the shadow has to mirror.
	shadow::ensure_directory(tmp.dir + L"imports");

	tmp.write(L"imports\\extra.nss", "item(title='FromImport')\r\n");
	auto root = tmp.write(L"root.nss", "import 'imports/extra.nss'\r\nitem(title='FromRoot')\r\n");

	// A good configuration is parsed and shadowed.
	Parsed good(root);
	CHECK_MSG(good.ok, "the configuration must parse before it can be shadowed");
	CHECK_EQ(good.root_items, (size_t)2);
	CHECK(shadow::save(store, root, good.loaded));

	// The user saves a typo. This is what every process that starts from now
	// on will read.
	tmp.write(L"root.nss", "item(title='Broken'\r\n");

	Parsed broken(root);
	CHECK_MSG(!broken.ok, "the edited configuration must genuinely fail to parse");

	// A fresh process has nothing in memory, so it reaches for the shadow -
	// and gets the whole configuration back, imports included, because the
	// mirror preserved the layout that relative imports resolve against.
	auto resolved = shadow::resolve(store);
	CHECK(!resolved.empty());
	if(resolved.empty())
		return;

	Parsed recovered(resolved);
	CHECK_MSG(recovered.ok, "the shadow parses");
	CHECK_MSG(recovered.root_items == 2,
			  "and it is the whole configuration, not just the root file");

	// Fixing the file takes over again, and re-shadows.
	tmp.write(L"root.nss", "item(title='Fixed')\r\nitem(title='AlsoFixed')\r\nitem(title='Third')\r\n");
	Parsed fixed(root);
	CHECK(fixed.ok);
	CHECK_EQ(fixed.root_items, (size_t)3);
	CHECK(shadow::save(store, root, fixed.loaded));

	Parsed reshadowed(shadow::resolve(store));
	CHECK(reshadowed.ok);
	CHECK_MSG(reshadowed.root_items == 3, "the shadow follows the file it shadows");

	// Clean up the store; TempConfigDir only removes the flat files it wrote.
	::DeleteFileW(shadow::join(store, shadow::ManifestName).c_str());
	::DeleteFileW(shadow::join(store, L"root.nss").c_str());
	::DeleteFileW(shadow::join(shadow::join(store, L"imports"), L"extra.nss").c_str());
	::RemoveDirectoryW(shadow::join(store, L"imports").c_str());
	::RemoveDirectoryW(store.c_str());
	::DeleteFileW((tmp.dir + L"imports\\extra.nss").c_str());
	::RemoveDirectoryW((tmp.dir + L"imports").c_str());
}
/*
	The discriminator `shell.exe -check` rests on.

	Parser::Load() returns *true* when the root file could not be read - see the
	`l->length == 0` early return in Parser.cpp - because a machine with no
	shell.nss must still get a working shell rather than an error on every menu.
	A validator cannot use that as its answer: somebody who mistypes a path
	would be told their configuration is fine.

	Initializer::check() therefore treats "no file was read" as the failure,
	and reads it off LoadedFiles(), which open_root fills only once load_File
	has actually succeeded. If that ever stopped being true - if the parser
	started recording a file it failed to open - `-check` would silently go back
	to reporting a missing file as ok, and nothing else in the tree would
	notice. This is what notices.

	docs/refactor/03-config-safety.md section 1b step 4.
*/
TEST(parser_imports, a_file_that_cannot_be_read_records_no_loaded_file)
{
	ensure_initializer();
	TempConfigDir tmp;

	Parsed parsed(tmp.dir + L"there_is_no_such_file.nss");

	CHECK_MSG(parsed.loaded.empty(),
			  "a file that never opened must not be recorded as loaded");
}

// The other half of the same rule: a file that *was* read is recorded, so
// "loaded is empty" means what check() takes it to mean and nothing broader.
TEST(parser_imports, a_file_that_was_read_is_recorded_even_when_it_declares_nothing)
{
	ensure_initializer();
	TempConfigDir tmp;

	// Long enough to clear the parser's five-character minimum for a config.
	auto root = tmp.write(L"empty.nss", "// nothing here at all\r\n");

	Parsed parsed(root);

	CHECK_MSG(parsed.loaded.size() == 1,
			  "a file with no declarations is still a file that was read");
	CHECK_EQ(parsed.root_items, (size_t)0);
}

// ---------------------------------------------------------------------------
// parser_malformed - inputs that used to take explorer.exe down during config
// load rather than being reported.
//
// These live in this file, under their own suite name, because they need the
// same TempConfigDir / ensure_initializer / Parsed harness above and a second
// translation unit would construct a second Initializer.
//
// All three faults are the family AGENTS.md describes: an access violation
// inside the hook's SEH region, which catch(...) under /EHsc does not catch. The
// symptom is a menu that never appears, with explorer.exe still running and
// nothing in any log - so what each of these asserts is simply that the parse
// RETURNS. Whether the file is accepted or rejected is secondary; not faulting
// is the invariant.
// ---------------------------------------------------------------------------

// Parser::parse_func returns nullptr whenever parse_ident fails, which is any
// non-alphabetic character after the dot. parse_color dereferenced it unguarded.
TEST(parser_malformed, a_colour_with_a_non_identifier_postfix_is_reported_not_fatal)
{
	ensure_initializer();
	TempConfigDir tmp;

	auto root = tmp.write(L"colour.nss",
						  "menu(title='x')\r\n"
						  "{\r\n"
						  "\titem(title='y' image=#ff0000.5)\r\n"
						  "}\r\n");

	Parsed parsed(root);

	CHECK_MSG(parsed.loaded.size() == 1, "the parse returned rather than faulting");
	CHECK_MSG(!parsed.ok, "a non-identifier colour postfix is malformed input");
}

// Ident::equals(uint32_t) compares the LAST segment, and verify_ident's
// icon/image/img/svg arms classify any second segment as an Identifier with no
// lower bound on argc - so these reach the IDENT_FOR / IDENT_FOREACH argument
// checks with `args` empty and indexed out of range.
TEST(parser_malformed, a_wildcard_namespace_ending_in_for_does_not_index_an_empty_argument_list)
{
	ensure_initializer();
	TempConfigDir tmp;

	auto root = tmp.write(L"wildfor.nss",
						  "menu(title='x')\r\n"
						  "{\r\n"
						  "\titem(title='y' image=icon.for)\r\n"
						  "}\r\n");

	Parsed parsed(root);

	CHECK_MSG(parsed.loaded.size() == 1, "the parse returned rather than faulting");
}

TEST(parser_malformed, a_wildcard_namespace_ending_in_foreach_does_not_index_an_empty_argument_list)
{
	ensure_initializer();
	TempConfigDir tmp;

	// svg's default arm is check(argc >= 0, ...), so it accepts zero arguments
	// and this reached both args[0] and args[1].
	auto root = tmp.write(L"wildforeach.nss",
						  "menu(title='x')\r\n"
						  "{\r\n"
						  "\titem(title='y' image=svg.foreach)\r\n"
						  "}\r\n");

	Parsed parsed(root);

	CHECK_MSG(parsed.loaded.size() == 1, "the parse returned rather than faulting");
}

// The one-argument form reached args[1] with a single element present.
TEST(parser_malformed, a_foreach_with_too_few_arguments_does_not_index_past_its_list)
{
	ensure_initializer();
	TempConfigDir tmp;

	auto root = tmp.write(L"shortforeach.nss",
						  "menu(title='x')\r\n"
						  "{\r\n"
						  "\titem(title='y' image=icon.foreach(@a))\r\n"
						  "}\r\n");

	Parsed parsed(root);

	CHECK_MSG(parsed.loaded.size() == 1, "the parse returned rather than faulting");
}

// A `for` with the wrong arity is reported rather than being built.
// verify_ident requires argc == 3, so this never reaches ForStatement at all.
//
// NOTE ON WHAT THIS DOES *NOT* COVER. There is a second null dereference in
// ForStatement::Eval (Expression.cpp): Context::get_variable answers nullptr
// when the name resolves nowhere - node scope, then the Parent chain, then
// runtime/local/global all miss - and the result was passed straight to
// ->Copy(). That has been guarded, but it is NOT covered by any test here, and
// this test is deliberately not written as though it were: it was first drafted
// to claim that coverage and PASSED against the unguarded build, because
// Parsed only calls parser.Load(). Load parses; it never evaluates an item
// title, so ForStatement::Eval is never entered. Reaching it needs a harness
// that drives Context::Eval over a composed menu, which this file does not
// have.
//
// The guard is retained on the strength of the code rather than a test: the
// very next function up in the same file, VariableExpression::Eval, already
// writes `if(auto variable = context->get_variable(Id, this); variable)`, so
// nullptr is the documented-by-usage answer from that call and ForStatement is
// the one caller that did not check it.
TEST(parser_malformed, a_for_with_the_wrong_arity_is_reported_not_fatal)
{
	ensure_initializer();
	TempConfigDir tmp;

	auto root = tmp.write(L"forvar.nss",
						  "menu(title='x')\r\n"
						  "{\r\n"
						  "\titem(title=for(i, 10))\r\n"
						  "}\r\n");

	Parsed parsed(root);

	CHECK_MSG(parsed.loaded.size() == 1, "the parse returned rather than faulting");
}
