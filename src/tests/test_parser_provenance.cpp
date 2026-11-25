// Rule provenance, driven through the real Parser against real files.
//
// docs/refactor/05-capabilities.md section 7 names threading file-and-line
// provenance through the parser as the rule inspector's long pole. This is what
// pins it: every NativeMenu the parser builds records which file it was written
// in and which line it opened on, and `CACHE::file_name` turns the index back
// into a path.
//
// Four of these fail for a *different* reason if the obvious shortcuts are
// taken, which is why they are separate tests:
//
//   - recording the root file for everything passes any single-file test;
//   - taking the position *after* parsing the rule passes for `item(...)` on
//     one line and points at the closing brace for every `menu(...) { ... }`;
//   - keying the file by a stack parallel to Parser::_imports desynchronises on
//     the failure path inside load_import, where a lexer is pushed and no file
//     is ever recorded;
//   - using 0 for "unknown" names the root configuration, which is a real file
//     and the commonest right answer, so a wrong answer reads as a right one.
//
// test_parser_imports.cpp drives the same machinery for de-duplication and has
// the same fixtures; they are deliberately not shared, because a test file that
// depends on another test file's helpers fails in two places at once.

#include "test.h"

#include <pch.h>

#include <string>
#include <vector>

using namespace Nilesoft;
using namespace Nilesoft::Shell;

namespace
{
	struct TempConfigDir
	{
		std::wstring dir;
		std::vector<std::wstring> written;

		TempConfigDir()
		{
			wchar_t base[MAX_PATH]{};
			::GetTempPathW(MAX_PATH, base);

			wchar_t unique[MAX_PATH]{};
			::swprintf_s(unique, L"%snss_prov_%lu_%llu\\", base,
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

	void ensure_initializer()
	{
		static Initializer *one = new Initializer();
		(void)one;
	}

	// One parse, with the file list carried into the generation exactly as
	// Initializer::load_generation does it. That copy is what makes the index
	// resolvable after the parser is gone, so the fixture performs it rather
	// than reaching into the parser at assertion time.
	struct Parsed
	{
		CACHE cache;
		bool ok{};

		explicit Parsed(const std::wstring &path)
		{
			Parser parser{ string(path.c_str()) };
			parser.context.Cache = &cache;
			parser.context.variables.global = &cache.variables.global;
			parser.context.variables.runtime = &cache.variables.runtime;

			ok = parser.Load();
			cache.files = parser.LoadedFiles();
		}

		const std::vector<NativeMenu *> &items() const { return cache.dynamic.items; }

		// The item declared at top level with this title, or nullptr. Titles
		// here are constant strings, so the rule's own title expression is not
		// evaluated - the parser stores it and the test finds the rule by
		// position instead, which is why callers index rather than search.
		const NativeMenu *item(size_t index) const
		{
			return index < cache.dynamic.items.size() ? cache.dynamic.items[index] : nullptr;
		}

		std::wstring file_of(const NativeMenu *rule) const
		{
			auto name = rule ? cache.file_name(rule->provenance) : nullptr;
			return name ? std::wstring(name) : std::wstring();
		}
	};

	bool ends_with(const std::wstring &text, const wchar_t *suffix)
	{
		std::wstring tail(suffix);
		return text.size() >= tail.size()
			&& _wcsicmp(text.c_str() + (text.size() - tail.size()), tail.c_str()) == 0;
	}
}

TEST(rule_provenance, an_unset_provenance_is_not_a_file)
{
	// Zero would be the root configuration, and NoFile is deliberately not
	// zero for that reason. A default-constructed rule must answer "nobody
	// knows" rather than "line 0 of shell.nss".
	RuleProvenance nothing;
	CHECK_MSG(!nothing.known(), "a rule nobody recorded has no provenance");
	CHECK_EQ((int)nothing.file, (int)RuleProvenance::NoFile);
	CHECK_EQ((int)nothing.line, 0);

	// And a line without a file, or a file without a line, is neither.
	CHECK(!RuleProvenance::at(0, 0).known());
	CHECK(RuleProvenance::at(0, 1).known());
	CHECK(!RuleProvenance::at(RuleProvenance::NoFile, 12).known());
	CHECK(!RuleProvenance::at((size_t)RuleProvenance::NoFile + 5000, 12).known());
}

TEST(rule_provenance, a_rule_records_the_line_it_was_written_on)
{
	ensure_initializer();
	TempConfigDir tmp;

	auto root = tmp.write(L"root.nss",
						  "\r\n"                      // 1
						  "item(title='First')\r\n"   // 2
						  "\r\n"                      // 3
						  "\r\n"                      // 4
						  "item(title='Second')\r\n"); // 5

	Parsed parsed(root);
	CHECK_MSG(parsed.ok, "the configuration must parse");
	CHECK_EQ(parsed.items().size(), (size_t)2);

	CHECK_MSG(parsed.item(0)->provenance.known(), "a parsed rule has provenance");
	CHECK_EQ((int)parsed.item(0)->provenance.line, 2);
	CHECK_EQ((int)parsed.item(1)->provenance.line, 5);

	// Both are the root file, which is index 0.
	CHECK_EQ((int)parsed.item(0)->provenance.file, 0);
	CHECK_EQ((int)parsed.item(1)->provenance.file, 0);
}

TEST(rule_provenance, a_submenu_records_where_it_opens_not_where_it_closes)
{
	// The reason `here()` is called before parse_menu_item rather than after.
	// A menu consumes its whole body, so a position taken afterwards points at
	// the line following the closing brace - which is somewhere between "one
	// line late" for a small menu and "a different rule entirely" for a large
	// one.
	ensure_initializer();
	TempConfigDir tmp;

	auto root = tmp.write(L"root.nss",
						  "menu(title='Outer')\r\n"        // 1
						  "{\r\n"                          // 2
						  "\titem(title='Inner')\r\n"      // 3
						  "\titem(title='Inner2')\r\n"     // 4
						  "}\r\n"                          // 5
						  "item(title='After')\r\n");      // 6

	Parsed parsed(root);
	CHECK_MSG(parsed.ok, "the configuration must parse");
	CHECK_EQ(parsed.items().size(), (size_t)2);

	auto outer = parsed.item(0);
	CHECK_EQ((int)outer->provenance.line, 1);
	CHECK_EQ((int)parsed.item(1)->provenance.line, 6);

	// The children carry their own lines, not their parent's.
	CHECK_EQ(outer->items.size(), (size_t)2);
	CHECK_EQ((int)outer->items[0]->provenance.line, 3);
	CHECK_EQ((int)outer->items[1]->provenance.line, 4);
}

TEST(rule_provenance, a_rule_in_an_imported_file_names_that_file)
{
	// The test that fails if provenance records "the file being parsed" as the
	// root rather than as the lexer's own.
	ensure_initializer();
	TempConfigDir tmp;

	tmp.write(L"parts.nss",
			  "\r\n"                        // 1
			  "\r\n"                        // 2
			  "item(title='FromImport')\r\n"); // 3
	auto root = tmp.write(L"root.nss",
						  "item(title='FromRoot')\r\n"   // 1
						  "import 'parts.nss'\r\n");     // 2

	Parsed parsed(root);
	CHECK_MSG(parsed.ok, "the configuration must parse");
	CHECK_EQ(parsed.items().size(), (size_t)2);

	auto from_root = parsed.item(0);
	auto from_import = parsed.item(1);

	CHECK_EQ((int)from_root->provenance.line, 1);
	CHECK_EQ((int)from_import->provenance.line, 3);

	CHECK_MSG(from_root->provenance.file != from_import->provenance.file,
			  "two files must not share one index");

	CHECK_MSG(ends_with(parsed.file_of(from_root), L"root.nss"),
			  "the root rule resolves to the root file");
	CHECK_MSG(ends_with(parsed.file_of(from_import), L"parts.nss"),
			  "the imported rule resolves to the imported file, not the root");
}

TEST(rule_provenance, the_lexer_goes_back_to_the_importing_file_when_the_import_ends)
{
	// Provenance is read off the lexer, and pop_import restores it - so a rule
	// written after an import must be attributed to the importing file again.
	// A file index tracked in a stack parallel to Parser::_imports gets this
	// right too, right up until load_import's failure path pushes a lexer for a
	// file it never records.
	ensure_initializer();
	TempConfigDir tmp;

	tmp.write(L"one.nss", "item(title='One')\r\n");
	tmp.write(L"two.nss", "item(title='Two')\r\n");
	auto root = tmp.write(L"root.nss",
						  "import 'one.nss'\r\n"          // 1
						  "import 'missing-file.nss'\r\n" // 2 - never loads
						  "import 'two.nss'\r\n"          // 3
						  "item(title='Last')\r\n");      // 4

	Parsed parsed(root);
	CHECK_MSG(parsed.ok, "a failed import must not fail the parse");
	CHECK_EQ(parsed.items().size(), (size_t)3);

	CHECK_MSG(ends_with(parsed.file_of(parsed.item(0)), L"one.nss"), "first import");
	CHECK_MSG(ends_with(parsed.file_of(parsed.item(1)), L"two.nss"),
			  "the import that failed in between must not shift the next file's index");
	CHECK_MSG(ends_with(parsed.file_of(parsed.item(2)), L"root.nss"),
			  "after the imports, rules belong to the importing file again");
	CHECK_EQ((int)parsed.item(2)->provenance.line, 4);
}

TEST(rule_provenance, a_modify_rule_records_where_it_was_written)
{
	// `modify` rules land in cache.statics rather than in the menu tree, and
	// they are the rules a user is most often surprised by - the inspector's
	// "matched modify/moveto rule locations" is about these.
	ensure_initializer();
	TempConfigDir tmp;

	auto root = tmp.write(L"root.nss",
						  "\r\n"                                        // 1
						  "modify(find='Refresh' menu='')\r\n"          // 2
						  "\r\n"                                        // 3
						  "modify(find='Paste' vis=remove)\r\n");       // 4

	Parsed parsed(root);
	CHECK_MSG(parsed.ok, "the configuration must parse");
	CHECK_EQ(parsed.cache.statics.size(), (size_t)2);

	CHECK_MSG(parsed.cache.statics[0]->provenance.known(), "a modify rule has provenance");
	CHECK_EQ((int)parsed.cache.statics[0]->provenance.line, 2);
	CHECK_EQ((int)parsed.cache.statics[1]->provenance.line, 4);
	CHECK_MSG(ends_with(parsed.file_of(parsed.cache.statics[0]), L"root.nss"),
			  "and resolves to the file it was written in");
}

TEST(rule_provenance, an_index_past_the_file_list_resolves_to_nothing)
{
	// file_name answers nullptr rather than an empty string, so a caller cannot
	// print ":41" and pass it off as a location. The index can outrun the list
	// only if a rule and a generation are ever mismatched, which is exactly the
	// case that must not produce a plausible-looking answer.
	CACHE cache;
	cache.files.push_back(L"C:\\one.nss");

	CHECK(cache.file_name(RuleProvenance::at(0, 7)) != nullptr);
	CHECK(cache.file_name(RuleProvenance::at(1, 7)) == nullptr);
	CHECK(cache.file_name(RuleProvenance{}) == nullptr);
}
