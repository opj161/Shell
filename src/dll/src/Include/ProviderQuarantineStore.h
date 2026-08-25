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
				// Under the lock, like every other read of it. refresh_if_stale
				// below already took it for exactly this, and this one did not -
				// so a set_path_for_testing racing a menu's refresh could read
				// the string while it was being assigned. Not reachable from a
				// shipping path today, which is the argument for fixing it while
				// it still is not. docs/refactor/09-remediation-plan.md R6.4.
				std::wstring path;
				{
					std::lock_guard<std::mutex> lock(_mutex);
					path = _path_override.empty() ? Quarantine::default_path() : _path_override;
				}

				auto entries = Quarantine::load(path);

				std::vector<uint32_t> hashes;
				hashes.reserve(entries.size());
				for(const auto &entry : entries)
					hashes.push_back(entry.hash);

				std::lock_guard<std::mutex> lock(_mutex);
				_hashes.swap(hashes);
				_loaded = true;
				_checked_tick.store(::GetTickCount64(), std::memory_order_relaxed);
				_stamp = write_time(path);
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
				auto last = _checked_tick.load(std::memory_order_relaxed);

				{
					std::lock_guard<std::mutex> lock(_mutex);
					if(_loaded && last != 0 && (now - last) < CheckIntervalMs)
						return;
				}

				std::wstring path;
				{
					std::lock_guard<std::mutex> lock(_mutex);
					path = _path_override.empty() ? Quarantine::default_path() : _path_override;
				}

				auto stamp = write_time(path);

				{
					std::lock_guard<std::mutex> lock(_mutex);
					_checked_tick.store(now, std::memory_order_relaxed);
					if(_loaded && stamp == _stamp)
						return;		// nothing moved
				}

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
