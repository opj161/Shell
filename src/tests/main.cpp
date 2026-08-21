#include "test.h"

#include <windows.h>
#include <objbase.h>

#include "System.h"

// Usage: tests.exe [suite-substring]
int main(int argc, char **argv)
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
