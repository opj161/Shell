
#include <pch.h>
#include "Include\Hooker.h"
#include "Include/ContextMenu.h"
#include "ConfigShadow.h"

namespace Nilesoft
{
	namespace Shell
	{
		bool Initializer::init(HINSTANCE hInstance)
		{
			HInstance = hInstance;
			instance = this;

			try
			{
				application.Name = APP_NAME;
				application.FileName = APP_FILENAME_TITLE;
				application.Version = APP_VERSION;

				application.Path = Path::Module(hInstance).move();

				application.Root = Path::GetRoot(application.Path);
				application.Dirctory = Path::Parent(application.Path).move();

				// Nilesoft Shell Script (NSS)
				// Nilesoft Shell Object (NSO)
				application.ConfigPortable = Path::Join(application.Dirctory, L"\\shell.nss").move();
				application.Config = application.ConfigPortable;

				auto len = application.Path.length();
				if(len > 4)
				{
					string path = application.Path.substr(0, len - 3).move();
					application.Manager = (path + L"exe").move();
				}

				auto hwnd_foreground = ::GetForegroundWindow();
				if(Windows::Version::Instance().IsWindowsVersionOrGreater(10, 0, 14393))
					dpi = DLL::User32<uint32_t>("GetDpiForWindow", hwnd_foreground);
				else
					dpi = DC(hwnd_foreground).GetDeviceCapsY();

				//dpi = ::GetDpiForWindow(::GetForegroundWindow());//win10 version 1607 //10.0.14393
				is_elevated = Security::Elevation::IsElevated();
				//WIC::init();
			}
			catch(...)
			{
#ifdef _DEBUG
				Logger::Exception(__func__);
#endif
				return false;
			}
			return true;
		}

		Initializer::~Initializer()
		{
		}

		bool Initializer::init()
		{
			std::unique_lock<std::mutex> reload_lock(_reload_mutex);

			if(load_generation(nullptr))
				return true;

			// Where and why the file the user edits failed. Captured before
			// anything else runs, because a successful shadow parse clears it.
			auto failure = LastError;

			Status.Error.store(true, std::memory_order_relaxed);

			// The publish never ran, so whatever generation was already live is
			// untouched and still correct. That is the difference between
			// StaleWithError, which keeps serving menus, and a process that has
			// genuinely never loaded a configuration.
			auto served = has_snapshot();

			// A process that starts while the file is broken has nothing in
			// memory to fall back on, and that is the case the in-memory half of
			// last-known-good cannot cover. Try the shadow of the last set that
			// parsed cleanly. If that fails too, there is nothing to serve and
			// query() refuses, which is the old behaviour and the right one.
			// docs/refactor/03-config-safety.md section 1b
			if(!served)
			{
				auto shadow = ConfigShadow::resolve(ConfigShadow::default_directory());
				if(!shadow.empty())
				{
					string path = shadow.c_str();
					if(load_generation(&path))
					{
						served = true;
						Logger::Warning(L"configuration failed to parse; serving the last known good copy from '%s'",
										shadow.c_str());
					}
				}
			}

			// A successful parse - of the shadow - reports itself as a healthy
			// load, and that is the wrong story to tell here: the file the user
			// edits is still broken, and the error is what a UI reports and what
			// `shell.exe` has to be able to show. Put it back.
			if(served)
			{
				LastError = failure;
				Status.Error.store(true, std::memory_order_relaxed);
			}

			Status.Stale.store(served, std::memory_order_relaxed);

			// A reload was requested and has now been attempted. Leaving the flag
			// set would re-parse the broken file for every menu.
			Status.Refresh.store(false, std::memory_order_relaxed);

			// Served, from an older generation or from the shadow. Returning
			// false here would cost exactly one menu: query() passes this result
			// straight back to its caller, and only the *next* attempt would see
			// the snapshot and serve it.
			return served;
		}

		namespace
		{
			/*
				Copy into a fixed field, truncating rather than overflowing.

				string::Copy's count-taking overload writes count characters and
				*then* a terminator, so it needs count + 1 slots - passing the
				capacity overruns by one. That is the same off-by-one family as
				the `release(n - 1)` shape AGENTS.md records, so this says the
				rule in terms of the array's own size and never takes a count.
			*/
			template<size_t N>
			void copy_into(wchar_t (&dst)[N], const wchar_t *src)
			{
				dst[0] = L'\0';
				if(!src)
					return;

				size_t i = 0;
				for(; i + 1 < N && src[i]; i++)
					dst[i] = src[i];
				dst[i] = L'\0';
			}
		}

		/*
			The compiled COM activation policy, published for the hook to read.

			Separate from the CACHE and from _snapshot_mutex on purpose:
			CoCreateInstanceHook runs on every in-process and local-server
			activation in the host, and taking a mutex there to answer "is there
			anything to do?" would be the cost this compile exists to remove.
			An atomic shared_ptr swap gives the hook a lock-free read and the
			same publish-once semantics the config snapshot has.

			docs/refactor/01-takeover-contract.md section 9.
		*/
		namespace
		{
			std::mutex _policy_mutex;
			std::shared_ptr<const ComActivationPolicy> _policy;
		}

		std::shared_ptr<const ComActivationPolicy> current_com_policy()
		{
			std::lock_guard<std::mutex> lock(_policy_mutex);
			return _policy;
		}

		/*
			Compile it, from the same rules the hook used to walk per activation.

			Every CLSID any static rule names goes in, whether or not the rule
			carries a `where` - a mentioned CLSID is worth evaluating, and only
			the evaluation can say whether it is actually blocked right now. What
			the set buys is the far commoner answer: this CLSID is named by
			nothing, so there is no rule to evaluate and no Context to build.
		*/
		void Initializer::compile_com_policy(const CACHE *cache)
		{
			auto policy = std::make_shared<ComActivationPolicy>();

			if(cache)
			{
				for(auto rule : cache->statics)
				{
					if(!rule || !rule->has_clsid)
						continue;

					for(auto &id : rule->clsid)
					{
						if(id.empty())
							continue;

						// Sixteen bytes of GUID, copied. No parsing, no
						// allocation, and no dependence on GUID's layout beyond
						// its size - which is what the static_assert checks.
						static_assert(sizeof(GUID) == 2 * sizeof(unsigned long long),
									  "a CLSID key is exactly the GUID's bytes");
						ClsidKey key{};
						::memcpy(&key, static_cast<const GUID *>(&id), sizeof(GUID));
						policy->mention(key, rule->where != nullptr);
					}
				}

				// The Win11 priority rule matches one Windows CLSID rather than
				// a configured one, so it needs the hook on its own.
				policy->set_suppresses_modern_menu(cache->settings.priority != nullptr);
			}

			{
				std::lock_guard<std::mutex> lock(_policy_mutex);
				_policy = policy;
			}
		}

		/*
			The process's one configuration watcher.

			Process lifetime and no destructor, like the other services here.
			uninit() stops it; nothing destroys it, because a static whose
			destructor could run while its thread is still in a callback is a
			crash waiting for an unlucky shutdown.
		*/
		ConfigWatcher &config_watcher()
		{
			static auto *watcher = new ConfigWatcher();
			return *watcher;
		}

		/*
			What the watcher does when one of the loaded files is written.

			Runs the ordinary init() publish path on the watcher's own thread.
			That is safe for the reason the snapshot design exists: init()
			builds a whole new CACHE and swaps a shared_ptr under
			_snapshot_mutex, and a menu that is already open holds its own copy
			of the generation it started with. It never touches a live session.

			init() also takes _reload_mutex, which is what serialises this
			against a menu thread reloading from a keyboard combo at the same
			moment.

			A failed parse is not an error here: last-known-good keeps the
			previous generation live and the error surfaces the usual way. That
			is precisely what makes reloading on every save safe to do at all -
			a half-typed file mid-edit costs nothing.
		*/
		void Initializer::on_config_file_changed()
		{
			if(auto *initializer = Initializer::instance)
			{
				if(Status.Disabled.load(std::memory_order_relaxed))
					return;

				initializer->init();
			}
		}

		/*
			Point the watcher at the set that was just loaded.

			Best-effort throughout: a configuration on a network share may never
			notify at all (FindFirstChangeNotification's own page says so), so a
			watcher that fails to start is not an error and the keyboard combos
			remain exactly as they were.
		*/
		void Initializer::start_watching(const std::vector<std::wstring> &files)
		{
			config_watcher().start(files, &Initializer::on_config_file_changed);
		}

		/*
			The setup a Parser needs before Load() can be called.

			Extracted so that check() can build exactly what the publishing path
			builds - a parse against a half-initialised CACHE is not the same
			parse, and a `-check` that answered a different question from the one
			the DLL asks at load time would be worse than not having it.
		*/
		std::unique_ptr<Parser> Initializer::prepare_parser(const string *config_path, CACHE *cache)
		{
			cache->glyph.name = FontCache::Default;
			load_mui(cache);

			auto parser = config_path ? std::make_unique<Parser>(*config_path)
									  : std::make_unique<Parser>();
			parser->context.Cache = cache;
			parser->context.variables.global = &cache->variables.global;
			parser->context.variables.runtime = &cache->variables.runtime;
			return parser;
		}

		/*
			Parse and report. Publishes nothing, bumps no generation, writes no
			shadow, and leaves Status and LastError exactly as it found them -
			`shell.exe -check` runs in its own short-lived process, but the
			export is callable from anywhere and a diagnostic that changed what
			it was diagnosing would be a trap.

			docs/refactor/03-config-safety.md section 1b step 4.
		*/
		int Initializer::check(const wchar_t *config_path, ConfigCheckResult &result)
		{
			// cbSize belongs to the caller; everything else is ours to fill.
			auto size = result.cbSize;
			result = ConfigCheckResult{};
			result.cbSize = size;

			try
			{
				// Never published, never handed to a menu. It exists so the
				// parse has somewhere to build into.
				auto cache = std::make_shared<CACHE>();

				string path;
				if(config_path && *config_path)
					path = config_path;

				auto parser = prepare_parser(path.empty() ? nullptr : &path, cache.get());

				auto loaded = parser->Load();

				// Path() is the file the parser was in when it stopped, which
				// for a failed import is not the file that was asked about.
				if(auto where = parser->Path(); where && *where)
					copy_into(result.path, where);

				if(!loaded || parser->HasError())
				{
					auto code = parser->Error();
					result.error = static_cast<int32_t>(code);
					result.line = static_cast<uint32_t>(parser->Line());
					result.column = static_cast<uint32_t>(parser->Column());
					copy_into(result.message, ParserException::errortostr(code));
					return CONFIG_CHECK_FAILED;
				}

				const auto &files = parser->LoadedFiles();

				/*
					A file that could not be opened is a *successful* parse as
					far as Load() is concerned - it returns true when the root
					lexer read nothing (Parser.cpp, the `l->length == 0` early
					return), because a machine with no shell.nss must still get
					a working shell rather than an error on every menu.

					For a validator that answer is exactly wrong: somebody who
					runs -check on a path they typed wrong would be told their
					configuration is fine. LoadedFiles() is empty in precisely
					that case - open_root only records the root once load_File
					has succeeded - so that is the discriminator.
				*/
				if(files.empty())
				{
					result.error = static_cast<int32_t>(TokenError::CannotFoundConfigFile);
					copy_into(result.message, path.empty()
						? L"no configuration file was found"
						: L"the file could not be read");
					if(!path.empty())
						copy_into(result.path, path.c_str());
					return CONFIG_CHECK_FAILED;
				}

				result.files = static_cast<uint32_t>(files.size());
				result.entries = parser->TotalMenuCount;

				/*
					What `settings { priority }` came to.

					Evaluated the same way CoCreateInstanceHook evaluates it, so
					the report describes the code's own reading rather than the
					text. A literal 0 or 1 answers cleanly; anything that
					depends on the click - `priority = sys.something` - is
					reported as dynamic rather than guessed at, because a
					validator that runs on a file has no click to evaluate
					against.

					The caller pairs this with the machine's TreatAs state:
					on a machine registered with -treat the setting is inert
					whatever it says, and saying so is the whole point of
					carrying it out here.
					docs/refactor/01-takeover-contract.md section 9b.
				*/
				result.priority = CONFIG_CHECK_PRIORITY_UNSET;
				if(cache->settings.priority)
				{
					result.priority = CONFIG_CHECK_PRIORITY_DYNAMIC;
					try
					{
						Context context;
						Object value = context.Eval(cache->settings.priority).move();
						if(value.is_number())
						{
							// to_bool(), not static_cast<bool>. Object declares
							// a non-template `explicit operator bool` that means
							// *not null*, and being non-template it wins the
							// overload against the numeric conversion template -
							// so static_cast<bool> on a numeric zero is true.
							// The hook next door avoids it by assigning to a
							// bool, where the explicit operator is not a
							// candidate; this says which one it wants.
							result.priority = value.to_bool()
								? CONFIG_CHECK_PRIORITY_ON
								: CONFIG_CHECK_PRIORITY_OFF;
						}
					}
					catch(...)
					{
						// An expression that throws when evaluated without a
						// selection is exactly the dynamic case.
					}
				}

				// The root, which is the file a user asked about even when they
				// named none. Only overwritten on success: on failure the
				// erroring file is the more useful answer.
				copy_into(result.path, files.front().c_str());

				return CONFIG_CHECK_OK;
			}
			catch(...)
			{
				// A parse can throw ParserException, and anything that escapes
				// here still has to produce a report rather than a crash in a
				// process the user ran to be told what is wrong.
				if(result.message[0] == L'\0')
					copy_into(result.message, L"the configuration could not be parsed");
				if(result.error == 0)
					result.error = static_cast<int32_t>(TokenError::Unknown);
				return CONFIG_CHECK_FAILED;
			}
		}

		/*
			One parse into one new generation, published only if it succeeds.

			`config_path` is null for the real configuration and set when
			re-parsing the last-known-good shadow, which is the one case where
			the file being read is not the one the user edits - so it is also
			the one case that must not overwrite the shadow with itself.
		*/
		bool Initializer::load_generation(const string *config_path)
		{
			try
			{
				auto new_cache = std::make_shared<CACHE>();
				new_cache->generation = ++_generation;

				auto parser = prepare_parser(config_path, new_cache.get());

				if(!parser->Load())
					return false;

				for(auto &id : new_cache->muid)
				{
					auto uid = &new_cache->muid[id.first];
					for(auto &si : new_cache->images)
					{
						if(si.equals(uid->id))
						{
							uid->image = si.value;
							break;
						}
					}
				}

				new_cache->fonts.init(HInstance);
				ContextMenu::FontNotFound = false;

				{
					std::lock_guard<std::mutex> lock(_snapshot_mutex);
					_snapshot = new_cache;
				}

				LastError = {};
				Status.Loaded.store(true, std::memory_order_relaxed);
				Status.Error.store(false, std::memory_order_relaxed);
				Status.Stale.store(false, std::memory_order_relaxed);
				Status.Refresh.store(false, std::memory_order_relaxed);
				Status.Disabled.store(false, std::memory_order_relaxed);

				// Shadow the set that just parsed, so a process that starts
				// after this file is broken still has something to serve. Not
				// when re-parsing the shadow itself: that would be copying it
				// over itself, and the paths would be the shadow's own.
				// save() is a no-op when the manifest would not change, which
				// is what stops every process on the machine rewriting it.
				if(!config_path)
				{
					const auto &loaded = parser->LoadedFiles();
					if(!loaded.empty())
						ConfigShadow::save(ConfigShadow::default_directory(), loaded.front(), loaded);
				}

				// Compile the activation policy from the generation that has
				// just been published, so the hook reads a policy that matches
				// the configuration serving menus.
				// docs/refactor/01-takeover-contract.md section 9.
				compile_com_policy(new_cache.get());

				// Watch what was actually read, whichever set that turned out
				// to be. Re-pointed on every successful load, because an edit
				// can add or remove an import and the old watch set would then
				// be watching the wrong directories.
				// docs/refactor/03-config-safety.md section 3.
				start_watching(parser->LoadedFiles());

				return true;
			}
			catch(...)
			{
#ifdef _DEBUG
				Logger::Exception(__func__);
#endif
			}
			return false;
		}

		bool Initializer::uninit()
		{
			// Before the snapshot goes, so nothing is watching for a
			// configuration this process has stopped serving.
			config_watcher().stop();

			try
			{
				{
					std::lock_guard<std::mutex> lock(_snapshot_mutex);
					_snapshot.reset();
				}
			}
			catch(...)
			{
#ifdef _DEBUG
				Logger::Exception(__func__);
#endif
			}

			Status.Loaded.store(false, std::memory_order_relaxed);
			Status.Refresh.store(false, std::memory_order_relaxed);
			return true;
		}

		bool Initializer::query()
		{
			try
			{
				switch(decide_config_serve(Status.Disabled.load(std::memory_order_relaxed),
										   Status.Refresh.load(std::memory_order_relaxed),
										   Status.Error.load(std::memory_order_relaxed),
										   has_snapshot()))
				{
					case ConfigVerdict::Refuse:
						if(Status.Disabled.load(std::memory_order_relaxed))
							uninit();
						return false;

					case ConfigVerdict::Reparse:
						return init();

					case ConfigVerdict::Serve:
						return true;
				}

				return false;
			}
			catch(...)
			{
#ifdef _DEBUG
				Logger::Exception(__func__);
#endif
			}
			return false;
		}

		//determine config file changed
		bool Initializer::config_has_changed()
		{
			bool res = false;
			try
			{
				auto_handle hConfig = ::CreateFileW(application.Config, GENERIC_READ, FILE_SHARE_READ,
													nullptr, OPEN_EXISTING, 0, nullptr);
				if(hConfig)
				{
					FILETIME ft_lastWriteTime{};
					auto last_write_time = reinterpret_cast<uintptr_t *>(&ft_lastWriteTime);
					if(::GetFileTime(hConfig, nullptr, nullptr, &ft_lastWriteTime) && *last_write_time != 0)
					{
						if(*last_write_time != _last_write_time)
						{
							res = _last_write_time > 0;
							_last_write_time = *last_write_time;
						}
					}
				}
			}
			catch(...)
			{
#ifdef _DEBUG
				Logger::Exception(__func__);
#endif
			}
			return res;
		}

		bool Initializer::has_error(bool detect_changes)
		{
			if(Status.Error.load(std::memory_order_relaxed))
			{
				// StaleWithError. The newest parse failed, but a generation
				// published by an earlier one is still in memory, so there is
				// something to serve and the callers that use this to decide
				// whether to take the menu over should keep taking it. Refusing
				// here is what made one typo in shell.nss remove the context
				// menu from every process on the desktop.
				if(has_snapshot())
					return false;

				return detect_changes ? !config_has_changed() : true;
			}
			return false;
		}

		void Initializer::load_mui(CACHE *new_cache)
		{
			if(!new_cache) return;

			auto ldstr = [&, this](const wchar_t *dll,
								std::initializer_list<MUID> list)
			{
				auto hModule = ::GetModuleHandleW(dll);
				bool load_as_dynamic = false;
				if(!hModule)
				{
					// Every name here is a Windows system module; LoadSafe keeps the
					// current directory out of the search. Resource-only mapping means
					// no code runs, but the strings would still be the planted file's.
					//
					// Not all of them are in System32, which the search-path hardening
					// missed: regedit.exe is in the Windows directory and
					// powershell.exe under System32\WindowsPowerShell\v1.0. Both
					// silently stopped loading, and every item whose localised title
					// comes from them fell back to the English string compiled in here
					// - "Merge" on .reg files, "Run with PowerShell" on .ps1. Still no
					// search path either way: LoadSafeWindows qualifies the path too.
					const DWORD flags = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;

					if(::wcspbrk(dll, L"\\/"))
						hModule = DLL::LoadSafeWindows(dll, flags);
					else
					{
						hModule = DLL::LoadSafe(dll, flags);
						if(!hModule)
							hModule = DLL::LoadSafeWindows(dll, flags);
					}

					// Only when there is something to free: FreeLibrary(nullptr) is
					// outside its specification.
					load_as_dynamic = (hModule != nullptr);
				}

				if(hModule)
				{
					for(auto &it : list)
					{
						auto uid = &new_cache->muid[it.id];
						if(uid->title.empty())
						{
							*uid = it;
							auto loaded = string::LoadStringW_full(hModule, uid->res_id);
							if(!loaded.empty())
							{
								uid->title = loaded.move();
								uid->set_hash();
							}
						}
					}
				}
				else
				{
					for(auto &it : list)
					{
						auto uid = &new_cache->muid[it.id];
						if(uid->title.empty())
						{
							*uid = it;
							uid->title = it.title;
							uid->set_hash();
						}
					}
				}

				if(load_as_dynamic)
					::FreeLibrary(hModule);
			};

			auto parse_menu = [&](HMODULE hModule, uint32_t menu_id,
								 std::initializer_list<MUID> list)
			{
				if(hModule)
				{
					auto hMenu = ::LoadMenuW(hModule, MAKEINTRESOURCEW(menu_id));
					if(hMenu)
					{
						for(auto &it : list)
						{
							auto uid = &new_cache->muid[it.id];
							if(uid->title.empty())
							{
								*uid = it;
								// Sizes the string first - see Include\MenuText.h.
								MENUITEMINFOW mii{ sizeof(mii), MIIM_STRING };
								auto read = read_menu_text(hMenu, uid->res_id, FALSE, &mii,
														   [uid](UINT count) { return uid->title.buffer(count + 1); });
								if(read && mii.cch > 0)
								{
									uid->title.release(mii.cch);
									uid->set_hash();
								}
								else if(it.title_fallback.length() > 0)
								{
									uid->title = it.title_fallback;
									uid->set_hash();
								}
							}
						}
						::DestroyMenu(hMenu);
					}
					else for(auto &it : list)
					{
						auto uid = &new_cache->muid[it.id];
						*uid = it;
					}
				}
				else for(auto &it : list)
				{
					auto uid = &new_cache->muid[it.id];
					*uid = it;
				}
			};

			auto ldmenu = [&](const wchar_t *dll, uint32_t menu_id,
								 std::initializer_list<MUID> list)
			{
				HMODULE lib{}, hModule = ::GetModuleHandleW(dll);
				if(!hModule)
				{
					lib = DLL::LoadSafe(dll, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
					hModule = lib;
				}

				parse_menu(hModule, menu_id, list);

				if(lib) ::FreeLibrary(lib);
			};

			struct menu_res {
				uint32_t id{};
				std::initializer_list<MUID> items;
			};

			auto ldmenu2 = [&](const wchar_t *dll, std::initializer_list<menu_res> list)
			{
				HMODULE lib{}, hModule = ::GetModuleHandleW(dll);
				if(!hModule)
				{
					lib = DLL::LoadSafe(dll, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
					hModule = lib;
				}

				if(hModule)
				{
					for(auto &it : list)
					{
						parse_menu(hModule, it.id, it.items);
					}
				}
				else for(auto &it : list)
				{
					for(auto &ui : it.items)
					{
						auto uid = &new_cache->muid[ui.id];
						if(uid->title.empty())
						{
							*uid = ui;
						}
					}
				}

				if(lib) ::FreeLibrary(lib);
			};

			auto add_uid = [&](std::initializer_list<MUID> list) {
				for(auto &&uid : list)
					new_cache->muid.emplace(uid.id, uid);
			};

			const auto *os = &Windows::Version::Instance();
			auto isw11 = [os](uint32_t val1, uint32_t val2)->uint32_t { return os->IsWindows11OrGreater() ? val1 : val2; };
			auto isw8 = [os](uint32_t val1, uint32_t val2)->uint32_t { return os->IsWindows8OrGreater() ? val1 : val2; };

			auto user32 = L"user32.dll";
			auto shell32 = L"shell32.dll";
			auto explorerframe = L"explorerframe.dll";
			auto explorer = L"explorer.exe";
			
			ldstr(L"acppage.dll", {
				{IDENT_ID_TROUBLESHOOT_COMPATIBILITY, 2022},
			 });

			if(os->Major >= 10 && os->Build > 10240)
			{
				ldstr(L"ntshrui.dll", {
				   {IDENT_ID_GIVE_ACCESS_TO, 103},
				   {IDENT_ID_SHARE_WITH, 104},
				   {IDENT_ID_SHARE, 107},
				});
			}
			else 
			{
				ldstr(L"ntshrui.dll", {
				   {IDENT_ID_SHARE_WITH, 103},
				   {IDENT_ID_SHARE, 107},
				});

				add_uid({
					{ IDENT_ID_GIVE_ACCESS_TO, 0}
				});
			}

			ldstr(L"appresolver.dll", {
				{ IDENT_ID_OPEN, 8550 },
				{ IDENT_ID_RUN_AS_ADMINISTRATOR, 8551},
				{IDENT_ID_RUN_AS_DIFFERENT_USER, 8558},
				//{ IDENT_ID_OPEN_FILE_LOCATION, 8552, { L'\uE0E8', 0 } },
			});

			ldstr(L"windows.storage.dll", {
				{IDENT_ID_OPEN_IN_NEW_WINDOW, 8517},
				{IDENT_ID_OPEN_IN_NEW_TAB, 8519},
			});

			ldmenu2(shell32, 
			{ 
				{208, {
					{IDENT_ID_OPEN_WITH, 1},
					//{ IDENT_ID_CANCEL, 0 },
				}},
				{	210, {
					{ IDENT_ID_CREATE_SHORTCUT, 16},
					{ IDENT_ID_CUT, 24},
					{ IDENT_ID_COPY, 25},
					{ IDENT_ID_PASTE, 26},
					{ IDENT_ID_RENAME, 18},
					{ IDENT_ID_DELETE, 17},
				}},
				{	211, {
					{ IDENT_ID_PROPERTIES, 19 } 
				}},
				{	215, {
					{ IDENT_ID_ARRANGE_BY, 28678},
					{ IDENT_ID_VIEW, 28674},
					{ IDENT_ID_GROUP_BY, 28676},
					{ IDENT_ID_SORT_BY, 28673},
					{ IDENT_ID_REFRESH, 28931},
					{ IDENT_ID_CUSTOMIZE_THIS_FOLDER, 28722},
					{ IDENT_ID_PASTE_SHORTCUT, 28700},
					{ IDENT_ID_UNDO, 28699},
					{ IDENT_ID_REDO, 28704}
				}},
				/*{216, {
				    { IDENT_ID_PROPERTIES, 28691},
					{ IDENT_ID_COPY_TO_FOLDER, 28702},
					{ IDENT_ID_MOVE_TO_FOLDER, 28703},
				}},*/
				{217, { 
					{ IDENT_ID_EXTRA_LARGE_ICONS, 28749},
					{ IDENT_ID_LARGE_ICONS, 28751},
					{ IDENT_ID_MEDIUM_ICONS, 28750},
					{ IDENT_ID_SMALL_ICONS, 28752},
					{ IDENT_ID_LIST, 28753},
					{ IDENT_ID_DETAILS, 28747},
					{ IDENT_ID_TILES, 28748},
					{ IDENT_ID_CONTENT, 28754},
					{ IDENT_ID_EXPAND_ALL_GROUPS , 28755},
					{ IDENT_ID_COLLAPSE_ALL_GROUPS , 28756},
					{ IDENT_ID_COPY_TO_FOLDER, 28702},
					{ IDENT_ID_MOVE_TO_FOLDER, 28703},
					{ IDENT_ID_SELECT_ALL, isw11(28707, 28705)},
					{ IDENT_ID_INVERT_SELECTION, isw11(28708,28706)}
				}},
				{	218, {
					{ IDENT_ID_SHOW_DESKTOP_ICONS, 29698}
				}},
				{	223, {
					{IDENT_ID_SEND_TO, 79},
				}},
				{	225, {
					{ IDENT_ID_MAP_NETWORK_DRIVE, 14},
					{IDENT_ID_DISCONNECT_NETWORK_DRIVE, 9},
				}}, 
				{	226, {
					{ IDENT_ID_RESTORE, 139},
				}},
				{	228, {
					{ IDENT_ID_ADD_A_NETWORK_LOCATION, 1},
				}},
				{	247, {
					{ IDENT_ID_FORMAT, 8},
					{ IDENT_ID_DISCONNECT, 9 },
					{ IDENT_ID_EJECT, 10},
					{ IDENT_ID_ERASE_THIS_DISC, 3},
				}},
				{	275, {
					{ IDENT_ID_SHOW_THIS_PC, 4},
					{ IDENT_ID_SHOW_NETWORK, 5},
					{ IDENT_ID_SHOW_LIBRARIES, 3},
					//{ IDENT_ID_SHOW_ALL_FOLDERS, isw8(1, 0), {} },
					//{ IDENT_ID_EXPAND_TO_CURRENT_FOLDER, isw8(2, 1),/*, {L'\uE1B1', 0}*/ },
				}},
				/*{	280, {
					{ IDENT_ID_OPEN_FILE_LOCATION, 0, { L'\uE0E8', 0 } },
				}},
				{	394, {
					{ IDENT_ID_EDIT, 400, { L'\uE0A1', 0 } }
				}},
				{	360, {
					{IDENT_ID_OPEN, 25},
					{IDENT_ID_RUN_AS_ADMINISTRATOR, 32, {L'\uE057', L'\uE058'}},
				}},*/
				{	502, {
					{ IDENT_ID_REMOVE_PROPERTIES, 2 }
				}},
				{	200, {
					{IDENT_ID_COPY_HERE, 1},
					{ IDENT_ID_MOVE_HERE, 2},
					{IDENT_ID_CREATE_SHORTCUTS_HERE, 3},
					{IDENT_ID_CANCEL, 0},
				}},
				{262, {
					{IDENT_ID_INCLUDE_IN_LIBRARY, 0}
				}},
			});

			ldmenu2(explorerframe, { {
				3400, {
					{ IDENT_ID_EXPAND, 8},
					{ IDENT_ID_COLLAPSE, 9}
				}},
				{260,{
						{IDENT_ID_SELECT, 0, {}, {}, L"Select"} //shell32/menu/260/0  | 49927
					}
				},
				{266,{
						{IDENT_ID_GO_TO, 33104}
					}
				}
			});

			ldmenu2(explorer, {
				{ 205,{
					{IDENT_ID_ADJUST_DATE_TIME, 408, {L'\uE1F2'}, {},L"&Adjust date/time"},
					{IDENT_ID_CUSTOMIZE_NOTIFICATION_ICONS, 421, {L'\uE190'}, {},L"&Customize notification icons"},
					{IDENT_ID_CORTANA,445, {L'\ue170'}, {},L"Cortana"},
					{IDENT_ID_SHOW_CORTANA_BUTTON ,449, {L'\ue170'}, {},L"Show Cortana button"},
					{IDENT_ID_NEWS_AND_INTERESTS,621, {}, {},L"&News and interests"},
					{IDENT_ID_SHOW_TASK_VIEW_BUTTON,430, {L'\uE204'}, {},L"Show Task &View button"},
					{IDENT_ID_SHOW_PEOPLE_ON_THE_TASKBAR,435, {L'\uE106'}, {},L"Show &People on the taskbar"},
					{IDENT_ID_SHOW_PEN_BUTTON,437, {}, {},L"Show Pen button"},
					{IDENT_ID_SHOW_TOUCH_KEYBOARD_BUTTON,436, {L'\ue18c'}, {},L"Show touch ke&yboard button"},
					{IDENT_ID_SHOW_TOUCHPAD_BUTTON,438, {L'\uE1EB'}, {},L"Show touch&pad button"},
					{IDENT_ID_CASCADE_WINDOWS,403, {}, {},L"Casca&de windows"},
					{IDENT_ID_SHOW_WINDOWS_STACKED,405, {},{}, L"Show windows stack&ed"},
					{IDENT_ID_SHOW_WINDOWS_SIDE_BY_SIDE,404, {}, {},L"Show windows s&ide by side" },
					{IDENT_ID_SHOW_THE_DESKTOP,407, {}, {},L"&Show the desktop"},
					{IDENT_ID_TASK_MANAGER,420, {}, {},L"Tas&k Manager"},
					{IDENT_ID_LOCK_THE_TASKBAR,424, {}, {},L"&Lock the taskbar"},
					{IDENT_ID_LOCK_ALL_TASKBARS,425, {}, {},L"&Lock all taskbars"},
					{IDENT_ID_TASKBAR_SETTINGS, 413, {},{}, L"Taskbar se&ttings"},
					{IDENT_ID_EXIT_EXPLORER, 518, {L'\uE0D0'},{}, L"E&xit Explorer"}
				}},
				{12000,{
					{IDENT_ID_RESTORE_ALL_WINDOWS, 65493, {}, {}, L"&Restore all windows"},
					{IDENT_ID_MINIMIZE_ALL_WINDOWS, 65492,{}, {}, L"&Minimize all windows"},
					{IDENT_ID_CLOSE_ALL_WINDOWS, 65491, {}, {}, L"&Close all windows"},
				}}
			});

			ldmenu2(user32, {
				{
					1, {{ IDENT_ID_INSERT_UNICODE_CONTROL_CHARACTER, 32787, {L'\uE14e'} }}
				},
				{ 
					16, {
					{ IDENT_ID_MINIMIZE, 61472 },
					{ IDENT_ID_MAXIMIZE, 61488 },
					{ IDENT_ID_MOVE, 61456 },
					{ IDENT_ID_SIZE, 61440 },
				}}
			});

			ldmenu2(L"wpdshext.dll", { 
				{20, {
					{IDENT_ID_IMPORT_PICTURES_AND_VIDEOS, 21}
				}},
				{30, {
					{ IDENT_ID_FORMAT, 31 }
				}}
			});

			ldstr(user32, {
				{IDENT_ID_RECONVERSION, 705, {L'\uE096'}},
			});

			//IDENT_ID_FILE_EXPLORER OPTIONS 22985

			ldstr(shell32, {
				{IDENT_ID_ADD_TO_PLAYLIST, 37427},
				{IDENT_ID_AUTOPLAY, 31384, {L'\ue1f9'}},
				{IDENT_ID_CAST_TO_DEVICE, 31289},
				{IDENT_ID_COMMAND_PROMPT, 22022},
				{IDENT_ID_CONTROL_PANEL, 4161, {L'\uE0F3'}},
				{IDENT_ID_COPY_TO, 30339},
				{IDENT_ID_MOVE_TO, 30340},
				{IDENT_ID_COPY_TO_FOLDER, 30304},
				{IDENT_ID_MOVE_TO_FOLDER, 30305},
				{IDENT_ID_COPY_AS_PATH, 30328},
				{IDENT_ID_COPY_PATH, 30329},
				{IDENT_ID_DESKTOP, 4162},
				{IDENT_ID_DEVICE_MANAGER, 24225},
				{IDENT_ID_EMPTY_RECYCLE_BIN, 10564},
				{IDENT_ID_EXTRACT_ALL, 37514},
				{IDENT_ID_EXTRACT_TO, 37516},
				{IDENT_ID_FILE_EXPLORER, 12352},
				{IDENT_ID_FILE_EXPLORER_OPTIONS, 22985},
				{IDENT_ID_RESTORE_DEFAULT_LIBRARIES, 34645},
				{IDENT_ID_MANAGE, 37602},
				{IDENT_ID_MAP_AS_DRIVE, 37440},
				{IDENT_ID_MOUNT, 31472},
				{IDENT_ID_NEW, 30315},
				{IDENT_ID_OPEN_AUTOPLAY, 8507},
				{IDENT_ID_OPEN_COMMAND_PROMPT, 37415},
				{IDENT_ID_OPEN_COMMAND_WINDOW_HERE, 8506},
				{IDENT_ID_OPEN_FILE_LOCATION, 1033 },
				{IDENT_ID_OPEN_FOLDER_LOCATION, 1040},
				{IDENT_ID_OPEN_IN_NEW_PROCESS, 8518},
				{IDENT_ID_OPEN_NEW_TAB, 37616},
				{IDENT_ID_OPEN_NEW_WINDOW, 37411},
				{IDENT_ID_OPEN_POWERSHELL_WINDOW_HERE, 8508},
				{IDENT_ID_OPEN_WINDOWS_POWERSHELL, 37446},
				{IDENT_ID_OPTIONS, 37459},
				{IDENT_ID_PIN_TO_QUICK_ACCESS, 51377},
				{IDENT_ID_PIN_CURRENT_FOLDER_TO_QUICK_ACCESS, 51388},
				{IDENT_ID_PIN_TO_START, 51201},
				{IDENT_ID_ADD_TO_FAVORITES, 51389},		//37408  windows 11 22H2
				{IDENT_ID_REMOVE_FROM_FAVORITES, 51390},//windows 11 22H2
				{IDENT_ID_REMOVE_FROM_RECENT, 51391},
				{IDENT_ID_PIN_TO_TASKBAR, 5386},
				{IDENT_ID_PLAY, 31283, {L'\uE1A5'}},
				{IDENT_ID_PREVIEW, 51260, {L'\uE275'}},
				{IDENT_ID_PRINT, 31250, {L'\uE0A8', 0}},
				{IDENT_ID_REMOVE_FROM_QUICK_ACCESS, 51381},
				{IDENT_ID_ROTATE_LEFT, 37610},
				{IDENT_ID_ROTATE_RIGHT, 37612},
				{IDENT_ID_RUN, 12710},
				{IDENT_ID_RUN_AS_ANOTHER_USER, 37419},
				{IDENT_ID_SEARCH, 12708, {L'\uE187'}},
				{IDENT_ID_EDIT, 37398},
				//{IDENT_ID_TROUBLESHOOT_COMPATIBILITY, 37425},
				{IDENT_ID_UNPIN_FROM_QUICK_ACCESS,51379},
				{IDENT_ID_UNPIN_FROM_START, 51394},
				{IDENT_ID_UNPIN_FROM_TASKBAR, 5387},
				{IDENT_ID_COMPRESSED, 31462},
				{IDENT_ID_CLEANUP, 37494},
				{IDENT_ID_NEW_FOLDER, 16859},
				{IDENT_ID_NEW_ITEM, 37400},
				{IDENT_ID_AUTO_ARRANGE_ICONS, 31157},//28785
				{IDENT_ID_ALIGN_ICONS_TO_GRID, 31158},// 28788
				{IDENT_ID_MORE_OPTIONS, 31153},
				{IDENT_ID_INSTALL, 10210},
				{IDENT_ID_CONFIGURE, 10209, { L'\uE0F3'}},
				{IDENT_ID_SELECT_ALL, 31276},
				{IDENT_ID_SELECT_NONE, 37378},
				{IDENT_ID_ALL_CONTROL_PANEL_ITEMS, 32012 },
				{IDENT_ID_IMPORT_PICTURES_AND_VIDEOS, 31285, {L'\uE14F'} },
				{IDENT_ID_CLOSE, 31450},
				{IDENT_ID_PERMANENTLY_DELETE, 37394}
			});

			//window 7
			if(!os->IsWindows8OrGreater())
			{
				ldstr(shell32, {
					{IDENT_ID_PIN_TO_START_MENU, 5381},
					{IDENT_ID_UNPIN_FROM_START_MENU, 5382}
				});
			}
			else 
			{
				add_uid({
					{ IDENT_ID_PIN_TO_START_MENU, 0},
					{ IDENT_ID_UNPIN_FROM_START_MENU, 0}
				});
			}
			
			ldstr(explorerframe, {
				{IDENT_ID_FOLDER_OPTIONS, 1030},
				//{IDENT_ID_SHARE, 49928},
				//{IDENT_ID_SHARE_WITH, 49930},
				{IDENT_ID_EXPAND_GROUP, 41220},
				{IDENT_ID_COLLAPSE_GROUP, 41221},
				{IDENT_ID_SELECT, 49927} //shell32/menu/260/0
			});

			/*ldstr(L"unregmp2.exe", {
				{IDENT_ID_ADD_TO_WINDOWS_MEDIA_PLAYER_LIST, 9800},
				{IDENT_ID_PLAY_WITH_WINDOWS_MEDIA_PLAYER, 9801},
			});*/

			ldstr(L"display.dll", { {IDENT_ID_DISPLAY_SETTINGS, 4} });
			ldstr(L"themecpl.dll", { {IDENT_ID_PERSONALIZE, 10} });
			ldstr(L"regedit.exe", { {IDENT_ID_MERGE, 310, {L'\uE142'} } });
			ldstr(L"System32\\WindowsPowerShell\\v1.0\\powershell.exe",
				  { {IDENT_ID_RUN_WITH_POWERSHELL, 108} });

			ldstr(L"stobject.dll", {
				{IDENT_ID_SET_AS_DESKTOP_BACKGROUND, 417},
				{IDENT_ID_NEXT_DESKTOP_BACKGROUND, 416}
			 });

			ldstr(L"fvecpl.dll", {
				{IDENT_ID_TURN_ON_BITLOCKER, 1140},
				{IDENT_ID_TURN_OFF_BITLOCKER, 1141},
			});

			ldstr(L"twext.dll", {
				{IDENT_ID_RESTORE_PREVIOUS_VERSIONS, 1037},
				//{IDENT_ID_RESTORE, 1033},
			});

			ldstr(L"twinui.dll", {
				{IDENT_ID_SETTINGS, 5621},
				{IDENT_ID_STORE, 10669, {L'\uE1B9'}},
				{IDENT_ID_POWER_OPTIONS, 5950},
				//{IDENT_ID_AUTOPLAY, 9914},
				//{IDENT_ID_REFRESH, 10620},
				{IDENT_ID_REMOVE, 11454, {L'\uE256'}},
				{IDENT_ID_ACCOUNT, 11452, {L'\uE106'}},
				{IDENT_ID_COPY_TO_CLIPBOARD, 13648},
				{IDENT_ID_WINDOWS_POWERSHELL, 10928}
			});

			ldstr(L"twinui.pcshell.dll", {
				{IDENT_ID_WINDOWS_POWERSHELL, 10928, {}, {}, L"Windows PowerShell"},
				{IDENT_ID_WINDOWS_TERMINAL, 10944},
				{IDENT_ID_TERMINAL, 10960, {}, {}, L"Terminal" }
			});

			ldstr(L"wpdshext.dll", {
				{ IDENT_ID_OPEN_AS_PORTABLE, 349 }
			});

			//Import_pictures_and_videos
			ldstr(L"isoburn.exe", {
				{IDENT_ID_BURN_DISC_IMAGE, 351}
			});

			ldstr(explorer, {
				{IDENT_ID_WINDOWS_TERMINAL, 22016, {}, {}, L"Windows Terminal"},
				{IDENT_ID_WINDOWS, 11104, {}, {}, L"Windows"},
				{IDENT_ID_TASKBAR, 518},
				{IDENT_ID_FILE_EXPLORER, 6020},
				{IDENT_ID_SHOW_OPEN_WINDOWS, 850},
				//{IDENT_ID_HIDE, 1003},
				//{IDENT_ID_SHOW, 1004}
			});


			add_uid({
				{ IDENT_ID_SET_AS_DESKTOP_WALLPAPER, 0},
				{ IDENT_ID_SHOW_FILE_EXTENSIONS, 0},
				{ IDENT_ID_SHOW_HIDDEN_FILES,  0},
				{ IDENT_ID_MAKE_AVAILABLE_OFFLINE, 0},
				{ IDENT_ID_MAKE_AVAILABLE_ONLINE, 0},
				{ IDENT_ID_SHIELD, 0,}
			});
		}

		// enabled/reload
		// disabled
		// mix
		/*
			Read the keyboard once, into plain data.

			The count the classifier wants is "keys the user is deliberately
			holding", so the buttons that caused this click come off first: the
			right button itself, and the Shift+F10 pair when the click arrived
			as the context-menu key rather than as a mouse click.
		*/
		GestureState Initializer::read_gesture(bool istaskbar)
		{
			Keyboard kb;
			int count = kb.get_keys_state();

			if(kb.is_rbutton())
				count--;

			if(kb.is_contextmenu())
				count -= 2;

			GestureState state;
			state.held = count;
			state.ctrl = kb.key_ctrl();
			state.shift = kb.key_shift();
			state.win = kb.key_win();
			state.alt = kb.key_alt();
			state.lbutton = kb.is_lbutton();
			state.f5 = kb.key(VK_F5);
			state.taskbar_lbutton = istaskbar && kb.is_lbutton();
			return state;
		}

		bool Initializer::OnState(bool istaskbar)
		{
			return OnState(classify_click(istaskbar));
		}

		/*
			Act on an already-classified gesture.

			The hook classifies once and passes the result to both this and its
			own bypass check, so the two cannot disagree about what the user
			pressed - which they could when each read the keyboard for itself.
			See Include/TakeoverGesture.h.
		*/
		bool Initializer::OnState(Gesture gesture)
		{
			bool reloaded = false;

			int changed = 0;

			switch(gesture)
			{
				case Gesture::ReloadConfig:
					Status.Disabled = false;
					Status.Refresh = true;
					changed = 1;
					break;

				case Gesture::PreferModern:
					Status.Refresh = false;
					changed = 1;
					break;

				case Gesture::DisableShell:
					Status.Disabled = true;
					Status.Refresh = false;
					changed = 1;
					break;

				// Handled by the caller, before any Shell work runs. It
				// deliberately changes no configuration state - the whole point
				// is that this one click leaves everything as it was.
				case Gesture::BypassOnce:
				case Gesture::None:
					break;
			}

			if(changed > 0)
			{
				if(Status.Disabled)
					Initializer::instance->uninit();
				else if(Status.Refresh)
				{
					// Status.Refresh is already set, and that is what makes
					// query() re-parse. It used to also need `changed` to be 2
					// to get past the error gate, so Ctrl+right-click could not
					// recover a configuration that had failed to parse and
					// Shift+Ctrl+right-click was the only way back.
					Initializer::instance->query();
					reloaded = true;
				}
			}
			return reloaded;
		}
	}
}