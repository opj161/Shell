#pragma once

#include <windows.h>
#include <string>
#include <string_view>

namespace Nilesoft::Shell::SelectionPaths
{
	enum class WindowsPathForm
	{
		Empty,
		FullyQualified,
		RootRelative,
		DriveRelative,
		Relative
	};

	// Expression-context path resolution only. This deliberately does not live
	// in the generic Path wrapper: the base is a Shell selection, not process
	// state. Windows distinguishes fully qualified, root-relative, drive-relative,
	// and ordinary relative forms:
	// https://learn.microsoft.com/dotnet/standard/io/file-path-formats
	class SelectionPathResolver final
	{
	public:
		SelectionPathResolver(std::wstring_view directory, std::wstring_view parent)
			: _base(directory.empty() ? parent : directory)
		{
		}

		const std::wstring &base() const noexcept { return _base; }

		static WindowsPathForm classify(std::wstring_view path) noexcept
		{
			if(path.empty())
				return WindowsPathForm::Empty;

			if(path.size() >= 2 && is_separator(path[0]) && is_separator(path[1]))
				return WindowsPathForm::FullyQualified; // UNC or device namespace.

			if(has_drive_prefix(path))
			{
				if(path.size() >= 3 && is_separator(path[2]))
					return WindowsPathForm::FullyQualified;
				return WindowsPathForm::DriveRelative;
			}

			if(is_separator(path[0]))
				return WindowsPathForm::RootRelative;

			return WindowsPathForm::Relative;
		}

		std::wstring resolve(std::wstring_view path) const
		{
			if(path.empty() || _base.empty())
				return std::wstring(path);

			switch(classify(path))
			{
				case WindowsPathForm::FullyQualified:
					return std::wstring(path);

				case WindowsPathForm::RootRelative:
				{
					auto root = volume_root(_base);
					if(root.empty())
						return std::wstring(path);
					auto first = size_t(0);
					while(first < path.size() && is_separator(path[first]))
						++first;
					return combine(root, path.substr(first));
				}

				case WindowsPathForm::DriveRelative:
				{
					auto base_drive = drive_letter(_base);
					if(base_drive && equal_drive(base_drive, path[0]))
						return combine(_base, path.substr(2));

					// A different-drive D:foo means the current directory on D:, not
					// D:\foo. Preserve that documented meaning, but consume the
					// process state exactly once into an absolute path so the eventual
					// filesystem call cannot race a later CWD change.
					// https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-getfullpathnamew
					return qualify_drive_relative_once(path);
				}

				case WindowsPathForm::Relative:
					return combine(_base, path);

				case WindowsPathForm::Empty:
				default:
					return std::wstring(path);
			}
		}

	private:
		std::wstring _base;

		static constexpr bool is_separator(wchar_t c) noexcept
		{
			return c == L'\\' || c == L'/';
		}

		static constexpr bool is_drive_letter(wchar_t c) noexcept
		{
			return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
		}

		static constexpr wchar_t fold_drive(wchar_t c) noexcept
		{
			return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - (L'a' - L'A')) : c;
		}

		static constexpr bool equal_drive(wchar_t left, wchar_t right) noexcept
		{
			return fold_drive(left) == fold_drive(right);
		}

		static constexpr bool has_drive_prefix(std::wstring_view path) noexcept
		{
			return path.size() >= 2 && is_drive_letter(path[0]) && path[1] == L':';
		}

		static constexpr bool ascii_equal(wchar_t left, wchar_t right) noexcept
		{
			if(left >= L'a' && left <= L'z') left = static_cast<wchar_t>(left - (L'a' - L'A'));
			if(right >= L'a' && right <= L'z') right = static_cast<wchar_t>(right - (L'a' - L'A'));
			return left == right;
		}

		static bool starts_with_unc_token(std::wstring_view path, size_t offset) noexcept
		{
			return path.size() >= offset + 4
				&& ascii_equal(path[offset], L'U')
				&& ascii_equal(path[offset + 1], L'N')
				&& ascii_equal(path[offset + 2], L'C')
				&& is_separator(path[offset + 3]);
		}

		static wchar_t drive_letter(std::wstring_view path) noexcept
		{
			if(has_drive_prefix(path))
				return path[0];

			// \\?\C:\... and \\.\C:\...
			if(path.size() >= 7 && is_separator(path[0]) && is_separator(path[1])
				&& (path[2] == L'?' || path[2] == L'.') && is_separator(path[3])
				&& is_drive_letter(path[4]) && path[5] == L':' && is_separator(path[6]))
				return path[4];

			return 0;
		}

		static size_t find_separator(std::wstring_view path, size_t first) noexcept
		{
			for(auto i = first; i < path.size(); ++i)
				if(is_separator(path[i])) return i;
			return std::wstring_view::npos;
		}

		static std::wstring unc_root(std::wstring_view path, size_t server_start)
		{
			auto server_end = find_separator(path, server_start);
			if(server_end == std::wstring_view::npos)
				return {};

			auto share_start = server_end + 1;
			while(share_start < path.size() && is_separator(path[share_start]))
				++share_start;
			if(share_start == path.size())
				return {};

			auto share_end = find_separator(path, share_start);
			if(share_end == std::wstring_view::npos)
			{
				std::wstring root(path);
				root.push_back(L'\\');
				return root;
			}

			return std::wstring(path.substr(0, share_end + 1));
		}

		static std::wstring volume_root(std::wstring_view path)
		{
			if(path.size() >= 3 && has_drive_prefix(path) && is_separator(path[2]))
				return std::wstring(path.substr(0, 3));

			if(path.size() < 2 || !is_separator(path[0]) || !is_separator(path[1]))
				return {};

			auto component_start = size_t(2);
			if(path.size() >= 4 && (path[2] == L'?' || path[2] == L'.')
				&& is_separator(path[3]))
			{
				component_start = 4;
				if(path.size() >= component_start + 3
					&& is_drive_letter(path[component_start])
					&& path[component_start + 1] == L':'
					&& is_separator(path[component_start + 2]))
					return std::wstring(path.substr(0, component_start + 3));

				if(starts_with_unc_token(path, component_start))
					return unc_root(path, component_start + 4);

				// Volume GUID and other device-volume forms root at their first
				// component after the device prefix.
				auto component_end = find_separator(path, component_start);
				if(component_end != std::wstring_view::npos)
					return std::wstring(path.substr(0, component_end + 1));
				return {};
			}

			return unc_root(path, component_start);
		}

		static std::wstring combine(std::wstring_view base, std::wstring_view relative)
		{
			if(base.empty()) return std::wstring(relative);
			if(relative.empty()) return std::wstring(base);

			std::wstring result(base);
			if(!is_separator(result.back()))
				result.push_back(L'\\');
			result.append(relative);
			return result;
		}

		static std::wstring qualify_drive_relative_once(std::wstring_view path)
		{
			std::wstring source(path);
			// 32,767 is the documented extended maximum. One fill call avoids a
			// size/fill pair observing two different process current directories.
			constexpr DWORD capacity = 32768;
			std::wstring absolute(capacity, L'\0');
			auto length = ::GetFullPathNameW(source.c_str(), capacity, absolute.data(), nullptr);
			if(length == 0 || length >= capacity)
				return source;
			absolute.resize(length);
			return absolute;
		}
	};
}
