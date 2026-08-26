#include "test.h"

#include <windows.h>
#include "Include/PackagesCache.h"

// PackagesCache: the NSS package.* / appx.* functions, and what they do when the
// catalog behind them is missing rather than empty.
//
// R3 rewrote this class from a self-managing index into a snapshot reader, which
// is the right shape - the shipped configuration evaluates
// package.exists("WindowsTerminal") on every single menu
// (src/bin/imports/terminal.nss), and that must be a read rather than a scan.
// It shipped with no tests at all, and the reason it could not have any is the
// finding: catalog() named PackageCatalogService::instance(), a process-wide
// singleton with a worker thread behind it, so there was nothing to stand in
// front of. Every other service on this branch has that seam. This one now does
// too, and these are what it was hiding.

using namespace Nilesoft::Shell;

namespace
{
	// What the injected provider will hand back, and how many times it was
	// asked. The count is an assertion in its own right: a package query must
	// cost one snapshot read, not an enumeration.
	std::shared_ptr<CatalogSnapshot> g_snapshot;
	int g_calls = 0;

	std::shared_ptr<const CatalogSnapshot> provide()
	{
		g_calls++;
		return g_snapshot;
	}

	// string::c_str() answers nullptr for an empty string (AGENTS.md), and
	// `string == const wchar_t *` is ambiguous against Nilesoft::Text's overload
	// set, so comparisons go through here rather than through either trap.
	bool same(const Nilesoft::Text::string &s, const wchar_t *expected)
	{
		if(s.empty())
			return expected == nullptr || *expected == L'\0';
		return ::wcscmp(s.c_str(), expected) == 0;
	}

	PackageEntry entry(const wchar_t *full_name, const wchar_t *path)
	{
		PackageEntry e;
		parse_package_full_name(full_name, e.identity);
		e.install_path = path;
		return e;
	}

	// Two real shapes from this machine, so the name matching is exercised
	// against what the registry actually holds rather than against a tidy
	// invention.
	const wchar_t *TERMINAL =
		L"Microsoft.WindowsTerminal_1.21.3231.0_x64__8wekyb3d8bbwe";
	const wchar_t *CALC =
		L"Microsoft.WindowsCalculator_11.2402.25.0_x64__8wekyb3d8bbwe";

	void reset(bool with_snapshot = true)
	{
		g_calls = 0;
		g_snapshot.reset();
		if(!with_snapshot)
			return;

		g_snapshot = std::make_shared<CatalogSnapshot>();
		g_snapshot->packages.push_back(entry(TERMINAL, L"C:\\Program Files\\WindowsApps\\Terminal"));
		g_snapshot->packages.push_back(entry(CALC, L"C:\\Program Files\\WindowsApps\\Calc"));
		g_snapshot->built_at = 1000;
		g_snapshot->generation = 1;
	}
}

TEST(packages_cache, a_query_reads_the_published_snapshot)
{
	reset();
	PackagesCache cache(&provide);

	CHECK(cache.exists(L"WindowsTerminal"));
	CHECK(cache.exists(L"WindowsCalculator"));
	CHECK(!cache.exists(L"NotInstalledAtAll"));
}

TEST(packages_cache, the_path_comes_from_the_snapshot_rather_than_from_the_registry)
{
	reset();
	PackagesCache cache(&provide);

	// The repository subkey records a package full name, not a path, so this
	// has to be resolved somewhere. R3's whole point is that the catalog worker
	// already did it, once, off the menu thread.
	auto path = cache.path(L"WindowsTerminal");
	CHECK(!path.empty());
	CHECK(same(path, L"C:\\Program Files\\WindowsApps\\Terminal"));
}

TEST(packages_cache, one_query_is_one_snapshot_read)
{
	reset();
	PackagesCache cache(&provide);

	cache.exists(L"WindowsTerminal");
	CHECK_EQ(g_calls, 1);

	cache.path(L"WindowsTerminal");
	CHECK_EQ(g_calls, 2);

	cache.find_identity(L"WindowsTerminal");
	CHECK_EQ(g_calls, 3);
}

// The case R3 named as the worst possible outcome, and the reason catalog()
// calls snapshot_for_menu() rather than snapshot(). Nothing has been published:
// either the first scan has not finished or it failed. A *confident false* here
// takes the stock configuration's Terminal item out of the menu with nothing to
// show for it.
//
// PackagesCache cannot tell those two apart and should not try - it is the
// snapshot provider's job to wait for the first scan, which
// PackageCatalogService::snapshot_for_menu does under catalog.first_wait. What
// is pinned here is the half this class owns: no snapshot means no answer, and
// in particular no enumeration of its own.
TEST(packages_cache, no_snapshot_means_no_answer_and_no_scan_of_its_own)
{
	reset(false);
	PackagesCache cache(&provide);

	CHECK(!cache.exists(L"WindowsTerminal"));
	CHECK(cache.path(L"WindowsTerminal").empty());
	CHECK(!cache.find_identity(L"WindowsTerminal").has_value());
	CHECK(cache.all().empty());

	// Four queries, four reads, and nothing else. Before R3 this path could
	// enter ensure_index() and enumerate the package repository on the menu
	// thread, or block on another thread's scan.
	CHECK_EQ(g_calls, 4);
}

TEST(packages_cache, an_empty_snapshot_is_not_the_same_object_as_no_snapshot)
{
	// A machine with no packages installed publishes an empty snapshot, and
	// that is a real answer. The distinction only exists because the provider
	// can hand back nothing at all; if the two were ever collapsed, D3's failed
	// scan would be indistinguishable from a clean machine again.
	reset();
	g_snapshot->packages.clear();
	PackagesCache cache(&provide);

	CHECK(!cache.exists(L"WindowsTerminal"));
	CHECK(cache.all().empty());
	CHECK(g_snapshot != nullptr);
}

TEST(packages_cache, all_reports_every_published_identity)
{
	reset();
	PackagesCache cache(&provide);

	auto everything = cache.all();
	CHECK_EQ(everything.size(), (size_t)2);
}

TEST(packages_cache, an_empty_or_null_name_asks_nothing)
{
	reset();
	PackagesCache cache(&provide);

	CHECK(!cache.exists(nullptr));
	CHECK(!cache.exists(L""));

	// Rejected before the snapshot is even fetched: `if(!name || !*name)` is the
	// first line of find_entry. AGENTS.md's c_str()/wstring_view family - a
	// null name reaching a matcher is the shape that faults.
	CHECK_EQ(g_calls, 0);
}

TEST(packages_cache, a_query_holds_the_snapshot_it_read)
{
	// find_entry returns a pointer into the snapshot, and keeps the shared_ptr
	// alive for the length of the expression. Replacing the published snapshot
	// underneath a caller must not invalidate what it is holding.
	reset();
	PackagesCache cache(&provide);

	auto held = cache.path(L"WindowsCalculator");
	g_snapshot.reset();				// the worker publishes something else

	CHECK(!held.empty());
	CHECK(same(held, L"C:\\Program Files\\WindowsApps\\Calc"));
	CHECK(!cache.exists(L"WindowsCalculator"));
}

TEST(packages_cache, the_default_constructed_cache_still_uses_the_live_service)
{
	// CACHE holds one of these as a default-constructed member, so a null
	// provider has to mean the singleton rather than "answer nothing". This
	// asserts the fallback exists without asserting what the machine contains.
	PackagesCache cache;
	(void)cache.exists(L"SomethingThatIsNotInstalled");
	CHECK(true);
}
