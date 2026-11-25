#include "test.h"

#include <windows.h>
#include "Include/IconResource.h"

// Icon resource strings, and who frees the bitmap that comes out of one.
//
// The leak these exist for was measurable rather than theoretical. GetIconInfo
// documents that "The calling application must manage these bitmaps and delete
// them with DeleteObject call when they are no longer necessary", and the
// packaged-verb path kept the colour bitmap and freed nothing: on this machine,
// 16 of 23 handlers return an icon, so a right-click leaked 16 GDI objects in
// explorer.exe against a default per-process limit of 10,000.
//
// The last two tests are the ones that matter. One asserts the handle is
// actually released; the other asserts the *test* would notice if it were not,
// because a leak test that cannot see a leak is worse than no test at all.

using namespace Nilesoft::Shell;

namespace
{
	// A file that is present on every Windows installation and has icons in it.
	const wchar_t *SHELL32 = L"C:\\Windows\\System32\\shell32.dll";

	DWORD gdi_objects()
	{
		return ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
	}

	// The first extraction in a process allocates GDI objects that are not the
	// caller's and are never freed - shell32 building its own internal caches.
	// Measured here: 4 objects, once, on whichever test happened to run first.
	// Counting handles without discarding that first pass makes the result
	// depend on test order, which is how this was discovered.
	void warm_up_the_shell_icon_cache()
	{
		static bool done = false;
		if(done)
			return;
		done = true;
		icon_bitmap_from_resource(SHELL32);
	}
}

TEST(icon_resource, a_plain_path_has_no_index)
{
	auto location = parse_icon_location(L"C:\\Windows\\System32\\shell32.dll");

	CHECK(location.valid);
	CHECK_EQ(location.index, 0);
	CHECK(::lstrcmpW(location.path, L"C:\\Windows\\System32\\shell32.dll") == 0);
}

TEST(icon_resource, a_trailing_index_is_split_off)
{
	auto location = parse_icon_location(L"C:\\Windows\\System32\\imageres.dll,3");

	CHECK(location.valid);
	CHECK_EQ(location.index, 3);
	CHECK(::lstrcmpW(location.path, L"C:\\Windows\\System32\\imageres.dll") == 0);
}

TEST(icon_resource, a_negative_index_is_a_resource_identifier)
{
	// The "file,-id" form is what IExplorerCommand::GetIcon returns, and
	// SHDefExtractIconW takes the negative value as-is.
	auto location = parse_icon_location(L"shell32.dll,-16769");

	CHECK(location.valid);
	CHECK_EQ(location.index, -16769);
	CHECK(::lstrcmpW(location.path, L"shell32.dll") == 0);
}

TEST(icon_resource, only_the_last_comma_separates_the_index)
{
	// A path really can contain one.
	auto location = parse_icon_location(L"C:\\Program Files\\Vendor, Inc\\app.exe,2");

	CHECK(location.valid);
	CHECK_EQ(location.index, 2);
	CHECK(::lstrcmpW(location.path, L"C:\\Program Files\\Vendor, Inc\\app.exe") == 0);
}

TEST(icon_resource, a_comma_that_is_not_an_index_stays_in_the_path)
{
	// Otherwise a directory called "Vendor, Inc" loses half its name.
	auto location = parse_icon_location(L"C:\\Vendor, Inc\\app.exe");

	CHECK(location.valid);
	CHECK_EQ(location.index, 0);
	CHECK(::lstrcmpW(location.path, L"C:\\Vendor, Inc\\app.exe") == 0);
}

TEST(icon_resource, quotes_are_stripped)
{
	auto location = parse_icon_location(L"\"C:\\Windows\\System32\\shell32.dll\",5");

	CHECK(location.valid);
	CHECK_EQ(location.index, 5);
	CHECK(::lstrcmpW(location.path, L"C:\\Windows\\System32\\shell32.dll") == 0);
}

TEST(icon_resource, nothing_is_not_a_location)
{
	CHECK(!parse_icon_location(nullptr).valid);
	CHECK(!parse_icon_location(L"").valid);
	CHECK(!parse_icon_location(L",3").valid);
	CHECK(!parse_icon_location(L"\"\"").valid);
}

TEST(icon_resource, an_unresolvable_location_yields_nothing_rather_than_a_stray_handle)
{
	warm_up_the_shell_icon_cache();
	auto before = gdi_objects();
	{
		auto bitmap = icon_bitmap_from_resource(L"C:\\this\\does\\not\\exist.dll,1");
		CHECK(!bitmap);
	}
	CHECK_EQ(gdi_objects(), before);
}

TEST(icon_resource, an_extracted_bitmap_is_released_when_it_goes_out_of_scope)
{
	warm_up_the_shell_icon_cache();
	auto before = gdi_objects();

	{
		auto bitmap = icon_bitmap_from_resource(SHELL32);
		CHECK_MSG(bitmap.get() != nullptr, "shell32.dll should yield an icon");
		CHECK(::GetObjectType(bitmap.get()) == OBJ_BITMAP);
	}

	// Every handle it created is gone again. This is the whole contract.
	CHECK_EQ(gdi_objects(), before);
}

TEST(icon_resource, releasing_hands_the_handle_over_and_the_caller_must_free_it)
{
	warm_up_the_shell_icon_cache();
	auto before = gdi_objects();

	HBITMAP taken = nullptr;
	{
		auto bitmap = icon_bitmap_from_resource(SHELL32);
		taken = bitmap.release();
		CHECK(!bitmap);
	}

	// Still alive, because ownership moved - this is the path menuitem_t takes,
	// and the reason menuitem_t needed a destructor for it.
	CHECK(taken != nullptr);
	CHECK(::GetObjectType(taken) == OBJ_BITMAP);
	CHECK(gdi_objects() > before);

	::DeleteObject(taken);
	CHECK_EQ(gdi_objects(), before);
}

TEST(icon_resource, the_leak_this_replaces_would_have_been_visible_here)
{
	warm_up_the_shell_icon_cache();
	// A leak test that cannot see a leak is worse than no test. This reproduces
	// the shipping behaviour exactly as it was - keep ICONINFO::hbmColor, free
	// only the mask - and asserts the object count climbs, so the tests above
	// are known to be measuring something real.
	auto before = gdi_objects();

	HICON large = nullptr, small_icon = nullptr;
	HBITMAP leaked = nullptr;
	if(SUCCEEDED(::SHDefExtractIconW(SHELL32, 0, 0, &large, &small_icon,
									 static_cast<UINT>(MAKELONG(16, 16)))))
	{
		auto use = small_icon ? small_icon : large;
		if(use)
		{
			ICONINFO info{};
			if(::GetIconInfo(use, &info))
			{
				if(info.hbmMask) ::DeleteObject(info.hbmMask);
				leaked = info.hbmColor;
			}
		}
		if(small_icon) ::DestroyIcon(small_icon);
		if(large && large != small_icon) ::DestroyIcon(large);
	}

	CHECK_MSG(leaked != nullptr, "the old path should have produced a bitmap");
	CHECK_MSG(gdi_objects() > before, "GDI object count should have grown - if it "
									  "did not, these tests prove nothing");

	::DeleteObject(leaked);
	CHECK_EQ(gdi_objects(), before);
}
