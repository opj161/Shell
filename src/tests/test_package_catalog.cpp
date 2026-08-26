#include "test.h"

#include "Include/PackageCatalogService.h"

#include <thread>
#include <vector>

// CatalogStore is the whole policy of the packaged-verb catalog: what counts
// as stale, who is allowed to start a scan, and what happens to a scan whose
// answer arrived after the machine changed underneath it. No threads and no
// clock of its own, so all of that is testable directly.
//
// What it replaced: a 30 s TTL where whichever caller found the TTL expired
// performed the rescan itself, on the menu thread, before the first pixel -
// 244 manifests read and parsed, measured at 111 ms cold.
// docs/refactor/02-first-paint-latency.md section 1.

using Nilesoft::Shell::CatalogStore;
using Nilesoft::Shell::ExplorerCommandRegistration;

namespace
{
	// A recognisable payload, so a test can tell one published scan from
	// another without caring what a registration contains.
	std::vector<ExplorerCommandRegistration> payload(uint32_t marker)
	{
		std::vector<ExplorerCommandRegistration> out;
		ExplorerCommandRegistration reg;
		reg.clsid.Data1 = marker;
		out.push_back(reg);
		return out;
	}

	uint32_t marker_of(const std::shared_ptr<const Nilesoft::Shell::CatalogSnapshot> &s)
	{
		if(!s || s->commands.empty())
			return 0;
		return s->commands.front().clsid.Data1;
	}

	// Claim, scan, publish - what the worker thread does, minus the scanning.
	bool run_one_scan(CatalogStore &store, uint64_t now, uint32_t marker)
	{
		uint64_t token = 0;
		if(!store.claim_refresh(now, &token))
			return false;
		return store.publish(payload(marker), now, token);
	}
}

TEST(package_catalog, nothing_published_means_a_scan_is_due)
{
	CatalogStore store;
	CHECK(!store.current());

	uint64_t token = 0;
	CHECK(store.claim_refresh(1000, &token));
	store.abandon_refresh();
}

TEST(package_catalog, a_fresh_snapshot_does_not_trigger_a_scan)
{
	CatalogStore store;
	store.set_ttl(1000);
	CHECK(run_one_scan(store, 5000, 0xAA));

	uint64_t token = 0;
	CHECK_MSG(!store.claim_refresh(5500, &token),
			  "within the TTL, asking for the catalog must cost nothing");
	CHECK_EQ((long long)marker_of(store.current()), 0xAA);
}

TEST(package_catalog, an_expired_snapshot_is_still_served_while_it_refreshes)
{
	// The point of the whole design: expiry queues work, it does not withhold
	// an answer. A caller past the TTL gets the old catalog immediately.
	CatalogStore store;
	store.set_ttl(1000);
	CHECK(run_one_scan(store, 5000, 0xAA));

	uint64_t token = 0;
	CHECK(store.claim_refresh(6001, &token));
	CHECK_MSG((long long)marker_of(store.current()) == 0xAA,
			  "the previous catalog is still there while the rescan runs");

	CHECK(store.publish(payload(0xBB), 6100, token));
	CHECK_EQ((long long)marker_of(store.current()), 0xBB);
}

TEST(package_catalog, refreshes_coalesce)
{
	CatalogStore store;
	store.set_ttl(1000);
	CHECK(run_one_scan(store, 5000, 0xAA));

	uint64_t first = 0, second = 0;
	CHECK(store.claim_refresh(6001, &first));
	CHECK_MSG(!store.claim_refresh(6002, &second),
			  "a second caller must not start a second scan of the same thing");
	CHECK(store.refreshing());

	CHECK(store.publish(payload(0xBB), 6100, first));
	CHECK(!store.refreshing());
}

TEST(package_catalog, a_failed_scan_leaves_the_old_catalog_in_place)
{
	// An empty catalog would remove every packaged verb from the menu. Keeping
	// yesterday's answer is strictly better than publishing nothing.
	CatalogStore store;
	store.set_ttl(1000);
	CHECK(run_one_scan(store, 5000, 0xAA));

	uint64_t token = 0;
	CHECK(store.claim_refresh(6001, &token));
	store.abandon_refresh();

	CHECK_EQ((long long)marker_of(store.current()), 0xAA);
	CHECK_MSG(!store.refreshing(), "the in-flight slot must be released or nothing ever scans again");

	uint64_t again = 0;
	CHECK_MSG(store.claim_refresh(6002, &again), "and the retry is allowed immediately");
}

TEST(package_catalog, a_scan_overtaken_by_a_change_does_not_publish)
{
	// invalidate() means the package set changed. A scan that started before
	// that describes the machine as it was, so its answer is dropped.
	CatalogStore store;
	store.set_ttl(1000000);
	CHECK(run_one_scan(store, 5000, 0xAA));

	uint64_t token = 0;
	CHECK(store.claim_refresh(5001, &token) == false);	// still fresh

	store.invalidate();
	CHECK_MSG(store.claim_refresh(5002, &token),
			  "invalidate outranks the TTL");

	store.invalidate();									// changed again mid-scan
	CHECK_MSG(!store.publish(payload(0xBB), 5003, token),
			  "the result describes a machine that no longer exists");
	CHECK_EQ((long long)marker_of(store.current()), 0xAA);

	// And the slot is free, so the retry can run.
	uint64_t retry = 0;
	CHECK(store.claim_refresh(5004, &retry));
	CHECK(store.publish(payload(0xCC), 5005, retry));
	CHECK_EQ((long long)marker_of(store.current()), 0xCC);
}

TEST(package_catalog, invalidate_does_not_discard_what_is_published)
{
	CatalogStore store;
	CHECK(run_one_scan(store, 5000, 0xAA));
	store.invalidate();
	CHECK_MSG((long long)marker_of(store.current()) == 0xAA,
			  "a menu opening during a package install still gets a catalog");
}

TEST(package_catalog, a_reader_keeps_the_snapshot_it_started_with)
{
	// Menus hold their catalog for as long as they are open. Publishing must
	// not pull it out from under one.
	CatalogStore store;
	store.set_ttl(1000);
	CHECK(run_one_scan(store, 5000, 0xAA));

	auto held = store.current();
	CHECK(run_one_scan(store, 9000, 0xBB));

	CHECK_EQ((long long)marker_of(held), 0xAA);
	CHECK_EQ((long long)marker_of(store.current()), 0xBB);
	CHECK_EQ((long long)held->generation, 1);
	CHECK_EQ((long long)store.current()->generation, 2);
}

TEST(package_catalog, a_clock_that_goes_backwards_costs_a_scan_not_a_frozen_catalog)
{
	// GetTickCount64 does not go backwards, but the arithmetic is unsigned and
	// this is the failure that would never be noticed: a snapshot that reads as
	// permanently fresh.
	CatalogStore store;
	store.set_ttl(1000);
	CHECK(run_one_scan(store, 5000, 0xAA));

	uint64_t token = 0;
	CHECK(store.claim_refresh(4000, &token));
}

TEST(package_catalog, concurrent_callers_start_exactly_one_scan)
{
	CatalogStore store;
	store.set_ttl(1000);
	CHECK(run_one_scan(store, 5000, 0xAA));

	std::atomic<int> claims{ 0 };
	std::vector<std::thread> threads;
	for(int i = 0; i < 8; i++)
	{
		threads.emplace_back([&store, &claims]
		{
			uint64_t token = 0;
			if(store.claim_refresh(9000, &token))
				claims++;
		});
	}
	for(auto &t : threads)
		t.join();

	CHECK_EQ(claims.load(), 1);
}

// ---- the package half of the same snapshot ------------------------------
//
// The scan already enumerated every installed package and resolved every
// install path in the course of finding packaged verbs, and then threw both
// away - so PackageIndex enumerated the same registry again, on the menu
// thread, to answer package.exists(). Publishing them together is what turns
// that scan into a read. docs/refactor/09-remediation-plan.md R3.

namespace
{
	std::vector<Nilesoft::Shell::PackageEntry> package_payload(const wchar_t *name,
															   const wchar_t *path)
	{
		std::vector<Nilesoft::Shell::PackageEntry> out;
		Nilesoft::Shell::PackageEntry entry;
		entry.identity.full_name = name;
		entry.identity.name = name;
		entry.install_path = path;
		out.push_back(std::move(entry));
		return out;
	}
}

TEST(package_catalog, a_publish_carries_packages_as_well_as_commands)
{
	CatalogStore store;

	uint64_t token = 0;
	CHECK(store.claim_refresh(1000, &token));
	CHECK(store.publish(payload(0xAA),
						package_payload(L"Microsoft.WindowsTerminal_1.0_x64__abc",
										L"C:\\Program Files\\WindowsApps\\wt"),
						1000, token));

	auto snapshot = store.current();
	CHECK(snapshot != nullptr);
	CHECK_EQ(snapshot->packages.size(), (size_t)1);
	CHECK(snapshot->packages[0].identity.full_name
		  == L"Microsoft.WindowsTerminal_1.0_x64__abc");

	// The path is the one the scan resolved. Asking GetPackagePathByFullName
	// again on the menu thread was the cost this removes.
	CHECK(snapshot->packages[0].install_path
		  == L"C:\\Program Files\\WindowsApps\\wt");
}

TEST(package_catalog, both_halves_are_replaced_together_by_a_later_scan)
{
	CatalogStore store;

	uint64_t token = 0;
	CHECK(store.claim_refresh(1000, &token));
	CHECK(store.publish(payload(0xAA), package_payload(L"One_1.0_x64__abc", L"C:\\one"),
						1000, token));

	auto first = store.current();

	CHECK(store.claim_refresh(1000 + CatalogStore::DefaultTtlMs + 1, &token));
	CHECK(store.publish(payload(0xBB), package_payload(L"Two_1.0_x64__abc", L"C:\\two"),
						1000 + CatalogStore::DefaultTtlMs + 1, token));

	auto second = store.current();

	// The older snapshot is untouched - a menu holding it keeps a consistent
	// view of both halves, which is the whole reason they share a publish.
	CHECK_EQ(first->packages.size(), (size_t)1);
	CHECK(first->packages[0].identity.full_name == L"One_1.0_x64__abc");
	CHECK(second->packages[0].identity.full_name == L"Two_1.0_x64__abc");
	CHECK_EQ(marker_of(second), 0xBBu);
}

// A discarded scan must not leave half of itself behind either.
TEST(package_catalog, a_scan_invalidated_while_running_publishes_neither_half)
{
	CatalogStore store;

	uint64_t token = 0;
	CHECK(store.claim_refresh(1000, &token));
	store.invalidate();
	CHECK(!store.publish(payload(0xCC), package_payload(L"Late_1.0_x64__abc", L"C:\\late"),
						 1000, token));

	CHECK(store.current() == nullptr);
}

// ---- the wiring, not just the primitives -------------------------------
//
// Everything above tests CatalogStore, and it always passed. D3 lived one level
// up, in the worker loop: abandon_refresh() existed, carried exactly the right
// reasoning on it - "an empty catalog would remove every packaged verb from the
// menu, which is worse than an old one" - was tested by the case above, and had
// no production caller. The loop published every scan, including one that had
// failed to read the registry at all.
//
// What that cost: scan_package_catalog returns an empty CatalogScan when
// RegistryPackageSource::enumerate_full_names fails, publish() stamps it
// built_at = now, and DefaultTtlMs is five minutes. So a transient registry
// error made package.exists() answer a confident false about every package for
// five minutes - and the stock configuration asks
// package.exists("WindowsTerminal") on every single menu
// (src/bin/imports/terminal.nss). The item would simply not be there, with
// nothing anywhere saying why.
//
// The loop is refresh_catalog() now, with the clock and the scan injected, so
// these run it.

namespace
{
	using Nilesoft::Shell::CatalogScan;

	CatalogScan good_scan(uint32_t marker)
	{
		CatalogScan scan;
		scan.commands = payload(marker);
		scan.ok = true;
		return scan;
	}

	// What scan_package_catalog returns when it could not read the machine:
	// empty, and indistinguishable from "nothing is installed" except for ok.
	CatalogScan failed_scan()
	{
		return CatalogScan{};
	}
}

TEST(package_catalog, a_failed_scan_is_abandoned_rather_than_published)
{
	CatalogStore store;
	store.set_ttl(1000);
	CHECK(run_one_scan(store, 5000, 0xAA));

	int scans = 0;
	Nilesoft::Shell::refresh_catalog(
		store,
		[] { return (uint64_t)6001; },
		[&scans] { scans++; return failed_scan(); });

	CHECK_EQ(scans, 1);

	// Yesterday's answer survives. Publishing the empty one would have replaced
	// it and stamped it fresh.
	CHECK_EQ((long long)marker_of(store.current()), 0xAA);
	CHECK_MSG(!store.refreshing(), "the slot must be released or nothing ever scans again");
}

TEST(package_catalog, a_failed_scan_does_not_hold_the_catalog_for_a_ttl)
{
	// The half that makes the previous test matter. Publishing a failed scan
	// also reset built_at, so the next needs_refresh() said no for a whole TTL.
	// Abandoning leaves the old timestamp alone, so the very next kick rescans.
	CatalogStore store;
	store.set_ttl(1000);
	CHECK(run_one_scan(store, 5000, 0xAA));

	Nilesoft::Shell::refresh_catalog(store, [] { return (uint64_t)6001; },
									 [] { return failed_scan(); });

	CHECK_MSG(store.needs_refresh(6002),
			  "a failed attempt must leave the catalog as stale as it found it");
}

TEST(package_catalog, a_failed_scan_then_a_good_one_publishes_normally)
{
	CatalogStore store;
	store.set_ttl(1000);
	CHECK(run_one_scan(store, 5000, 0xAA));

	Nilesoft::Shell::refresh_catalog(store, [] { return (uint64_t)6001; },
									 [] { return failed_scan(); });
	CHECK_EQ((long long)marker_of(store.current()), 0xAA);

	Nilesoft::Shell::refresh_catalog(store, [] { return (uint64_t)6002; },
									 [] { return good_scan(0xBB); });
	CHECK_EQ((long long)marker_of(store.current()), 0xBB);
}

TEST(package_catalog, the_first_scan_of_a_process_failing_publishes_nothing_at_all)
{
	// Nothing has ever been published, so there is no old answer to fall back
	// on. Publishing the empty scan here is the worst version of D3: it turns
	// "we do not know yet" into "there are no packages", permanently as far as
	// the next five minutes are concerned.
	CatalogStore store;
	store.set_ttl(1000);

	Nilesoft::Shell::refresh_catalog(store, [] { return (uint64_t)5000; },
									 [] { return failed_scan(); });

	CHECK_MSG(!store.current(),
			  "no snapshot is 'unknown'; an empty snapshot would be 'none'");
	CHECK(store.needs_refresh(5001));
}

TEST(package_catalog, a_scan_overtaken_by_a_change_is_retried_within_the_same_kick)
{
	// The reason refresh_catalog loops. invalidate() during a scan discards its
	// result, and the machine state that result described is already gone - so
	// the kick that is already awake rescans rather than waiting for another.
	CatalogStore store;
	store.set_ttl(1000000);

	int scans = 0;
	Nilesoft::Shell::refresh_catalog(
		store,
		[] { return (uint64_t)5000; },
		[&store, &scans]
		{
			scans++;
			if(scans == 1)
				store.invalidate();		// arrives while the first scan is running
			return good_scan(0xCC);
		});

	CHECK_EQ(scans, 2);
	CHECK_EQ((long long)marker_of(store.current()), 0xCC);
}

TEST(package_catalog, a_machine_changing_faster_than_it_can_be_scanned_does_not_spin)
{
	CatalogStore store;
	store.set_ttl(1000000);

	int scans = 0;
	Nilesoft::Shell::refresh_catalog(
		store,
		[] { return (uint64_t)5000; },
		[&store, &scans]
		{
			scans++;
			store.invalidate();			// every scan is overtaken
			return good_scan(0xDD);
		});

	CHECK_EQ(scans, 4);
	CHECK_MSG(!store.current(), "nothing was ever current enough to publish");
	CHECK_MSG(!store.refreshing(), "and the slot is not left claimed");
}
