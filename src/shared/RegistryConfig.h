#pragma once

namespace Nilesoft
{
	struct REGOP
	{
		bool REGISTER{};
		bool UNREGISTER{};
		bool TREAT{};
		bool CONTEXTMENU{};
		bool FOLDEREXTENSIONS{};
		bool ICONOVERLAY{};
		bool RESTART{};
		bool SILENT{};
	};

	// Command-level idempotence: an absent registration is already in the
	// requested state, while a present registration must actually be removed.
	template<typename Remove>
	bool unregister_if_present(bool registered, Remove remove)
	{
		return !registered || remove();
	}

	class RegistryConfig
	{
#define	APP_SIG						L"\u0020@nilesoft.shell"
#define	APP_COMP_NAME				APP_COMPANY L"." APP_NAME
#define APP_KEY						L"SOFTWARE\\" APP_COMPANY L"\\" APP_NAME

//L"SOFTWARE\\Classes\\Drive\\shellex\\FolderExtensions\\"
#define HKLM_DRIVE_FolderExtensions	L"SOFTWARE\\Classes\\Drive\\shellex\\FolderExtensions\\" CLS_FolderExtensions
#define HKCR_CONTEXTMENUHANDLERS	L"\\shellex\\ContextMenuHandlers\\" APP_SIG

#define HKLM_CURRENTVERSION			L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
#define HKLM_APPROVED				HKLM_CURRENTVERSION L"\\Shell Extensions\\Approved"
#define HKLM_ICONOVERLAY			HKLM_CURRENTVERSION L"\\Explorer\\ShellIconOverlayIdentifiers\\" APP_SIG

		//Computer\HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}
		//reg.exe add "HKCU\Software\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\InprocServer32" /f /ve

		public:
			static const wchar_t *GetKeyPath()
			{/*
#ifndef _WIN64
				//if(Is64BitWindows())
				return L"Software\\Classes\\Wow6432Node";
#endif
*/
				return L"Software\\Classes";
			}

			static bool IsRegistered()
			{
				return IsContextMenu() || IsIconOverlay() || IsFolderExtensions();
			}

			static bool IsRegisteredCached(bool force_refresh = false)
			{
				static std::atomic<uint64_t> cached_tick{ 0 };
				static std::atomic<bool> cached_state{ false };

				auto now = ::GetTickCount64();
				auto last = cached_tick.load(std::memory_order_relaxed);

				if(!force_refresh && last != 0 && (now - last) < 2000)
				{
					return cached_state.load(std::memory_order_relaxed);
				}

				bool state = IsRegistered();
				cached_state.store(state, std::memory_order_relaxed);
				cached_tick.store(now, std::memory_order_relaxed);
				return state;
			}

			/*
				Is the Windows 11 modern context menu redirected to us?

				`shell.exe -register -treat` writes a TreatAs on
				{86ca1aa0-...} naming Shell's CLSID, and from then on COM
				substitutes Shell for the modern menu class. HKCR rather than
				either hive on its own, because HKCR is the merged view COM
				itself resolves against - a per-user redirect shadows the
				machine one, and reading only HKLM would miss it.

				Why this exists, rather than the code just assuming: two
				separate mechanisms suppress the modern menu and only one of
				them is a setting. See CoCreateInstanceHook and
				docs/refactor/01-takeover-contract.md section 9b.
			*/
			static bool ModernMenuRedirectedToUs()
			{
				string key;
				key.format(L"CLSID\\%s\\TreatAs",
						   string::ToString(IID_FileExplorerContextMenu, 2).c_str());

				wchar_t treatas[64]{};
				DWORD cb = sizeof(treatas);
				if(ERROR_SUCCESS != ::RegGetValueW(HKCR, key.c_str(), nullptr,
												   RRF_RT_REG_SZ, nullptr, treatas, &cb))
					return false;

				// RegGetValueW with RRF_RT_REG_SZ terminates for us - unlike
				// RegQueryValueEx, whose page says a REG_SZ "is NOT guaranteed
				// to be null-terminated". The buffer is still bounded and the
				// comparison is against a fixed string, so a value longer than
				// this simply is not ours.
				return string(treatas).equals(CLS_ContextMenu);
			}

			// Read on every modern-menu activation, so it is cached on the same
			// two-second terms as IsRegisteredCached. Registration is an
			// installer-time act; two seconds is far tighter than it needs to be
			// and keeps a machine that was just registered from looking stale.
			static bool ModernMenuRedirectedToUsCached()
			{
				static std::atomic<uint64_t> cached_tick{ 0 };
				static std::atomic<bool> cached_state{ false };

				auto now = ::GetTickCount64();
				auto last = cached_tick.load(std::memory_order_relaxed);

				if(last != 0 && (now - last) < 2000)
					return cached_state.load(std::memory_order_relaxed);

				bool state = ModernMenuRedirectedToUs();
				cached_state.store(state, std::memory_order_relaxed);
				cached_tick.store(now, std::memory_order_relaxed);
				return state;
			}

			static bool IsHKCR(const string &key)
			{
				return Registry::Exists(HKCR, L"CLSID\\" + key);
			}

			static bool IsContextMenu()
			{
				return IsHKCR(CLS_ContextMenu);
			}

			static bool IsIconOverlay()
			{
				return IsHKCR(CLS_IconOverlay);
			}

			static bool IsFolderExtensions()
			{
				return IsHKCR(CLS_FolderExtensions);
			}

			// register COM-object Add HKCR\CLSID\{<CLSID>} key.
			static bool RegisterInprocServer(const wchar_t *module, const wchar_t *clsid, const wchar_t *value)
			{
				bool ret = false;
				auto keyCLSID = Registry::ClassesRoot.OpenSubKey(L"CLSID", false, true);
				if(keyCLSID)
				{
					auto keyGuid = keyCLSID.CreateSubKey(clsid);
					if(keyGuid)
					{
						if(keyGuid.SetString(nullptr, value))
						{
							auto keyInprocServer32 = keyGuid.CreateSubKey(L"InprocServer32");
							if(keyInprocServer32)
							{
								if(keyInprocServer32.SetString(nullptr, module))
								{
									ret = keyInprocServer32.SetString(L"ThreadingModel", L"Apartment");
								}
								keyInprocServer32.Close();
							}
						}

						keyGuid.Close();

						if(ret == false)
							keyCLSID.DeleteSubKey(clsid);
					}
					keyCLSID.Close();
				}
				return ret;
			}

			// Require admininstrator's rights!
			static bool Register(const wchar_t *dllPath, REGOP reg)
			{
				if(!dllPath || !dllPath[0])
					return false;
				
				if(!::IsWindows7OrGreater())
					return false;

				int ret = 0;

				if(reg.FOLDEREXTENSIONS)
				{
					if(RegisterInprocServer(dllPath, CLS_FolderExtensions, APP_COMP_NAME))
					{
						ret++;
						auto key = Registry::LocalMachine.CreateSubKey(HKLM_DRIVE_FolderExtensions, false);
						if(key)
						{
							ret += key.SetString(nullptr, APP_COMP_NAME);
							ret += key.SetInt(L"DriveMask", 0xff);
							key.Close();
						}
						else
						{
							Registry::DeleteSubKey(HKCR, L"CLSID\\" CLS_FolderExtensions);
							ret = 0;
						}
					}
				}
				
				if(reg.ICONOVERLAY)
				{
					// register COM-object for overlay icon handler
					if(RegisterInprocServer(dllPath, CLS_IconOverlay, APP_COMP_NAME))
					{
						// register overlay icon handler
						ret += Registry::LocalMachine.SetString(HKLM_ICONOVERLAY, nullptr, CLS_IconOverlay, false);
					}
				}

				if(reg.CONTEXTMENU)
				{
					// register COM-object for shortcut menu handler
					if(RegisterInprocServer(dllPath, CLS_ContextMenu, APP_COMP_NAME))
					{
						if(Registry::LocalMachine.SetString(HKLM_APPROVED, CLS_ContextMenu, APP_SIG, false))
						{
							//register shortcut menu handler
							ret += RegisterContextMenuHandler(true);
						}
					}
				}

				if(ret > 0)
				{
					auto keyNSS = Registry::ClassesRoot.CreateSubKey(L".nss", false);
					if(keyNSS)
					{
						keyNSS.SetString(L"Content Type", L"text/plain");
						auto key_CMD = keyNSS.CreateSubKey(L"shell\\open\\command");
						if(key_CMD)
						{
							key_CMD.SetString(nullptr, L"notepad \"%1\"");
							key_CMD.Close();
						}
						keyNSS.Close();
					}
					return true;
				}
				return false;
			}

			// Require administrator's rights!
			static bool Unregister()
			{
				int ret = 0;

				ret += Registry::DeleteSubKey(HKCR, L"CLSID\\" CLS_FolderExtensions);
				//ret += Registry::DeleteSubKey(HKLM, HKLM_DRIVE_FolderExtensions);

				ret += Registry::DeleteSubKey(HKLM, HKLM_ICONOVERLAY);
				// unregister COM-object Delete the HKCR\CLSID\{<CLSID>} key.
				ret += Registry::DeleteSubKey(HKCR, L"CLSID\\" CLS_IconOverlay);

				ret += Registry::DeleteKeyValue(HKLM, HKLM_APPROVED, CLS_ContextMenu);
				// unregister COM-object Delete the HKCR\CLSID\{<CLSID>} key.
				ret += Registry::DeleteSubKey(HKCR, L"CLSID\\" CLS_ContextMenu);
				ret += RegisterContextMenuHandler(false);

				// The Windows 11 modern menu reaches us through a TreatAs redirect on
				// {86ca1aa0-...}. Deleting our CLSID above without clearing it strands
				// the redirect on a class that no longer resolves, so the user
				// uninstalls and is left on the classic menu with nothing to explain
				// it. This used to be handled only by shell.exe -unregister -treat, so
				// a bare -unregister left the machine modified.
				//
				// Only removed when it actually names our server. The key is closed
				// before the delete rather than deleted through a live handle.
				{
					string key_treatas;
					key_treatas.format(L"CLSID\\%s\\TreatAs",
									   string::ToString(IID_FileExplorerContextMenu, 2).c_str());

					wchar_t treatas[64]{};
					DWORD cb = sizeof(treatas);
					auto ours = ERROR_SUCCESS == ::RegGetValueW(HKCR, key_treatas.c_str(), nullptr,
																RRF_RT_REG_SZ, nullptr, treatas, &cb)
							 && string(treatas).equals(CLS_ContextMenu);

					if(ours)
						Registry::DeleteSubKey(HKCR, key_treatas);
				}
				/*
					Remove what registration put under .nss, not the whole key.

					This used to be DeleteSubKey(HKCR, L".nss"), which deletes the
					file-extension key and everything beneath it. Registration
					creates exactly two things there - a "Content Type" value and a
					shell\open\command chain - but the key itself is shared. Anything
					else that had registered for .nss, an editor's ProgID under
					OpenWithProgids or the user's own file association, went with it,
					and if .nss already existed before Shell was installed then
					uninstalling Shell deleted a key it never owned.

					So: take back the two, then remove .nss only if nothing else is
					left in it.
				*/
				if(auto keyNSS = Registry::ClassesRoot.OpenSubKey(L".nss", false, true); keyNSS)
				{
					keyNSS.DeleteValue(L"Content Type");
					Registry::DeleteSubKey(HKCR, L".nss\\shell\\open\\command");
					Registry::DeleteSubKey(HKCR, L".nss\\shell\\open");
					Registry::DeleteSubKey(HKCR, L".nss\\shell");

					bool empty = Registry::EnumNames(keyNSS.Handle(), true).empty()
							  && Registry::EnumNames(keyNSS.Handle(), false).empty();
					keyNSS.Close();

					if(empty)
						Registry::DeleteSubKey(HKCR, L".nss");
				}

				return ret > 0;// == 5;
			}

			// NOTE: The function add or removes the {{<CLSID>}} key under
			// HKCU\Software\Classes\<type>\shellex\ContextMenuHandlers in the registry.
			static bool RegisterContextMenuHandler(const wchar_t *fileType, bool bRegister)
			{
				if(!fileType || !fileType[0])
					return false;

				wchar_t default_value[MAX_PATH] { 0 };
				if(*fileType == L'.')
				{
					auto key = Registry::ClassesRoot.OpenSubKey(fileType);
					// If the key exists and its default value is not empty,
					// use the ProgID as the file type.
					if(key)
					{
						if(key.GetString(nullptr, default_value, MAX_PATH))
							fileType = default_value;
						key.Close();
					}
				}

				string subKey = fileType;
				subKey += HKCR_CONTEXTMENUHANDLERS;
				// key HKCR\<Types>\shellex\ContextMenuHandlers\{{<CLSID>}}
				// Create
				if(bRegister)
					return Registry::SetKeyValue(HKCR, subKey, nullptr, CLS_ContextMenu);
				// Remove 
				return Registry::DeleteSubKey(HKCR, subKey);
			}

			// Update Registry
			// true	 = Register the context menu handler. 
			// false = Unregister the context menu handler.
			static bool RegisterContextMenuHandler(bool _register)
			{
				int res = 0;
				// The context menu handler is associated with the classes.
				res += RegisterContextMenuHandler(L"*", _register);
				res += RegisterContextMenuHandler(L"Directory", _register);
				//res += RegisterContextMenuHandler(L"AllFileSystemObjects", _register);
				res += RegisterContextMenuHandler(L"Drive", _register);
				res += RegisterContextMenuHandler(L"Folder", _register);
				res += RegisterContextMenuHandler(L"Directory\\Background", _register);
				res += RegisterContextMenuHandler(L"DesktopBackground", _register);
				res += RegisterContextMenuHandler(L"LibraryFolder", _register);
				res += RegisterContextMenuHandler(L"LibraryFolder\\Background", _register);
				return res > 0;
			}

			static bool get(const wchar_t* subkey, const wchar_t *name, string &value)
			{
				string _subkey = APP_KEY;
				if(subkey)
				{
					if(subkey[0] != L'\\')
						_subkey += L"\\";
					_subkey += subkey;
				}

				if(auto key = Registry::CurrentUser.OpenSubKey(_subkey, true, false); key)
					return key.ReadString(name, value);
				return false;
			}

			static bool get(const wchar_t *subkey, const wchar_t *name, int &value)
			{
				string _subkey = APP_KEY;
				if(subkey)
				{
					if(subkey[0] != L'\\')
						_subkey += L"\\";
					_subkey += subkey;
				}
				
				if(auto key = Registry::CurrentUser.OpenSubKey(_subkey, true, false); key)
				{
					value = key.GetInt(name, value);
					return true;
				}
				return false;
			}
		};
	}
