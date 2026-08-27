// Package identity parsing and the shared name matcher.
//
// The parse: the previous implementation split the full name on '_' with a
// splitter that collapsed the empty ResourceId field, produced three fields
// where four were required, and left family/version/id empty - so find() never
// matched and the stock Windows Terminal item never appeared.
//
// The cost model this file used to guard as well - answering package.exists()
// must not resolve a single display name - is now pinned where the answering
// happens: the packages_cache, display_name_memo and package_catalog suites.

#include "test.h"

#include "..\dll\src\Include\Packages.h"

using namespace Nilesoft::Shell;

TEST(packages, a_full_name_parses_into_its_documented_fields)
{
	PackageIdentity id;
	CHECK(parse_package_full_name(
		L"Microsoft.WindowsTerminal_1.11.3471.0_x64__8wekyb3d8bbwe", id));

	// The empty ResourceId field is exactly what the old splitter dropped.
	CHECK(id.name == L"Microsoft.WindowsTerminal");
	CHECK(id.version == L"1.11.3471.0");
	CHECK(id.publisher == L"8wekyb3d8bbwe");
	CHECK(id.family == L"Microsoft.WindowsTerminal_8wekyb3d8bbwe");
	CHECK(id.full_name == L"Microsoft.WindowsTerminal_1.11.3471.0_x64__8wekyb3d8bbwe");
}

TEST(packages, a_non_empty_resource_id_still_parses)
{
	PackageIdentity id;
	CHECK(parse_package_full_name(
		L"Contoso.App_2.0.0.0_x64_en-us_abcdefghijklm", id));

	CHECK(id.name == L"Contoso.App");
	CHECK(id.version == L"2.0.0.0");
	CHECK(id.publisher == L"abcdefghijklm");
	CHECK(id.family == L"Contoso.App_abcdefghijklm");
}

TEST(packages, garbage_is_refused_rather_than_half_parsed)
{
	PackageIdentity id;
	CHECK(!parse_package_full_name(L"", id));
	CHECK(!parse_package_full_name(L"NoUnderscores", id));
	CHECK(!parse_package_full_name(L"OnlyOne_Underscore", id));
	CHECK(!parse_package_full_name(L"_leading", id));
	CHECK(!parse_package_full_name(L"trailing_a_", id));
}

// ---- the snapshot the menu path actually reads --------------------------
//
// What the *menu* asks - exists, identity, list, path - is answered out of the
// catalog snapshot, because the index that used to answer it lived in the
// immutable config CACHE and could enumerate the package repository on the
// thread between a right-click and the first pixel. That index is now deleted;
// this file keeps the two pieces of it the snapshot path still depends on. It
// was not a power-user path either: the shipped configuration evaluates
// package.exists("WindowsTerminal") on every menu
// (src/bin/imports/terminal.nss line 8).
//
// docs/refactor/09-remediation-plan.md R3, docs/refactor/02 section 2.1 step 4.

TEST(packages, a_full_name_and_its_path_come_out_of_one_walk)
{
	// What scan_package_catalog now publishes: the identity parse and the
	// resolved install path, from the enumeration it was doing anyway.
	PackageIdentity identity;
	CHECK(parse_package_full_name(
		L"Microsoft.WindowsTerminal_1.11.3471.0_x64__8wekyb3d8bbwe", identity));

	CHECK(identity.name == L"Microsoft.WindowsTerminal");
	CHECK(identity.version == L"1.11.3471.0");
	CHECK(identity.family == L"Microsoft.WindowsTerminal_8wekyb3d8bbwe");
}

// The matcher the snapshot reader uses, pinned to its documented behaviour:
// "packageName may be a full name or any part of one". One shared function, so
// "WindowsTerminal" cannot come to mean two different things depending on who
// answers it.
TEST(packages, the_snapshot_matcher_is_the_index_matcher)
{
	std::wstring full = L"Microsoft.WindowsTerminal_1.11.3471.0_x64__8wekyb3d8bbwe";

	CHECK(package_full_name_matches(full, L"WindowsTerminal"));
	CHECK(package_full_name_matches(full, L"windowsterminal"));		// case-insensitive
	CHECK(package_full_name_matches(full, full.c_str()));
	CHECK(package_full_name_matches(full, L"8wekyb3d8bbwe"));		// any part

	CHECK(!package_full_name_matches(full, L"Notepad"));
	CHECK(!package_full_name_matches(full, L""));
	CHECK(!package_full_name_matches(full, nullptr));

	// A query longer than the name cannot be contained in it.
	CHECK(!package_full_name_matches(L"short", L"a much longer query than that"));
}
