#pragma once

/*
	Reading a menu item's string out of an HMENU.

	Shell's job on the IShellExtInit path is reproducing somebody else's menu, so
	the item's string is the payload. Every read in the tree used to hand
	GetMenuItemInfoW a fixed MAX_PATH buffer with cch = MAX_PATH, which truncates
	silently at 259 characters. Third-party extensions cross that line routinely -
	archivers that embed the target name in "Add to ...", "Open with" entries,
	VCS shells.

	The documented contract is a two-call pattern:

		"first find the size of the string by setting the dwTypeData member of
		MENUITEMINFO to NULL and then calling GetMenuItemInfo. The value of cch+1
		is the size needed. Then allocate a buffer of this size, place the pointer
		to the buffer in dwTypeData, increment cch by one, and then call
		GetMenuItemInfo once again to fill the buffer with the string."

		https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmenuiteminfow

	A non-string item answers cch == 0 to the sizing call, so the pattern costs
	one extra GetMenuItemInfoW and no allocation at all for separators and
	bitmap items - which is why it is safe to use unconditionally.

	This header deliberately includes nothing but <windows.h>, so the test project
	can drive the real implementation rather than a copy of it. MenuItem.h pulls
	in the whole Shell header graph and cannot be reached from there.
*/

#include <windows.h>

namespace Nilesoft
{
	namespace Shell
	{
		/*
			Fills mii->dwTypeData with the item's string.

			`allocate` is called with the required character count (not counting
			the terminator) and must return a writable buffer of at least
			count + 1 characters, or nullptr to abandon the read.

			mii keeps whatever other MIIM_ bits the caller set; they are filled by
			the second call as usual. On return mii->cch is the character count.

			Returns false if either call failed, in which case mii->cch is 0.
		*/
		template<typename Allocate>
		inline bool read_menu_text(HMENU hMenu, UINT item, BOOL byPosition,
								   MENUITEMINFOW *mii, Allocate allocate)
		{
			if(!mii)
				return false;

			mii->fMask |= MIIM_STRING;
			mii->dwTypeData = nullptr;
			mii->cch = 0;

			if(!::GetMenuItemInfoW(hMenu, item, byPosition, mii))
			{
				mii->cch = 0;
				return false;
			}

			// Non-string item, or an item whose string is empty. Either way there
			// is nothing to fetch and no buffer to allocate.
			if(mii->cch == 0)
				return true;

			auto buffer = allocate(mii->cch);
			if(!buffer)
			{
				mii->cch = 0;
				return false;
			}

			mii->dwTypeData = buffer;
			mii->cch += 1;

			if(!::GetMenuItemInfoW(hMenu, item, byPosition, mii))
			{
				mii->cch = 0;
				return false;
			}

			return true;
		}
	}
}
