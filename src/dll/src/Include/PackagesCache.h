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

#include <memory>
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

			`display_name` still resolves on demand, and it is not a cheap read:
			`RegistryPackageSource::resolve_display_name` can call
			`SHLoadIndirectString`, which for a `@{PackageFullName?ms-resource:...}`
			form loads the package's `Resources.pri`
			(https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-shloadindirectstring),
			and can then walk the MrtCache tree. Only `appx.name`/`package.name`
			reach it, no shipped configuration uses them, and it is timed under
			its own phase so a report says when somebody's does.
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

		class PackagesCache
		{
		public:
			PackagesCache() = default;
			explicit PackagesCache(CatalogProvider provider) noexcept
				: _provider(provider) {}

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

			// The only entry point that touches localized resources, and the one
			// exception to "a package query is a snapshot read". See the note
			// above; the phase is what keeps it honest.
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
				RegistryPackageSource source;
				auto resolved = source.resolve_display_name(found->identity.full_name);
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
				return found;
			}

			CatalogProvider _provider{};
		};
	}
}
