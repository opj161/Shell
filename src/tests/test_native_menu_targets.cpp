// Which native submenus a parent-moving rule needs opening.
//
// docs/refactor/04-code-health.md section 6. The wiring in ContextMenu.cpp is
// an expression evaluation and one branch; what can be *wrong* is the path
// arithmetic, and every way of getting it wrong produces a menu that still
// works and is either slower than it needs to be or silently missing a move.
//
// The rules pinned here, each with a plausible opposite:
//
//   - an ancestor of a target is needed, because there is no way to open
//     "a/b/c" without opening "a" and "a/b";
//   - a *sibling whose name shares a prefix* is not, which is the difference
//     between a segment comparison and a character one;
//   - the wildcard set is exactly the one string is_location() treats as
//     matching every level, and no more, because treating every leading
//     asterisk as a wildcard gives back the optimisation for locations that
//     are in fact literal;
//   - normalisation matches is_location()'s, or a rule that matches at menu
//     time names a submenu this never opened.

#include "test.h"

#include "..\dll\src\Include\NativeMenuTargets.h"

using namespace Nilesoft::Shell;

TEST(native_menu_targets, an_empty_set_needs_nothing)
{
	NativeTargets targets;
	CHECK(targets.empty());
	CHECK(!targets.needs(L"open with"));
	CHECK(!targets.needs(L""));
}

TEST(native_menu_targets, a_target_is_needed)
{
	NativeTargets targets;
	targets.add(L"open with");

	CHECK(targets.needs(L"open with"));
	CHECK(!targets.needs(L"send to"));
}

TEST(native_menu_targets, an_ancestor_of_a_target_is_needed)
{
	NativeTargets targets;
	targets.add(L"a/b/c");

	// There is no way to reach a/b/c without opening these on the way.
	CHECK(targets.needs(L"a"));
	CHECK(targets.needs(L"a/b"));
	CHECK(targets.needs(L"a/b/c"));

	// But nothing below it, and nothing beside it.
	CHECK(!targets.needs(L"a/b/c/d"));
	CHECK(!targets.needs(L"a/x"));
}

TEST(native_menu_targets, a_sibling_that_shares_a_name_prefix_is_not_an_ancestor)
{
	// The whole reason the prefix test is on segments. "open" and "open with"
	// are siblings; a character-wise prefix test would open every submenu whose
	// name starts with the same letters, which on a real Explorer menu is most
	// of the reason this exists.
	NativeTargets targets;
	targets.add(L"open with");

	CHECK(!targets.needs(L"open"));
	CHECK(!targets.needs(L"open w"));
	CHECK(targets.needs(L"open with"));
}

TEST(native_menu_targets, a_deeper_target_still_needs_its_shallow_ancestors_only_once)
{
	NativeTargets targets;
	targets.add(L"a/b");
	targets.add(L"a/c");

	CHECK_EQ(targets.size(), size_t(2));
	CHECK(targets.needs(L"a"));
	CHECK(targets.needs(L"a/b"));
	CHECK(targets.needs(L"a/c"));
	CHECK(!targets.needs(L"a/d"));
}

TEST(native_menu_targets, the_same_target_twice_is_stored_once)
{
	NativeTargets targets;
	targets.add(L"send to");
	targets.add(L"Send To");
	targets.add(L"  send to  ");

	// Rules are compared case-insensitively, so these are one target.
	CHECK_EQ(targets.size(), size_t(1));
}

TEST(native_menu_targets, matching_ignores_case_because_the_rules_do)
{
	NativeTargets targets;
	targets.add(L"Open With");

	CHECK(targets.needs(L"open with"));
	CHECK(targets.needs(L"OPEN WITH"));
}

TEST(native_menu_targets, the_only_wildcard_is_a_single_asterisk)
{
	// Read off is_location() rather than assumed: it returns true for exactly
	// "*", and compares everything else for equality. `*foo` would only match a
	// submenu genuinely called that.
	CHECK(NativeTargets::is_wildcard(L"*"));
	CHECK(NativeTargets::is_wildcard(L"  *  "));

	CHECK(!NativeTargets::is_wildcard(L"**"));
	CHECK(!NativeTargets::is_wildcard(L"*foo"));
	CHECK(!NativeTargets::is_wildcard(L"open with"));
	CHECK(!NativeTargets::is_wildcard(L""));
}

TEST(native_menu_targets, normalisation_follows_is_location)
{
	// Outer whitespace, then slashes at either end - the order is_location
	// trims them in.
	CHECK(NativeTargets::normalize(L"  /open with/  ") == std::wstring(L"open with"));
	CHECK(NativeTargets::normalize(L"/a/b/") == std::wstring(L"a/b"));

	// One asterisk of a '**' pair is dropped, which is what is_location does
	// before its equality test. So `**foo` names a submenu called `*foo`.
	CHECK(NativeTargets::normalize(L"**foo") == std::wstring(L"*foo"));

	// A single leading asterisk is not touched.
	CHECK(NativeTargets::normalize(L"*foo") == std::wstring(L"*foo"));

	CHECK(NativeTargets::normalize(L"Open With") == std::wstring(L"open with"));
}

TEST(native_menu_targets, an_empty_location_adds_no_target)
{
	// A location of "" means the root, which needs no descendant. Adding it
	// would put a path in the set that is a prefix of everything, turning
	// targeted discovery back into the eager walk it replaces.
	NativeTargets targets;
	targets.add(L"");
	targets.add(L"   ");
	targets.add(L"///");

	CHECK(targets.empty());
	CHECK(!targets.needs(L"anything"));
}

TEST(native_menu_targets, a_root_level_path_is_never_needed_by_accident)
{
	NativeTargets targets;
	targets.add(L"open with");

	// The root itself has an empty path. Asking about it must not come back
	// true through some prefix rule - the root is always materialised anyway,
	// and answering yes here would read as "descend into the root's parent".
	CHECK(!targets.needs(L""));
	CHECK(!targets.needs(L"   "));
}
