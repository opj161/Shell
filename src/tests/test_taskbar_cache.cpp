// Taskbar hit-test result cache.
//
// The UI Automation query itself belongs on a worker thread and cannot be
// exercised here, but the cache is what decides how often that query happens at
// all - and the old one, keyed on the exact pixel with a 250 ms lifetime, only
// ever absorbed the WM_MOUSEACTIVATE/WM_CONTEXTMENU pair of a single click.

#include "test.h"

#include "..\dll\src\Include\TaskbarHitCache.h"

using namespace Nilesoft::Shell;

namespace
{
	HWND wnd(uintptr_t n) { return reinterpret_cast<HWND>(n); }
}

TEST(taskbar_cache, an_empty_cache_answers_nothing)
{
	TaskbarHitCache cache;
	CHECK(!cache.lookup(wnd(1), { 100, 100 }, 1000).has_value());
}

TEST(taskbar_cache, a_stored_answer_comes_back)
{
	TaskbarHitCache cache;
	cache.store(wnd(1), { 100, 100 }, true, 1000);

	auto hit = cache.lookup(wnd(1), { 100, 100 }, 1000);
	CHECK(hit.has_value());
	if(hit) CHECK(*hit == true);
}

TEST(taskbar_cache, nearby_points_share_one_answer)
{
	TaskbarHitCache cache;
	cache.store(wnd(1), { 100, 100 }, true, 1000);

	// Same 16-pixel bucket: a second click a few pixels away must not cost
	// another UI Automation query.
	CHECK(cache.lookup(wnd(1), { 103, 104 }, 1000).has_value());
	CHECK(cache.lookup(wnd(1), { 111, 111 }, 1000).has_value());

	// A different bucket is a different question.
	CHECK(!cache.lookup(wnd(1), { 200, 100 }, 1000).has_value());
	CHECK(!cache.lookup(wnd(1), { 100, 200 }, 1000).has_value());
}

TEST(taskbar_cache, negative_coordinates_bucket_consistently)
{
	// A secondary monitor to the left of the primary has negative x.
	CHECK_EQ(TaskbarHitCache::bucket_of(-1), TaskbarHitCache::bucket_of(-16));
	CHECK(TaskbarHitCache::bucket_of(-17) != TaskbarHitCache::bucket_of(-16));
	CHECK(TaskbarHitCache::bucket_of(-1) != TaskbarHitCache::bucket_of(0));

	TaskbarHitCache cache;
	cache.store(wnd(1), { -1900, 500 }, true, 1000);
	CHECK(cache.lookup(wnd(1), { -1895, 502 }, 1000).has_value());
}

TEST(taskbar_cache, each_taskbar_is_answered_separately)
{
	TaskbarHitCache cache;
	cache.store(wnd(1), { 100, 100 }, true, 1000);

	// Same point on the secondary taskbar is a different element.
	CHECK(!cache.lookup(wnd(2), { 100, 100 }, 1000).has_value());
}

TEST(taskbar_cache, an_answer_expires)
{
	TaskbarHitCache cache;
	cache.store(wnd(1), { 100, 100 }, true, 1000);

	CHECK(cache.lookup(wnd(1), { 100, 100 }, 1000 + TaskbarHitCache::TTL_MS).has_value());
	CHECK(!cache.lookup(wnd(1), { 100, 100 }, 1000 + TaskbarHitCache::TTL_MS + 1).has_value());
}

TEST(taskbar_cache, a_repeat_answer_refreshes_rather_than_duplicates)
{
	TaskbarHitCache cache;
	cache.store(wnd(1), { 100, 100 }, true, 1000);
	cache.store(wnd(1), { 102, 102 }, false, 5000);

	CHECK_EQ(cache.size(), size_t(1));

	auto hit = cache.lookup(wnd(1), { 100, 100 }, 5000);
	CHECK(hit.has_value());
	if(hit) CHECK(*hit == false);
}

TEST(taskbar_cache, the_cache_is_bounded)
{
	TaskbarHitCache cache;
	for(long i = 0; i < 500; i++)
		cache.store(wnd(1), { i * TaskbarHitCache::BUCKET, 0 }, true, 1000 + static_cast<uint32_t>(i));

	CHECK(cache.size() <= TaskbarHitCache::CAPACITY);
}

TEST(taskbar_cache, invalidation_drops_everything)
{
	TaskbarHitCache cache;
	cache.store(wnd(1), { 100, 100 }, true, 1000);
	cache.store(wnd(2), { 100, 100 }, true, 1000);

	// The taskbar was recreated or the display topology changed.
	cache.invalidate();

	CHECK_EQ(cache.size(), size_t(0));
	CHECK(!cache.lookup(wnd(1), { 100, 100 }, 1000).has_value());
}

TEST(taskbar_cache, a_null_taskbar_is_never_cached_or_matched)
{
	TaskbarHitCache cache;
	cache.store(nullptr, { 100, 100 }, true, 1000);
	CHECK_EQ(cache.size(), size_t(0));
	CHECK(!cache.lookup(nullptr, { 100, 100 }, 1000).has_value());
}
