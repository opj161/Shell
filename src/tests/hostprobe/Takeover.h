#pragma once

/*
	Puts Shell's hook into this process, so the same scenarios can be run
	through takeover and diffed against the untouched-Windows baseline.

	This is the half of the harness docs/refactor/06-phases-and-tests.md section
	2 describes as "still to build", and it was scheduled behind "a deployed,
	injected build" - which turned out to be the wrong prerequisite. Shell is a
	COM in-process server: any process that asks it for a class object gets the
	popup hook installed, because that is where BootstrapOnce is called from.
	The comment on ShellCheckConfig in src/dll/src/Main.cpp says so from the
	other direction:

		"Deliberately does *not* call BootstrapOnce(): loading this DLL to ask
		 it a question must not install a hook into the asking process. DllMain
		 does nothing but record the instance, so a plain LoadLibrary is inert."

	So LoadLibrary alone is not enough and DllGetClassObject is exactly enough.
	No injection, no deployment, no Explorer restart - and the menus being
	tracked are this process's own, which is what makes the comparison sound.

	Two properties of that call are worth knowing before relying on it:

	  - BootstrapOnce() runs *before* DllGetClassObject's registration,
	    disabled and parse-error checks, so the hook is installed even when the
	    call goes on to return CLASS_E_CLASSNOTAVAILABLE. A refusal here says
	    nothing about whether takeover is active.
	  - Shell pins its own module once hooks are installed (PinModule), so the
	    library cannot be unloaded afterwards and takeover lasts for the life of
	    the process. A run must not mix takeover and native scenarios.

	Which configuration gets loaded follows from the path: Initializer derives
	`shell.nss` from the directory of the module it was given
	(src/dll/src/Initializer.cpp, `application.ConfigPortable`). Pointing at the
	build output therefore gives Shell with no configuration at all - the
	identity config section 2 of the plan asks for - while pointing at the
	installed copy gives the user's real rules.
*/

#include <windows.h>
#include <psapi.h>

#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace hostprobe
{
	struct TakeoverLoad
	{
		bool loaded{};
		bool bootstrapped{};
		std::wstring path;
		std::wstring detail;

		// Whether `path` is the copy COM will activate. It matters more than it
		// looks: a scenario that builds its menu through the shell namespace
		// causes COM to load the *registered* Shell by path, so pointing
		// --shell somewhere else leaves two Shells mapped in one process and
		// silently tests the registered one. See `registered` below.
		bool is_the_registered_copy{};
		std::wstring registered;
	};

	namespace detail
	{
		// {BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1} - Shell's context-menu class.
		// Spelled out rather than included from src/shared/Globals.h so the
		// harness keeps depending on nothing but the Windows SDK.
		inline const GUID &shell_context_menu_clsid()
		{
			static const GUID id = { 0xbae3934b, 0x8a6a, 0x4bfb,
									 { 0x81, 0xbd, 0x3f, 0xc5, 0x99, 0xa1, 0xba, 0xf1 } };
			return id;
		}

		inline std::wstring registered_shell_path()
		{
			HKEY key = nullptr;
			auto path = L"CLSID\\{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}\\InprocServer32";
			if(::RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
				return std::wstring();

			// RegQueryValueEx "is NOT guaranteed to be null-terminated" for a
			// string value, so size from the byte count it reports and add the
			// terminator here rather than trusting one to be present.
			// https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regqueryvalueexw
			DWORD bytes = 0, type = 0;
			std::wstring value;
			if(::RegQueryValueExW(key, nullptr, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS
			   && (type == REG_SZ || type == REG_EXPAND_SZ) && bytes >= sizeof(wchar_t))
			{
				std::wstring buffer(bytes / sizeof(wchar_t) + 1, L'\0');
				if(::RegQueryValueExW(key, nullptr, nullptr, nullptr,
									  reinterpret_cast<LPBYTE>(buffer.data()),
									  &bytes) == ERROR_SUCCESS)
				{
					buffer.resize(bytes / sizeof(wchar_t));
					while(!buffer.empty() && buffer.back() == L'\0')
						buffer.pop_back();
					value = buffer;
				}
			}
			::RegCloseKey(key);
			return value;
		}
	}

	// Every copy of shell.dll currently mapped into this process, by full path.
	// More than one is not a harness bug but it is a trap, so it gets printed.
	inline std::vector<std::wstring> loaded_shells()
	{
		std::vector<std::wstring> out;
		HMODULE modules[1024]{};
		DWORD needed = 0;
		if(!::EnumProcessModules(::GetCurrentProcess(), modules, sizeof(modules), &needed))
			return out;

		auto count = needed / sizeof(HMODULE);
		for(size_t i = 0; i < count; i++)
		{
			wchar_t name[MAX_PATH]{};
			if(!::GetModuleFileNameW(modules[i], name, MAX_PATH))
				continue;

			auto path = std::wstring(name);
			auto slash = path.find_last_of(L'\\');
			auto leaf = slash == std::wstring::npos ? path : path.substr(slash + 1);
			if(::_wcsicmp(leaf.c_str(), L"shell.dll") == 0)
				out.push_back(path);
		}
		return out;
	}

	// An empty `dll` means the copy the machine has registered.
	inline TakeoverLoad load_shell(const std::wstring &dll)
	{
		TakeoverLoad out;
		out.registered = detail::registered_shell_path();
		out.path = dll.empty() ? out.registered : dll;
		out.is_the_registered_copy = !out.registered.empty()
			&& ::_wcsicmp(out.path.c_str(), out.registered.c_str()) == 0;

		if(out.path.empty())
		{
			out.detail = L"no path given and no registered InprocServer32";
			return out;
		}

		auto module = ::LoadLibraryW(out.path.c_str());
		if(!module)
		{
			wchar_t buffer[128];
			::swprintf_s(buffer, L"LoadLibrary failed: %lu", ::GetLastError());
			out.detail = buffer;
			return out;
		}
		out.loaded = true;

		using get_class_object_t = HRESULT(__stdcall *)(REFCLSID, REFIID, void **);
		auto entry = reinterpret_cast<get_class_object_t>(
			::GetProcAddress(module, "DllGetClassObject"));
		if(!entry)
		{
			out.detail = L"the module exports no DllGetClassObject";
			return out;
		}

		IUnknown *factory = nullptr;
		auto hr = entry(detail::shell_context_menu_clsid(), IID_IClassFactory,
						reinterpret_cast<void **>(&factory));
		if(factory)
			factory->Release();

		// Installed either way - see the note at the top about the order of
		// BootstrapOnce and the refusals that follow it.
		out.bootstrapped = true;

		wchar_t buffer[128];
		::swprintf_s(buffer, L"DllGetClassObject -> 0x%08lX%s",
					 static_cast<unsigned long>(hr),
					 SUCCEEDED(hr) ? L"" : L" (hook still installed)");
		out.detail = buffer;
		return out;
	}
}
