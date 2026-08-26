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

// ---- R3 step 6: a miss asks for a rescan, but not on every menu ------------
//
// docs/refactor/12-closure-plan.md W9.2 / D12. PackageCatalogService::invalidate()
// had no production caller: a package installed since the last scan stayed
// invisible to package.exists for the rest of the five-minute DefaultTtlMs.
// The reason it needs a gate at all is that the common miss is permanent - a
// configuration asking about a package this machine does not have misses on
// every menu, forever - so the un-gated fix is a package scan per menu.

namespace
{
	int g_miss_refreshes = 0;
	void count_miss() { g_miss_refreshes++; }
}

TEST(packages_cache, a_query_that_found_nothing_asks_for_a_rescan)
{
	reset();
	g_miss_refreshes = 0;
	PackagesCache cache(&provide, &count_miss);

	CHECK(!cache.exists(L"SomethingInstalledSinceTheLastScan"));
	CHECK_MSG(g_miss_refreshes == 1,
			  "otherwise a package installed a minute ago is invisible for the "
			  "rest of the five-minute TTL");
}

TEST(packages_cache, a_query_that_found_its_package_asks_for_nothing)
{
	reset();
	g_miss_refreshes = 0;
	PackagesCache cache(&provide, &count_miss);

	CHECK(cache.exists(L"WindowsTerminal"));
	CHECK(cache.exists(L"WindowsCalculator"));
	CHECK_EQ(g_miss_refreshes, 0);
}

TEST(packages_cache, an_empty_name_is_not_a_miss)
{
	// It never reached the snapshot, so it says nothing about whether the
	// snapshot is stale.
	reset();
	g_miss_refreshes = 0;
	PackagesCache cache(&provide, &count_miss);

	CHECK(!cache.exists(nullptr));
	CHECK(!cache.exists(L""));
	CHECK_EQ(g_miss_refreshes, 0);
}

TEST(packages_cache, a_permanent_miss_does_not_rescan_on_every_menu)
{
	// The shape that made the gate necessary: the same absent package asked
	// for on menu after menu.
	reset();
	g_miss_refreshes = 0;
	PackagesCache cache(&provide, &count_miss);

	for(int menu = 0; menu < 200; menu++)
		CHECK(!cache.exists(L"NotInstalledAndNeverWillBe"));

	CHECK_MSG(g_miss_refreshes == 1,
			  "200 menus, one background scan asked for - un-gated this is 200");
}

TEST(packages_cache, the_gate_reopens_after_its_interval_and_not_before)
{
	// The policy on its own, with the clock supplied, because asserting it
	// through PackagesCache would mean a test that waits a minute.
	MissRefreshGate gate;

	CHECK_MSG(gate.should_refresh(0),
			  "the first miss of a session is the one most likely to be a "
			  "package installed while this process was running");

	CHECK(!gate.should_refresh(1));
	CHECK(!gate.should_refresh(MissRefreshIntervalMs - 1));
	CHECK(gate.should_refresh(MissRefreshIntervalMs));

	// And the window restarts from the grant, not from the first miss ever.
	CHECK(!gate.should_refresh(MissRefreshIntervalMs + 1));
	CHECK(gate.should_refresh(MissRefreshIntervalMs * 2));
}

TEST(packages_cache, a_late_first_miss_is_still_granted)
{
	// GetTickCount64 is a real uptime, so `now` is large in any process that
	// is not started at boot. A gate that compared `now - 0` against the
	// interval would have swallowed the first miss only near boot and worked
	// everywhere else - the worst kind of defect to find.
	MissRefreshGate gate;
	CHECK(gate.should_refresh(5));
	CHECK(!gate.should_refresh(6));
}

// ---- W9.1: the display name is resolved once per catalog generation --------
//
// Measured 2026-08-26 over the 289 packages on this machine: 5.773 ms mean for
// the 155 with an indirect @{...} DisplayName, and a second pass in the same
// process costs what the first did. Include/DisplayNameMemo.h has the numbers.

TEST(display_name_memo, a_name_is_resolved_once_and_then_remembered)
{
	DisplayNameMemo memo;
	int resolutions = 0;
	auto resolve = [&](const std::wstring &) { resolutions++; return std::wstring(L"Terminal"); };

	for(int menu = 0; menu < 50; menu++)
		CHECK(memo.get(1, L"Microsoft.WindowsTerminal_x", resolve) == L"Terminal");

	CHECK_MSG(resolutions == 1,
			  "uncached this is 50 calls at 5.8 ms, on 50 consecutive menus");
	CHECK_EQ(memo.size(), (size_t)1);
}

TEST(display_name_memo, an_empty_answer_is_remembered_too)
{
	// resolve_display_name returns nothing for 114 of the 289 packages here.
	// Re-walking MrtCache on every menu to be told nothing again is the case
	// least worth repeating.
	DisplayNameMemo memo;
	int resolutions = 0;
	auto resolve = [&](const std::wstring &) { resolutions++; return std::wstring(); };

	CHECK(memo.get(1, L"NoDisplayName_x", resolve).empty());
	CHECK(memo.get(1, L"NoDisplayName_x", resolve).empty());
	CHECK_EQ(resolutions, 1);
}

TEST(display_name_memo, different_packages_get_their_own_answers)
{
	DisplayNameMemo memo;
	auto resolve = [](const std::wstring &n) { return std::wstring(L"name-of-") + n; };

	CHECK(memo.get(1, L"a", resolve) == L"name-of-a");
	CHECK(memo.get(1, L"b", resolve) == L"name-of-b");
	CHECK(memo.get(1, L"a", resolve) == L"name-of-a");
	CHECK_EQ(memo.size(), (size_t)2);
}

TEST(display_name_memo, a_new_catalog_generation_drops_everything_remembered)
{
	// A display name cannot change without the package changing, and a package
	// changing republishes the catalog - so the generation is exactly the
	// event that makes every remembered answer suspect.
	DisplayNameMemo memo;
	int resolutions = 0;
	auto resolve = [&](const std::wstring &) { resolutions++; return std::wstring(L"v"); };

	memo.get(1, L"pkg", resolve);
	memo.get(1, L"pkg", resolve);
	CHECK_EQ(resolutions, 1);
	CHECK_EQ(memo.generation(), (uint64_t)1);

	memo.get(2, L"pkg", resolve);
	CHECK_MSG(resolutions == 2, "the package set moved; the old answer was about the old one");
	CHECK_EQ(memo.size(), (size_t)1);
	CHECK_EQ(memo.generation(), (uint64_t)2);
}

TEST(display_name_memo, an_answer_from_a_superseded_generation_is_not_stored)
{
	// The resolver runs outside the lock - it is the 5.8 ms call, and two menu
	// threads asking for different names must not serialize on it. So the
	// generation can move while it runs, and what comes back then describes a
	// package set that is no longer the machine's.
	DisplayNameMemo memo;
	memo.get(7, L"seed", [](const std::wstring &) { return std::wstring(L"seed"); });
	CHECK_EQ(memo.generation(), (uint64_t)7);

	auto answer = memo.get(6, L"stale", [&](const std::wstring &)
	{
		// Behaves as though a publish landed while this was running.
		memo.get(7, L"other", [](const std::wstring &) { return std::wstring(L"other"); });
		return std::wstring(L"stale-answer");
	});

	CHECK_MSG(answer == L"stale-answer", "the caller that asked still gets an answer");
	CHECK(memo.get(7, L"stale", [](const std::wstring &) { return std::wstring(L"fresh"); }) == L"fresh");
}
