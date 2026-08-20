// Package identity index.
//
// Two things are being protected here. The cost model: answering
// package.exists() must not resolve a single display name, because that used to
// mean opening every package key, reading every DisplayName and walking the
// MrtCache tree on the menu-building thread. And the parse: the previous
// implementation split the full name on '_' with a splitter that collapsed the
// empty ResourceId field, produced three fields where four were required, and
// left family/version/id empty - so find() never matched and the stock Windows
// Terminal item never appeared.
//
// The source is faked so the assertions do not depend on what happens to be
// installed on the machine running the suite.

#include "test.h"

#include "..\dll\src\Include\Packages.h"

#include <thread>

using namespace Nilesoft::Shell;

namespace
{
	struct CountingSource : IPackageSource
	{
		std::vector<std::wstring> names;
		int enumerations = 0;
		int path_calls = 0;
		int display_calls = 0;
		bool fail_next = false;

		bool enumerate_full_names(std::vector<std::wstring> &out) override
		{
			enumerations++;
			if(fail_next)
			{
				fail_next = false;
				return false;
			}
			out = names;
			return true;
		}

		std::wstring resolve_path(const std::wstring &full_name) override
		{
			path_calls++;
			return L"C:\\Program Files\\WindowsApps\\" + full_name;
		}

		std::wstring resolve_display_name(const std::wstring &) override
		{
			display_calls++;
			return L"Windows Terminal";
		}
	};

	CountingSource sample()
	{
		CountingSource s;
		s.names = {
			L"Microsoft.WindowsTerminal_1.11.3471.0_x64__8wekyb3d8bbwe",
			L"Microsoft.WindowsCalculator_11.2103.8.0_x64__8wekyb3d8bbwe",
			L"Microsoft.Paint_11.2201.22.0_x64__8wekyb3d8bbwe",
		};
		return s;
	}
}

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

TEST(packages, exists_matches_part_of_a_name_and_resolves_nothing_else)
{
	auto source = sample();
	PackageIndex index(&source);

	CHECK(index.exists(L"WindowsTerminal"));
	CHECK(index.exists(L"windowsterminal"));	// documented as case-insensitive
	CHECK(!index.exists(L"NotInstalledAnywhere"));

	// The whole point of the change.
	CHECK_EQ(source.display_calls, 0);
	CHECK_EQ(source.path_calls, 0);
}

TEST(packages, repeated_lookups_scan_once)
{
	auto source = sample();
	PackageIndex index(&source);

	for(int i = 0; i < 10; i++)
		index.exists(L"Paint");

	CHECK_EQ(source.enumerations, 1);
}

TEST(packages, path_resolves_per_package_and_never_touches_display_names)
{
	auto source = sample();
	PackageIndex index(&source);

	auto p = index.path(L"WindowsTerminal");
	CHECK(p.has_value());
	if(p)
	{
		// An actual installation directory, not the package full name the
		// repository subkey is called.
		CHECK(p->find(L"C:\\Program Files\\WindowsApps\\") == 0);
	}

	CHECK_EQ(source.path_calls, 1);
	CHECK_EQ(source.display_calls, 0);

	// Cached.
	index.path(L"WindowsTerminal");
	CHECK_EQ(source.path_calls, 1);
}

TEST(packages, the_display_name_is_resolved_only_when_asked_for_and_only_once)
{
	auto source = sample();
	PackageIndex index(&source);

	auto n = index.display_name(L"WindowsTerminal");
	CHECK(n.has_value());
	if(n)
		CHECK(*n == L"Windows Terminal");

	CHECK_EQ(source.display_calls, 1);

	index.display_name(L"WindowsTerminal");
	CHECK_EQ(source.display_calls, 1);

	// A different package is resolved separately, not by re-reading everything.
	index.display_name(L"Paint");
	CHECK_EQ(source.display_calls, 2);
}

TEST(packages, identity_lookup_answers_the_documented_properties)
{
	auto source = sample();
	PackageIndex index(&source);

	auto id = index.find_identity(L"WindowsTerminal");
	CHECK(id.has_value());
	if(id)
	{
		CHECK(id->full_name == L"Microsoft.WindowsTerminal_1.11.3471.0_x64__8wekyb3d8bbwe");
		CHECK(id->family == L"Microsoft.WindowsTerminal_8wekyb3d8bbwe");
		CHECK(id->version == L"1.11.3471.0");
	}
	CHECK_EQ(source.display_calls, 0);
}

TEST(packages, listing_enumerates_identities_without_localised_names)
{
	auto source = sample();
	PackageIndex index(&source);

	auto all = index.all_identities();
	CHECK_EQ(all.size(), size_t(3));
	CHECK_EQ(source.display_calls, 0);
	CHECK_EQ(source.path_calls, 0);
}

TEST(packages, a_failed_scan_stays_retryable)
{
	auto source = sample();
	source.fail_next = true;

	PackageIndex index(&source);

	CHECK(!index.exists(L"WindowsTerminal"));
	CHECK_EQ(source.enumerations, 1);

	// Packages come and go; a transient failure must not be cached forever.
	CHECK(index.exists(L"WindowsTerminal"));
	CHECK_EQ(source.enumerations, 2);
}

TEST(packages, clear_lets_the_index_rebuild)
{
	auto source = sample();
	PackageIndex index(&source);

	CHECK(index.exists(L"Paint"));
	index.clear();
	CHECK(index.exists(L"Paint"));
	CHECK_EQ(source.enumerations, 2);
}

TEST(packages, concurrent_readers_do_not_repeat_the_scan)
{
	struct SlowSource : CountingSource
	{
		bool enumerate_full_names(std::vector<std::wstring> &out) override
		{
			::Sleep(30);
			return CountingSource::enumerate_full_names(out);
		}
	};

	SlowSource source;
	source.names = sample().names;

	PackageIndex index(&source);

	std::vector<std::thread> threads;
	for(int i = 0; i < 8; i++)
		threads.emplace_back([&index] { index.exists(L"WindowsTerminal"); });

	for(auto &t : threads)
		t.join();

	CHECK_EQ(source.enumerations, 1);
}

TEST(packages, an_index_without_a_source_answers_rather_than_crashes)
{
	PackageIndex index;
	CHECK(!index.exists(L"anything"));
	CHECK(!index.find_identity(L"anything").has_value());
	CHECK(!index.path(L"anything").has_value());
	CHECK(!index.display_name(L"anything").has_value());
	CHECK_EQ(index.all_identities().size(), size_t(0));
}
