#pragma once
#include "System/Diagnostics/Process.h"

namespace Nilesoft
{
	namespace Windows
	{
		constexpr auto SHGETSETSETTINGS = "SHGetSetSettings";
		constexpr auto SHCHANGENOTIFY="SHChangeNotify";
		constexpr auto SHELL32=L"Shell32.dll";
		constexpr auto KEY_CURRENTVERSION = LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)";

		class Version
		{
		public:
			uint32_t Major = 0;
			uint32_t Minor = 0;
			uint32_t Build = 0;
			uint32_t ReleaseId = 0;
			uint32_t Type = 1;
			uint32_t Architecture = PROCESSOR_ARCHITECTURE_UNKNOWN;

			// Windows Insider build flags
			bool IsCanaryBuild = false;
			bool IsDevBuild = false;
			bool IsBetaBuild = false;
			bool IsPreviewBuild = false;

			//bool Is64Bit = false;
			string ProductType;
			string Name;
			string BuildNumber;
			string FullBuildNumber;
			string BuildStr;

			Version() noexcept
			{
				//Is64Bit = Is64BitProcess();

				Architecture = ProcessorArchitecture();

				Type = Is64BitProcess();

				if (!Type)
				{
					switch (Architecture)
					{
					case PROCESSOR_ARCHITECTURE_AMD64:
					case PROCESSOR_ARCHITECTURE_IA64:
					case PROCESSOR_ARCHITECTURE_ARM64:
						Type = 1;
						break;
					default:
						Type = 0;
						break;
					}
				}

				//OSVERSIONINFOEXW osvi = { };
				//osvi.dwOSVersionInfoSize = sizeof(osvi);
				//DLL::Invoke<long>(L"ntdll", "RtlGetVersion", &osvi);
				// RtlGetNtVersionNumbers is an undocumented ntdll export - there is
				// no Learn page for it - so what it returns is established by
				// measurement, not by reading. It is used rather than GetVersionEx
				// or the VersionHelpers because those are manifest-gated and report
				// 6.2 to an unmanifested caller, which this DLL is: it is loaded by
				// whatever host activates it, so it cannot rely on a manifest of
				// its own.
				//
				// Probed 2026-11-25 on Windows 11 26H2 26300.9974 x64, against
				// HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\CurrentBuild:
				//
				//   build returned : 0xF00066BC
				//   masked         : 26300
				//   CurrentBuild   : 26300   -> match
				//
				// The high nibble carries flags, which is what the mask below
				// removes; 0x66BC is the build. Re-run that comparison if the
				// version ever reads wrong - a changed flag layout would show up
				// as a build number in the billions, and every IsWindows11OrGreater
				// branch depends on it.
				DLL::Invoke(L"ntdll", "RtlGetNtVersionNumbers", &Major, &Minor, &Build);
				if(Major > 0)
				{
					Build &= ~0xF0000000;
					// Build &= ~0xFFFFFFF0;
					//Major = osvi.dwMajorVersion;
					//Minor = osvi.dwMinorVersion;
					//Build = osvi.dwBuildNumber;

					RegistryKey keyLM(HKEY_LOCAL_MACHINE, false, Is64Bit() ? KEY_WOW64_64KEY : 0);//RRF_SUBKEY_WOW6432KEY
					if(auto key = keyLM.OpenSubKey(KEY_CURRENTVERSION))
					{
						//string build = key.GetString(L"CurrentBuild").move();
						//Build = build.ToInt();
						if(Major == 10 && Build >= 22000)
						{
							// Check for Windows Insider build
							if (auto insiderKey = keyLM.OpenSubKey(L"SOFTWARE\\Microsoft\\WindowsSelfHost\\Applicability"))
							{
								string flightRing = insiderKey.GetString(L"FlightRing").move();
								if(!flightRing.empty())
								{
									if(flightRing.equals(L"Canary"))
										IsCanaryBuild = true;
									else if(flightRing.equals(L"Dev"))
										IsDevBuild = true;
									else if(flightRing.equals(L"Beta"))
										IsBetaBuild = true;
									else if(flightRing.equals(L"ReleasePreview"))
										IsPreviewBuild = true;
								}
								insiderKey.Close();
							}

							//https://dennisbabkin.com/blog/?t=how-to-tell-the-real-version-of-windows-your-app-is-running-on#ver_string
							/*
							%WINDOWS_GENERIC%
							%WINDOWS_SHORT%
							%WINDOWS_LONG%
							%WINDOWS_PRODUCT%
							%WINDOWS_COPYRIGHT%
							%MICROSOFT_COMPANYNAME%
							%MICROSOFT_ACCOUNT%
							%MICROSOFT_ACCOUNTS%
							%WINDOWS_CLIENT_VERSION_6_1%
							%WINDOWS_SERVER_VERSION_6_1%
							%WINDOWS_CLIENT_VERSION_6_2%
							%WINDOWS_SERVER_VERSION_6_2%
							%WINDOWS_ARM_VERSION_6_2%
							%WINDOWS_CLIENT_VERSION_6_3%
							%WINDOWS_SERVER_VERSION_6_3%
							%WINDOWS_ARM_VERSION_6_3%
							%WINDOWS_CLIENT_VERSION_6_4%
							%WINDOWS_SERVER_VERSION_6_4%
							%WINDOWS_ARM_VERSION_6_4%
							%WINDOWS_CLIENT_VERSION_10_0%
							%WINDOWS_SERVER_VERSION_10_0%
							%WINDOWS_SERVER_VERSION_10_0_2019_LTSC%
							%IDS_WINDOWS_SERVER_VERSION_10_0_1809_SAC% 
							*/

							auto brandingFormatString = "BrandingFormatString";
							DLL dll(L"winbrand.dll");
							if(dll.load() && dll.is_func(brandingFormatString))
							{
								if(auto ret = dll.invoke<wchar_t *>(brandingFormatString, L"%WINDOWS_LONG%"); ret)
								{
									Name = ret;
									::GlobalFree((HGLOBAL)ret);
								}
							}
					
							if(Name.empty())
							{
								Name = L"Windows 11";
								string editionID = key.GetString(L"EditionID").move();
								if(!editionID.empty())
									Name += L" " + editionID;
							}
						}
						else 
						{
							// "Windows 7 Ultimate"
							// "Windows 10 Pro"  (same string on Windows 11. Microsoft SUCKS!)
							Name = key.GetString(L"ProductName").move();
						}

						Name += Type ? L" 64-bit" : L" 32-bit";

						BuildStr.format(L"%u", Build);

						if(Major > 6)
						{
							ReleaseId = key.GetString(L"ReleaseId").to_int<int>();
							FullBuildNumber.format(L"%u Bulid %u.%u.%u", ReleaseId, Major, Minor, Build);
							BuildNumber.format(L"%u.%u.%u", Major, Minor, Build);
							

							int ubr = 0;
							if(key.GetInt(L"UBR", &ubr))
							{
								FullBuildNumber.append_format(L".%u", ubr);
								BuildNumber.append_format(L".%u", ubr);
								BuildStr.append_format(L".%u", ubr);
							}
						}
						else
						{
							FullBuildNumber.format(L"%u.%u.%u", Major, Minor, Build);
							BuildNumber = FullBuildNumber;
						}

						ProductType = key.GetString(L"EditionID").move();

						key.Close();
					}
				}
			}

			~Version()
			{
			}

			string ToString() const
			{
				return (Name + L" Version " + BuildNumber).move();
			}

			bool IsWindows11OrGreater() const { return (Major > 10 || (Major == 10 && Build >= 22000)); }
			bool IsWindows11CanaryOrDev() const { return IsWindows11OrGreater() && (IsCanaryBuild || IsDevBuild); }
			bool IsWindows10OrGreater() const { return IsWindowsVersionOrGreater(10, 0); }
			bool IsWindows81OrGreater() const { return IsWindowsVersionOrGreater(6, 3); }
			bool IsWindows8OrGreater() const { return IsWindowsVersionOrGreater(6, 2); }
			bool IsWindows7OrGreater() const { return IsWindowsVersionOrGreater(6, 1); }
			bool IsWindowsVistaOrGreater() const { return IsWindowsVersionOrGreater(6, 0); }
			bool IsWindowsXPOrGreater() const { return IsWindowsVersionOrGreater(5, 1); }

			bool IsWindows10OrEarlier() const
			{
				return (Major < 10) || ((Major == 10) && (Build < 22000));
			}

			bool IsWindowsVersionOrGreater(uint32_t majorVersion, uint32_t minorVersion, uint32_t buildVersion) const
			{
				if(Major > majorVersion)
					return true;
				return (Major == majorVersion) && (Minor >= minorVersion) && (Build >= buildVersion );
			}

			bool IsWindowsVersionOrGreater(uint32_t majorVersion, uint32_t minorVersion) const
			{
				if(Major > majorVersion)
					return true;
				return (Major == majorVersion) && (Minor >= minorVersion);
			}

			bool IsWindowsVersionOrGreater(uint32_t majorVersion) const
			{
				return (Major >= majorVersion);
			}

			bool IsWindowsVersion(uint32_t majorVersion) const
			{
				return (Major == majorVersion);
			}

			bool IsWindowsVersion(uint32_t majorVersion, uint32_t minorVersion) const
			{
				return (Major == majorVersion && Minor == minorVersion);
			}

			bool Is64Bit() { return Type == 1; }

			static Version &Instance()
			{
				static Version version;
				return version;
			}

			static uint16_t ProcessorArchitecture()
			{
				SYSTEM_INFO si = { };
				::GetNativeSystemInfo(&si);
				return si.wProcessorArchitecture;
			}

			static bool IsARMProcess()
			{
				auto pt = ProcessorArchitecture();
				return pt == PROCESSOR_ARCHITECTURE_ARM64 || pt == PROCESSOR_ARCHITECTURE_ARM;
			}

			static bool Is64BitProcess()
			{
				return sizeof(uintptr_t) > sizeof(uint32_t);
			}
		};

		// Get and display the user name. 
		static inline string UserName()
		{
			DWORD len = MAX_PATH;
			string temp(MAX_PATH);
			// lpnSize is only meaningful once the call has succeeded, and the
			// count it reports includes the terminating null.
			// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getusernamew
			if(!::GetUserNameW(temp.buffer(), &len) || len == 0)
				return nullptr;
			return temp.release(len - 1).move();
		}

		// Get and display the name of the computer. 
		static inline string ComputerName()
		{
			DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
			string temp(len);
			// Failure returns 0. On success nSize is the copied TCHAR count
			// excluding the terminator — unlike GetUserNameW, which includes it.
			// Ignoring failure leaves a NUL-run that is not empty().
			// https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-getcomputernamew
			if(!::GetComputerNameW(temp.buffer(), &len))
				return nullptr;
			return temp.release(len).move();
		}


		/*
		INPUT input;
		WORD vkey = VK_F12; // see link below
		input.type = INPUT_KEYBOARD;
		input.ki.wScan = MapVirtualKey(vkey, MAPVK_VK_TO_VSC);
		input.ki.time = 0;
		input.ki.dwExtraInfo = 0;
		input.ki.wVk = vkey;
		input.ki.dwFlags = 0; // there is no KEYEVENTF_KEYDOWN
		SendInput(1, &input, sizeof(INPUT));

		input.ki.dwFlags = KEYEVENTF_KEYUP;
		SendInput(1, &input, sizeof(INPUT));
		*/

		/*
		#define WINVER 0x0500
		#include<windows.h>
		void pressKeyB(char mK)
		{
			HKL kbl = GetKeyboardLayout(0);
			INPUT ip;
			ip.type = INPUT_KEYBOARD;
			ip.ki.time = 0;
			ip.ki.dwFlags = KEYEVENTF_UNICODE;
			if ((int)mK<65 && (int)mK>90) //for lowercase
			{
				ip.ki.wScan = 0;
				ip.ki.wVk = VkKeyScanEx( mK, kbl );
			}
			else //for uppercase
			{
				ip.ki.wScan = mK;
				ip.ki.wVk = 0;

			}
			ip.ki.dwExtraInfo = 0;
			SendInput(1, &ip, sizeof(INPUT));
		}
		*/

		/*
		// IMPORTANT: Current keyboard layout 0xf0010413 (Netherland with USA kbd)!!!!!!!
		WORD vkCode = 0x36; // '6'
		INPUT keyEvent = {0};
		keyEvent.type = INPUT_KEYBOARD;
		keyEvent.ki.wVk = vkCode;
		keyEvent.ki.wScan = MapVirtualKeyEx(vkCode, 0, (HKL)0xf0010413);
		SendInput(1, &amp;keyEvent, sizeof(keyEvent));
		*/


		class Explorer
		{
		public:
			// Live: ToggleHiddenFiles / ToggleExtensions call it below Windows 10.
			static void Refresh(bool all_windows = false)
			{
				// 28931 Windows XP, 41504 Windows Vista or greater.
				auto refresh = 41504;
				if(all_windows)
					::SendMessageW(HWND_BROADCAST, WM_COMMAND, refresh, NULL);
				else
					::SendMessageW(::GetActiveWindow(), WM_COMMAND, refresh, NULL);
			}

			static bool Restart()
			{
				uint32_t res = 0;
				auto v = &Version::Instance();
				if (!v->Is64BitProcess() && v->Is64Bit())
				{
					wchar_t win_path[MAX_PATH]{};
					std::ignore = ::GetWindowsDirectoryW(win_path, MAX_PATH);

					wchar_t sys_path[MAX_PATH]{};
					std::ignore = ::GetSystemDirectoryW(sys_path, MAX_PATH);

					string params, cmd = sys_path;
					cmd +=  L"\\cmd.exe";
					params.format(L"/c taskkill /f /im explorer.exe && start %s\\explorer", win_path);
					::ShellExecuteW(nullptr, nullptr, cmd, params, nullptr, SW_HIDE);
					//WinExec("taskkill /f /im explorer.exe && start explorer.exe", SW_HIDE);
					return res > 0;
				}

				for(auto &process : Nilesoft::Diagnostics::Process::EnumInfo())
				{
					if(process.th32ProcessID)
					{
						if(string::Equals(process.szExeFile, L"explorer.exe"))
						{
							if(auto hProcess = ::OpenProcess(PROCESS_TERMINATE, FALSE,
															 process.th32ProcessID); hProcess)
							{
								res += ::TerminateProcess(hProcess, 0);
								::CloseHandle(hProcess);
							}
						}
					}
				}
				return res > 0;
			}

			/*
				RegRead, HiddenFilesStatus, HKEY_CURRENT_USER, Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced, Hidden
				HiddenFilesStatus := HiddenFilesStatus != 1 ? 1 : 2 ; 1 = show hidden files / 2 = hide hidden files
				RegWrite, REG_DWORD, HKEY_CURRENT_USER, Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced, Hidden, %HiddenFilesStatus%
			*/
			static void ToggleHiddenFiles()
			{
				typedef void (WINAPI *SHGetSetSettingsProc)(LPSHELLSTATE lpss, DWORD dwMask, BOOL bSet);
				SHGetSetSettingsProc shGetSetSettings;
				if(DLL::Get(SHELL32, SHGETSETSETTINGS, &shGetSetSettings))
				{
					SHELLSTATE ss = { 0 };
					shGetSetSettings(&ss, SSF_SHOWALLOBJECTS | SSF_SHOWSYSFILES | SSF_SHOWSUPERHIDDEN, FALSE);
					ss.fShowAllObjects = ss.fShowAllObjects ? FALSE : TRUE;
					ss.fShowSysFiles = ss.fShowAllObjects;
					ss.fShowSuperHidden = ss.fShowAllObjects;
					shGetSetSettings(&ss, SSF_SHOWALLOBJECTS | SSF_SHOWSYSFILES | SSF_SHOWSUPERHIDDEN, TRUE);
					if(!::IsWindows10OrGreater())
						Refresh();
				}
			}

			static void ToggleExtensions()
			{
				typedef void (WINAPI *SHGetSetSettingsProc)(LPSHELLSTATE lpss, DWORD dwMask, BOOL bSet);
				SHGetSetSettingsProc shGetSetSettings;
				if(DLL::Get(SHELL32, SHGETSETSETTINGS, &shGetSetSettings))
				{
					SHELLSTATE ss = { 0 };
					shGetSetSettings(&ss, SSF_SHOWEXTENSIONS, FALSE);
					ss.fShowExtensions = ss.fShowExtensions ? FALSE : TRUE;
					shGetSetSettings(&ss, SSF_SHOWEXTENSIONS, TRUE);
					if(!::IsWindows10OrGreater())
						Refresh();
				}
			}

			/*
				SHELLSTATE state;
				state.fShowAllObjects = (uint32_t)1;
				SHGetSetSettings(&state, SSF_SHOWALLOBJECTS, true);
			*/
		};
	}
}
