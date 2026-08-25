#pragma once

/*
	The packaged-verb catalog, off the menu thread.

	What this replaces. catalog_snapshot() in ExplorerCommand.cpp held the
	registrations behind a 30 s TTL, and whichever caller happened to find the
	TTL expired performed the rescan itself - synchronously, on the thread
	between the user's right-click and the first menu pixel. The rescan
	enumerates the package repository, resolves each install path, and reads
	and parses every AppxManifest.xml it finds.

	Measured on this machine (Windows 11 26200.8875 x64, 2026-08-23), with a
	probe running exactly that sequence:

		289 packages   244 manifests read   15 with verbs   23 registrations
		cold 111.6 ms, warm 63-68 ms; the registry enumeration is 2 ms of it

	So the cost is the manifest I/O, and every thirtieth second one unlucky
	right-click paid all of it before anything was drawn. That is the R1
	violation in docs/refactor/00-master-plan.md: bounded local work only,
	before the first pixel.

	The shape here is stale-while-revalidate:

	  - snapshot() never scans and never blocks. It returns the last published
	    snapshot, and if that snapshot is past its TTL it queues a refresh.
	  - The refresh runs on one process-lifetime worker thread and publishes
	    atomically. Readers holding an older shared_ptr keep it, which is what
	    makes the swap safe with a menu open.
	  - Refreshes are coalesced: a second request while one is in flight is
	    dropped, not queued.
	  - Nothing polls. A refresh is only ever triggered by somebody asking, so
	    an idle Explorer does no work at all.

	A stale catalog is benign. It names CLSIDs to activate; an entry for a
	package that has since gone away fails activation and is skipped, which is
	the same path a broken registration already takes.

	Not here, deliberately: the on-disk cache from
	docs/refactor/02-first-paint-latency.md section 2.1 step 3. Warm-on-start
	already removes the stall, and persistence buys only the first moments
	after Explorer launch in exchange for a new file format and a new trust
	boundary - a file a medium-integrity process can write, read by every host
	that raises a menu, including elevated ones. That trade needs a
	measurement, not an assumption.
*/

#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdint>

#include "Include/ExplorerCommandCatalog.h"
#include "Include/Packages.h"

namespace Nilesoft
{
	namespace Shell
	{
		/*
			One installed package, as the NSS package and appx functions see it.

			Carried on the catalog snapshot rather than indexed separately,
			because the scan already has both halves in hand: it enumerates
			every full name and calls GetPackagePathByFullName on each one
			before reading its manifest. Throwing the path away and asking for
			it again on the menu thread bought nothing.

			docs/refactor/09-remediation-plan.md R3, and it is what makes master
			plan invariant 1 true for exists/identity/list/path: those became
			reads of this vector, and a read of a published vector cannot
			enumerate a registry.
		*/
		struct PackageEntry
		{
			PackageIdentity identity;
			std::wstring install_path;
		};

		struct CatalogSnapshot
		{
			std::vector<ExplorerCommandRegistration> commands;

			// Every installed package, with its resolved install path. Same
			// scan, same publish, same lifetime as `commands`.
			std::vector<PackageEntry> packages;

			uint64_t built_at{};		// clock reading when this was published
			uint64_t generation{};		// 1 for the first publish, then upward
		};

		/*
			Freshness bookkeeping and the published pointer. No threads, no
			registry, no clock of its own - so the whole policy is testable,
			which is where every interesting decision in this file lives.
		*/
		class CatalogStore
		{
		public:
			// Long, because a refresh is triggered by use rather than by a
			// timer: a machine nobody right-clicks on does nothing at all, and
			// a machine somebody does right-click on pays for the rescan on a
			// worker thread. Package installs are rare and, with the catalog
			// off the menu path, a stale entry costs a verb that appears a few
			// minutes late rather than a menu that freezes now.
			static constexpr uint64_t DefaultTtlMs = 5 * 60 * 1000;

			std::shared_ptr<const CatalogSnapshot> current() const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return _current;
			}

			bool has_snapshot() const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return _current != nullptr;
			}

			// Would a scan start if one were asked for? Answers without claiming
			// anything, for the reader that only needs to decide whether to
			// wake the worker.
			bool needs_refresh(uint64_t now) const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return !_refreshing && stale_locked(now);
			}

			// True when a scan should start now: nothing published yet, or what
			// is published is past its TTL or has been invalidated - and no scan
			// is already running. Returning true claims the in-flight slot, so
			// two callers cannot both start one.
			bool claim_refresh(uint64_t now, uint64_t *token = nullptr)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				if(_refreshing)
					return false;
				if(_current && !stale_locked(now))
					return false;

				_refreshing = true;
				if(token)
					*token = _invalidations;
				return true;
			}

			// Publishes the result of a claimed scan and releases the slot.
			// Returns false when the scan is discarded because the package set
			// changed while it was running: its answer describes the machine as
			// it was before that change, and the caller should scan again.
			// Commands only, for the tests that are about freshness and
			// coalescing rather than about package identities.
			bool publish(std::vector<ExplorerCommandRegistration> commands,
						 uint64_t now, uint64_t token)
			{
				return publish(std::move(commands), {}, now, token);
			}

			bool publish(std::vector<ExplorerCommandRegistration> commands,
						 std::vector<PackageEntry> packages,
						 uint64_t now, uint64_t token)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_refreshing = false;

				if(token != _invalidations)
					return false;

				auto next = std::make_shared<CatalogSnapshot>();
				next->commands = std::move(commands);
				next->packages = std::move(packages);
				next->built_at = now;
				next->generation = ++_generation;
				_current = std::move(next);
				_current_token = token;
				return true;
			}

			// Releases the slot without publishing, for a scan that failed
			// outright. What is published stays published: an empty catalog
			// would remove every packaged verb from the menu, which is worse
			// than an old one.
			void abandon_refresh()
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_refreshing = false;
			}

			// Marks what is published as needing a rescan, without discarding
			// it. A scan already in flight will not publish over the top of the
			// change that caused this.
			void invalidate()
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_invalidations++;
			}

			bool refreshing() const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return _refreshing;
			}

			uint64_t generation() const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return _generation;
			}

			void set_ttl(uint64_t milliseconds)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_ttl_ms = milliseconds;
			}

		private:
			bool stale_locked(uint64_t now) const
			{
				if(!_current)
					return true;
				// Something told us the package set changed after the scan that
				// produced this snapshot was claimed.
				if(_current_token != _invalidations)
					return true;
				// Unsigned subtraction, so a clock that goes backwards reads as
				// a very large age and costs one extra scan rather than pinning
				// the snapshot as fresh forever.
				return (now - _current->built_at) >= _ttl_ms;
			}

			mutable std::mutex _mutex;
			std::shared_ptr<const CatalogSnapshot> _current;
			bool _refreshing{};
			uint64_t _generation{};
			uint64_t _invalidations{};
			uint64_t _current_token{};
			uint64_t _ttl_ms{ DefaultTtlMs };
		};

		/*
			The store plus one worker thread. Process lifetime, no windows, no
			C++ destructor - the module is pinned for the life of the host, and
			the same reasoning as TaskbarUiaWorker applies.
		*/
		class PackageCatalogService
		{
		public:
			// The first menu in a process must not silently lose every packaged
			// verb, so if nothing has been published yet it waits this long for
			// the first scan. Comfortably over the 111 ms cold measurement, and
			// in practice the wait is zero: warm_async() starts the scan at the
			// tail of BootstrapOnce, which is before the configuration is even
			// parsed.
			static constexpr DWORD FirstScanBudgetMs = 400;

			static PackageCatalogService &instance();

			// Menu thread. Never scans here.
			std::shared_ptr<const CatalogSnapshot> snapshot();

			// Menu thread, first menu only: waits out a budget for the first
			// scan, then behaves exactly like snapshot().
			std::shared_ptr<const CatalogSnapshot> snapshot_for_menu();

			// Starts the worker and the first scan. Called from BootstrapOnce.
			void warm_async();

			void invalidate();

			CatalogStore &store() { return _store; }

		private:
			PackageCatalogService() = default;

			bool ensure_started();
			void kick();
			static DWORD WINAPI thread_main(void *self);
			void run();

			CatalogStore _store;
			std::mutex _start_mutex;
			std::atomic<bool> _started{ false };
			HANDLE _work{};			// auto-reset: a scan is wanted
			HANDLE _published{};	// manual-reset: a snapshot exists at last
		};

		// The result of one scan: the packaged verbs, and the packages they
		// were found in. Both come out of the same walk.
		struct CatalogScan
		{
			std::vector<ExplorerCommandRegistration> commands;
			std::vector<PackageEntry> packages;
		};

		// The scan itself, exposed so a probe or a test can time it.
		CatalogScan scan_package_catalog();
	}
}
