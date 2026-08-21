#include "test.h"

#include <windows.h>
#include <wincodec.h>
#include <atomic>
#include <thread>
#include <vector>

#include "System.h"

using Nilesoft::Drawing::WIC;

namespace
{
	IWICImagingFactory *create_factory()
	{
		IWICImagingFactory *factory = nullptr;
		auto hr = ::CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
			CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
		if(FAILED(hr))
			::CoCreateInstance(CLSID_WICImagingFactory1, nullptr,
				CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
		return factory;
	}
}

TEST(wic, an_already_pbgra_source_avoids_a_redundant_converter)
{
	auto factory = create_factory();
	CHECK(factory != nullptr);
	if(!factory) return;

	IWICBitmap *bitmap = nullptr;
	CHECK(SUCCEEDED(factory->CreateBitmap(2, 2, GUID_WICPixelFormat32bppPBGRA,
		WICBitmapCacheOnLoad, &bitmap)));
	if(bitmap)
	{
		auto result = WIC::To32bppPBGRA(bitmap);
		CHECK(result == bitmap);
		if(result) result->Release();
	}
	factory->Release();
}

TEST(wic, straight_alpha_bgra_is_converted_to_premultiplied_bgra)
{
	auto factory = create_factory();
	CHECK(factory != nullptr);
	if(!factory) return;

	IWICBitmap *bitmap = nullptr;
	CHECK(SUCCEEDED(factory->CreateBitmap(2, 2, GUID_WICPixelFormat32bppBGRA,
		WICBitmapCacheOnLoad, &bitmap)));
	if(bitmap)
	{
		auto result = WIC::To32bppPBGRA(bitmap);
		CHECK(result != nullptr);
		CHECK(result != bitmap);
		if(result)
		{
			WICPixelFormatGUID format{};
			CHECK(SUCCEEDED(result->GetPixelFormat(&format)));
			CHECK(format == GUID_WICPixelFormat32bppPBGRA);
			result->Release();
		}
	}
	factory->Release();
}

TEST(wic, converted_source_ownership_is_released_exactly_once)
{
	auto factory = create_factory();
	CHECK(factory != nullptr);
	if(!factory) return;

	IWICBitmap *bitmap = nullptr;
	CHECK(SUCCEEDED(factory->CreateBitmap(2, 2, GUID_WICPixelFormat32bppBGRA,
		WICBitmapCacheOnLoad, &bitmap)));
	if(bitmap)
	{
		// Keep one independent reference so the source remains observable after
		// To32bppPBGRA consumes the reference passed to it.
		bitmap->AddRef();
		auto result = WIC::To32bppPBGRA(bitmap);
		CHECK(result != nullptr);
		CHECK(result != bitmap);
		if(result) result->Release();

		// Release is documented to return the new count for test purposes.
		CHECK_EQ(bitmap->Release(), ULONG(0));
	}
	factory->Release();
}

TEST(wic, init_failure_still_releases_the_transferred_source)
{
	// Callers transfer ownership. If CoCreateInstance cannot run because this
	// thread has no apartment, To32bppPBGRA must still Release the bitmap.
	std::atomic<ULONG> last_release{ ULONG(0xFFFFFFFF) };
	std::thread t([&]
		{
			auto hr = ::CoInitializeEx(nullptr,
				COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			if(FAILED(hr))
				return;

			auto factory = create_factory();
			if(!factory)
			{
				::CoUninitialize();
				return;
			}

			IWICBitmap *bitmap = nullptr;
			if(FAILED(factory->CreateBitmap(2, 2, GUID_WICPixelFormat32bppPBGRA,
				WICBitmapCacheOnLoad, &bitmap)) || !bitmap)
			{
				factory->Release();
				::CoUninitialize();
				return;
			}

			bitmap->AddRef();
			factory->Release();
			WIC::release();
			::CoUninitialize();

			auto result = WIC::To32bppPBGRA(bitmap);
			if(result)
				result->Release();
			last_release.store(bitmap->Release(), std::memory_order_relaxed);
		});
	t.join();
	CHECK_EQ(last_release.load(std::memory_order_relaxed), ULONG(0));
}

TEST(wic, apartment_local_factories_survive_concurrent_menu_threads)
{
	std::atomic<int> successes{};
	std::vector<std::thread> threads;
	for(int i = 0; i < 4; ++i)
	{
		threads.emplace_back([&]
			{
				auto hr = ::CoInitializeEx(nullptr,
					COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
				if(SUCCEEDED(hr))
				{
					auto source = ::CreateBitmap(2, 2, 1, 32, nullptr);
					auto converted = WIC::ToBitmap32(source);
					if(converted)
					{
						successes.fetch_add(1, std::memory_order_relaxed);
						::DeleteObject(converted);
					}
					if(source) ::DeleteObject(source);

					// Release every apartment-owned interface before closing COM.
					WIC::release();
					::CoUninitialize();
				}
			});
	}
	for(auto &thread : threads)
		thread.join();

	CHECK_EQ(successes.load(std::memory_order_relaxed), 4);
}

namespace
{
	// S_FALSE still counts as success and still needs CoUninitialize.
	// RPC_E_CHANGED_MODE means do not CoUninitialize.
	// https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex
	// https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-couninitialize
	bool wic_cycles_on_this_thread(DWORD coinit, int cycles)
	{
		int ok = 0;
		for(int i = 0; i < cycles; ++i)
		{
			auto hr = ::CoInitializeEx(nullptr, coinit);
			if(hr == RPC_E_CHANGED_MODE)
				return false;
			if(FAILED(hr))
				return false;

			auto source = ::CreateBitmap(2, 2, 1, 32, nullptr);
			auto converted = WIC::ToBitmap32(source);
			if(converted)
			{
				++ok;
				::DeleteObject(converted);
			}
			if(source)
				::DeleteObject(source);

			WIC::release();
			::CoUninitialize();
		}
		return ok == cycles;
	}
}

TEST(wic, factory_survives_repeated_sta_init_use_release_uninit)
{
	std::atomic<int> ok{};
	std::thread t([&]
		{
			if(wic_cycles_on_this_thread(
				COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE, 4))
				ok.store(1, std::memory_order_relaxed);
		});
	t.join();
	CHECK_EQ(ok.load(std::memory_order_relaxed), 1);
}

TEST(wic, factory_survives_repeated_mta_init_use_release_uninit)
{
	std::atomic<int> ok{};
	std::thread t([&]
		{
			if(wic_cycles_on_this_thread(COINIT_MULTITHREADED, 4))
				ok.store(1, std::memory_order_relaxed);
		});
	t.join();
	CHECK_EQ(ok.load(std::memory_order_relaxed), 1);
}
