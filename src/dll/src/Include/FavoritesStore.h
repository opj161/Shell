#pragma once

/*
	The favorites list, as the menu path sees it.

	src/shared/Favorites.h is the file format and the reasoning. This is the
	process-lifetime holder that turns it into a vector of integers a menu can
	walk, and the one place a use is written back.

	Loaded lazily and re-read when the file's write time moves, checked at most
	once every couple of seconds. Deliberately the same shape - and for the same
	reason - as ProviderQuarantineStore: a stat call per right-click is cheap
	but not free, and a favourite pinned in one window appearing in another
	within two seconds is soon enough.

	## The measured path never touches this unless somebody asked

	`ranks()` is called only when `settings { favorites = N }` is set to
	something above zero, so a configuration that does not use the feature pays
	nothing: no file, no stat, no allocation. That gate lives at the call site
	in ContextMenu::apply_favorites rather than here, because here is also where
	`shell.exe` and the invoke path come in, and neither of those is governed by
	a setting.

	## Writing happens after the menu is down

	`record_use` reads the file, increments, and writes it back. That is three
	file operations and it is not on any path a user waits on: it runs from
	InvokeCommand, after the menu has closed and after the chosen command has
	been dispatched. Doing it during composition would put a write between a
	right-click and the first pixel, which is what R1 exists to prevent.

	Read-modify-write rather than a cached list written on exit: several hosts
	have this file at once (every Explorer window, the taskbar, any file
	manager), none of them knows when it will end, and last-writer-wins over a
	whole cached list would silently discard the other hosts' counts. Re-reading
	first means two hosts racing lose at worst one increment rather than one
	host's entire history.
*/

#include <Favorites.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "MenuFavorites.h"

namespace Nilesoft
{
	namespace Shell
	{
		class FavoritesStore
		{
		public:
			static FavoritesStore &instance()
			{
				static FavoritesStore *store = new FavoritesStore();
				return *store;
			}

			FavoritesStore(const FavoritesStore &) = delete;
			FavoritesStore &operator=(const FavoritesStore &) = delete;

			// What the planner needs, and nothing else. Copied out rather than
			// handed back by reference: composition runs arbitrary third-party
			// code (docs/refactor/08-handoff.md section 4, "hold no lock across
			// it"), so nothing may hold this object's lock while a menu is
			// being built.
			std::vector<FavoriteRank> ranks()
			{
				refresh_if_stale();

				std::lock_guard<std::mutex> lock(_mutex);
				std::vector<FavoriteRank> out;
				out.reserve(_entries.size());
				for(const auto &entry : _entries)
				{
					FavoriteRank rank;
					rank.hash = entry.identity.hash;
					rank.pinned = entry.pinned;
					rank.uses = entry.uses;
					out.push_back(rank);
				}
				return out;
			}

			size_t size()
			{
				refresh_if_stale();
				std::lock_guard<std::mutex> lock(_mutex);
				return _entries.size();
			}

			/*
				Count one use. Never called while a menu is up - see the header.

				The file is re-read first so a count from another host is not
				overwritten, and the in-memory copy is refreshed from what was
				actually written so the next menu in this process agrees with
				the file.
			*/
			bool record_use(const MenuIdentity::Identity &identity)
			{
				if(!identity.valid())
					return false;

				auto path = current_path();
				if(path.empty())
					return false;

				auto entries = Favorites::load(path);
				if(!Favorites::record_use(entries, identity))
					return false;

				auto ok = Favorites::save(path, entries);

				std::lock_guard<std::mutex> lock(_mutex);
				_entries.swap(entries);
				_loaded = true;
				_checked_tick.store(::GetTickCount64(), std::memory_order_relaxed);
				_stamp = write_time(path);
				return ok;
			}

			void reload()
			{
				auto path = current_path();
				auto entries = Favorites::load(path);

				std::lock_guard<std::mutex> lock(_mutex);
				_entries.swap(entries);
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
			FavoritesStore() = default;

			// Two seconds, matching ProviderQuarantineStore for the reason
			// given there: short enough that nobody notices, long enough that a
			// burst of right-clicks does not stat the file for every one.
			static constexpr uint64_t CheckIntervalMs = 2000;

			std::wstring current_path()
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return _path_override.empty() ? Favorites::default_path() : _path_override;
			}

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
					if(_loaded && (now - last) < CheckIntervalMs)
						return;
				}

				auto path = current_path();
				auto stamp = write_time(path);

				{
					std::lock_guard<std::mutex> lock(_mutex);
					_checked_tick.store(now, std::memory_order_relaxed);
					if(_loaded && stamp == _stamp)
						return;
				}

				reload();
			}

			std::mutex _mutex;
			std::vector<Favorites::Entry> _entries;
			std::wstring _path_override;
			uint64_t _stamp{};
			bool _loaded{};
			std::atomic<uint64_t> _checked_tick{ 0 };
		};
	}
}
