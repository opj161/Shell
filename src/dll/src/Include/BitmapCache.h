#pragma once

// Standalone so the test suite can exercise the ownership contract without
// pulling in the whole config cache. Needs nothing but a map, mutex, and HBITMAP.

#include <windows.h>
#include <unordered_map>
#include <string>
#include <mutex>
#include <cstdint>

namespace Nilesoft
{
	namespace Shell
	{
		// Rasterised menu icons, keyed on the resolved SVG text and the pixel
		// size it was rendered at. Synchronized and bounded to prevent unbounded
		// memory growth across multithreaded third-party hosts.
		//
		// Ownership model:
		// When an icon is cached, the BitmapCache owns the HBITMAP until the
		// cache itself is cleared/destroyed (e.g. on config reload or process exit).
		// Borrowing callers mark their item `inherited = true`.
		//
		// To ensure memory safety with borrowed raw handles, the cache NEVER
		// destructively evicts entries while live borrowers can exist. Once the
		// capacity bound (MaxEntries = 256) is reached, new entries are simply
		// not cached; callers retain exclusive ownership and destroy their bitmap
		// on item teardown.
		struct BitmapCache
		{
			static constexpr size_t MaxEntries = 256;

			struct Key
			{
				uint64_t hash{};
				long cx{};
				long cy{};
				std::wstring text;

				bool operator==(const Key &other) const noexcept
				{
					return hash == other.hash && cx == other.cx && cy == other.cy && text == other.text;
				}
			};

			struct KeyHash
			{
				size_t operator()(const Key &k) const noexcept
				{
					return static_cast<size_t>(k.hash);
				}
			};

			HBITMAP find(const wchar_t *text, size_t length, long cx, long cy) const
			{
				if(!text || !length)
					return nullptr;

				Key k = make_key(text, length, cx, cy);
				std::lock_guard<std::mutex> lock(_mutex);
				auto it = _items.find(k);
				return it != _items.end() ? it->second : nullptr;
			}

			// Takes ownership of hbmp if cached. Returns the active HBITMAP handle.
			// If out_cached is provided, sets *out_cached to true if the handle is
			// owned by the cache, or false if capacity is reached and the caller
			// retains ownership.
			HBITMAP add(const wchar_t *text, size_t length, long cx, long cy, HBITMAP hbmp, bool *out_cached = nullptr)
			{
				if(out_cached)
					*out_cached = false;

				if(hbmp && text && length)
				{
					Key k = make_key(text, length, cx, cy);
					std::lock_guard<std::mutex> lock(_mutex);

					if(auto it = _items.find(k); it != _items.end())
					{
						if(it->second != hbmp)
							::DeleteObject(hbmp);
						if(out_cached)
							*out_cached = true;
						return it->second;
					}

					// Bounded capacity: once full, do not insert or destructively evict.
					// The caller retains ownership of hbmp (out_cached = false).
					if(_items.size() >= MaxEntries)
					{
						return hbmp;
					}

					_items.emplace(std::move(k), hbmp);
					if(out_cached)
						*out_cached = true;
				}
				return hbmp;
			}

			void clear()
			{
				std::lock_guard<std::mutex> lock(_mutex);
				for(auto &it : _items)
				{
					if(it.second)
						::DeleteObject(it.second);
				}
				_items.clear();
			}

			size_t size() const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return _items.size();
			}

			~BitmapCache()
			{
				clear();
			}

		private:
			static Key make_key(const wchar_t *text, size_t length, long cx, long cy)
			{
				uint64_t h = 14695981039346656037ULL;
				auto mix = [&h](uint8_t b) { h ^= b; h *= 1099511628211ULL; };

				for(size_t i = 0; i < length; i++)
				{
					auto c = static_cast<uint16_t>(text[i]);
					mix(static_cast<uint8_t>(c));
					mix(static_cast<uint8_t>(c >> 8));
				}

				for(auto v : { cx, cy })
				{
					auto u = static_cast<uint32_t>(v);
					for(int s = 0; s < 32; s += 8)
						mix(static_cast<uint8_t>(u >> s));
				}
				return Key{ h, cx, cy, std::wstring(text, length) };
			}

			mutable std::mutex _mutex;
			std::unordered_map<Key, HBITMAP, KeyHash> _items;
		};
	}
}
