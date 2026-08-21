// OneShotMarshal: CoMarshalInterThreadInterfaceInStream on the source STA,
// CoGetInterfaceAndReleaseStream on the destination STA. Failure leaves no
// raw-pointer fallback.
//
//   https://learn.microsoft.com/windows/win32/com/single-threaded-apartments
//   https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-comarshalinterthreadinterfaceinstream
//   https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-cogetinterfaceandreleasestream

#include "test.h"

#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <thread>

#include "Include/OneShotMarshal.h"

using Nilesoft::Shell::OneShotMarshal;

TEST(oneshot_marshal, consume_of_an_empty_stream_is_empty)
{
	OneShotMarshal m;
	CHECK(m.empty());
	IUnknown *p = reinterpret_cast<IUnknown *>(1);
	CHECK(!m.consume(IID_IUnknown, reinterpret_cast<void **>(&p)));
	CHECK(p == nullptr);
	CHECK(!m.consume(IID_IUnknown, reinterpret_cast<void **>(&p)));
}

TEST(oneshot_marshal, desktop_folder_round_trips_to_a_second_sta)
{
	// The source STA must pump messages while the proxy is used. Joining a
	// worker from the source apartment without a pump deadlocks.
	// https://learn.microsoft.com/windows/win32/com/single-threaded-apartments
	std::atomic<int> ready{};
	std::atomic<int> finished{};
	std::atomic<int> ok{};
	OneShotMarshal m;

	std::thread source([&]
		{
			auto hr = ::CoInitializeEx(nullptr,
				COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			if(FAILED(hr))
			{
				finished.store(1, std::memory_order_relaxed);
				return;
			}

			IShellFolder *desktop = nullptr;
			if(SUCCEEDED(::SHGetDesktopFolder(&desktop)) && desktop
				&& m.assign(desktop, IID_IShellFolder))
			{
				ready.store(1, std::memory_order_release);
				while(!finished.load(std::memory_order_acquire))
				{
					MSG msg{};
					while(::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
					{
						::TranslateMessage(&msg);
						::DispatchMessageW(&msg);
					}
					::Sleep(1);
				}
				desktop->Release();
			}
			else
			{
				finished.store(1, std::memory_order_relaxed);
			}
			::CoUninitialize();
		});

	std::thread dest([&]
		{
			while(!ready.load(std::memory_order_acquire)
				&& !finished.load(std::memory_order_acquire))
				::Sleep(1);

			if(finished.load(std::memory_order_acquire))
				return;

			auto hr = ::CoInitializeEx(nullptr,
				COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			if(FAILED(hr))
			{
				finished.store(1, std::memory_order_relaxed);
				return;
			}

			IShellFolder *proxy = nullptr;
			// Unmarshal success is the contract under test. Method calls on the
			// proxy also require the source STA to pump; Release of the last
			// proxy reference is local to the stub until the count hits zero.
			if(m.consume(IID_IShellFolder, reinterpret_cast<void **>(&proxy)) && proxy)
			{
				ok.store(1, std::memory_order_relaxed);
				proxy->Release();
			}
			::CoUninitialize();
			finished.store(1, std::memory_order_release);
		});

	dest.join();
	source.join();

	CHECK_EQ(ok.load(std::memory_order_relaxed), 1);
	CHECK(m.empty());
}
