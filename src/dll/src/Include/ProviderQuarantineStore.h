#pragma once

/*
	The quarantine list, as the menu path sees it.

	src/shared/ProviderQuarantine.h is the file format and the reasoning - why a
	quarantined provider is *skipped* rather than refused at CoCreateInstance,
	why it lives in %LocalAppData%, and why the file holds GUIDs while the report
	shows hashes. This is the process-lifetime holder that turns it into an O(n)
	integer test on the menu thread.

	Loaded lazily on the first menu and re-read when the file's write time moves,
	which is checked at most once every couple of seconds. That is the same
	shape - and for the same reason - as RegistryConfig::IsRegisteredCached: a
	stat call per right-click is cheap but not free, and quarantining an
	extension is not something that needs to take effect within one right-click.
	`shell.exe -quarantine` tells the user it applies to the next menu.

	Not folded into ProviderHealth. Health is *measured*, per process, and resets
	when the process does; quarantine is *declared*, persists, and is the same
	for every host. Keeping them apart is what makes "deferred because it has
	never been quick" and "excluded because you said so" two different words in
	the report rather than one.
*/

#include <ProviderQuarantine.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		class ProviderQuarantineStore
		{
		public:
			static ProviderQuarantineStore &instance()
			{
				static ProviderQuarantineStore *store = new ProviderQuarantineStore();
				return *store;
			}

			ProviderQuarantineStore(const ProviderQuarantineStore &) = delete;
			ProviderQuarantineStore &operator=(const ProviderQuarantineStore &) = delete;

			// The one question the menu path asks. Almost always false, and
			// almost always against an empty vector, so the common cost is a
			// load and a size check.
			bool contains(uint32_t clsid_hash)
			{
				refresh_if_stale();

				std::lock_guard<std::mutex> lock(_mutex);
				for(auto hash : _hashes)
				{
					if(hash == clsid_hash)
						return true;
				}
				return false;
			}

			size_t size()
			{
				refresh_if_stale();
				std::lock_guard<std::mutex> lock(_mutex);
				return _hashes.size();
			}

			// For tests, and for a caller that has just written the file and
			// wants the next menu to see it without waiting out the interval.
			void reload()
			{
				auto loaded = Quarantine::read(current_path());

				// Missing is a real state and loads as an empty list. Failed is
				// not: keep whatever is cached and leave the stamp alone, so the
				// next check tries again rather than believing this one.
				if(!loaded.usable())
					return;

				std::vector<uint32_t> hashes;
				hashes.reserve(loaded.entries.size());
				for(const auto &entry : loaded.entries)
					hashes.push_back(entry.hash);

				// The stamp comes from the same handle as the content, so a
				// rewrite between the read and the stat cannot leave old hashes
				// cached under a new timestamp - which would freeze this process
				// on stale data until the *next* write.
				commit(std::move(hashes), loaded.write_time);
			}

			void set_path_for_testing(const std::wstring &path)
			{
				{
					std::lock_guard<std::mutex> lock(_mutex);
					_path_override = path;
					_loaded = false;
				}
				_checked_tick.store(0, std::memory_order_relaxed);
				reload();
			}

		private:
			ProviderQuarantineStore() = default;

			// How often the file's write time is looked at. Two seconds, for the
			// same reason RegistryConfig caches its registration check for two:
			// short enough that nobody notices, long enough that a burst of
			// right-clicks does not stat the file for every one.
			static constexpr uint64_t CheckIntervalMs = 2000;

			/*
				One lock scope per function, and nothing but the lock inside it.

				refresh_if_stale used to open three separate lock_guard scopes,
				two of them with a `return` inside, and reload a fourth. That is
				correct code and the analyzer cannot follow it: six warnings,
				C26110 "Caller failing to hold lock" and C26117 "Releasing unheld
				lock", at what were lines 84, 98, 144, 150, 159 and 162. R6.4's
				mutex fix added two of them and R6.5's restructuring - the half
				that would have removed all six - was not done.

				Suppressing them was never the answer. The analyzer is complaining
				that ownership is hard to see, and a reader has the same problem:
				each helper below takes the lock once, does one thing, and
				returns, so both of them can tell where it is held by looking at
				one function at a time.
			*/
			std::wstring current_path()
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return _path_override.empty() ? Quarantine::default_path() : _path_override;
			}

			// Has the interval elapsed? Also the only place _checked_tick is
			// compared, so "how recently did anyone look" has one reader.
			bool checked_recently(uint64_t now)
			{
				auto last = _checked_tick.load(std::memory_order_relaxed);
				std::lock_guard<std::mutex> lock(_mutex);
				return _loaded && last != 0 && (now - last) < CheckIntervalMs;
			}

			// Records that the file was looked at, and answers whether it has
			// moved since the cached copy was taken.
			bool unchanged_since(uint64_t now, uint64_t stamp)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_checked_tick.store(now, std::memory_order_relaxed);
				return _loaded && stamp == _stamp;
			}

			void commit(std::vector<uint32_t> hashes, uint64_t stamp)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_hashes.swap(hashes);
				_loaded = true;
				_checked_tick.store(::GetTickCount64(), std::memory_order_relaxed);
				_stamp = stamp;
			}

			// A cheap "has it moved" probe, deliberately not the coherent read:
			// reload() takes content and stamp from one handle, and this only
			// decides whether to call it.
			static uint64_t write_time(const std::wstring &path)
			{
				if(path.empty())
					return 0;

				WIN32_FILE_ATTRIBUTE_DATA data{};
				if(!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
					return 0;	// missing is a real state, and it is state zero

				ULARGE_INTEGER value{};
				value.LowPart = data.ftLastWriteTime.dwLowDateTime;
				value.HighPart = data.ftLastWriteTime.dwHighDateTime;
				return value.QuadPart;
			}

			void refresh_if_stale()
			{
				auto now = ::GetTickCount64();
				if(checked_recently(now))
					return;

				if(unchanged_since(now, write_time(current_path())))
					return;

				reload();
			}

			mutable std::mutex _mutex;
			std::vector<uint32_t> _hashes;
			std::wstring _path_override;
			std::atomic<uint64_t> _checked_tick{ 0 };
			uint64_t _stamp{};
			bool _loaded{};
		};
	}
}
