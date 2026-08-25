#pragma once

/*
	MSIX/AppX package identity, lookup and (lazily) localized display names.

	The documented shape of the NSS surface this backs:

		appx.id("WindowsTerminal")      Microsoft.WindowsTerminal_1.11.3471.0_x64__8wekyb3d8bbwe
		appx.family("WindowsTerminal")  Microsoft.WindowsTerminal_8wekyb3d8bbwe
		appx.version("WindowsTerminal") 1.11.3471.0
		appx.path("WindowsTerminal")    C:\Program Files\WindowsApps\Microsoft.WindowsTerminal_...
		appx.name("WindowsTerminal")    Windows Terminal

	`packageName` may be a full name or any part of one.

	Two things drove this being split out of Cache.h.

	Cost: answering package.exists() used to enumerate every package in the
	repository, open every subkey, read every DisplayName, and for each
	indirect name fall back to walking the whole MrtCache tree - all on the
	menu-building thread with the cache mutex held. Only package.name() needs
	any of that, so display names are now resolved on demand for one package.

	Correctness: the identity was parsed by splitting the full name on '_' with
	a splitter that collapses the empty ResourceId field, so the parse produced
	three fields where the code required four and left family, version and id
	empty. find() therefore never matched anything and the stock Windows
	Terminal item never appeared - after paying for the full scan to say no.
	The full name is now parsed against its documented layout:

		Name_Version_Architecture_ResourceId_PublisherId

	Install paths come from the documented AppModel API rather than from the
	repository key, which holds the full name and not a path:
	https://learn.microsoft.com/en-us/windows/win32/api/appmodel/nf-appmodel-getpackagepathbyfullname
*/

#include <windows.h>
#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <condition_variable>

namespace Nilesoft
{
	namespace Shell
	{
		struct PackageIdentity
		{
			std::wstring full_name;		// appx.id
			std::wstring name;			// Microsoft.WindowsTerminal
			std::wstring version;		// appx.version
			std::wstring publisher;		// 8wekyb3d8bbwe
			std::wstring family;		// appx.family

			bool empty() const noexcept { return full_name.empty(); }
		};

		/*
			Does this full name answer to `query`?

			Case-insensitive substring, which is the documented NSS behaviour:
			"`packageName` may be a full name or any part of one". Exposed so the
			snapshot reader in Cache.h and PackageIndex cannot drift into two
			different ideas of what "WindowsTerminal" matches.
		*/
		bool package_full_name_matches(const std::wstring &full_name, const wchar_t *query);

		// Parses Name_Version_Architecture_ResourceId_PublisherId. ResourceId is
		// empty for almost every package, which is exactly the field the previous
		// splitter dropped. Returns false for anything that is not a full name.
		bool parse_package_full_name(const std::wstring &full_name, PackageIdentity &out);

		/*
			Where package data comes from. Split out so a test can assert what a
			query actually touches - specifically that exists() and path() never
			reach the display-name resolver.
		*/
		struct IPackageSource
		{
			virtual ~IPackageSource() = default;

			// Cheap: subkey names only, no per-package key is opened.
			virtual bool enumerate_full_names(std::vector<std::wstring> &out) = 0;

			// Both are only ever called for a package that already matched.
			virtual std::wstring resolve_path(const std::wstring &full_name) = 0;
			virtual std::wstring resolve_display_name(const std::wstring &full_name) = 0;
		};

		// The real one: package repository registry for identities, AppModel for
		// paths, DisplayName/SHLoadIndirectString/MrtCache for display names.
		class RegistryPackageSource : public IPackageSource
		{
		public:
			bool enumerate_full_names(std::vector<std::wstring> &out) override;
			std::wstring resolve_path(const std::wstring &full_name) override;
			std::wstring resolve_display_name(const std::wstring &full_name) override;
		};

		// Documented two-call buffer sizing. Empty if the package is not installed
		// for this user or the API is unavailable.
		std::wstring GetInstalledPackagePath(const std::wstring &full_name);

		// Full names of every installed package in one family, using the
		// documented count/array/shared-buffer contract:
		// https://learn.microsoft.com/en-us/windows/win32/api/appmodel/nf-appmodel-getpackagesbypackagefamily
		std::vector<std::wstring> GetPackageFullNamesByFamily(const std::wstring &family);

		/*
			Identity index with per-package lazy path and display-name resolution.

			**No production caller since 2026-08-25.** The menu path reads the
			catalog snapshot instead (Include/Cache.h, PackagesCache), which is
			the point of docs/refactor/09-remediation-plan.md R3: the same walk
			that finds packaged verbs now publishes package identities and
			install paths, so nothing has to enumerate the repository again on
			the thread between a right-click and the first pixel.

			Kept rather than deleted in that change, on this repository's own
			rule that a move and a deletion do not belong in one commit
			(docs/refactor/04-code-health.md section 4). It is still the tested
			implementation of identity matching and of display-name resolution -
			`test_packages.cpp` drives it against an injected source - and
			`RegistryPackageSource` beneath it is very much live. Removing the
			index itself is a decision for whoever next audits dead code, and
			needs the display-name path to have a home first.

			The index itself is one registry subkey enumeration. Everything more
			expensive than that is done for a single matched package, outside the
			lock, and published afterwards.
		*/
		class PackageIndex
		{
		public:
			explicit PackageIndex(IPackageSource *source = nullptr) noexcept
				: _source(source) {}

			void set_source(IPackageSource *source) noexcept { _source = source; }

			bool exists(const wchar_t *query) const;
			std::optional<PackageIdentity> find_identity(const wchar_t *query) const;
			std::optional<std::wstring> path(const wchar_t *query) const;
			std::optional<std::wstring> display_name(const wchar_t *query) const;
			std::vector<PackageIdentity> all_identities() const;

			void clear();

			/*
				Freshness.

				Once the index reached Ready it stayed there for the life of the
				process, and the process here is Explorer - which outlives any
				number of package installs, updates and uninstalls. Every package.*
				answer after the first menu was built described the machine as it
				had been at that moment, with no way back short of clearing the
				whole configuration cache.

				A time-to-live is the cheap half of the fix: the scan is one
				registry subkey enumeration, so paying for it occasionally is not
				the cost that mattered - the cost that mattered was doing it for
				every menu, which is what the index exists to avoid.

				invalidate() is the exact half, for when something knows the
				package set has changed. A generation counter means a scan already
				in flight when it is called does not publish its now-stale results.
			*/
			static constexpr uint64_t DefaultTtlMs = 30000;

			// Marks the index stale without discarding it, so a scan in progress
			// cannot publish over the top of the change that caused this.
			void invalidate();

			// Test seams. The clock is injectable so freshness can be tested
			// without a test that sleeps.
			void set_ttl(uint64_t milliseconds) noexcept { _ttl_ms = milliseconds; }
			void set_clock(uint64_t (*now)()) noexcept { _clock = now; }

		private:
			enum class State { Empty, Loading, Ready };

			static uint64_t default_clock() { return ::GetTickCount64(); }
			uint64_t now() const { return _clock ? _clock() : default_clock(); }

			struct Entry
			{
				PackageIdentity identity;
				std::wstring install_path;
				std::wstring display;
				bool path_resolved{};
				bool display_resolved{};
			};

			// Returns the index of the match, or -1. Assumes the index is ready.
			static long long match_locked(const std::vector<Entry> &list, const wchar_t *query);

			// Builds the index if needed. A second caller waits for the first
			// rather than repeating the scan; a failed build stays retryable.
			bool ensure_index() const;

			mutable std::mutex _mutex;
			mutable std::condition_variable _cv;
			mutable State _state{ State::Empty };
			mutable std::vector<Entry> _list;
			IPackageSource *_source{};

			mutable uint64_t _ready_at{};
			mutable uint64_t _generation{};
			uint64_t _ttl_ms{ DefaultTtlMs };
			uint64_t (*_clock)(){};
		};
	}
}
