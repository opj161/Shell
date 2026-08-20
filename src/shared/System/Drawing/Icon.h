#pragma once

namespace Nilesoft
{
	namespace Drawing
	{
		class Icon
		{
		public:
			inline static int default_size() { return ::GetSystemMetrics(SM_CXSMICON); }

			static HICON FromFile(const wchar_t* path, int size)
			{
				auto cx = default_size();
				return (HICON)::LoadImageW(nullptr, path, IMAGE_ICON,
										   size > cx ? size : cx,
										   size > cx ? size : cx,
										  LR_LOADFROMFILE | LR_SHARED);
			}

			/*static HICON FromAssociated(const wchar_t* path, int size)
			{
				SHFILEINFOW sfi = { };
				auto hr = ::SHGetFileInfoW(path, FILE_ATTRIBUTE_NORMAL, 
											 &sfi, sizeof(sfi), 
											 SHGFI_ICON | (size > cx ? SHGFI_LARGEICON : SHGFI_SMALLICON));
				return SUCCEEDED(hr) ? sfi.hIcon : nullptr;
			}*/

			static HICON FromFileTypeIcon(const wchar_t *extension, int size)
			{
				auto cx = default_size();
				//	if(extension[0] != '.') extension = '.' + extension;
				SHFILEINFOW sfi = { };
				auto hr = ::SHGetFileInfoW(extension, 0,
										   &sfi, sizeof(sfi),
										   SHGFI_OPENICON | SHGFI_ICON | (size > cx ? SHGFI_LARGEICON : SHGFI_SMALLICON) | SHGFI_USEFILEATTRIBUTES);
				return SUCCEEDED(hr) ? sfi.hIcon : nullptr;
			}

			static HICON FromResource(const wchar_t* path, int size)
			{
				string p = path;
				int index = IO::Path::ParseLocation(p);
				return FromResourceByIndex(p, index, size);
			}

			static HICON FromResourceById(const wchar_t* path, const wchar_t* id)
			{
				return ::LoadIconW(DLL(path), id);
			}

			static HICON FromResourceByIndex(const wchar_t *path, int index, int size)
			{
				auto cx = default_size();
				HICON hIcon = nullptr;
				if(size > cx)
					::ExtractIconExW(path, index, &hIcon, nullptr, 1);
				else
					::ExtractIconExW(path, index, nullptr, &hIcon, 1);
				return hIcon;
			}
		};
	}
}
