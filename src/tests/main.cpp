#include "test.h"

#include <windows.h>
#include <objbase.h>

#include "System.h"

// Usage: tests.exe [suite-substring]
//
// __cdecl is explicit because x86 builds use /Gz (StdCall), matching Shell.vcxproj
// and exe.vcxproj so the prebuilt x86 libraries link. /Gz already excludes main -
// "specifies the __stdcall calling convention for all functions except C++ member
// functions, functions named main, ..." - and the compiler forces __cdecl anyway,
// but it warns (C4007, "must be '__cdecl'") that the attribute was not stated, and
// this project treats warnings as errors.
// https://learn.microsoft.com/cpp/build/reference/gd-gr-gv-gz-calling-convention
// https://learn.microsoft.com/cpp/error-messages/compiler-warnings/compiler-warning-level-2-c4007
int __cdecl main(int argc, char **argv)
{
	// The shell-extension suite drives real shell items and data objects, and a
	// capture marshals one into a stream. Both operations need an apartment, and
	// the host thread these stand in for always has one.
	auto hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	auto result = ::nss_test::run(argc > 1 ? argv[1] : nullptr);

	if(SUCCEEDED(hr))
	{
		// Release Shell-owned WIC references before balancing this process's
		// CoInitializeEx. CoUninitialize closes the COM library on the thread.
		// https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-couninitialize
		Nilesoft::Drawing::WIC::release();
		::CoUninitialize();
	}

	return result;
}
