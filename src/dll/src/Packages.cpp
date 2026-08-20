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
			for(DWORD i = 0;; i++)
			{
				wchar_t name[512]{};
				DWORD cch = static_cast<DWORD>(std::size(name));
				auto rc = ::RegEnumKeyExW(key, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
				if(rc == ERROR_SUCCESS)
					out.emplace_back(name, cch);
				else if(rc != ERROR_MORE_DATA)
					break;
			}

			::RegCloseKey(key);
			return true;
		}

		std::wstring RegistryPackageSource::resolve_path(const std::wstring &full_name)
		{
			return GetInstalledPackagePath(full_name);
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

		bool PackageIndex::ensure_index() const
		{
			std::vector<std::wstring> names;
			IPackageSource *source = nullptr;

			{
				std::unique_lock<std::mutex> lock(_mutex);

				// Another thread is already scanning: wait for it rather than
				// repeat the same enumeration.
				_cv.wait(lock, [this] { return _state != State::Loading; });

				if(_state == State::Ready)
					return true;

				if(!_source)
					return false;

				_state = State::Loading;
				source = _source;
			}

			// If the scan throws - it allocates once per package - the state must
			// not be left at Loading, or every later caller waits on a condition
			// variable nobody will notify, hanging the thread building the menu.
			struct loading_guard
			{
				PackageIndex const *self;
				bool done;
				~loading_guard()
				{
					if(done)
						return;
					{
						std::lock_guard<std::mutex> lock(self->_mutex);
						if(self->_state == State::Loading)
							self->_state = State::Empty;
					}
					self->_cv.notify_all();
				}
			} guard{ this, false };

			auto ok = source->enumerate_full_names(names);

			{
				std::lock_guard<std::mutex> lock(_mutex);

				if(ok)
				{
					_list.clear();
					_list.reserve(names.size());
					for(auto &n : names)
					{
						Entry e;
						if(parse_package_full_name(n, e.identity))
							_list.push_back(std::move(e));
					}
					_state = State::Ready;
				}
				else
				{
					// A transient failure must not be cached forever - packages
					// come and go, and the next menu should try again.
					_state = State::Empty;
				}
			}

			guard.done = true;
			_cv.notify_all();
			return ok;
		}

		long long PackageIndex::match_locked(const std::vector<Entry> &list, const wchar_t *query)
		{
			for(size_t i = 0; i < list.size(); i++)
			{
				if(icontains(list[i].identity.full_name, query))
					return static_cast<long long>(i);
			}
			return -1;
		}

		bool PackageIndex::exists(const wchar_t *query) const
		{
			if(!query || !*query || !ensure_index())
				return false;

			std::lock_guard<std::mutex> lock(_mutex);
			return match_locked(_list, query) >= 0;
		}

		std::optional<PackageIdentity> PackageIndex::find_identity(const wchar_t *query) const
		{
			if(!query || !*query || !ensure_index())
				return std::nullopt;

			std::lock_guard<std::mutex> lock(_mutex);
			auto i = match_locked(_list, query);
			if(i < 0)
				return std::nullopt;
			return _list[static_cast<size_t>(i)].identity;
		}

		std::optional<std::wstring> PackageIndex::path(const wchar_t *query) const
		{
			if(!query || !*query || !ensure_index())
				return std::nullopt;

			std::wstring full_name;
			IPackageSource *source = nullptr;

			{
				std::lock_guard<std::mutex> lock(_mutex);
				auto i = match_locked(_list, query);
				if(i < 0)
					return std::nullopt;

				auto &entry = _list[static_cast<size_t>(i)];
				if(entry.path_resolved)
					return entry.install_path;

				full_name = entry.identity.full_name;
				source = _source;
			}

			if(!source)
				return std::nullopt;

			// Outside the lock: this reaches the package manager.
			auto resolved = source->resolve_path(full_name);

			std::lock_guard<std::mutex> lock(_mutex);
			for(auto &entry : _list)
			{
				if(entry.identity.full_name == full_name)
				{
					entry.install_path = resolved;
					entry.path_resolved = true;
					break;
				}
			}
			return resolved;
		}

		std::optional<std::wstring> PackageIndex::display_name(const wchar_t *query) const
		{
			if(!query || !*query || !ensure_index())
				return std::nullopt;

			std::wstring full_name;
			IPackageSource *source = nullptr;

			{
				std::lock_guard<std::mutex> lock(_mutex);
				auto i = match_locked(_list, query);
				if(i < 0)
					return std::nullopt;

				auto &entry = _list[static_cast<size_t>(i)];
				if(entry.display_resolved)
					return entry.display;

				full_name = entry.identity.full_name;
				source = _source;
			}

			if(!source)
				return std::nullopt;

			auto resolved = source->resolve_display_name(full_name);

			std::lock_guard<std::mutex> lock(_mutex);
			for(auto &entry : _list)
			{
				if(entry.identity.full_name == full_name)
				{
					entry.display = resolved;
					entry.display_resolved = true;
					break;
				}
			}
			return resolved;
		}

		std::vector<PackageIdentity> PackageIndex::all_identities() const
		{
			std::vector<PackageIdentity> out;
			if(!ensure_index())
				return out;

			std::lock_guard<std::mutex> lock(_mutex);
			out.reserve(_list.size());
			for(auto &entry : _list)
				out.push_back(entry.identity);
			return out;
		}

		void PackageIndex::clear()
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_cv.wait(lock, [this] { return _state != State::Loading; });
			_list.clear();
			_state = State::Empty;
		}
	}
}
