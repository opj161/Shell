#pragma once
#include "Expression\Variable.h"
#include "Include\Theme.h"
#include "Include\BitmapCache.h"
#include <Resource.h>
#include <mutex>
#include <memory>

namespace Nilesoft
{
	namespace Shell
	{
		static bool SHLoadIndirectString(const wchar_t *pszSource, wchar_t *pszOutBuf, uint32_t cchOutBuf, void **ppvReserved = nullptr)
		{
			return SUCCEEDED(DLL::Invoke<HRESULT>(L"shlwapi.dll", "SHLoadIndirectString",
												  pszSource, pszOutBuf, cchOutBuf, ppvReserved));
		}

		struct Package
		{
			string name;
			string path;
			string family;
			string id;
			string version;
		};

		class PackagesCache
		{
			inline static const string _local_settings = LR"(Software\Classes\Local Settings)";
			inline static const string _packages = _local_settings + LR"(\Software\Microsoft\Windows\CurrentVersion\AppModel\Repository\Packages)";
			inline static const string _mrt_cache = _local_settings + LR"(\MrtCache)";

		public:

			PackagesCache() = default;
			~PackagesCache() = default;

			std::vector<Package> all() const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				if(!_loaded)
				{
					const_cast<PackagesCache *>(this)->ensure_loaded_locked();
				}
				return _list;
			}

			void clear()
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_list.clear();
				_loaded = false;
			}

			std::optional<Package> find(const wchar_t *name) const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				if(!_loaded)
				{
					const_cast<PackagesCache *>(this)->ensure_loaded_locked();
				}
				for(const auto &pk : _list)
				{
					if(pk.id.contains(name))
						return pk;
				}
				return std::nullopt;
			}

			bool exists(const wchar_t *name) const
			{
				return find(name).has_value();
			}

		private:

			mutable std::mutex _mutex;
			mutable bool _loaded = false;
			std::vector<Package> _list;

			bool ensure_loaded_locked()
			{
				if(_loaded)
					return true;

				std::vector<Package> next;
				if(!load_into(next))
					return false;

				_list = std::move(next);
				_loaded = true;
				return true;
			}

			bool load_into(std::vector<Package> &out_list)
			{
				HKEY hkeyPackages = nullptr;
				TResult res = ::RegOpenKeyExW(HKEY_CURRENT_USER, _packages, 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &hkeyPackages);

				if(!res || !hkeyPackages)
					return false;

				res = ERROR_SUCCESS;
				for(int i = 0; res.get() != ERROR_NO_MORE_ITEMS; i++)
				{
					string name(MAX_PATH);
					DWORD cchName = MAX_PATH;
					res = ::RegEnumKeyExW(hkeyPackages, i, name.data(), &cchName, nullptr, nullptr, nullptr, nullptr);
					if(res)
					{
						Package pk;
						pk.path = name.release(cchName);

						std::vector<string> id;
						pk.path.split(id, L'_');
						if(id.size() >= 4)
						{
							pk.family = id[0];
							pk.version = id[1];
							pk.id = id[id.size() - 1];
						}

						HKEY hkeyPackage = nullptr;
						res = ::RegOpenKeyExW(hkeyPackages, pk.path, 0, KEY_QUERY_VALUE, &hkeyPackage);
						if(res)
						{
							load_package(&pk, hkeyPackage);
							::RegCloseKey(hkeyPackage);
						}
						out_list.push_back(pk);
					}
				}
				::RegCloseKey(hkeyPackages);
				return true;
			}

			string get_value(const wchar_t *name, HKEY hkey)
			{
				string str(MAX_PATH);
				DWORD cb = MAX_PATH * sizeof(wchar_t);
				DWORD dtype = REG_SZ;
				TResult res = ::RegGetValueW(hkey, nullptr, name, RRF_RT_REG_SZ, &dtype, str.data(), &cb);
				if(res)
					return str.release(cb / sizeof(wchar_t));
				return nullptr;
			}

			void load_package(Package *pk, HKEY hkeyPackage)
			{
				pk->name = get_value(L"DisplayName", hkeyPackage).move();

				if(pk->name.length() > 3)
				{
					if(pk->name.starts_with(L"@{", false) &&
					   pk->name.back(L'}', false))
					{
						wchar_t displayName[MAX_PATH]{};
						if(!SHLoadIndirectString(pk->name, displayName, MAX_PATH, nullptr) || pk->name.equals(displayName))
						{
							displayName[0] = 0;
							HKEY hkeyPackages_mrt = nullptr;
							TResult res = ::RegOpenKeyExW(HKEY_CURRENT_USER, _mrt_cache, 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &hkeyPackages_mrt);
							if(res)
							{
								for(int i = 0; res.get() != ERROR_NO_MORE_ITEMS; i++)
								{
									string name(MAX_PATH);
									DWORD cchName = MAX_PATH;
									res = ::RegEnumKeyExW(hkeyPackages_mrt, i, name.data(), &cchName, nullptr, nullptr, nullptr, nullptr);
									if(res)
									{
										name.release(cchName);
										if(name.contains(pk->id))
										{
											auto hkeyLang = get_langKey(hkeyPackages_mrt, name);
											if(hkeyLang)
											{
												for(i = 0; res.get() != ERROR_NO_MORE_ITEMS; i++)
												{
													wchar_t valueName[MAX_PATH]{};
													DWORD cbDisplayName = MAX_PATH;
													displayName[0] = 0;
													DWORD cchValueName = MAX_PATH;
													DWORD dtype = 0;
													res = ::RegEnumValueW(hkeyLang, i, valueName, &cchValueName, nullptr, &dtype, reinterpret_cast<LPBYTE>(displayName), &cbDisplayName);
													if(res && pk->name.equals(valueName))
													{
														pk->name = displayName;
														break;
													}
												}
												::RegCloseKey(hkeyLang);
											}
											break;
										}
									}
								}
								::RegCloseKey(hkeyPackages_mrt);
							}
						}
						pk->name = displayName;
					}
				}
			}

			HKEY get_langKey(HKEY hkey, const wchar_t *subkey = nullptr)
			{
				HKEY result = nullptr;
				wchar_t name[MAX_PATH]{};
				DWORD cchName = MAX_PATH;
				HKEY hKeyLangList = nullptr;

				TResult res = ::RegOpenKeyExW(hkey, subkey, 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &hKeyLangList);
				if(res)
				{
					res = ::RegEnumKeyExW(hKeyLangList, 0, name, &cchName, nullptr, nullptr, nullptr, nullptr);
					if(res)
					{
						HKEY hKeyLang = nullptr;
						res = ::RegOpenKeyExW(hKeyLangList, name, 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &hKeyLang);
						if(res)
						{
							name[0] = {};
							cchName = MAX_PATH;
							res = ::RegEnumKeyExW(hKeyLang, 0, name, &cchName, nullptr, nullptr, nullptr, nullptr);
							if(res)
								::RegOpenKeyExW(hKeyLang, name, 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &result);
							::RegCloseKey(hKeyLang);
						}
					}
					::RegCloseKey(hKeyLangList);
				}
				return result;
			}
		};

		class FontCache
		{
			std::unordered_map<uint32_t, Font*> fonts;
			HANDLE _handle{};
			mutable std::mutex _mutex;

		public:
			static constexpr auto  Default = L"Nilesoft.Shell";
			static constexpr auto  SegoeFluentIcons = L"Segoe Fluent Icons";
			static constexpr auto  SegoeMDL2Assets = L"Segoe MDL2 Assets";

		public:
			uint32_t _dpi = 96;
			uint32_t default_id = 0;

			FontCache() = default;

			~FontCache()
			{
				clear();
				if(_handle)
					::RemoveFontMemResourceEx(_handle);
			}

			void init(HINSTANCE hinstance, uint32_t dpi = 96)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				clear_unlocked();
				_dpi = dpi;
				auto hRes = ::FindResourceW(hinstance, L"FONTICON", RT_RCDATA);
				if(!hRes)
				{
					Logger::Warning(L"FONTICON not found");
					return;
				}

				auto hResData = ::LoadResource(hinstance, hRes);
				if(hResData)
				{
					auto lpFileView = ::LockResource(hResData);
					if(lpFileView)
					{
						auto cjSize = ::SizeofResource(hinstance, hRes);
						DWORD numFonts = 0;
						_handle = ::AddFontMemResourceEx(lpFileView, cjSize, nullptr, &numFonts);
						UnlockResource(lpFileView);
					}
					::FreeResource(hResData);
				}
			}

			Font *at(uint32_t id) const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				auto it = fonts.find(id);
				return it != fonts.end() ? it->second : nullptr;
			}

			Font *at(HFONT hfont) const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				for(auto &font : fonts)
				{
					if(hfont == font.second->get()) 
						return font.second;
				}
				return nullptr;
			}

			bool remove(HFONT hfont)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				for(auto font = fonts.begin(); font != fonts.end(); ++font)
				{
					if(hfont == font->second->get())
					{
						delete font->second;
						fonts.erase(font);
						return true;
					}
				}
				return false;
			}

			HFONT add(const string &name, int size, int charset = DEFAULT_CHARSET)
			{
				return add(name, size, _dpi, charset);
			}

			HFONT add(const string &name, int size, uint32_t dpi, int charset = DEFAULT_CHARSET)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				auto id = Hash::dohash(size + dpi, name.hash());
				auto it = fonts.find(id);
				if(it != fonts.end() && it->second)
					return it->second->get();

				auto font = new Font(name, size, CLEARTYPE_QUALITY, charset);
				if(font->get())
				{
					fonts[id] = font;
					return font->get();
				}
				delete font;
				return nullptr;
			}

			void clear()
			{
				std::lock_guard<std::mutex> lock(_mutex);
				clear_unlocked();
			}

			size_t size() const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return fonts.size();
			}

		private:
			void clear_unlocked()
			{
				for(auto &font : fonts)
					delete font.second;

				fonts.clear();
				default_id = {};
			}
		};

		struct ImageCache
		{
			std::vector<uint32_t> id;
			auto_expr value;

			bool equals(uint32_t ident) const
			{
				for(auto &it : id)
				{
					if(it == ident) return true;
				}
				return false;
			}

			bool equals(std::vector<uint32_t> const &idents) const
			{
				for(auto& i : idents)
				{
					if(equals(i)) return true;
				}
				return false;
			}

			void add(uint32_t ident)
			{
				for(auto &it : id)
					if(it == ident) return;
				id.push_back(ident);
			}

			void add(std::vector<uint32_t> ids)
			{
				for(auto &it : ids) add(it);
			}
		};

		struct CACHE
		{
			uint64_t					generation{ 0 };
			Settings					settings;

			struct {
				Scope global;
				Scope runtime;
				Scope loc;
			} variables;

			std::vector<NativeMenu*>	statics;
			NativeMenu					dynamic;
			FontCache					fonts;
			PackagesCache				Packages;
			std::vector<ImageCache>		images;
			BitmapCache					bitmaps;
			std::unordered_map<uint32_t, MUID> muid;

			struct
			{
				uint32_t color = IDENT_DEFAULT;
				uint32_t back = IDENT_DEFAULT;
				string name = FontCache::Default;
			}
			glyph;

			CACHE() = default;

			~CACHE()
			{
				clear();
			}

			const MUID* find_muid(uint32_t hash) const
			{
				auto it = muid.find(hash);
				if(it != muid.end())
					return &it->second;
				for(const auto &p : muid)
				{
					if(p.second.id == hash)
						return &p.second;
				}
				return nullptr;
			}

			void clear()
			{
				while(!statics.empty())
				{
					delete statics.back();
					statics.pop_back();
				}

				images.clear();
				bitmaps.clear();
				muid.clear();
				
				variables.global.clear(true);
				variables.runtime.clear(true);
				variables.loc.clear(true);

				Packages.clear();
				fonts.clear();
			}

			void reload(uint32_t dpi = 96)
			{
				fonts._dpi = dpi;
				fonts.add(glyph.name, Theme::SystemMetrics(SM_CXSMICON, dpi), dpi);
			}

			Expression *get_image(uint32_t id) const
			{
				for(auto &si : images)
				{
					if(si.equals(id))
						return si.value;
				}
				return nullptr;
			}

			void add_image(std::vector<uint32_t> &&ids, Expression *e)
			{
				if(e)
				{
					for(auto &img : images)
					{
						if(img.equals(ids))
						{
							img.add(ids);
							img.value = e;
							return;
						}
					}

					images.emplace_back(std::move(ids), e);
				}
			}
		};
	}
}
