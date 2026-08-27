// Deliberately does not pull in <pch.h>: the test project compiles this file
// directly, the same way it does ShellExt.cpp.
#include "Include\Packages.h"

#include <shlwapi.h>
#include <algorithm>

namespace Nilesoft
{
	namespace Shell
	{
		namespace
		{
			constexpr const wchar_t *PACKAGES_KEY =
				LR"(Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel\Repository\Packages)";

			constexpr const wchar_t *MRTCACHE_KEY =
				LR"(Software\Classes\Local Settings\MrtCache)";

			wchar_t upper(wchar_t c) noexcept
			{
				return static_cast<wchar_t>(reinterpret_cast<UINT_PTR>(
					::CharUpperW(reinterpret_cast<LPWSTR>(static_cast<UINT_PTR>(c)))));
			}

			bool iequal(wchar_t a, wchar_t b) noexcept { return upper(a) == upper(b); }

			// Case-insensitive substring, matching the documented "full name or
			// part of name" behaviour of appx()/package().
			bool icontains(const std::wstring &haystack, const wchar_t *needle)
			{
				if(!needle || !*needle)
					return false;

				std::wstring n(needle);
				if(n.size() > haystack.size())
					return false;

				return std::search(haystack.begin(), haystack.end(),
								   n.begin(), n.end(), iequal) != haystack.end();
			}

			std::wstring reg_string(HKEY key, const wchar_t *name)
			{
				DWORD cb = 0;
				DWORD type = 0;
				if(::RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, &type, nullptr, &cb) != ERROR_SUCCESS
				   || cb < sizeof(wchar_t))
					return {};

				std::wstring value(cb / sizeof(wchar_t), L'\0');
				if(::RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, &type, value.data(), &cb) != ERROR_SUCCESS)
					return {};

				auto cch = cb / sizeof(wchar_t);
				value.resize(cch ? cch - 1 : 0);
				return value;
			}
		}

		bool parse_package_full_name(const std::wstring &full_name, PackageIdentity &out)
		{
			// Name_Version_Architecture_ResourceId_PublisherId. ResourceId is
			// normally empty, so the string usually carries a literal "__".
			auto first = full_name.find(L'_');
			if(first == std::wstring::npos || first == 0)
				return false;

			auto second = full_name.find(L'_', first + 1);
			if(second == std::wstring::npos)
				return false;

			auto last = full_name.rfind(L'_');
			if(last == std::wstring::npos || last < second || last + 1 >= full_name.size())
				return false;

			out.full_name = full_name;
			out.name = full_name.substr(0, first);
			out.version = full_name.substr(first + 1, second - first - 1);
			out.publisher = full_name.substr(last + 1);
			out.family = out.name + L'_' + out.publisher;
			return true;
		}

		std::wstring GetInstalledPackagePath(const std::wstring &full_name)
		{
			if(full_name.empty())
				return {};

			using fn_t = LONG(WINAPI *)(PCWSTR, UINT32 *, PWSTR);

			// Present since Windows 8; resolved once.
			static const fn_t fn = []() noexcept -> fn_t
			{
				if(auto k = ::GetModuleHandleW(L"kernel32.dll"))
					return reinterpret_cast<fn_t>(reinterpret_cast<void *>(
						::GetProcAddress(k, "GetPackagePathByFullName")));
				return nullptr;
			}();

			if(fn)
			{
				// Documented two-call pattern: ask for the length, then fill.
				UINT32 length = 0;
				if(fn(full_name.c_str(), &length, nullptr) == ERROR_INSUFFICIENT_BUFFER && length > 0)
				{
					std::wstring path(length, L'\0');
					if(fn(full_name.c_str(), &length, path.data()) == ERROR_SUCCESS)
					{
						path.resize(length ? length - 1 : 0);
						return path;
					}
				}
			}

			// The API answers only for packages registered to this user. The
			// repository key records the same directory, so fall back to it.
			std::wstring subkey = PACKAGES_KEY;
			subkey += L'\\';
			subkey += full_name;

			HKEY key = nullptr;
			if(::RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
			{
				auto root = reg_string(key, L"PackageRootFolder");
				::RegCloseKey(key);
				return root;
			}

			return {};
		}

		std::vector<std::wstring> GetPackageFullNamesByFamily(const std::wstring &family)
		{
			std::vector<std::wstring> out;
			if(family.empty())
				return out;

			using fn_t = LONG(WINAPI *)(PCWSTR, UINT32 *, PWSTR *, UINT32 *, PWSTR);

			static const fn_t fn = []() noexcept -> fn_t
			{
				if(auto k = ::GetModuleHandleW(L"kernel32.dll"))
					return reinterpret_cast<fn_t>(reinterpret_cast<void *>(
						::GetProcAddress(k, "GetPackagesByPackageFamily")));
				return nullptr;
			}();

			if(!fn)
				return out;

			// The API writes `count` pointers into the array and packs the strings
			// themselves into the shared character buffer, so both have to be
			// sized from what the first call reports - not guessed.
			UINT32 count = 0;
			UINT32 buffer_length = 0;
			if(fn(family.c_str(), &count, nullptr, &buffer_length, nullptr) != ERROR_INSUFFICIENT_BUFFER
			   || count == 0 || buffer_length == 0)
				return out;

			std::vector<PWSTR> names(count, nullptr);
			std::vector<wchar_t> buffer(buffer_length, L'\0');

			if(fn(family.c_str(), &count, names.data(), &buffer_length, buffer.data()) != ERROR_SUCCESS)
				return out;

			out.reserve(count);
			for(UINT32 i = 0; i < count; i++)
			{
				if(names[i])
					out.emplace_back(names[i]);
			}
			return out;
		}

		bool RegistryPackageSource::enumerate_full_names(std::vector<std::wstring> &out)
		{
			HKEY key = nullptr;
			if(::RegOpenKeyExW(HKEY_CURRENT_USER, PACKAGES_KEY, 0,
							   KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS)
				return false;

			// Subkey names only. Opening every package key and reading every value
			// is what used to make this scan expensive.
			//
			// Only ERROR_NO_MORE_ITEMS ends this walk successfully. RegEnumKeyEx
			// distinguishes three outcomes and this loop used to collapse all of
			// them: "If the function succeeds, the return value is ERROR_SUCCESS.
			// If the function fails, the return value is a system error code. If
			// there are no more subkeys available, the function returns
			// ERROR_NO_MORE_ITEMS. If the lpName buffer is too small to receive
			// the name of the key, the function returns ERROR_MORE_DATA."
			// https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regenumkeyexw
			//
			// The Remarks say the same thing as an instruction: call it "until
			// there are no more subkeys (meaning the function returns
			// ERROR_NO_MORE_ITEMS)".
			//
			// `else if(rc != ERROR_MORE_DATA) break; ... return true` therefore
			// reported a partial enumeration as a complete one, and the caller
			// published the result as the machine's whole package set. What that
			// costs is in PackageCatalogService::run.
			//
			// ERROR_MORE_DATA is treated as a failure rather than skipped, and
			// that is the one behaviour change here rather than a strictly
			// stricter reading. A package full name is bounded by
			// PACKAGE_FULL_NAME_MAX_LENGTH - name 50 + version 23 +
			// architecture + resource id 30 + publisher id 13 with separators,
			// about 127 characters (Windows Kits/10/Include/.../um/minappmodel.h)
			// - so a 512-character buffer cannot overflow in practice, and a hit
			// means something is wrong with the key rather than with the buffer.
			// Silently advancing past an indexed key is the behaviour worth
			// removing.
			for(DWORD i = 0;; i++)
			{
				wchar_t name[512]{};
				DWORD cch = static_cast<DWORD>(std::size(name));
				auto rc = ::RegEnumKeyExW(key, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
				if(rc == ERROR_SUCCESS)
				{
					out.emplace_back(name, cch);
					continue;
				}

				::RegCloseKey(key);
				return rc == ERROR_NO_MORE_ITEMS;
			}
		}

		std::wstring RegistryPackageSource::resolve_display_name(const std::wstring &full_name)
		{
			std::wstring subkey = PACKAGES_KEY;
			subkey += L'\\';
			subkey += full_name;

			HKEY key = nullptr;
			if(::RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
				return {};

			auto display = reg_string(key, L"DisplayName");
			::RegCloseKey(key);

			// A plain string is already the answer. Only an indirect reference
			// "@{...}" needs resource loading, and only that case is worth the
			// MrtCache walk below.
			if(display.size() <= 3 || display.compare(0, 2, L"@{") != 0 || display.back() != L'}')
				return display;

			wchar_t resolved[512]{};
			if(SUCCEEDED(::SHLoadIndirectString(display.c_str(), resolved,
												static_cast<UINT>(std::size(resolved)), nullptr))
			   && resolved[0] && display != resolved)
			{
				return resolved;
			}

			// SHLoadIndirectString cannot reach resources for a package that is
			// not currently registered; MrtCache keeps the last resolved strings.
			PackageIdentity identity;
			if(!parse_package_full_name(full_name, identity))
				return {};

			HKEY mrt = nullptr;
			if(::RegOpenKeyExW(HKEY_CURRENT_USER, MRTCACHE_KEY, 0,
							   KEY_ENUMERATE_SUB_KEYS, &mrt) != ERROR_SUCCESS)
				return {};

			std::wstring result;

			for(DWORD i = 0;; i++)
			{
				wchar_t name[512]{};
				DWORD cch = static_cast<DWORD>(std::size(name));
				if(::RegEnumKeyExW(mrt, i, name, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
					break;

				// Matched on the package name, not the full name: MrtCache keys
				// embed the version that was current when the strings were
				// cached, which drifts behind the installed one.
				std::wstring entry(name, cch);
				if(!icontains(entry, identity.name.c_str()))
					continue;

				// <package>\<merged resource file>\<language> holds the values.
				HKEY level = nullptr;
				if(::RegOpenKeyExW(mrt, entry.c_str(), 0,
								   KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &level) != ERROR_SUCCESS)
					break;

				for(int depth = 0; depth < 2 && level; depth++)
				{
					wchar_t child[512]{};
					DWORD child_cch = static_cast<DWORD>(std::size(child));
					HKEY next = nullptr;
					if(::RegEnumKeyExW(level, 0, child, &child_cch, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
						::RegOpenKeyExW(level, child, 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &next);

					::RegCloseKey(level);
					level = next;
				}

				if(level)
				{
					for(DWORD v = 0;; v++)
					{
						wchar_t value_name[512]{};
						wchar_t value_data[512]{};
						DWORD name_cch = static_cast<DWORD>(std::size(value_name));
						DWORD data_cb = sizeof(value_data);
						DWORD type = 0;

						if(::RegEnumValueW(level, v, value_name, &name_cch, nullptr, &type,
										   reinterpret_cast<LPBYTE>(value_data), &data_cb) != ERROR_SUCCESS)
							break;

						if(display == value_name)
						{
							result = value_data;
							break;
						}
					}
					::RegCloseKey(level);
				}
				break;
			}

			::RegCloseKey(mrt);
			return result;
		}

		bool package_full_name_matches(const std::wstring &full_name, const wchar_t *query)
		{
			return icontains(full_name, query);
		}
	}
}
