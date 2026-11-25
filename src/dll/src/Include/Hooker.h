#pragma once

namespace Nilesoft::Shell
{
	struct IATHook
	{
		HMODULE _hModule{};
		std::string _import{};
		uintptr_t _orignal{};
		uintptr_t _detour{};			// trampoline 
		IMAGE_THUNK_DATA *_thunk{};

		IATHook() {}

		IATHook(HMODULE hModule, const char *import = nullptr, void *orignal = nullptr, void *detour = nullptr)
		{
			init(hModule, import, orignal, detour);
		}

		IATHook(const wchar_t *mdule, const char *import = nullptr, void *orignal = nullptr, void *detour = nullptr)
		{
			init(mdule, import, orignal, detour);
		}

		IATHook(IATHook &&other) noexcept
		{
			_hModule = std::exchange(other._hModule, nullptr);
			_import = std::move(other._import);
			_orignal = std::exchange(other._orignal, 0);
			_detour = std::exchange(other._detour, 0);
			_thunk = std::exchange(other._thunk, nullptr);
		}

		~IATHook()
		{
			// uninstall(true) clears _hModule, so reading it afterwards always
			// found null and the reference GetModuleHandleExW took in init() was
			// never released. Every hooked module stayed pinned for the life of
			// the process on top of the pin Shell already holds deliberately.
			HMODULE module = _hModule;
			uninstall(true);
			if(module) ::FreeLibrary(module);
		}

		IATHook &init(HMODULE hModule, const char *import, void *orignal, void *detour)
		{
			// Increment module reference count to prevent other threads from unloading it while we're working with it
			if(::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)hModule, &_hModule) && _hModule)
			{
				//_hModule = hModule;
				_import = import;
				_orignal = reinterpret_cast<uintptr_t>(orignal);
				_detour = reinterpret_cast<uintptr_t>(detour);
			}
			return *this;
		}

		IATHook &init(const wchar_t *mdule, const char *import, void *orignal, void *detour)
		{
			return init(::GetModuleHandleW(mdule), import, orignal, detour);
		}

		IATHook &init(const wchar_t *mdule, const char *import, const char *orignal, void *detour)
		{
			if(auto h = ::GetModuleHandleA(import); h)
			{
				return init(::GetModuleHandleW(mdule), import, static_cast<void *>(::GetProcAddress(h, orignal)), detour);
			}
			return *this;
		}

		//attach
		bool install(bool flush = true)
		{
			if(installed())
				return true;
			return get_thunk() && commit(_detour, flush);
		}

		//detach
		bool uninstall(bool cleare = false, bool flush = true)
		{
			auto ret = false;

			if(installed())
				ret = commit(_orignal, flush);

			if(cleare)
			{
				_hModule = {};
				_import = {};
				_orignal = {};
				_detour = {};
				_thunk = {};
			}
			return ret;
		}

		bool installed() const
		{
			return _thunk &&_thunk->u1.Function == _detour;
		}

		template<typename T>
		T orignal() { return static_cast<T>(_orignal); }

		/*template <typename T, typename... Args>
		auto invoke(Args&&... args)
		{
			if(_orignal && _installed)
				return reinterpret_cast<T*>(_orignal)(args...);
			return reinterpret_cast<T *>(T);
		}*/

		template <typename T, typename... Args>
		auto invoke(Args&&... args)
		{
			return reinterpret_cast<T *>(_orignal)(args...);
		}

	private:

		template<typename T, typename O = DWORD>
		static inline T *Ptr(LPVOID base, O rva = 0)
		{
			return reinterpret_cast<T *>(reinterpret_cast<uint8_t *>(base) + rva);
		}

		bool commit(uintptr_t lpfunc, bool flush = true)
		{
			if(_thunk && lpfunc) __try
			{
				if(lpfunc == _thunk->u1.Function)
					return true;

				DWORD protect;
				if(::VirtualProtect(&_thunk->u1.Function, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &protect))
				{
					_thunk->u1.Function = lpfunc;
					if(::VirtualProtect(&_thunk->u1.Function, sizeof(uintptr_t), protect, &protect))
					{
						if(flush)
						{
							//https://devblogs.microsoft.com/oldnewthing/20190902-00/?p=102828
							::FlushInstructionCache(::GetCurrentProcess(), &_thunk->u1.Function, sizeof(ULONG_PTR));
						}
						return true;
					}
				}
			} except {}
			return false;
		}

		bool get_thunk()
		{
			if(_thunk) return true;

			if(!_hModule || _import.empty() || !_orignal)
				return false;

			// Query to get existing execute access.
			//MEMORY_BASIC_INFORMATION mbi{};
			//if(::VirtualQuery(lpfunc, &mbi, sizeof(mbi)) == 0)
			//	return false;
			__try
			{
				auto dos = Ptr<IMAGE_DOS_HEADER>(_hModule);

				if(!dos || (dos->e_magic != IMAGE_DOS_SIGNATURE))
					return false;

				auto nt = Ptr<IMAGE_NT_HEADERS>(dos, dos->e_lfanew);
				if(!nt || (nt->Signature != IMAGE_NT_SIGNATURE))
					return false;

				auto va = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
				auto id = Ptr<IMAGE_IMPORT_DESCRIPTOR>(dos, va);

				//IMAGE_DELAYLOAD_DESCRIPTOR

				if(!va || !id) return false;

				// Get a reference to the import table to locate the DLL entry
				for(uint32_t i = 0; id[i].Characteristics && id[i].FirstThunk; i++) __try
				{
					auto dll_import = Ptr<char>(dos, id[i].Name);
					if(::_stricmp(dll_import, _import.c_str())) //memicmp
						continue;
					for(auto ftd = Ptr<IMAGE_THUNK_DATA>(dos, id[i].FirstThunk); ftd->u1.Function; ftd++) __try
					{
						if(_orignal == ftd->u1.Function)
						{
							_thunk = ftd;
							return true;
						}
					} except { return false; }
				} except { return false; }
			} except { return false; }
			return false;
		}

		bool get_delay_thunk()
		{
			if(_thunk) return true;

			if(!_hModule || _import.empty() || !_orignal)
				return false;

			// Query to get existing execute access.
			//MEMORY_BASIC_INFORMATION mbi{};
			//if(::VirtualQuery(lpfunc, &mbi, sizeof(mbi)) == 0)
			//	return false;
			__try
			{
				auto dos = Ptr<IMAGE_DOS_HEADER>(_hModule);

				if(!dos || (dos->e_magic != IMAGE_DOS_SIGNATURE))
					return false;

				auto nt = Ptr<IMAGE_NT_HEADERS>(dos, dos->e_lfanew);
				if(!nt || (nt->Signature != IMAGE_NT_SIGNATURE))
					return false;

				auto va = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
				auto dl = Ptr<IMAGE_DELAYLOAD_DESCRIPTOR>(dos, va);

				if(!va || !dl) return false;

				while(dl->DllNameRVA)
				{
					auto dll_import = Ptr<char>(dos,dl->DllNameRVA);
					if(0 == ::_stricmp(dll_import, _import.c_str())) //memicmp
					{
						auto firstthunk = Ptr<IMAGE_THUNK_DATA>(dos, dl->ImportNameTableRVA);
						auto functhunk = Ptr<IMAGE_THUNK_DATA>(dos, dl->ImportAddressTableRVA);
						while(firstthunk->u1.AddressOfData)
						{
							if(firstthunk->u1.Function == _orignal)
							{
								_thunk = functhunk;
								return true;
							}

							/*if(firstthunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
							{
								if(!(*((WORD *)&(funcName)+1)) && IMAGE_ORDINAL32(firstthunk->u1.Ordinal) == (DWORD)funcName)
								{
									_thunk = functhunk;
									return true;
								}
							}
							else
							{
								auto byName = Ptr<IMAGE_IMPORT_BY_NAME>(dos, firstthunk->u1.AddressOfData);
								if((*((WORD *)&(funcName)+1)) && !_stricmp((char *)byName->Name, funcName))
								{
									_thunk = functhunk;
									return true;
								}
							}
							*/
							functhunk++;
							firstthunk++;
						}
					}
					dl++;
				}
			} except{ return false; }
			return false;
		}
	};

	class WindowsHook
	{
	private:
		HHOOK _handle{};
		int _idHook{};
		HOOKPROC _hookproc{};
		HINSTANCE _hModule{};
		DWORD _dwThreadId{};

	public:
		HWND hWindow{};

		WindowsHook()
		{
			init();
		}

		~WindowsHook()
		{
			unhook();
		}

		explicit operator bool() const { return _handle; }
		explicit operator HHOOK() const { return _handle; }

		HHOOK get() const { return _handle; }

		BOOL unhook()
		{
			if(_handle && Unhook(_handle))
				_handle = nullptr;
			return _handle == nullptr;
		}

		BOOL hook(int idHook, HOOKPROC hookproc, HWND hWnd)
		{
			return hook(idHook, hookproc, nullptr, ::GetWindowThreadProcessId(hWnd, nullptr));
		}

		BOOL hook(int idHook, HOOKPROC hookproc, DWORD dwThreadId)
		{
			return hook(idHook, hookproc, nullptr, dwThreadId);
		}

		BOOL hook(int idHook, HOOKPROC hookproc, HMODULE hModule, DWORD dwThreadId)
		{
			unhook();
			_idHook = idHook;
			_hookproc = hookproc;
			_hModule = hModule;
			_dwThreadId = dwThreadId;
			_handle = Hook(idHook, hookproc, hModule, dwThreadId);
			return _handle != nullptr;
		}

		LRESULT callnext(int nCode, WPARAM wParam, LPARAM lParam) const
		{
			return CallNext(_handle, nCode, wParam, lParam);
		}

		static HHOOK Hook(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId)
		{
			if(!init()) return 0;
			return _setWindowsHookExW(idHook, lpfn, hmod, dwThreadId);
		}

		static BOOL Unhook(HHOOK handle)
		{
			if(!init()) return 0;
			return !handle ? 0 : _unhookWindowsHookEx(handle);
		}

		static LRESULT CallNext(HHOOK handle, int nCode, WPARAM wParam, LPARAM lParam)
		{
			if(!init()) return 0;
			return _callNext(handle, nCode, wParam, lParam);
		}

		static bool init()
		{
			if(!user32_hModule)
				user32_hModule = ::GetModuleHandleW(L"user32.dll");
			
			if(user32_hModule)
			{
				if(!_setWindowsHookExW)
					_setWindowsHookExW = (LPFSetWindowsHookExW)::GetProcAddress(user32_hModule, "SetWindowsHookExW");

				if(!_callNext)
					_callNext = (LPFCallNext)::GetProcAddress(user32_hModule, "CallNextHookEx");

				if(!_unhookWindowsHookEx)
					_unhookWindowsHookEx = (LPFUnhookWindowsHookEx)::GetProcAddress(user32_hModule, "UnhookWindowsHookEx");
				
				return true;
			}
			return false;
		}

	private:
		using LPFUnhookWindowsHookEx = BOOL(__stdcall *)(HHOOK handle);
		using LPFSetWindowsHookExW = HHOOK(__stdcall *)(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId);
		using LPFCallNext = LRESULT(__stdcall *)(HHOOK handle, int nCode, WPARAM wParam, LPARAM lParam);

		inline static HMODULE user32_hModule = nullptr;
		inline static LPFCallNext _callNext = nullptr;
		inline static LPFUnhookWindowsHookEx _unhookWindowsHookEx = nullptr;
		inline static LPFSetWindowsHookExW _setWindowsHookExW = nullptr;
	};

	class WinEventHook
	{
		HWINEVENTHOOK _handle{};

	public:
		WinEventHook() {}

		~WinEventHook()
		{
			unhook();
		}

		explicit operator bool() const { return _handle; }
		explicit operator HWINEVENTHOOK() const { return _handle; }

		HWINEVENTHOOK get() const { return _handle; }

		BOOL unhook()
		{
			if(_handle && ::UnhookWinEvent(_handle))
				_handle = nullptr;
			return _handle == nullptr;
		}

		BOOL hook(DWORD eventMin, DWORD eventMax, HMODULE hmodWinEventProc, WINEVENTPROC pfnWinEventProc, DWORD idProcess, DWORD idThread, DWORD dwFlags = WINEVENT_INCONTEXT)
		{
			_handle = ::SetWinEventHook(eventMin, eventMax, hmodWinEventProc, pfnWinEventProc, idProcess, idThread, dwFlags);
			return _handle != nullptr;
		}
	};

	class WindowSubclass
	{
		HWND _hWnd{};
		SUBCLASSPROC _subclass{};
		UINT_PTR _uIdSubclass{};
		DWORD_PTR _dwRefData{};
		bool _hooked {};

	public:
		WindowSubclass() {}
		~WindowSubclass() 
		{
			unhook();
		}

		explicit operator bool() const { return _hooked; }

		BOOL hook(HWND hWnd, SUBCLASSPROC pfnSubclass, UINT_PTR uIdSubclass, auto dwRefData)
		{
			_hWnd = hWnd; 
			_subclass = pfnSubclass;
			_uIdSubclass = uIdSubclass;
			_dwRefData = reinterpret_cast<DWORD_PTR>(dwRefData);
			_hooked = Set(hWnd, pfnSubclass, uIdSubclass, _dwRefData);
			return _hooked;
		}

		BOOL unhook()
		{
			if(_subclass)
				Remove(_hWnd, _subclass, _uIdSubclass);

			_hWnd = {}; 
			_subclass = {}; 
			_uIdSubclass = {};
			_dwRefData = {};
			_hooked={};

			return _hooked;
		}

		//template<typename T>
		static BOOL Set(HWND hWnd, SUBCLASSPROC pfnSubclass, UINT_PTR uIdSubclass, auto dwRefData)
		{
			return ::SetWindowSubclass(hWnd, pfnSubclass, uIdSubclass, (DWORD_PTR)dwRefData);
		}

		static BOOL Remove(HWND hWnd, SUBCLASSPROC pfnSubclass, UINT_PTR uIdSubclass)
		{
			return ::RemoveWindowSubclass(hWnd, pfnSubclass, uIdSubclass);
		}

		template<typename T>
		static BOOL Get(HWND hWnd, SUBCLASSPROC pfnSubclass, UINT_PTR uIdSubclass, T *dwRefData)
		{
			return ::GetWindowSubclass(hWnd, pfnSubclass, uIdSubclass, static_cast<DWORD_PTR*>(dwRefData));
		}
	};

}