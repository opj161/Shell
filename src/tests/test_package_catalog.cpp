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
