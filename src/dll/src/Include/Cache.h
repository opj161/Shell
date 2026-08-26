#pragma once
#include "Expression\Variable.h"
#include "Include\Theme.h"
#include "Include\BitmapCache.h"
#include "Include\Packages.h"
#include "Include\PackageCatalogService.h"
#include "Include\PackagesCache.h"
#include <Resource.h>
#include <mutex>
#include <memory>

namespace Nilesoft
{
	namespace Shell
	{
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

			// Every file this generation was parsed from, root first, in load
			// order - Parser::LoadedFiles() copied out before the parser goes
			// away. `RuleProvenance::file` indexes it.
			//
			// Held per generation rather than process-wide because that is what
			// makes the index meaningful: an open menu holds its own generation
			// until it closes, so a rule pointer and this vector always come
			// from the same parse. See Include/RuleProvenance.h.
			std::vector<std::wstring>	files;
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

			// The path a rule's provenance names, or nullptr. Never an empty
			// string for a missing entry: a caller printing "" beside a line
			// number would produce ":41", which reads as a file called nothing
			// rather than as an answer nobody has.
			const wchar_t *file_name(const RuleProvenance &at) const
			{
				if(!at.known() || at.file >= files.size())
					return nullptr;
				return files[at.file].c_str();
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
				files.clear();
				
				variables.global.clear(true);
				variables.runtime.clear(true);
				variables.loc.clear(true);

				// Packages is deliberately not cleared. It holds no state of its
				// own any more - the answers come from the catalog service, which
				// is process-lifetime and has nothing to do with which
				// configuration generation is loaded. Clearing it here threw away
				// the package index on every config reload, and since the watcher
				// landed that is every save.
				// docs/refactor/02-first-paint-latency.md section 2.1 step 4.
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
