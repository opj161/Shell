#pragma once

namespace Nilesoft
{
	class DLL
	{
		HMODULE m_handle = nullptr;
		const wchar_t* m_dllPath = nullptr;
		bool loadLibrary = false;

	public:
		DLL() noexcept {}
		DLL(HMODULE handel) :m_handle(handel) {}
		DLL(const wchar_t* dllPath, bool load_as_data = false) 
			: m_dllPath(dllPath), m_handle(nullptr) 
		{
			load(load_as_data);
		}
		~DLL()
		{
			if(loadLibrary && m_handle)
				::FreeLibrary(m_handle);
		}

		HMODULE handel() { return m_handle; }
		operator HMODULE() { return m_handle; }

		explicit operator bool() const { return m_handle != nullptr; }

		/*
			Loads a module without handing the current directory a chance at it.

			A name with no directory in it is one of Windows' own DLLs - every such
			name in this codebase is: kernel32, user32, ntdll, shell32, uxtheme,
			OleAut32, Shcore, Userenv, Comdlg32, winbrand. Loading those by bare name
			uses the standard search order, which reaches "the current directory"
			ahead of PATH, and that is the opening a DLL preloading attack needs -
			the more so here, where this code runs inside explorer.exe and every
			other host that raises a shell menu.

			Microsoft's first two mitigations are to "specify a fully qualified path"
			or to "use the LOAD_LIBRARY_SEARCH flags with the LoadLibraryEx
			function". This does the second, and falls back to the first rather than
			to an unqualified load, because LOAD_LIBRARY_SEARCH_SYSTEM32 needs
			KB2533623 on Windows 7 and fails with ERROR_INVALID_PARAMETER without it.

			  https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-security

			A name that does carry a directory is a file the caller chose - an icon
			source, say - and is loaded exactly as given.
		*/
		static HMODULE LoadSafe(const wchar_t *name, DWORD flags = 0)
		{
			if(!name || !*name)
				return nullptr;

			if(::wcspbrk(name, L"\\/:"))
				return ::LoadLibraryExW(name, nullptr, flags);

			if(auto module = ::LoadLibraryExW(name, nullptr, flags | LOAD_LIBRARY_SEARCH_SYSTEM32))
				return module;

			if(::GetLastError() != ERROR_INVALID_PARAMETER)
				return nullptr;

			wchar_t path[MAX_PATH]{};
			auto length = ::GetSystemDirectoryW(path, ARRAYSIZE(path));
			if(length == 0 || length >= ARRAYSIZE(path))
				return nullptr;

			// Documented to omit the trailing separator except at a drive root.
			if(path[length - 1] != L'\\')
			{
				if(length + 1 >= ARRAYSIZE(path))
					return nullptr;
				path[length++] = L'\\';
			}

			for(auto p = name; *p; ++p)
			{
				if(length + 1 >= ARRAYSIZE(path))
					return nullptr;
				path[length++] = *p;
			}

			return ::LoadLibraryExW(path, nullptr, flags);
		}

		/*
			The same guarantee, for Windows binaries that are not in System32.

			LoadSafe refuses the search path, which is the point of it - but that
			leaves no way to reach the handful of Windows files kept elsewhere.
			regedit.exe lives in the Windows directory and powershell.exe under
			System32\WindowsPowerShell\v1.0, so a bare name finds neither, and
			both went from loading to ERROR_FILE_NOT_FOUND when the search-path
			hardening landed.

			`relative` is resolved against the Windows directory, so what reaches
			LoadLibraryEx is still a fully qualified path that was never searched
			for - Microsoft's first listed mitigation rather than its second.

			  https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-security
		*/
		static HMODULE LoadSafeWindows(const wchar_t *relative, DWORD flags = 0)
		{
			if(!relative || !*relative)
				return nullptr;

			wchar_t path[MAX_PATH]{};
			auto length = ::GetWindowsDirectoryW(path, ARRAYSIZE(path));
			if(length == 0 || length >= ARRAYSIZE(path))
				return nullptr;

			// Documented to omit the trailing separator except at a drive root.
			if(path[length - 1] != L'\\')
			{
				if(length + 1 >= ARRAYSIZE(path))
					return nullptr;
				path[length++] = L'\\';
			}

			for(auto p = relative; *p; ++p)
			{
				if(length + 1 >= ARRAYSIZE(path))
					return nullptr;
				path[length++] = *p;
			}

			return ::LoadLibraryExW(path, nullptr, flags);
		}

		bool load(bool load_as_data = false)
		{
			if(m_handle)
				return true;

			if(m_dllPath)
			{
				m_handle = ::GetModuleHandleW(m_dllPath);
				if(!m_handle)
				{
					m_handle = LoadSafe(m_dllPath, load_as_data ? LOAD_LIBRARY_AS_DATAFILE : 0);

					loadLibrary = true;
				}
				return m_handle != nullptr;
			}
			return false;
		}

		void unload()
		{
			if(m_handle && loadLibrary)
			{
				loadLibrary = false;
				::FreeLibrary(m_handle);
			}
			m_handle = nullptr;
		}

		bool is_func(const char* lpProcName)
		{
			if(m_handle)
				return ::GetProcAddress(m_handle, lpProcName) != nullptr;
			return false;
		}

		template<typename RET = int, typename... Args>
		RET invoke(const char *lpProcName, Args... arguments)
		{
			RET ret{};
			if(m_handle && lpProcName)
			{
				if(auto lpFunc = ::GetProcAddress(m_handle, lpProcName); lpFunc)
				{
					using func_call = RET(__stdcall *)(Args... args);
					ret = ((func_call)lpFunc)(arguments...);
				}
			}
			return ret;
		}

		static bool Load(const wchar_t* dllPath, HMODULE &hModule)
		{
			hModule = LoadSafe(dllPath);
			return (hModule != nullptr);
		}

		static bool Unload(HMODULE hModule)
		{
			if(hModule)
				return !!::FreeLibrary(hModule);
			return false;
		}

		template<typename T>
		T Get(const char* lpProcName)
		{
			if(lpProcName && m_handle)
				return (T)::GetProcAddress(m_handle, lpProcName);
			return nullptr;
		}

		template<typename T>
		T Get(int ordinal)
		{
			if(m_handle)
				return (T)::GetProcAddress(m_handle, MAKEINTRESOURCEA(ordinal));
			return nullptr;
		}

		template<typename T>
		bool Get(const char* lpProcName, T* lpFunc)
		{
			if(lpFunc)
			{
				*lpFunc = Get<T>(lpProcName);
				return *lpFunc != nullptr;
			}
			return false;
		}

		template<typename T>
		static T Get(const wchar_t* dllPath, const char* lpProcName)
		{
			T lpFunc = nullptr;
			if(dllPath && lpProcName)
			{
				auto hModule = ::GetModuleHandleW(dllPath);
				if(hModule)
					lpFunc = (T)::GetProcAddress(hModule, lpProcName);
				else
				{
					hModule = LoadSafe(dllPath);
					if(hModule)
					{
						lpFunc = (T)::GetProcAddress(hModule, lpProcName);
						::FreeLibrary(hModule);
					}
				}
			}
			return lpFunc;
		}

		template<typename T>
		static bool Get(const wchar_t* dllPath, const char* lpProcName, T* lpFunc)
		{
			if(lpFunc)
			{
				*lpFunc = Get<T>(dllPath, lpProcName);
				return *lpFunc != nullptr;
			}
			return false;
		}

		template<typename T>
		static T Get(HMODULE hModule, const char* lpProcName)
		{
			if(hModule && lpProcName)
				return reinterpret_cast<T>(::GetProcAddress(hModule, lpProcName));
			return nullptr;
		}

		template<typename T>
		static bool Get(HMODULE hModule, const char* lpProcName, T* lpFunc)
		{
			if(hModule && lpProcName && lpFunc)
			{
				*lpFunc = (T)::GetProcAddress(hModule, lpProcName);
				return *lpFunc != nullptr;
			}
			return false;
		}

		static bool IsFunc(HMODULE hModule, const char *lpProcName)
		{
			if(hModule)
				return ::GetProcAddress(hModule, lpProcName) != nullptr;
			return false;
		}

		static bool IsFunc(HMODULE hModule, uint32_t ordinal)
		{
			if(hModule)
				return ::GetProcAddress(hModule, MAKEINTRESOURCEA(ordinal)) != nullptr;
			return false;
		}

		template<typename RET=int, typename... Args>
		static RET Invoke(HMODULE hModule, const char *lpProcName, Args... arguments)
		{
			RET ret{};
			if(hModule && lpProcName)
			{
				if(auto lpFunc = ::GetProcAddress(hModule, lpProcName); lpFunc)
				{
					using func_call = RET(__stdcall *)(Args... args);
					ret = ((func_call)lpFunc)(arguments...);
				}
			}
			return ret;
		}

		template<typename RET = int, typename... Args>
		static RET Invoke(HMODULE hModule, uint32_t ordinal, Args... arguments)
		{
			RET ret{};
			try {
				ret = Invoke<RET>(hModule, MAKEINTRESOURCEA(ordinal), arguments...);
			}
			catch(...) {
			}
			return ret;
		}

		/*template<typename... Args>
		static void Invoke(HMODULE hModule, const char *lpProcName, Args... arguments)
		{
			if(hModule && lpProcName)
			{
				if(auto lpFunc = ::GetProcAddress(hModule, lpProcName))
				{
					using func_call = RET(__stdcall *)(Args... args);
					((func_call)lpFunc)(arguments...);
				}
			}
		}*/

		template<typename... Args>
		static void Invoke(const wchar_t*name, const char* lpProcName, Args... arguments)
		{
			if(name && lpProcName)
			{
				bool freeLibrary = false;
				auto hModule = ::GetModuleHandleW(name);
				if(!hModule)
				{
					hModule = LoadSafe(name);
					freeLibrary = true;
				}

				if(hModule)
				{
					if(auto lpFunc = ::GetProcAddress(hModule, lpProcName); lpFunc)
					{
						using func_call = void(__stdcall*)(Args... args);
						(func_call(lpFunc))(arguments...);
					}
					if(freeLibrary)
						::FreeLibrary(hModule);
				}
			}
		}

		template<typename Result, typename... Args>
		static Result Invoke(const wchar_t* name, const char* lpProcName, Args... arguments)
		{
			Result res{};
			if(name && lpProcName)
			{
				bool freeLibrary = false;
				auto hModule = ::GetModuleHandleW(name);
				if(!hModule)
				{
					hModule = LoadSafe(name);
					freeLibrary = true;
				}

				if(hModule)
				{
					using func_call = Result(__stdcall*)(Args... args);
					if(auto lpFunc = (func_call)::GetProcAddress(hModule, lpProcName); lpFunc)
					{
						res = lpFunc(arguments...);
					}
					if(freeLibrary)
						::FreeLibrary(hModule);
				}
			}
			return res;
		}

		
		static constexpr auto kernel32 = L"kernel32.dll";
		static constexpr auto user32 = L"user32.dll";

		template<typename Result, typename... Args>
		static Result Kernel32(const char *lpProcName, Args... arguments)
		{
			Result res {};
			if(lpProcName)
			{
				bool freeLibrary = false;
				auto hModule = ::GetModuleHandleW(kernel32);
				if(!hModule)
				{
					hModule = LoadSafe(kernel32);
					freeLibrary = true;
				}

				if(hModule)
				{
					using func_call = Result(__stdcall *)(Args... args);
					if(auto lpFunc = (func_call)::GetProcAddress(hModule, lpProcName); lpFunc)
					{
						res = lpFunc(arguments...);
					}

					if(freeLibrary)
						::FreeLibrary(hModule);
				}
			}
			return res;
		}

		template<typename Result, typename... Args>
		static Result User32(const char *lpProcName, Args... arguments)
		{
			Result res {};
			if(lpProcName)
			{
				bool freeLibrary = false;
				auto hModule = ::GetModuleHandleW(user32);
				if(!hModule)
				{
					hModule = LoadSafe(user32);
					freeLibrary = true;
				}

				if(hModule)
				{
					using func_call = Result(__stdcall *)(Args... args);
					if(auto lpFunc = (func_call)::GetProcAddress(hModule, lpProcName); lpFunc)
					{
						res = lpFunc(arguments...);
					}

					if(freeLibrary)
						::FreeLibrary(hModule);
				}
			}
			return res;
		}
	};
}