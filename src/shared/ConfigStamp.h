#pragma once

/*
	One file's identity for change detection: last-write time folded with size.

	Taken *before* the file is read, by whoever reads it. A later comparison
	against a fresh stamp is then conservative: a write that lands between the
	stamp and the read makes the two differ, which costs at most one redundant
	reload, and can never make a changed file look unchanged. Zero means the
	file could not be queried, which is what a missing file stamps as.
	https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfileattributesexw
*/

#include <windows.h>
#include <cstdint>
#include <string>

namespace Nilesoft
{
	namespace Shell
	{
		inline uint64_t config_file_stamp(const std::wstring &path)
		{
			WIN32_FILE_ATTRIBUTE_DATA data{};
			if(path.empty() || !::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
				return 0;

			uint64_t stamp = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32)
						   | data.ftLastWriteTime.dwLowDateTime;
			stamp ^= (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
			return stamp;
		}
	}
}
