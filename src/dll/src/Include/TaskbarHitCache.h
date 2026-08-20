#pragma once

/*
	Results of taskbar hit-testing, keyed by taskbar window and a coarse point.

	The UI Automation query behind these results cannot run on the taskbar's own
	input thread: Microsoft's guidance is explicit that a client interacting with
	its own UI on the UI thread can see "very slow performance, or even cause the
	application to stop responding", and that such calls belong on a separate MTA
	thread that owns no windows.

		https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading

	Shell is injected into explorer.exe, so a UIA call made from the taskbar's
	message handler is exactly that case. Every answer the worker produces is
	cached here so the overwhelmingly common right-click never has to ask.

	The previous cache keyed on the exact pixel with a 250 ms lifetime, which
	only ever absorbed the WM_MOUSEACTIVATE/WM_CONTEXTMENU pair of a single
	click. A 16-pixel bucket and a multi-second lifetime absorb repeated clicks
	in the same region, which is what people actually do.

	This is deliberately free of COM and UIA so it can be tested directly. No
	apartment-bound pointer is ever stored - only a plain bool.
*/

#include <windows.h>
#include <mutex>
#include <optional>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		class TaskbarHitCache
		{
		public:
			// Points within one bucket share an answer. Taskbar buttons are far
			// larger than this, so a bucket cannot straddle "on a button" and
			// "empty area" in a way that matters.
			static constexpr long BUCKET = 16;
			static constexpr uint32_t TTL_MS = 3000;
			static constexpr size_t CAPACITY = 64;

			static long bucket_of(long v) noexcept
			{
				// Floor division, so negative coordinates on a secondary monitor
				// to the left of the primary bucket the same way positive ones do.
				return (v >= 0 ? v : (v - BUCKET + 1)) / BUCKET;
			}

			std::optional<bool> lookup(HWND taskbar, POINT pt, uint32_t now) const
			{
				if(!taskbar)
					return std::nullopt;

				auto bx = bucket_of(pt.x);
				auto by = bucket_of(pt.y);

				std::lock_guard<std::mutex> lock(_mutex);
				for(auto &e : _entries)
				{
					if(e.taskbar == taskbar && e.bx == bx && e.by == by)
					{
						if((now - e.tick) <= TTL_MS)
							return e.allowed;
						return std::nullopt;
					}
				}
				return std::nullopt;
			}

			void store(HWND taskbar, POINT pt, bool allowed, uint32_t now)
			{
				if(!taskbar)
					return;

				auto bx = bucket_of(pt.x);
				auto by = bucket_of(pt.y);

				std::lock_guard<std::mutex> lock(_mutex);

				for(auto &e : _entries)
				{
					if(e.taskbar == taskbar && e.bx == bx && e.by == by)
					{
						e.allowed = allowed;
						e.tick = now;
						return;
					}
				}

				if(_entries.size() >= CAPACITY)
				{
					// Drop the oldest rather than growing without bound; the set
					// of buckets a user clicks is small.
					size_t oldest = 0;
					for(size_t i = 1; i < _entries.size(); i++)
					{
						if((now - _entries[i].tick) > (now - _entries[oldest].tick))
							oldest = i;
					}
					_entries[oldest] = { taskbar, bx, by, allowed, now };
					return;
				}

				_entries.push_back({ taskbar, bx, by, allowed, now });
			}

			// The taskbar was recreated, the work area changed, or the display
			// topology changed: every cached answer is about a layout that no
			// longer exists.
			void invalidate()
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_entries.clear();
			}

			size_t size() const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return _entries.size();
			}

		private:
			struct Entry
			{
				HWND taskbar;
				long bx;
				long by;
				bool allowed;
				uint32_t tick;
			};

			mutable std::mutex _mutex;
			std::vector<Entry> _entries;
		};
	}
}
