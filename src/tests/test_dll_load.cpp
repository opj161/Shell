// Loading the Windows modules the menu reads its localised titles from.
//
// Initializer::load_mui pulls the display text of standard menu items out of
// the system binaries that own them - "Merge" from regedit.exe, "Run with
// PowerShell" from powershell.exe, and a dozen more - hashes each title, and
// matches host menu items against it to attach the right icon. When a module
// does not load, the entry silently falls back to the English string compiled
// into the DLL, so on a localised Windows the item stops matching and loses its
// icon. Nothing reports it.
//
// The search-path hardening routed all of those loads through DLL::LoadSafe,
// which refuses the search path and looks only in System32. Two of them are not
// in System32:
//
//     regedit.exe      C:\Windows\regedit.exe
//     powershell.exe   C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe
//
// Both went to ERROR_FILE_NOT_FOUND. These pin the fix without giving the
// search path back: LoadSafeWindows still hands LoadLibraryEx a fully qualified
// path, which is Microsoft's first listed mitigation.
//
//   https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-security

#include "test.h"

#include <windows.h>
#include "System.h"

using namespace Nilesoft;

namespace
{
	constexpr DWORD ResourceOnly = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;

	struct ScopedModule
	{
		HMODULE handle = nullptr;
		explicit ScopedModule(HMODULE h) : handle(h) {}
		~ScopedModule() { if(handle) ::FreeLibrary(handle); }
		ScopedModule(const ScopedModule &) = delete;
		ScopedModule &operator=(const ScopedModule &) = delete;
	};

	// The resource ids load_mui actually asks these modules for.
	bool has_string(HMODULE module, UINT id)
	{
		wchar_t buffer[256]{};
		return module && ::LoadStringW(module, id, buffer, ARRAYSIZE(buffer)) > 0;
	}
}

TEST(dll_load, a_system32_module_still_loads_by_bare_name)
{
	ScopedModule m(DLL::LoadSafe(L"ntshrui.dll", ResourceOnly));
	CHECK_MSG(m.handle != nullptr, "the System32 path must keep working");
}

// regedit.exe is in the Windows directory, not System32.
TEST(dll_load, regedit_loads_and_still_has_its_merge_string)
{
	ScopedModule bare(DLL::LoadSafe(L"regedit.exe", ResourceOnly));
	ScopedModule windows(DLL::LoadSafeWindows(L"regedit.exe", ResourceOnly));

	CHECK_MSG(windows.handle != nullptr,
			  "regedit.exe lives in the Windows directory and must load from there");
	CHECK_MSG(has_string(windows.handle, 310),
			  "resource 310 is the localised 'Merge' title load_mui reads");

	// Whether the bare name finds it depends on the bitness, which is why this
	// went unnoticed: C:\\Windows\\SysWOW64\\regedit.exe exists and
	// C:\\Windows\\System32\\regedit.exe does not, so a 32-bit Shell resolves it
	// through WOW64 redirection while the 64-bit and ARM64 builds - the ones that
	// ship to most machines - do not. Not asserted either way, because it is a
	// property of the host, not of this code.
	(void)bare.handle;
}

// powershell.exe is under System32\WindowsPowerShell\v1.0.
TEST(dll_load, powershell_loads_from_its_own_directory)
{
	ScopedModule m(DLL::LoadSafeWindows(L"System32\\WindowsPowerShell\\v1.0\\powershell.exe",
								  ResourceOnly));
	CHECK_MSG(m.handle != nullptr, "powershell.exe is not in System32");
	CHECK_MSG(has_string(m.handle, 108),
			  "resource 108 is the localised 'Run with PowerShell' title");
}

TEST(dll_load, the_windows_fallback_refuses_nonsense)
{
	ScopedModule missing(DLL::LoadSafeWindows(L"no-such-module-here.dll", ResourceOnly));
	CHECK(missing.handle == nullptr);

	ScopedModule empty(DLL::LoadSafeWindows(L"", ResourceOnly));
	CHECK(empty.handle == nullptr);

	ScopedModule null(DLL::LoadSafeWindows(nullptr, ResourceOnly));
	CHECK(null.handle == nullptr);
}

// The hardening this must not undo: a bare name is never resolved through the
// current directory or the rest of the search path.
TEST(dll_load, neither_loader_consults_the_current_directory)
{
	wchar_t previous[MAX_PATH]{};
	::GetCurrentDirectoryW(ARRAYSIZE(previous), previous);

	wchar_t temp[MAX_PATH]{};
	::GetTempPathW(ARRAYSIZE(temp), temp);
	::SetCurrentDirectoryW(temp);

	// A name that exists nowhere on the system must not be found, whatever the
	// current directory happens to be.
	ScopedModule a(DLL::LoadSafe(L"nilesoft-should-not-resolve.dll", ResourceOnly));
	ScopedModule b(DLL::LoadSafeWindows(L"nilesoft-should-not-resolve.dll", ResourceOnly));
	CHECK(a.handle == nullptr);
	CHECK(b.handle == nullptr);

	::SetCurrentDirectoryW(previous);
}
