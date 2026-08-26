#pragma once

/*
	Resolved package display names, remembered for as long as the catalog they
	were resolved against is the current one.

	Why this exists
	---------------

	docs/refactor/09-remediation-plan.md R3.5 offered two ways to give
	`appx.name` / `package.name` an answer and shipped a third: synchronous,
	named, and *uncached*, without the measurement that option (b) made a
	condition of choosing it. docs/refactor/12-closure-plan.md W9.1 required
	one of the two to be closed properly. The measurement is now made, and it
	settles it.

	Measured 2026-08-26, Windows 11 26200 x64, MSVC 14.44.35207, against the
	real RegistryPackageSource::resolve_display_name over all 289 packages
	installed on this machine (scratchpad probe, see the commit):

	    DisplayName is a plain string   n = 134   mean  0.028 ms
	    DisplayName is "@{...}"         n = 155   mean  5.773 ms

	    all 289, first pass in-process (cold)   mean 3.119 ms   worst 16.45 ms
	    all 289, second pass (warm)             mean 3.014 ms   worst  8.01 ms

	Two things follow, and the second is the one that decides it.

	  - The indirect path is not cheap. 5.8 ms is more than a tenth of
	    MENU_BUDGET_US, and this call is not inside that budget - it is charged
	    to whichever phase evaluates the expression, which for a menu is
	    `native.modify_rules`.
	  - **It does not warm up.** The second pass costs what the first did. There
	    is no MUI or MrtCache effect doing the remembering for us, so an
	    uncached call pays 5.8 ms on *every menu, forever*, not merely on the
	    first. That is what makes memoization worth its mutex rather than a
	    premature optimisation.

	SHLoadIndirectString is what the indirect path spends it on: for the
	`@{PackageFullName?ms-resource://...}` form it loads the package's
	Resources.pri and resolves against the current resource context.
	https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-shloadindirectstring

	What invalidates an entry
	-------------------------

	The catalog generation, and nothing finer. `generation` is 1 for the first
	publish and increases with each one, and a publish is how a package being
	installed or removed reaches this process. A display name cannot change
	without the package changing, and a package changing republishes the
	catalog, so a generation bump is exactly the event that makes every
	remembered answer suspect - and dropping all of them then costs at most one
	re-resolution each.

	An empty answer is remembered too. `resolve_display_name` returns nothing
	for 114 of the 289 packages here, and re-walking MrtCache on every menu to
	be told nothing again is the case least worth repeating.

	Concurrency
	-----------

	The lock is not held across the resolver. Two menu threads asking for
	different names must not serialize on a call that can take 16 ms, and the
	resolver reaches the registry and SHLoadIndirectString - so the lock is
	taken to look, released to resolve, and taken again to store. Two threads
	racing on the same name resolve it twice and store the same answer, which
	costs one redundant call and nothing else.
*/

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		class DisplayNameMemo
		{
		public:
			/*
				The remembered answer for `full_name`, resolving it once if this
				is the first time it has been asked for under `generation`.

				`resolve` is injected rather than called directly so the policy
				can be tested without a registry, and so a test can count how
				many times it actually ran - which is the whole of what this
				class is for.
			*/
			template<typename Resolve>
			std::wstring get(uint64_t generation, const std::wstring &full_name,
							 Resolve &&resolve)
			{
				{
					std::lock_guard<std::mutex> lock(_mutex);
					if(generation != _generation)
					{
						// The catalog moved. Everything remembered was
						// resolved against a package set that is no longer
						// the machine's.
						_entries.clear();
						_generation = generation;
					}
					else
					{
						for(const auto &entry : _entries)
						{
							if(entry.first == full_name)
								return entry.second;
						}
					}
				}

				// Outside the lock: this is the 5.8 ms call.
				auto resolved = resolve(full_name);

				{
					std::lock_guard<std::mutex> lock(_mutex);

					// The generation can have moved while the resolver ran, in
					// which case this answer describes the old package set and
					// is returned to this caller but not remembered.
					if(generation != _generation)
						return resolved;

					for(const auto &entry : _entries)
					{
						if(entry.first == full_name)
							return entry.second;
					}
					_entries.emplace_back(full_name, resolved);
				}

				return resolved;
			}

			// What is remembered, for the tests and for nothing else.
			size_t size() const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return _entries.size();
			}

			uint64_t generation() const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return _generation;
			}

		private:
			// A vector rather than a map: only appx.name and package.name reach
			// this, no shipped configuration uses them, and a configuration that
			// does names a handful of packages. A linear scan over that is
			// faster than hashing and is one allocation instead of many.
			mutable std::mutex _mutex;
			uint64_t _generation{};
			std::vector<std::pair<std::wstring, std::wstring>> _entries;
		};
	}
}
