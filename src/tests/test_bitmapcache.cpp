// The icon cache hands out HBITMAPs that it owns, and menu items are expected
// to leave them alone. Getting that contract wrong leaks GDI handles, or worse,
// frees a bitmap the cache still points at and later draws through a dangling
// handle inside Explorer. These tests cover the ownership rules directly, using
// real GDI objects and the live handle count as the oracle.

#include <windows.h>
#include <thread>
#include <vector>
#include <atomic>
#include "test.h"
#include "../dll/src/Include/BitmapCache.h"

using Nilesoft::Shell::BitmapCache;

namespace
{
	HBITMAP make_bitmap(int cx = 16, int cy = 16)
	{
		BITMAPINFOHEADER bi{ sizeof(BITMAPINFOHEADER), cx, -cy, 1, 32 };
		void *bits = nullptr;
		return ::CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO *>(&bi),
								  DIB_RGB_COLORS, &bits, nullptr, 0);
	}

	// A handle is still live if we can read its header back.
	bool alive(HBITMAP h)
	{
		BITMAP bm{};
		return ::GetObjectW(h, sizeof(bm), &bm) != 0;
	}

	unsigned gdi_count()
	{
		return ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
	}
}

TEST(bitmapcache, identical_text_and_size_hits)
{
	BitmapCache cache;
	const wchar_t *svg = L"<svg viewBox='0 0 16 16'><path d='M0 0h16v16H0z'/></svg>";
	const size_t len = wcslen(svg);

	CHECK(cache.find(svg, len, 16, 16) == nullptr);

	bool cached = false;
	auto first = cache.add(svg, len, 16, 16, make_bitmap(), &cached);
	CHECK(first != nullptr);
	CHECK(cached == true);
	CHECK_EQ(cache.size(), size_t(1));
	CHECK(cache.find(svg, len, 16, 16) == first);
}

TEST(bitmapcache, same_text_at_a_different_size_is_a_separate_entry)
{
	BitmapCache cache;
	const wchar_t *svg = L"<svg viewBox='0 0 16 16'/>";
	const size_t len = wcslen(svg);

	auto at16 = cache.add(svg, len, 16, 16, make_bitmap(16, 16));
	auto at32 = cache.add(svg, len, 32, 32, make_bitmap(32, 32));

	CHECK(at16 != at32);
	CHECK_EQ(cache.size(), size_t(2));
	CHECK(cache.find(svg, len, 16, 16) == at16);
	CHECK(cache.find(svg, len, 32, 32) == at32);
	// A size that was never rasterised must miss rather than return either.
	CHECK(cache.find(svg, len, 24, 24) == nullptr);
}

TEST(bitmapcache, different_text_does_not_collide)
{
	BitmapCache cache;
	const wchar_t *a = L"<svg><circle r='4'/></svg>";
	const wchar_t *b = L"<svg><circle r='5'/></svg>";

	auto ha = cache.add(a, wcslen(a), 16, 16, make_bitmap());
	auto hb = cache.add(b, wcslen(b), 16, 16, make_bitmap());

	CHECK(ha != hb);
	CHECK(cache.find(a, wcslen(a), 16, 16) == ha);
	CHECK(cache.find(b, wcslen(b), 16, 16) == hb);
}

// The key is built from an explicit length, so a caller passing a prefix of a
// longer string must not be treated as having passed the whole thing.
TEST(bitmapcache, length_is_part_of_the_key)
{
	BitmapCache cache;
	const wchar_t *text = L"<svg/>EXTRA";

	auto shorter = cache.add(text, 6, 16, 16, make_bitmap());
	CHECK(cache.find(text, 11, 16, 16) == nullptr);
	CHECK(cache.find(text, 6, 16, 16) == shorter);
}

// Two builds racing to rasterise the same icon must converge on one handle,
// and the loser has to be freed rather than leaked.
TEST(bitmapcache, duplicate_add_keeps_the_incumbent_and_frees_the_loser)
{
	BitmapCache cache;
	const wchar_t *svg = L"<svg viewBox='0 0 16 16'/>";
	const size_t len = wcslen(svg);

	auto winner = cache.add(svg, len, 16, 16, make_bitmap());
	auto loser = make_bitmap();
	CHECK(loser != winner);

	auto returned = cache.add(svg, len, 16, 16, loser);

	CHECK(returned == winner);
	CHECK_EQ(cache.size(), size_t(1));
	CHECK(alive(winner));
	CHECK(!alive(loser));
}

// Re-adding the exact handle already stored must not free it: the caller is
// about to draw through what we return.
TEST(bitmapcache, re_adding_the_same_handle_is_a_no_op)
{
	BitmapCache cache;
	const wchar_t *svg = L"<svg/>";
	const size_t len = wcslen(svg);

	auto h = cache.add(svg, len, 16, 16, make_bitmap());
	auto again = cache.add(svg, len, 16, 16, h);

	CHECK(again == h);
	CHECK_EQ(cache.size(), size_t(1));
	CHECK(alive(h));
}

TEST(bitmapcache, clear_releases_every_handle)
{
	const wchar_t *svg = L"<svg viewBox='0 0 16 16'/>";
	HBITMAP handles[8]{};

	auto before = gdi_count();
	{
		BitmapCache cache;
		for(int i = 0; i < 8; i++)
		{
			wchar_t buf[64];
			swprintf_s(buf, L"%s<!--%d-->", svg, i);
			handles[i] = cache.add(buf, wcslen(buf), 16, 16, make_bitmap());
			CHECK(handles[i] != nullptr);
		}
		CHECK_EQ(cache.size(), size_t(8));
		CHECK(gdi_count() > before);
		cache.clear();
		CHECK_EQ(cache.size(), size_t(0));
	}

	for(int i = 0; i < 8; i++)
		CHECK(!alive(handles[i]));

	CHECK_EQ(gdi_count(), before);
}

// The destructor has to do the same thing clear() does, since CACHE owns one by
// value and teardown is the only place a config reload frees these.
TEST(bitmapcache, destructor_releases_every_handle)
{
	HBITMAP h = nullptr;
	auto before = gdi_count();
	{
		BitmapCache cache;
		h = cache.add(L"<svg/>", 6, 16, 16, make_bitmap());
		CHECK(alive(h));
	}
	CHECK(!alive(h));
	CHECK_EQ(gdi_count(), before);
}

// A null bitmap means rasterisation failed. Storing it would turn every later
// lookup for that icon into a hit that draws nothing.
TEST(bitmapcache, failed_rasterisation_is_not_cached)
{
	BitmapCache cache;
	const wchar_t *svg = L"<svg/>";

	CHECK(cache.add(svg, 6, 16, 16, nullptr) == nullptr);
	CHECK_EQ(cache.size(), size_t(0));
	CHECK(cache.find(svg, 6, 16, 16) == nullptr);
}

TEST(bitmapcache, empty_and_null_text_are_rejected)
{
	BitmapCache cache;
	auto h = make_bitmap();

	CHECK(cache.find(nullptr, 0, 16, 16) == nullptr);
	CHECK(cache.find(L"", 0, 16, 16) == nullptr);
	CHECK(cache.add(nullptr, 0, 16, 16, h) == h);
	CHECK(cache.add(L"", 0, 16, 16, h) == h);
	CHECK_EQ(cache.size(), size_t(0));

	// Nothing was stored, so the caller still owns it.
	CHECK(alive(h));
	::DeleteObject(h);
}

TEST(bitmapcache, bounded_capacity_stops_caching_at_limit)
{
	BitmapCache cache;
	std::vector<HBITMAP> unowned_handles;

	for(int i = 0; i < 300; i++)
	{
		wchar_t buf[64];
		swprintf_s(buf, L"<svg id='%d'/>", i);
		bool cached = false;
		auto h = cache.add(buf, wcslen(buf), 16, 16, make_bitmap(), &cached);
		if(i < int(BitmapCache::MaxEntries))
		{
			CHECK(cached == true);
		}
		else
		{
			CHECK(cached == false);
			unowned_handles.push_back(h);
		}
	}

	CHECK_EQ(cache.size(), BitmapCache::MaxEntries);

	// Clean up the un-cached handles that caller owns
	for(auto h : unowned_handles)
		::DeleteObject(h);
}

TEST(bitmapcache, concurrent_access_is_thread_safe)
{
	BitmapCache cache;
	std::vector<std::thread> workers;
	std::atomic<int> errors{ 0 };
	const int thread_count = 4;
	const int iterations = 50;

	for(int t = 0; t < thread_count; t++)
	{
		workers.emplace_back([&cache, &errors, t]()
		{
			for(int i = 0; i < iterations; i++)
			{
				wchar_t buf[64];
				swprintf_s(buf, L"<svg icon='%d'/>", (t * 100) + i);
				bool cached = false;
				auto h = cache.add(buf, wcslen(buf), 16, 16, make_bitmap(), &cached);
				if(!h)
					errors.fetch_add(1, std::memory_order_relaxed);

				auto found = cache.find(buf, wcslen(buf), 16, 16);
				if(cached && found != h)
					errors.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}

	for(auto &w : workers)
		w.join();

	CHECK_EQ(errors.load(), 0);
	CHECK(cache.size() <= BitmapCache::MaxEntries);
}