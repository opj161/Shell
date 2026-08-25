#pragma once
#include "Expression\Variable.h"
#include "Include\Theme.h"
#include "Include\BitmapCache.h"
#include "Include\Packages.h"
#include "Include\PackageCatalogService.h"
#include <Resource.h>
#include <mutex>
#include <memory>

namespace Nilesoft
{
	namespace Shell
	{
		/*
			Package (MSIX/AppX) lookup for the appx()/package() NSS functions.

			A bridge to the expression evaluator's string type, and nothing more.
			It owns no index and no registry source: the answers come out of the
			catalog snapshot that PackageCatalogService publishes from its worker
			thread, which already enumerates every installed package and resolves
			every install path in the course of finding packaged verbs.

			What this replaces, and why
			---------------------------

			`PackagesCache` used to hold a `PackageIndex` and a
			`RegistryPackageSource` of its own, as a member of the immutable
			config CACHE. Three things followed, all of them live on a stock
			install:

			  - a menu-thread `package.*` evaluation could enter ensure_index()
			    and enumerate the package repository (~2 ms) or block on a
			    condition variable waiting for another thread's scan;
			  - `CACHE::clear()` called `Packages.clear()`, so every config
			    reload threw the index away - and since the config watcher landed
			    (docs/refactor/03-config-safety.md section 3a) a reload happens on
			    every save;
			  - two mechanisms answered questions about the same packages.

			And it was not a power-user path. The shipped configuration evaluates
			`package.exists("WindowsTerminal")` on every menu
			(src/bin/imports/terminal.nss line 8) and `package.path(...)` on the
			line after it.

			docs/refactor/02-first-paint-latency.md section 2.1 step 4 asked for
			exactly this: "`CACHE::clear()` stops touching packages entirely".
			docs/refactor/09-remediation-plan.md R3 is the rest of it.

			The one exception, stated rather than hidden
			-------------------------------------------

			`display_name` still resolves on demand, and it is not a cheap read:
			`RegistryPackageSource::resolve_display_name` can call
			`SHLoadIndirectString`, which for a `@{PackageFullName?ms-resource:...}`
			form loads the package's `Resources.pri`
			(https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-shloadindirectstring),
			and can then walk the MrtCache tree. Only `appx.name`/`package.name`
			reach it, no shipped configuration uses them, and it is timed under
			its own phase so a report says when somebody's does.
		*/
		class PackagesCache
		{
		public:
			bool exists(const wchar_t *name) const
			{
				return static_cast<bool>(find_entry(name));
			}

			std::optional<PackageIdentity> find_identity(const wchar_t *name) const
			{
				if(auto found = find_entry(name))
					return found->identity;
				return std::nullopt;
			}

			// The real installation directory. The repository subkey records the
			// package full name and not a path, so it has to be resolved - which
			// the catalog scan already did, on its own thread, for every package
			// it walked. This is the published result of that.
			string path(const wchar_t *name) const
			{
				auto found = find_entry(name);
				if(found && !found->install_path.empty())
					return string(found->install_path.c_str());
				return {};
			}

			// The only entry point that touches localized resources, and the one
			// exception to "a package query is a snapshot read". See the note
			// above; the phase is what keeps it honest.
			string display_name(const wchar_t *name) const
			{
				auto found = find_entry(name);
				if(!found)
					return {};

				// Not wrapped in a MenuPerfScope, and the reason is worth
				// recording: including Include/Diagnostics/MenuPerf.h from this
				// header introduces `Nilesoft::Shell::Diagnostics` into every
				// translation unit that includes Cache.h, and inside
				// `namespace Nilesoft::Shell` an unqualified `Diagnostics` then
				// stops meaning `Nilesoft::Diagnostics` - which is what
				// Expression/FuncExpression.cpp's `Diagnostics::ShellExec::Run`
				// means. AGENTS.md, "Namespaces". The cost of this call is
				// therefore attributed to whichever phase is evaluating the
				// expression, which for a menu is `native.modify_rules`.
				RegistryPackageSource source;
				auto resolved = source.resolve_display_name(found->identity.full_name);
				if(resolved.empty())
					return {};
				return string(resolved.c_str());
			}

			std::vector<PackageIdentity> all() const
			{
				std::vector<PackageIdentity> out;
				auto snapshot = catalog();
				if(!snapshot)
					return out;
				out.reserve(snapshot->packages.size());
				for(const auto &entry : snapshot->packages)
					out.push_back(entry.identity);
				return out;
			}

		private:
			/*
				The published snapshot, with the same bounded first wait the
				packaged-verb path uses.

				Not `snapshot()`, which never waits: a cold `package.exists()`
				answering *false* would take the stock configuration's Terminal
				item out of the first menu of every process - a worse defect than
				the one being fixed, and a silent one. `snapshot_for_menu()`
				waits only for the first scan, only up to its budget, and counts
				the wait (`catalog.first_wait`), which is the instrument
				docs/refactor/02-first-paint-latency.md section 2.1 step 3 used
				to decline persistence.
			*/
			static std::shared_ptr<const CatalogSnapshot> catalog()
			{
				return PackageCatalogService::instance().snapshot_for_menu();
			}

			// Returns a pointer into a snapshot the caller does not hold. Safe
			// only because every caller here uses it and discards it within one
			// expression - see the shared_ptr kept alive for the duration below.
			struct Found
			{
				std::shared_ptr<const CatalogSnapshot> keep;
				const PackageEntry *entry{};
				const PackageEntry *operator->() const { return entry; }
				explicit operator bool() const { return entry != nullptr; }
			};

			Found find_entry(const wchar_t *name) const
			{
				Found found;
				if(!name || !*name)
					return found;

				found.keep = catalog();
				if(!found.keep)
					return found;

				for(const auto &entry : found.keep->packages)
				{
					if(package_full_name_matches(entry.identity.full_name, name))
					{
						found.entry = &entry;
						break;
					}
				}
				return found;
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
