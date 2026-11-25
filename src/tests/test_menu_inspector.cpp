// "Why is this here?", as text.
//
// src/dll/src/Include/MenuInspector.h. docs/refactor/05-capabilities.md
// section 7. The formatting is separated from the menu for the same reason
// Include/MenuColumns.h and Include/MenuFavorites.h are: the decision is pure
// and can be enumerated, and what is left in ContextMenu.cpp is a read of
// state it already holds.

#include "test.h"

#include <windows.h>
#include "..\dll\src\Include\MenuInspector.h"

using namespace Nilesoft::Shell;
using namespace Nilesoft::Shell::Inspector;

#define CHECK_TEXT(actual_, want_)                                             \
	CHECK_MSG((actual_) == std::wstring(want_),                                \
			  ("got " + ::nss_test::escape(std::wstring(actual_).c_str())      \
			   + ", want " + ::nss_test::escape(want_)).c_str())

TEST(menu_inspector, nothing_is_armed_until_something_arms_it)
{
	// The gate that keeps this free for every right-click that is not
	// inspecting: one thread-local bool, tested before anything else runs.
	CHECK(!active());

	arm(true);
	CHECK(active());
	arm(false);
	CHECK(!active());
}

TEST(menu_inspector, a_location_is_the_file_leaf_and_the_line)
{
	// The full path is too wide for a tooltip and the leaf is what a person
	// recognises. The line number still lands correctly once the file is open.
	auto at = RuleProvenance::at(0, 41);
	CHECK_TEXT(location(L"C:\\Program Files\\Nilesoft Shell\\shell.nss", at), L"shell.nss:41");
	CHECK_TEXT(location(L"C:\\x\\imports\\modify.nss", at), L"modify.nss:41");

	// A bare name with no directory is already the leaf.
	CHECK_TEXT(location(L"shell.nss", at), L"shell.nss:41");
}

TEST(menu_inspector, a_rule_nobody_recorded_has_no_location)
{
	// The failure this guards: printing ":41" or "shell.nss:0", either of which
	// reads as an answer rather than as its absence.
	CHECK_TEXT(location(L"C:\\x\\shell.nss", RuleProvenance{}), L"");
	CHECK_TEXT(location(nullptr, RuleProvenance::at(0, 41)), L"");
	CHECK_TEXT(location(L"", RuleProvenance::at(0, 41)), L"");
	CHECK_TEXT(location(L"C:\\x\\shell.nss", RuleProvenance::at(0, 0)), L"");
}

TEST(menu_inspector, an_item_says_where_it_came_from)
{
	Facts facts;
	facts.origin = L"your configuration";
	facts.identity = L"item:tools/terminal";
	facts.rules.push_back(L"shell.nss:41");

	CHECK_TEXT(describe(facts),
			   L"From your configuration\nRule shell.nss:41\nitem:tools/terminal");
}

TEST(menu_inspector, every_rule_that_touched_a_native_item_is_named)
{
	// A native item can be matched by several modify rules, and "which of my
	// rules did this" is the question section 7 exists to answer. Naming only
	// the first would answer it wrongly rather than partially.
	Facts facts;
	facts.origin = L"Windows";
	facts.identity = L"native:Refresh";
	facts.rules.push_back(L"shell.nss:12");
	facts.rules.push_back(L"modify.nss:88");

	CHECK_TEXT(describe(facts),
			   L"From Windows\nRule shell.nss:12, modify.nss:88\nnative:Refresh");
}

TEST(menu_inspector, an_item_no_rule_touched_says_only_where_it_came_from)
{
	// The common case for a native item on a stock configuration, and a tip
	// that padded out to a fixed shape with empty labels would read as broken.
	Facts facts;
	facts.origin = L"Windows";
	facts.identity = L"native:Paste";

	CHECK_TEXT(describe(facts), L"From Windows\nnative:Paste");
}

TEST(menu_inspector, an_item_that_cannot_be_named_still_says_what_it_is)
{
	Facts facts;
	facts.origin = L"a packaged extension";

	CHECK_TEXT(describe(facts), L"From a packaged extension");
}

TEST(menu_inspector, the_identity_is_printed_whole_because_it_is_the_pin_argument)
{
	// docs/refactor/05-capabilities.md section 1c: a diagnostic that names a
	// thing differently from the command that acts on it names it for nobody.
	// What this prints is exactly what `shell.exe -favorites:pin` takes, so it
	// is not abbreviated, elided or lower-cased on its way to the tooltip.
	Facts facts;
	facts.origin = L"a packaged extension";
	facts.identity = L"verb:{CAE3F1D4-7765-4D98-A060-52CD14D56EAB}";

	CHECK_TEXT(describe(facts),
			   L"From a packaged extension\nverb:{CAE3F1D4-7765-4D98-A060-52CD14D56EAB}");
}

TEST(menu_inspector, nothing_known_is_nothing_said)
{
	CHECK_TEXT(describe(Facts{}), L"");
}
