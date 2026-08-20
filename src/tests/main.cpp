#include "test.h"

#include <windows.h>
#include <objbase.h>

// Usage: tests.exe [suite-substring]
int main(int argc, char **argv)
{
	// The shell-extension suite drives real shell items and data objects, and a
	// capture holds an agile reference to one. Both need an apartment, and the
	// host thread these stand in for always has one.
	auto hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	auto result = ::nss_test::run(argc > 1 ? argv[1] : nullptr);

	if(SUCCEEDED(hr))
		::CoUninitialize();

	return result;
}