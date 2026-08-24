#pragma once

/*
	Turning an icon resource string into a bitmap, and owning the result.

	IExplorerCommand::GetIcon hands back "the icon resource string of the icon
	associated with this Windows Explorer command item" in the usual
	"file,-resourceid" form, which has to be extracted and converted before a
	menu can draw it.
	https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-geticon

	The reason this is a header of its own rather than three lines in
	ExplorerCommand.cpp is a leak that was live in the tree and measurable.

	GetIconInfo is explicit: "GetIconInfo creates bitmaps for the hbmMask and
	hbmColor or members of ICONINFO. The calling application must manage these
	bitmaps and delete them with DeleteObject call when they are no longer
	necessary."
	https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-geticoninfo

	The mask was deleted; the colour bitmap was handed back as the return value
	and never freed by anybody, because the field it was stored in - menuitem_t's
	image - is *borrowed* for every other kind of item. A native item's bitmap
	belongs to the host's MENUITEMINFO (MenuItemInfo::FindImage returns
	hbmpItem/hbmpUnchecked/hbmpChecked straight out of it), so a destructor that
	deleted unconditionally would destroy Explorer's own bitmaps. The result was
	that nothing deleted anything, and the one path that really did own its
	handle leaked it.

	Measured on this machine (Windows 11 26200.8875 x64, 2026-08-24): of 23
	registered packaged verb handlers, 16 return an icon, and a probe that
	extracted them the way the shipping code does and kept the colour bitmap
	took the process from 4 GDI objects to 164 across ten rounds - exactly the
	160 it had kept. That is 16 GDI objects per right-click, in explorer.exe,
	against a default per-process limit of 10,000: on the order of six hundred
	right-clicks before Explorer starts failing to draw.

	So the type says who owns what. IconBitmap owns; release() hands ownership
	to a caller that has somewhere to put it.
*/

#include <windows.h>
#include <shlobj_core.h>

namespace Nilesoft
{
	namespace Shell
	{
		// An HBITMAP this code created and is responsible for.
		class IconBitmap
		{
		public:
			IconBitmap() = default;
			explicit IconBitmap(HBITMAP bitmap) : _bitmap(bitmap) {}

			~IconBitmap() { reset(); }

			IconBitmap(const IconBitmap &) = delete;
			IconBitmap &operator=(const IconBitmap &) = delete;

			IconBitmap(IconBitmap &&other) noexcept : _bitmap(other._bitmap)
			{
				other._bitmap = nullptr;
			}

			IconBitmap &operator=(IconBitmap &&other) noexcept
			{
				if(this != &other)
				{
					reset();
					_bitmap = other._bitmap;
					other._bitmap = nullptr;
				}
				return *this;
			}

			HBITMAP get() const { return _bitmap; }
			explicit operator bool() const { return _bitmap != nullptr; }

			// Gives the handle away. The caller becomes responsible for
			// DeleteObject; this object holds nothing afterwards.
			HBITMAP release()
			{
				auto bitmap = _bitmap;
				_bitmap = nullptr;
				return bitmap;
			}

			void reset()
			{
				if(_bitmap)
				{
					::DeleteObject(_bitmap);
					_bitmap = nullptr;
				}
			}

		private:
			HBITMAP _bitmap{};
		};

		// Splits "path,index" the way the shell's icon-location strings are
		// written. A negative index is a resource identifier and is passed
		// through as-is; SHDefExtractIconW takes both.
		struct IconLocation
		{
			wchar_t path[MAX_PATH]{};
			int index{};
			bool valid{};
		};

		inline IconLocation parse_icon_location(const wchar_t *spec)
		{
			IconLocation location;
			if(!spec || !*spec)
				return location;

			size_t length = 0;
			while(spec[length] && length < 32768)
				length++;

			// Only the last comma separates the index; a path may contain one.
			size_t comma = length;
			for(size_t i = length; i > 0; i--)
			{
				if(spec[i - 1] == L',')
				{
					comma = i - 1;
					break;
				}
			}

			size_t path_length = comma;
			if(comma < length)
			{
				int sign = 1;
				size_t i = comma + 1;
				if(spec[i] == L'-') { sign = -1; i++; }
				else if(spec[i] == L'+') { i++; }

				long long value = 0;
				bool digits = false;
				for(; spec[i] >= L'0' && spec[i] <= L'9'; i++)
				{
					digits = true;
					value = value * 10 + (spec[i] - L'0');
					if(value > 0x7FFFFFFFLL)
						break;
				}
				// Trailing rubbish after the comma means it was not an index at
				// all, so the whole string is the path.
				if(!digits || spec[i] != L'\0')
					path_length = length;
				else
					location.index = static_cast<int>(sign * value);
			}

			// Quotes are common in registry-authored icon locations.
			size_t start = 0;
			while(start < path_length && spec[start] == L'"')
				start++;
			while(path_length > start && spec[path_length - 1] == L'"')
				path_length--;

			auto copy = path_length - start;
			if(copy == 0 || copy >= MAX_PATH)
				return location;

			for(size_t i = 0; i < copy; i++)
				location.path[i] = spec[start + i];
			location.path[copy] = L'\0';
			location.valid = true;
			return location;
		}

		// Extracts the 16x16 icon and converts it to a bitmap this code owns.
		//
		// nIconSize is LOWORD=large, HIWORD=small.
		// https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shdefextracticonw
		inline IconBitmap icon_bitmap_from_resource(const wchar_t *spec)
		{
			auto location = parse_icon_location(spec);
			if(!location.valid)
				return IconBitmap();

			HICON large = nullptr;
			HICON small_icon = nullptr;
			if(FAILED(::SHDefExtractIconW(location.path, location.index, 0, &large, &small_icon,
										  static_cast<UINT>(MAKELONG(16, 16)))))
				return IconBitmap();

			HICON use = small_icon ? small_icon : large;
			HBITMAP bitmap = nullptr;
			if(use)
			{
				ICONINFO info{};
				if(::GetIconInfo(use, &info))
				{
					// Both bitmaps belong to this code now. The mask is not
					// wanted; the colour bitmap becomes the return value, and
					// IconBitmap is what makes sure somebody eventually frees it.
					if(info.hbmMask)
						::DeleteObject(info.hbmMask);
					bitmap = info.hbmColor;
				}
			}

			if(small_icon)
				::DestroyIcon(small_icon);
			if(large && large != small_icon)
				::DestroyIcon(large);

			return IconBitmap(bitmap);
		}
	}
}
