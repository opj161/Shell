#pragma once

/*
	PackagesCache, split out of Include/Cache.h.

	The split is what makes it testable, and the reason it had no tests was
	never only the singleton behind catalog(). Cache.h reaches Theme.h,
	Expression/Variable.h and Resource.h, none of which a unit test can pull in
	on its own - so a test file that wanted this class got several hundred
	errors about OpenThemeData before it got to a package. Two obstacles, and
	removing either one alone leaves the class exactly as untestable as before.

	src/tests/test_packages_cache.cpp is what this bought.
*/

#include "Include\Packages.h"
#include "Include\PackageCatalogService.h"
#include "Include\DisplayNameMemo.h"

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// For the expression engine's string type, which path() and display_name()
// return. Everything else here is std or Packages.h.
#include <System.h>

namespace Nilesoft
{
	namespace Shell
	{
		/*
			Package (MSIX/AppX) lookup for the appx()/package() NSS functions.

			A bridge to the expression evaluator's string type, and nothing more.
			It owns no index and no registry source: the answers come out of the
			catalog snapshot that PackageCatalogService publishes from its worker
			thread, which already enumerates every installed package and resolves
			every install path in the course of finding packaged verbs.

			What this replaces, and why
			---------------------------

			`PackagesCache` used to hold a `PackageIndex` and a
			`RegistryPackageSource` of its own, as a member of the immutable
			config CACHE. Three things followed, all of them live on a stock
			install:

			  - a menu-thread `package.*` evaluation could enter ensure_index()
			    and enumerate the package repository (~2 ms) or block on a
			    condition variable waiting for another thread's scan;
			  - `CACHE::clear()` called `Packages.clear()`, so every config
			    reload threw the index away - and since the config watcher landed
			    (docs/refactor/03-config-safety.md section 3a) a reload happens on
			    every save;
			  - two mechanisms answered questions about the same packages.

			And it was not a power-user path. The shipped configuration evaluates
			`package.exists("WindowsTerminal")` on every menu
			(src/bin/imports/terminal.nss line 8) and `package.path(...)` on the
			line after it.

			docs/refactor/02-first-paint-latency.md section 2.1 step 4 asked for
			exactly this: "`CACHE::clear()` stops touching packages entirely".
			docs/refactor/09-remediation-plan.md R3 is the rest of it.

			The one exception, stated rather than hidden
			-------------------------------------------

			`display_name` is the one answer the catalog scan does not already
			hold, and it is not a cheap read:
			`RegistryPackageSource::resolve_display_name` can call
			`SHLoadIndirectString`, which for a `@{PackageFullName?ms-resource:...}`
			form loads the package's `Resources.pri`
			(https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-shloadindirectstring),
			and can then walk the MrtCache tree.

			Measured over the 289 packages installed here: **5.773 ms** mean for
			the 155 whose `DisplayName` is an indirect `@{...}` reference,
			against 0.028 ms for the 134 with a plain string - and, decisively,
			a second pass in the same process costs what the first did, so
			nothing underneath is doing the remembering. An uncached call would
			pay that on every menu forever rather than once.

			So it is memoized per catalog generation, by
			Include/DisplayNameMemo.h, which carries the full measurement and
			the invalidation rule. Only `appx.name`/`package.name` reach it and
			no shipped configuration uses them, so the memo holds a handful of
			entries at most.

			It is **not** wrapped in a `MenuPerfScope`. This paragraph used to
			claim that it was, two screens above the code that says it is not -
			the same defect class R6.2 was written to remove. The reason it
			cannot be is written on `display_name` itself.
		*/
		/*
			How a PackagesCache reaches the published catalog.

			Injected rather than hard-wired, and that is the whole reason this
			class had no tests: every query went through
			PackageCatalogService::instance(), a process-wide singleton with a
			worker thread behind it, so there was nothing a test could stand in
			front of. Every other service on this branch already has this seam -
			ProviderHealth's clock, PackageIndex's IPackageSource,
			ConfigWatcher's WaitForObjects, InlineDetourApi's whole table - and
			the one class R3 rewrote from scratch is the one that did not get it.

			A plain function pointer, like InlineDetourApi, so an instance costs
			CACHE one pointer and needs no allocation. Null means the live
			service, which is what CACHE's default-constructed member wants.
		*/
		using CatalogProvider = std::shared_ptr<const CatalogSnapshot> (*)();

		// What a query miss asks for. Null means the live service; injected so
		// a test can count the asking without a worker thread behind it.
		using MissHandler = void (*)();

		/*
			How often a query that found nothing may ask for a rescan.

			docs/refactor/09-remediation-plan.md R3 step 6 required a coalesced
			refresh on a query miss, and it was never wired up - `invalidate()`
			had no production caller at all. Without it, a package installed a
			minute ago stays invisible to `package.exists` for the rest of the
			five-minute `DefaultTtlMs`, which is a visibly wrong menu.

			The reason it needs a gate rather than a plain call is that a miss
			is usually permanent: a configuration asking about a package this
			machine does not have misses on every menu, forever. Un-gated, that
			is a full package scan per menu.

			One minute, and the number is chosen rather than picked. The
			`PackageIndex` this replaced used a 30 s TTL and additionally
			re-scanned immediately on failure (Packages.cpp), so a miss there
			caused a re-enumeration at least twice as often as this does. The
			scan runs on the worker thread and the old snapshot keeps being
			served throughout, so what a permanent miss costs is one background
			scan a minute - bounded, off the menu path, and less frequent than
			the behaviour that shipped before R3.
		*/
		inline constexpr uint64_t MissRefreshIntervalMs = 60 * 1000;

		/*
			Whether a miss may ask for a rescan yet.

			Separated out with the clock passed in, because "at most once per
			interval" is the entire policy and testing it against a real
			GetTickCount64 would mean a test that sleeps for a minute.
		*/
		class MissRefreshGate
		{
		public:
			bool should_refresh(uint64_t now)
			{
				std::lock_guard<std::mutex> lock(_mutex);

				// `_asked` rather than `_last != 0`: GetTickCount64 is small
				// shortly after boot, and `now - 0 < interval` would then
				// swallow the first miss of the session - the one most likely
				// to be a package installed while this process was running.
				if(_asked && (now - _last) < MissRefreshIntervalMs)
					return false;

				_asked = true;
				_last = now;
				return true;
			}

		private:
			mutable std::mutex _mutex;
			bool _asked{};
			uint64_t _last{};
		};

		// One per process, deliberately outliving every PackagesCache: CACHE is
		// rebuilt on every config reload, and a resolved display name does not
		// stop being true because a menu file was saved. Keeping it here rather
		// than as a member is the same argument R3 made for the catalog itself.
		inline DisplayNameMemo &display_name_memo()
		{
			static DisplayNameMemo memo;
			return memo;
		}

		class PackagesCache
		{
		public:
			PackagesCache() = default;
			explicit PackagesCache(CatalogProvider provider,
								   MissHandler on_miss = nullptr) noexcept
				: _provider(provider), _on_miss(on_miss) {}

			bool exists(const wchar_t *name) const
			{
				return static_cast<bool>(find_entry(name));
			}

			std::optional<PackageIdentity> find_identity(const wchar_t *name) const
			{
				if(auto found = find_entry(name))
					return found->identity;
				return std::nullopt;
			}

			// The real installation directory. The repository subkey records the
			// package full name and not a path, so it has to be resolved - which
			// the catalog scan already did, on its own thread, for every package
			// it walked. This is the published result of that.
			string path(const wchar_t *name) const
			{
				auto found = find_entry(name);
				if(found && !found->install_path.empty())
					return string(found->install_path.c_str());
				return {};
			}

			// The only entry point that touches localized resources, and the
			// one exception to "a package query is a snapshot read".
			string display_name(const wchar_t *name) const
			{
				auto found = find_entry(name);
				if(!found)
					return {};

				// Not wrapped in a MenuPerfScope, and the reason is worth
				// recording: including Include/Diagnostics/MenuPerf.h from this
				// header introduces `Nilesoft::Shell::Diagnostics` into every
				// translation unit that includes Cache.h, and inside
				// `namespace Nilesoft::Shell` an unqualified `Diagnostics` then
				// stops meaning `Nilesoft::Diagnostics` - which is what
				// Expression/FuncExpression.cpp's `Diagnostics::ShellExec::Run`
				// means. AGENTS.md, "Namespaces". The cost of this call is
				// therefore attributed to whichever phase is evaluating the
				// expression, which for a menu is `native.modify_rules`.
				//
				// Memoized against the snapshot's generation, because the call
				// costs 5.8 ms for an indirect name and does not get cheaper on
				// repetition. Include/DisplayNameMemo.h has the measurement and
				// the invalidation rule.
				auto resolved = display_name_memo().get(
					found.keep->generation, found->identity.full_name,
					[](const std::wstring &full_name)
					{
						RegistryPackageSource source;
						return source.resolve_display_name(full_name);
					});

				if(resolved.empty())
					return {};
				return string(resolved.c_str());
			}

			std::vector<PackageIdentity> all() const
			{
				std::vector<PackageIdentity> out;
				auto snapshot = catalog();
				if(!snapshot)
					return out;
				out.reserve(snapshot->packages.size());
				for(const auto &entry : snapshot->packages)
					out.push_back(entry.identity);
				return out;
			}

		private:
			/*
				The published snapshot, with the same bounded first wait the
				packaged-verb path uses.

				Not `snapshot()`, which never waits: a cold `package.exists()`
				answering *false* would take the stock configuration's Terminal
				item out of the first menu of every process - a worse defect than
				the one being fixed, and a silent one. `snapshot_for_menu()`
				waits only for the first scan, only up to its budget, and counts
				the wait (`catalog.first_wait`), which is the instrument
				docs/refactor/02-first-paint-latency.md section 2.1 step 3 used
				to decline persistence.
			*/
			std::shared_ptr<const CatalogSnapshot> catalog() const
			{
				return _provider ? _provider()
								 : PackageCatalogService::instance().snapshot_for_menu();
			}

			// Returns a pointer into a snapshot the caller does not hold. Safe
			// only because every caller here uses it and discards it within one
			// expression - see the shared_ptr kept alive for the duration below.
			struct Found
			{
				std::shared_ptr<const CatalogSnapshot> keep;
				const PackageEntry *entry{};
				const PackageEntry *operator->() const { return entry; }
				explicit operator bool() const { return entry != nullptr; }
			};

			Found find_entry(const wchar_t *name) const
			{
				Found found;
				if(!name || !*name)
					return found;

				found.keep = catalog();
				if(!found.keep)
					return found;

				for(const auto &entry : found.keep->packages)
				{
					if(package_full_name_matches(entry.identity.full_name, name))
					{
						found.entry = &entry;
						break;
					}
				}

				// R3 step 6. A name this snapshot does not hold is either a
				// package that is not installed - the usual case, and the
				// reason this is gated - or one installed since the scan, which
				// the five-minute TTL would otherwise hide for five minutes.
				// The rescan happens on the worker; this snapshot is still the
				// answer to *this* query.
				if(!found.entry)
					note_miss();

				return found;
			}

			void note_miss() const
			{
				if(!_miss.should_refresh(::GetTickCount64()))
					return;

				if(_on_miss)
					_on_miss();
				else
					PackageCatalogService::instance().invalidate();
			}

			CatalogProvider _provider{};
			MissHandler _on_miss{};

			// Mutable because the queries are const and asking a question is
			// not a modification of the answer - only of when the next scan is
			// allowed to be asked for.
			mutable MissRefreshGate _miss;
		};
	}
}
