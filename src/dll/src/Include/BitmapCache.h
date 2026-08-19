#pragma once

// Standalone so the test suite can exercise the ownership contract without
// pulling in the whole config cache. Needs nothing but a map and HBITMAP.

#include <unordered_map>
#include <cstdint>

namespace Nilesoft
{
	namespace Shell
	{
		// Rasterised menu icons, keyed on the resolved SVG text and the pixel
		// size it was rendered at.
		//
		// Every menu build re-ran ToUTF8 plus a full plutosvg parse and raster
		// for every item with an icon, and the shipped config is inline SVG
		// almost end to end. Measured on this machine at 16x16: 15.4us per icon
		// for the raster alone, so a 30-item menu repeated roughly half a
		// millisecond of identical work on every single right-click.
		//
		// The cache owns every HBITMAP it hands out. Callers mark the item image
		// `inherited`, which is the flag MenuItemInfo::IMAGE::destroy already
		// uses to mean "someone else owns this", so item teardown leaves them
		// alone. They are released when the CACHE is destroyed, which is also
		// what a config reload does.
		//
		// Entries are keyed by a 64-bit hash of the resolved text rather than
		// the text itself, to avoid holding a second copy of every icon in
		// every process that shows a menu. A collision would draw the wrong
		// icon, not crash; at ~100 entries that is around 1 in 10^16.
		struct BitmapCache
		{
			HBITMAP find(const wchar_t *text, size_t length, long cx, long cy) const
			{
				if(!text || !length)
					return nullptr;

				auto it = _items.find(key(text, length, cx, cy));
				return it != _items.end() ? it->second : nullptr;
			}

			// Takes ownership of hbmp. Returns it back for convenience.
			HBITMAP add(const wchar_t *text, size_t length, long cx, long cy, HBITMAP hbmp)
			{
				if(hbmp && text && length)
				{
					auto k = key(text, length, cx, cy);
					// A concurrent build could have raced us to the same icon.
					// Keep the entry already published and drop ours, so the
					// handle we hand back is always the one the cache owns.
					if(auto it = _items.find(k); it != _items.end())
					{
						if(it->second != hbmp)
							::DeleteObject(hbmp);
						return it->second;
					}
					_items.emplace(k, hbmp);
				}
				return hbmp;
			}

			void clear()
			{
				for(auto &it : _items)
				{
					if(it.second)
						::DeleteObject(it.second);
				}
				_items.clear();
			}

			size_t size() const { return _items.size(); }

			~BitmapCache() { clear(); }

		private:
			// FNV-1a over the UTF-16 code units, then over the target size so
			// the same icon at two sizes stays two entries.
			static uint64_t key(const wchar_t *text, size_t length, long cx, long cy)
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
				return h;
			}

			std::unordered_map<uint64_t, HBITMAP> _items;
		};
	}
}
