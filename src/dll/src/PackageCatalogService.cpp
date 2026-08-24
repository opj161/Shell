#include <pch.h>
#include "Include/PackageCatalogService.h"
#include "Include/Packages.h"
#include "Include/Diagnostics/MenuPerf.h"

#include <objbase.h>

namespace Nilesoft
{
	namespace Shell
	{
		namespace
		{
			const wchar_t PathSeparator = L'\\';

			std::wstring read_text_file(const std::wstring &path)
			{
				HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
					FILE_SHARE_READ, nullptr, OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL, nullptr);
				if(file == INVALID_HANDLE_VALUE)
					return {};

				LARGE_INTEGER size{};
				if(!::GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024)
				{
					::CloseHandle(file);
					return {};
				}

				std::vector<char> raw(static_cast<size_t>(size.QuadPart));
				DWORD read = 0;
				auto ok = ::ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr);
				::CloseHandle(file);
				if(!ok || read == 0)
					return {};

				auto data = reinterpret_cast<const unsigned char *>(raw.data());
				if(read >= 2 && data[0] == 0xFF && data[1] == 0xFE)
				{
					auto chars = (read - 2) / sizeof(wchar_t);
					return std::wstring(reinterpret_cast<const wchar_t *>(data + 2), chars);
				}

				int skip = 0;
				if(read >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
					skip = 3;

				int n = ::MultiByteToWideChar(CP_UTF8, 0, raw.data() + skip,
					static_cast<int>(read) - skip, nullptr, 0);
				if(n <= 0)
					return {};
				std::wstring wide(static_cast<size_t>(n), L'\0');
				::MultiByteToWideChar(CP_UTF8, 0, raw.data() + skip,
					static_cast<int>(read) - skip, wide.data(), n);
				return wide;
			}
		}

		std::vector<ExplorerCommandRegistration> scan_package_catalog()
		{
			std::vector<ExplorerCommandRegistration> out;
			RegistryPackageSource source;
			std::vector<std::wstring> names;
			if(!source.enumerate_full_names(names))
				return out;

			for(const auto &full : names)
			{
				// Documented two-call GetPackagePathByFullName, already wrapped:
				// https://learn.microsoft.com/en-us/windows/win32/api/appmodel/nf-appmodel-getpackagepathbyfullname
				auto root = GetInstalledPackagePath(full);
				if(root.empty())
					continue;
				auto manifest = root;
				if(!manifest.empty() && manifest.back() != PathSeparator && manifest.back() != L'/')
					manifest += PathSeparator;
				manifest += L"AppxManifest.xml";
				auto xml = read_text_file(manifest);
				if(xml.empty())
					continue;
				std::vector<ExplorerCommandRegistration> local;
				parse_file_explorer_context_menus(xml, local);
				for(const auto &reg : local)
				{
					for(const auto &type : reg.types)
						explorer_command_xml::merge(out, reg.clsid, type);
				}
			}
			return out;
		}

		PackageCatalogService &PackageCatalogService::instance()
		{
			// Process lifetime, deliberately never destroyed: the worker thread
			// outlives every caller and the module is pinned anyway. Same
			// reasoning as TaskbarUiaWorker in Main.cpp.
			static PackageCatalogService *service = new PackageCatalogService();
			return *service;
		}

		bool PackageCatalogService::ensure_started()
		{
			if(_started.load(std::memory_order_acquire))
				return true;

			std::lock_guard<std::mutex> lock(_start_mutex);
			if(_started.load(std::memory_order_relaxed))
				return true;

			// Every failure path puts back what it took, so a later attempt
			// tries again instead of leaking a pair of handles each time.
			auto abandon = [this]
			{
				if(_work) { ::CloseHandle(_work); _work = nullptr; }
				if(_published) { ::CloseHandle(_published); _published = nullptr; }
				return false;
			};

			_work = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
			// Manual reset, and never reset again: once a snapshot exists,
			// every later wait on it is meant to return immediately.
			_published = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if(!_work || !_published)
				return abandon();

			auto thread = ::CreateThread(nullptr, 0, &PackageCatalogService::thread_main, this, 0, nullptr);
			if(!thread)
				return abandon();
			::CloseHandle(thread);

			_started.store(true, std::memory_order_release);
			return true;
		}

		void PackageCatalogService::kick()
		{
			if(!ensure_started())
				return;
			::SetEvent(_work);
		}

		void PackageCatalogService::warm_async()
		{
			kick();
		}

		void PackageCatalogService::invalidate()
		{
			_store.invalidate();
			kick();
		}

		std::shared_ptr<const CatalogSnapshot> PackageCatalogService::snapshot()
		{
			auto current = _store.current();

			// Constant time, and it claims nothing: the worker is the only
			// thing that ever holds the in-flight slot, so a menu thread cannot
			// leave the catalog wedged mid-refresh by dying between claiming
			// and scanning. The scan itself never runs here.
			if(_store.needs_refresh(::GetTickCount64()))
				kick();

			return current;
		}

		std::shared_ptr<const CatalogSnapshot> PackageCatalogService::snapshot_for_menu()
		{
			if(auto current = snapshot(); current)
				return current;

			// Nothing has ever been published in this process. Returning empty
			// here would drop every packaged verb from the first menu, silently
			// - the failure mode AGENTS.md records for the taskbar worker,
			// where never blocking "would have broken the first right-click of
			// every sequence".
			//
			// So wait, with a budget, through CoWaitForMultipleHandles: on a
			// single-threaded apartment it "enters the COM modal loop, and the
			// thread's message loop will continue to dispatch messages", which
			// a raw wait on a menu thread must not stop doing.
			// https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cowaitformultiplehandles
			if(!ensure_started() || !_published)
				return {};

			// Timed, because this wait is the entire case for an on-disk
			// catalog. docs/refactor/02-first-paint-latency.md step 3 defers
			// persistence "behind measurement": warm-on-start already removes
			// the stall, and what persistence would buy is whatever is spent
			// here on the first menu of a process. A phase that is almost
			// always absent, and says how long when it is not, is that
			// measurement - and it costs nothing on every menu after the first,
			// because this whole function returns above.
			Diagnostics::MenuPerfScope perf(L"catalog.first_wait");

			DWORD index = 0;
			auto hr = ::CoWaitForMultipleHandles(0, FirstScanBudgetMs, 1, &_published, &index);
			if(FAILED(hr))
			{
				// Budget spent; the scan keeps running and lands for the next
				// menu. Counted separately, because "waited 400 ms and gave up"
				// and "waited 400 ms and got it" are opposite outcomes that a
				// duration alone cannot tell apart.
				perf.annotate(0);
				return {};
			}

			perf.annotate(1);
			return _store.current();
		}

		// run() serves refreshes for the life of the process and does not return,
		// so the result a ThreadProc has to name is unreachable (C4702). Same
		// suppression as TaskbarUiaWorker::thread_main in Main.cpp.
#pragma warning(push)
#pragma warning(disable: 4702)
		DWORD WINAPI PackageCatalogService::thread_main(void *self)
		{
			static_cast<PackageCatalogService *>(self)->run();
			return 0;
		}
#pragma warning(pop)

		void PackageCatalogService::run()
		{
			// Multi-threaded apartment, no windows. The scan is registry and
			// file I/O and creates no COM objects, but this thread must not be
			// mistaken for an STA by anything it calls into.
			//
			// There is no balancing CoUninitialize because there is no path
			// that reaches one: the loop below never exits, the thread runs for
			// the life of the process, and the module is pinned. Same shape as
			// TaskbarUiaWorker::run in Main.cpp.
			// https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex
			(void)::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);

			for(;;)
			{
				::WaitForSingleObject(_work, INFINITE);

				// Loop rather than scan once: an invalidate() that arrives
				// while a scan is running discards its result, and the machine
				// state it described is already gone.
				for(int attempts = 0; attempts < 4; attempts++)
				{
					uint64_t token = 0;
					if(!_store.claim_refresh(::GetTickCount64(), &token))
						break;

					auto scanned = scan_package_catalog();

					if(_store.publish(std::move(scanned), ::GetTickCount64(), token))
						break;
				}

				// Manual reset and never reset again: this releases whoever is
				// waiting for the first catalog, and every later waiter returns
				// immediately. Set even when the scan failed - a waiter should
				// stop waiting for an answer that is not coming, not sit out the
				// whole budget.
				::SetEvent(_published);
			}
		}
	}
}
