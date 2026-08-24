#include <pch.h>
#include <unordered_set>
#include "Include/ContextMenu.h"
#include "Include/ShellExt.h"
#include "Include/ComActivationPolicy.h"
#include "Include/HostContract.h"
#include "Include/Diagnostics/DiagnosticsRing.h"
#include "Include/PackageCatalogService.h"
#include "Include/Diagnostics/MenuPerf.h"
#include "Include/TaskbarHitCache.h"
#include "Include/TaskbarHitStats.h"
#include "Include/TakeoverBreaker.h"
#include "Include/TaskbarOrigin.h"
#include "Library/detours.h"
#include "RegistryConfig.h"
#include <UIAutomation.h>
using namespace Nilesoft::Shell;
using namespace Nilesoft::Diagnostics;

// Nilesoft::Diagnostics and Nilesoft::Shell::Diagnostics are both in scope in
// this translation unit, so the phase timers are reached through an alias.
namespace perf = Nilesoft::Shell::Diagnostics;

//MS also offers EV Code Certificates, which enable immediate acceptance by SmartScreeen of Windows Defender
//https://blogs.msdn.microsoft.com/ie/2012/08/14/microsoft-smartscreen-extended-validation-ev-code-signing-certificates/

#ifdef _DEBUG
#include <crtdbg.h>
#endif
/*
Windows 10
Version Build
-------------
1507	10240
1511	10586
1607	14393
1703	15063
1709	16299
1803	17134
1809	17763
1903	18362
1909	18363
2004	19041
20H2	19042
21H1	19043
21H2	19044
22H2	19045
*/

//#pragma comment(lib, "mincore.lib")
//#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "WindowsCodecs.lib")
#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "Shlwapi.lib")

#if defined(_M_ARM64)
#pragma comment(lib, "plutosvg-arm64.lib")
#pragma comment(lib, "detours-arm64.lib")
#elif defined(_M_ARM)
#pragma comment(lib, "plutosvg-arm.lib")
#pragma comment(lib, "detours-arm.lib")
#elif defined(_M_X64)
#pragma comment(lib, "plutosvg-x64.lib")
#pragma comment(lib, "detours-x64.lib")
#elif defined(_M_IX86)
#pragma comment(lib, "plutosvg-x86.lib")
#pragma comment(lib, "detours-x86.lib")
#endif

//#pragma optimize("ts", on) 

/*
#if defined(_M_X64)
#pragma comment(linker, "/export:DllCanUnloadNow=_DllCanUnloadNow")
#else
#pragma comment(linker, "/export:DllCanUnloadNow=__DllCanUnloadNow@0")
#endif
*/

#ifdef _DEBUG

struct MsgMapEntry { uint32_t msg; const wchar_t *name; };
static const MsgMapEntry msg_map0[] = {
	{ WM_DESTROY, L"WM_DESTROY" },
	{ WM_NCDESTROY, L"WM_NCDESTROY" },
	{ WM_CREATE, L"WM_CREATE" },
	{ WM_NCCREATE, L"WM_NCCREATE" },
	{ WM_PAINT, L"WM_PAINT" },
	{ WM_NCPAINT, L"WM_NCPAINT" },
	{ WM_PRINTCLIENT, L"WM_PRINTCLIENT" },
	{ WM_PRINT, L"WM_PRINT" },
	{ WM_NCCALCSIZE, L"WM_NCCALCSIZE" },
	{ WM_ERASEBKGND, L"WM_ERASEBKGND" },
	{ MN_SELECTITEM, L"MN_SELECTITEM" },
	{ WM_KEYDOWN, L"WM_KEYDOWN" },
	{ WM_WINDOWPOSCHANGED, L"WM_WINDOWPOSCHANGED" },
	{ WM_WINDOWPOSCHANGING, L"WM_WINDOWPOSCHANGING" },
	{ WM_NCHITTEST, L"WM_NCHITTEST" },
	{ WM_TIMER, L"WM_TIMER" },
	{ WM_SIZE, L"WM_SIZE" },
	{ WM_MOVE, L"WM_MOVE" },
	{ MN_SIZEWINDOW, L"MN_SIZEWINDOW" },
	{ MN_DBLCLK, L"MN_DBLCLK" },
	{ MN_CLOSEHIERARCHY, L"MN_CLOSEHIERARCHY" },
	{ MN_SETTIMERTOOPENHIERARCHY, L"MN_SETTIMERTOOPENHIERARCHY" },
	{ MN_FINDMENUWINDOWFROMPOINT, L"MN_FINDMENUWINDOWFROMPOINT" },
	{ MN_ENDMENU, L"MN_ENDMENU" },
	{ MN_CANCELMENUS, L"MN_CANCELMENUS" },
	{ MN_BUTTONDOWN, L"MN_BUTTONDOWN" },
	{ MN_BUTTONUP, L"MN_BUTTONUP" },
	{ WM_UAHDESTROYWINDOW, L"WM_UAHDESTROYWINDOW" },
	{ WM_UAHMEASUREMENUITEM, L"WM_UAHMEASUREMENUITEM" },
	{ WM_UAHINITMENUPOPUP, L"WM_UAHINITMENUPOPUP" },
	{ WM_UAHNCPAINTMENUPOPUP, L"WM_UAHNCPAINTMENUPOPUP" },
	{ WM_UAHDRAWMENU, L"WM_UAHDRAWMENU" },
	{ WM_NCUAHDRAWFRAME, L"WM_NCUAHDRAWFRAME" },
	{ WM_UAHDRAWMENUITEM, L"WM_UAHDRAWMENUITEM" },
	{ WM_ENTERMENULOOP, L"WM_ENTERMENULOOP" },
	{ WM_EXITMENULOOP, L"WM_EXITMENULOOP" },
	{ WM_INITMENU, L"WM_INITMENU" },
	{ WM_INITMENUPOPUP, L"WM_INITMENUPOPUP" },
	{ WM_UNINITMENUPOPUP, L"WM_UNINITMENUPOPUP" },
	{ WM_MENUSELECT, L"WM_MENUSELECT" },
	{ WM_NEXTMENU, L"WM_NEXTMENU" },
	{ WM_MENUCHAR, L"WM_MENUCHAR" },
	{ WM_MEASUREITEM, L"WM_MEASUREITEM" },
	{ WM_DRAWITEM, L"WM_DRAWITEM" },
	{ WM_MENUCOMMAND, L"WM_MENUCOMMAND" },
	{ WM_COMMAND, L"WM_COMMAND" },
	{ WM_SYSCOMMAND, L"WM_SYSCOMMAND" },
	{ WM_MENURBUTTONUP, L"WM_MENURBUTTONUP" },
	{ WM_CAPTURECHANGED, L"WM_CAPTURECHANGED" },
	{ WM_ENTERIDLE, L"WM_ENTERIDLE" },
	{ WM_SETCURSOR, L"WM_SETCURSOR" },
	{ WM_SHOWWINDOW , L"WM_SHOWWINDOW" },
	{ WM_PARENTNOTIFY, L"WM_PARENTNOTIFY"},
	{ WM_MOUSEACTIVATE, L"WM_MOUSEACTIVATE"},
	{ WM_CONTEXTMENU, L"WM_CONTEXTMENU"},
	{ WM_NOTIFY, L"WM_NOTIFY"},
	{ WM_SETFOCUS, L"WM_SETFOCUS"},
	{ WM_KILLFOCUS, L"WM_KILLFOCUS"},
	{ WM_GETOBJECT, L"WM_GETOBJECT"},
	{ WM_ACTIVATE, L"WM_ACTIVATE"},
	{ WM_NCACTIVATE, L"WM_NCACTIVATE"},
	{ WM_ACTIVATEAPP, L"WM_ACTIVATEAPP"},
	{ WM_CHANGEUISTATE, L"WM_CHANGEUISTATE"},
	{ WM_IME_SETCONTEXT, L"WM_IME_SETCONTEXT"},
	{ WM_IME_NOTIFY, L"WM_IME_NOTIFY"},
	{ WM_MOUSEMOVE, L"WM_MOUSEMOVE"},
	{ WM_DEVICECHANGE, L"WM_DEVICECHANGE"},
	{ WM_IME_REQUEST, L"WM_IME_REQUEST"},
	{ MN_GETHMENU, L"MN_GETHMENU"},
	{ WM_LBUTTONDOWN, L"WM_LBUTTONDOWN"},
	{ WM_RBUTTONDOWN, L"WM_RBUTTONDOWN"},
	{ WM_LBUTTONUP, L"WM_LBUTTONUP"},
	{ WM_RBUTTONUP, L"WM_RBUTTONUP"},
	{ WM_MOUSELEAVE,L"WM_MOUSELEAVE"},
	{ WM_USER, L"WM_USER"},
	{ WM_APP, L"WM_APP"},
	{ 0, nullptr }
};

#endif

#pragma region Entry Point

#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))  // windowsx.h
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))  // windowsx.h

HINSTANCE _hInstance = nullptr;
HANDLE watch_event = nullptr;
uint32_t MN_CONTEXTMENU = 0;

struct LoaderState
{
	string path;
	bool explorer{};
	HMODULE handle{};

	bool init()
	{
		handle = ::GetModuleHandleW(nullptr);
		if(!explorer && path.empty())
		{
			path = Path::Module(nullptr);
			explorer = path.ends_with(def_EXPLORER, true);
		}
		return explorer;
	}

	explicit operator bool() const { return explorer; }
};

struct RuntimeState
{
	HINSTANCE module{};
	Initializer initializer;
	LoaderState loader;

	WindowsHook taskbar_mouse;
	IATHook ntuser_popup_hook;
	std::vector<IATHook> popup_hooks;
	Detours<decltype(::CoCreateInstance)> co_create_instance_hook;

	std::unordered_map<HWND, Window> taskbar_windows;
	std::mutex taskbar_mutex;

	std::atomic<bool> hooks_installed{ false };
	std::atomic<bool> module_pinned{ false };
};

inline RuntimeState& Runtime()
{
	// Intentionally process-lifetime. Do not register a C++ destructor.
	static RuntimeState* state = new RuntimeState();
	return *state;
}

#define _log Logger::Instance()
inline const Windows::Version* Ver() { return &Windows::Version::Instance(); }
#define ver Ver()
#define _initializer (Runtime().initializer)
#define _loader (Runtime().loader)
#define _taskbar_mouse (Runtime().taskbar_mouse)
#define iathook_NtUserTrackPopupMenuEx (Runtime().ntuser_popup_hook)
#define iathook_TrackPopupMenu (Runtime().popup_hooks)
#define _CoCreateInstance (Runtime().co_create_instance_hook)

void BootstrapOnce();

inline bool PinModule()
{
	auto &rt = Runtime();
	if(rt.module_pinned.load(std::memory_order_relaxed))
		return true;

	HMODULE pinned = nullptr;
	if(!::GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCWSTR>(&BootstrapOnce),
			&pinned))
	{
		return false;
	}
	rt.module_pinned.store(true, std::memory_order_release);
	return true;
}

/*
	Taskbar hit-testing worker.

	Deciding whether a right-click landed on empty taskbar background needs UI
	Automation: on Windows 11 the taskbar is a single HWND hosting XAML, so there
	are no child windows to hit-test. But Shell runs inside explorer.exe, and
	Microsoft is explicit that a client which inspects its own UI from the UI
	thread can see "very slow performance, or even cause the application to stop
	responding", and that those calls belong on a separate thread which owns no
	windows and is a COM multithreaded apartment:

		https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading

	So one process-lifetime MTA worker owns the IUIAutomation and every element
	it produces; only a bool ever crosses back. That also satisfies the rule that
	interface pointers must be marshaled between apartments - nothing to marshal
	if nothing crosses:

		https://learn.microsoft.com/en-us/windows/win32/com/single-threaded-apartments

	The taskbar thread still has to have an answer before it decides whether to
	show Shell's menu, so it waits - but with a budget, and through
	CoWaitForMultipleHandles, which on a single-threaded apartment "enters the
	COM modal loop, and the thread's message loop will continue to dispatch
	messages". A raw wait there would deadlock the very thread the worker's UIA
	call needs to talk to.

		https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cowaitformultiplehandles

	If the budget runs out, the click falls through to Windows' own taskbar
	handling instead of freezing Explorer, and the answer lands in the cache for
	next time. Measured here: ~28 ms for the first query in a process (loading
	UIAutomationCore and connecting), ~2-3 ms after that, and a cache hit for
	every repeat click in the same region.
*/
class TaskbarUiaWorker
{
public:
	// Budget for one answer. Long enough that the measured ~3 ms query is never
	// cut short, short enough that a wedged provider costs a dropped menu rather
	// than a hung taskbar.
	static constexpr DWORD BUDGET_MS = 250;

	static TaskbarUiaWorker &instance()
	{
		// Process-lifetime, like the rest of RuntimeState. No C++ destructor.
		static TaskbarUiaWorker *worker = new TaskbarUiaWorker();
		return *worker;
	}

	Nilesoft::Shell::TaskbarHitCache &cache() { return _cache; }
	Nilesoft::Shell::TaskbarHitStats &stats() { return _stats; }

	// Called on the taskbar's UI thread. Returns the cached answer when there is
	// one, otherwise hands the point to the worker and waits out the budget.
	bool query(HWND taskbar, POINT pt, bool secondary)
	{
		using Outcome = Nilesoft::Shell::TaskbarHitStats::Outcome;

		auto now = ::GetTickCount64();
		if(auto hit = _cache.lookup(taskbar, pt, now); hit)
		{
			_stats.record(Outcome::CacheHit, 0);
			return *hit;
		}

		if(!ensure_started())
		{
			_stats.record(Outcome::Unavailable, 0);
			return false;
		}

		// One question at a time. Taskbars on several monitors can be serviced by
		// different threads, and without this the second caller would overwrite
		// the first one's request, both would wake on the same answer, and each
		// would cache the other's result against its own point.
		std::lock_guard<std::mutex> caller(_caller_mutex);

		// A caller that gave up leaves the worker still running; the sequence
		// number is what stops its late answer being read as this one's.
		uint64_t ticket = 0;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			ticket = ++_sequence;
			_request = { taskbar, pt, secondary, ticket };
			_answer = false;
			_answered_ticket = 0;
		}

		::ResetEvent(_done);
		::SetEvent(_work);

		// The wait is the thing docs/refactor/02 section 5 asks to be counted,
		// so it is timed whether or not anybody has turned logging on.
		LARGE_INTEGER wait_start{};
		::QueryPerformanceCounter(&wait_start);

		DWORD index = 0;
		::SetLastError(ERROR_SUCCESS);
		auto hr = ::CoWaitForMultipleHandles(0, BUDGET_MS, 1, &_done, &index);

		auto waited_us = elapsed_microseconds(wait_start);

		bool answer = false;
		bool answered = false;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			answered = (_answered_ticket == ticket);
			answer = _answer;
		}

		if(FAILED(hr) || !answered)
		{
			// Timed out, or the wait could not run, or what is on the slot
			// belongs to an earlier request. Let Windows handle this click; the
			// worker still publishes its answer to the cache for next time.
			//
			// This is the outcome that costs the user a menu, and until it was
			// counted there was no way to know it had ever happened.
			_stats.record(Outcome::TimedOut, waited_us);
			return false;
		}

		_stats.record(Outcome::Answered, waited_us);
		_cache.store(taskbar, pt, answer, ::GetTickCount64());
		return answer;
	}

private:
	struct Request
	{
		HWND taskbar{};
		POINT pt{};
		bool secondary{};
		uint64_t ticket{};
	};

	TaskbarUiaWorker() = default;

	static uint32_t elapsed_microseconds(const LARGE_INTEGER &start) noexcept
	{
		auto per_ms = perf::MenuPerf::ticks_per_ms();
		if(per_ms <= 0.0)
			return 0;

		LARGE_INTEGER now{};
		::QueryPerformanceCounter(&now);
		auto us = static_cast<double>(now.QuadPart - start.QuadPart) / per_ms * 1000.0;
		if(us < 0.0)
			return 0;
		return static_cast<uint32_t>(us);
	}

	bool ensure_started()
	{
		if(_started.load(std::memory_order_acquire))
			return true;

		std::lock_guard<std::mutex> lock(_start_mutex);
		if(_started.load(std::memory_order_relaxed))
			return true;

		// Anything that fails from here has to put back what it took. _started
		// stays false on every failure path, so a later menu tries again - and
		// leaked a fresh pair of event handles each time it did.
		auto abandon = [this]
		{
			if(_work) { ::CloseHandle(_work); _work = nullptr; }
			if(_done) { ::CloseHandle(_done); _done = nullptr; }
			return false;
		};

		_work = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
		_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if(!_work || !_done)
			return abandon();

		// The module is already pinned for process lifetime, so this thread
		// outliving any caller is safe.
		auto thread = ::CreateThread(nullptr, 0, &TaskbarUiaWorker::thread_main, this, 0, nullptr);
		if(!thread)
			return abandon();
		::CloseHandle(thread);

		_started.store(true, std::memory_order_release);
		return true;
	}

	// run() serves requests for the life of the process and does not return, so
	// the result a ThreadProc has to name is unreachable (C4702).
#pragma warning(push)
#pragma warning(disable: 4702)
	static DWORD WINAPI thread_main(void *param)
	{
		static_cast<TaskbarUiaWorker *>(param)->run();
		return 0;
	}
#pragma warning(pop)

	void run()
	{
		// MTA, and this thread deliberately creates no windows. If either step
		// fails the loop still runs: a worker that exits here would leave every
		// caller waiting out the full budget for an answer nobody will ever
		// produce, for the life of the process.
		auto com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

		IComPtr<IUIAutomation> uia;
		if(SUCCEEDED(com))
			uia.CreateInstance(__uuidof(CUIAutomation), CLSCTX_INPROC_SERVER);

		// Built once, used for every question. Asking for the three properties
		// up front turns four marshalled calls to the provider into one; see
		// evaluate() for the measurement and for why that is worth doing.
		IComPtr<IUIAutomationCacheRequest> properties;
		if(uia)
			build_property_request(uia, properties);

		for(;;)
		{
			::WaitForSingleObject(_work, INFINITE);

			Request request;
			{
				std::lock_guard<std::mutex> lock(_mutex);
				request = _request;
			}

			auto allowed = uia ? evaluate(uia, properties, request) : false;

			{
				std::lock_guard<std::mutex> lock(_mutex);
				_answer = allowed;
				_answered_ticket = request.ticket;
			}

			// Published even if the caller has already given up, so the next
			// click in this region is a cache hit.
			_cache.store(request.taskbar, request.pt, allowed, ::GetTickCount64());
			::SetEvent(_done);
		}
	}

	/*
		The three properties the verdict depends on, asked for in advance.

		A cache request names the properties an element should carry when it
		comes back, so the element and its properties arrive in one call instead
		of one call for the element and one per property. AutomationElementMode_None
		is what the docs call for when only cached data will be read - the
		element "has no reference to the underlying UI and contains only cached
		information", which is exactly this case and is cheaper to produce:

			https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomation-elementfrompointbuildcache
			https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/ne-uiautomationclient-automationelementmode
	*/
	static void build_property_request(IComPtr<IUIAutomation> &uia,
									   IComPtr<IUIAutomationCacheRequest> &request)
	{
		if(FAILED(uia->CreateCacheRequest(request)) || !request)
			return;

		request->AddProperty(UIA_AutomationIdPropertyId);
		request->AddProperty(UIA_ClassNamePropertyId);
		request->AddProperty(UIA_NamePropertyId);
		request->put_TreeScope(TreeScope_Element);
		request->put_AutomationElementMode(AutomationElementMode_None);
	}

	/*
		Everything UI Automation touches stays inside this function, on this
		thread. No element pointer is stored or returned.

		Measured on this machine (Windows 11 26200.8875 x64, 2026-08-24) over a
		1440-point grid covering the whole taskbar, out of process:

			ElementFromPoint + three get_Current* reads   p50 2.78 ms, p95 4.28 ms
			ElementFromPointBuildCache + cached reads     p50 2.25 ms, p95 3.25 ms

		Half a millisecond is not why this is written the way it is - the win is
		that a wedged provider now has one call to hang in rather than four,
		which is what the 250 ms budget above exists to survive. The two forms
		were checked to agree on the verdict at all 1440 points, and neither
		returns a NULL BSTR for the TaskbarFrame's empty name, so the guard
		below keeps its meaning.
	*/
	static bool evaluate(IComPtr<IUIAutomation> &uia,
						 IComPtr<IUIAutomationCacheRequest> &properties,
						 const Request &request)
	{
		IComPtr<IUIAutomationElement> element;
		if(properties)
		{
			if(FAILED(uia->ElementFromPointBuildCache(request.pt, properties, element)) || !element)
				return false;
		}
		else if(FAILED(uia->ElementFromPoint(request.pt, element)) || !element)
		{
			return false;
		}

		struct elem_t
		{
			BSTR type = nullptr;
			BSTR name = nullptr;
			BSTR id = nullptr;
			~elem_t()
			{
				freeString(type);
				freeString(name);
				freeString(id);
			}
			void freeString(BSTR s)
			{
				if(s) DLL::Invoke(L"OleAut32.dll", "SysFreeString", s);
			}
		} elem;

		/*
		id=[TaskbarFrame], type=[Taskbar.TaskbarFrameAutomationPeer], name=[]
		id=[StartButton], type=[ToggleButton], name=[Start]
		id=[SearchButton], type=[ToggleButton], name=[Search]
		id=[SystemTrayIcon], type=[SystemTray.NormalButton], name=[Show Hidden Icons]
		id=[SystemTrayIcon], type=[SystemTray.OmniButton], name=[Clock 7:04 PM 3/20/2023]
		id=[SystemTrayIcon], type=[SystemTray.ShowDesktopButton], name=[Show Desktop]
		*/
		// Cached reads when the element was built with the request, live ones
		// when it was not - AutomationElementMode_None means a cached element
		// has nothing to read a Current property from.
		if(properties)
		{
			element->get_CachedAutomationId(&elem.id);
			element->get_CachedClassName(&elem.type);
			element->get_CachedName(&elem.name);
		}
		else
		{
			element->get_CurrentAutomationId(&elem.id);
			element->get_CurrentClassName(&elem.type);
			element->get_CurrentName(&elem.name);
		}

		if(!elem.name || !elem.type || !elem.id)
			return false;

		// string::Equals is ordinal; lstrcmpiW, which this used, case-folds with
		// the user's locale. These are UI Automation identifiers and class names,
		// not text - matching them linguistically "yields unexpected results in
		// non-English locales", and the taskbar failing to be recognised there
		// means the menu simply does not appear.
		auto same = [](const wchar_t *a, const wchar_t *b)
		{
			return string::Equals(a, b, true);
		};

		if(same(elem.id, L"TaskbarFrame"))
			return same(elem.name, L"") && same(elem.type, L"Taskbar.TaskbarFrameAutomationPeer");

		if(request.secondary && same(elem.id, L"SystemTrayIcon"))
			return same(elem.type, L"SystemTray.OmniButton");

		return false;
	}

	Nilesoft::Shell::TaskbarHitCache _cache;
	Nilesoft::Shell::TaskbarHitStats _stats;

	std::mutex _start_mutex;
	std::atomic<bool> _started{ false };

	HANDLE _work{};
	HANDLE _done{};

	// Serialises callers; _mutex only guards the request/answer slot.
	std::mutex _caller_mutex;

	std::mutex _mutex;
	Request _request;
	uint64_t _sequence{};
	uint64_t _answered_ticket{};
	bool _answer{};
};

/*
	The takeover circuit breaker, one per process.

	Process lifetime and no destructor, like every other service here: the
	module is pinned for the life of the host, and a static whose destructor
	could run while a menu thread is still recording would be a crash waiting
	for an unlucky shutdown.
*/
inline Nilesoft::Shell::TakeoverBreaker &takeover_breaker()
{
	static auto *breaker = new Nilesoft::Shell::TakeoverBreaker();
	return *breaker;
}

/*
	Says, once, that the breaker has opened.

	Its own function rather than a line in the hook's __finally, for the reason
	AGENTS.md records: Logger::write is a variadic template whose
	string::Argument temporaries require unwinding, and MSVC refuses (C2712) to
	compile a function that mixes those with __try/__finally. Keeping the
	temporaries in this frame keeps the hook a plain-old-data function - the
	same shape as menu_perf_begin/menu_perf_end.
*/
void log_breaker_opened() noexcept
{
	_log.write(L"takeover failed %u times in a row; this process will use the host's own menus from now on\n",
			   Nilesoft::Shell::TakeoverBreaker::THRESHOLD);
}

LRESULT __stdcall TaskbarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT __stdcall TaskbarProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

__inline auto is_registered(bool force_refresh = false) -> bool
{
	return RegistryConfig::IsRegisteredCached(force_refresh);
}

struct taskbar_t
{
	static UINT MsgShellTrayWnd()
	{
		static UINT msg = ::RegisterWindowMessageW(Windows::WC_Shell_TrayWnd);
		return msg;
	}
	static UINT MsgShellSecondaryTrayWnd()
	{
		static UINT msg = ::RegisterWindowMessageW(Windows::WC_Shell_SecondaryTrayWnd);
		return msg;
	}

	static bool hook(HWND hTaskbar, UINT_PTR id)
	{
		Window window = Window::Find(Windows::WC_Composition_DesktopWindowContentBridge, hTaskbar);
		if(window && !window.is_prop(UxSubclass))
		{
			auto &rt = Runtime();
			std::lock_guard<std::mutex> lock(rt.taskbar_mutex);
			if(!rt.taskbar_windows.contains(window))
			{
				if(window.subclass(TaskbarSubclassProc, id, hTaskbar))
				{
					window.prop(UxSubclass, CONTEXTMENUSUBCLASS);
					rt.taskbar_windows[window] = window;
					return true;
				}
			}
		}
		return false;
	}

	static void try_hook(HWND hTaskbar, UINT_PTR id, int time = 500)
	{
		if(hTaskbar)
		{
			::SetTimer(hTaskbar, id, time, [](HWND hWnd, UINT, UINT_PTR id, DWORD)
			{
				static int atttempt = 0;
				if(hook(hWnd, id))
					atttempt = 50;
				if(atttempt++ >= 50)
					::KillTimer(hWnd, id);
			});
		}
	}

	static void hook_all()
	{
		::EnumWindows([](HWND hWnd, LPARAM)->BOOL {
			auto atom = Window::Atom(hWnd);
			if(atom == MsgShellTrayWnd() || atom == MsgShellSecondaryTrayWnd())
			{
				Window window = hWnd;
				if(!window.is_prop(UxSubclass))
				{
					auto &rt = Runtime();
					std::lock_guard<std::mutex> lock(rt.taskbar_mutex);
					if(!rt.taskbar_windows.contains(window))
					{
						auto proc = window.get_long(GWLP_WNDPROC);
						window.prop(UxSubclass, proc);
						window.set_long(GWLP_WNDPROC, TaskbarProc);
						rt.taskbar_windows[window] = window;
						return true;
					}
				}
			}
			return TRUE;
		}, 0);
	}

	static void hook_all(uint32_t delay)
	{
		::SetTimer(::GetShellWindow(), 0, delay, [](HWND hWnd, UINT, UINT_PTR id, DWORD)
		{						
			static int atttempt = 0;
			if(atttempt++ >= 10)
				::KillTimer(hWnd, id);
			
			taskbar_t::hook_all();
		});
	}
	
	static void unhook(HWND hWnd)
	{
		auto &rt = Runtime();
		std::lock_guard<std::mutex> lock(rt.taskbar_mutex);
		for(auto &window : rt.taskbar_windows)
		{
			if(window.first == hWnd)
			{
				window.second.set_long(GWLP_WNDPROC, window.second.prop(UxSubclass));
				window.second.remove_prop(UxSubclass);
				rt.taskbar_windows.erase(hWnd);
				break;
			}
		}
	}
	static void unhook_all()
	{
		auto &rt = Runtime();
		std::lock_guard<std::mutex> lock(rt.taskbar_mutex);
		for(auto &window : rt.taskbar_windows)
		{
			window.second.set_long(GWLP_WNDPROC, window.second.prop(UxSubclass));
			window.second.remove_prop(UxSubclass);
			rt.taskbar_windows.erase(window.first);
			break;
		}
		rt.taskbar_windows.clear();
	}

	// No UI Automation call is made on this thread; see TaskbarUiaWorker.
	static bool is_allowed_area(HWND hTaskbar, const Point &pt)
	{
		perf::MenuPerfScope timing(L"taskbar.hit_test");
		return TaskbarUiaWorker::instance().query(hTaskbar, { pt.x, pt.y }, is_secondary(hTaskbar));
	}

	// The cached answers describe a layout that no longer exists.
	static void invalidate_hit_cache()
	{
		TaskbarUiaWorker::instance().cache().invalidate();
	}

	static bool is_primary(HWND hTaskbar)
	{
		if(hTaskbar)
		{
			Window taskbar = hTaskbar;
			if(taskbar.hash() == WindowClass::WC_Shell_TrayWnd)
			{
				// Only the primary taskbar has a RebarWindow32
				return taskbar.find(Windows::WC_ReBarWindow32);
			}
		}
		return false;
	}

	static bool is_secondary(HWND hTaskbar)
	{
		return hTaskbar && (Window::class_hash(hTaskbar) == WindowClass::WC_Shell_SecondaryTrayWnd);
	}
};

HRESULT __stdcall CoCreateInstanceHook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID *ppv)
{
	auto is_local_server = (dwClsContext & CLSCTX_LOCAL_SERVER) != 0;
	auto is_inproc_server = (dwClsContext & CLSCTX_INPROC_SERVER) != 0;

	auto cache = _initializer.get_cache();
	if(cache && (is_inproc_server || is_local_server))
	{
		auto hr = E_NOINTERFACE; //CLASS_E_CLASSNOTAVAILABLE;
		auto process = true;

		if(rclsid == IID_FileExplorerContextMenu /*&& riid == {706461D1-AC5F-4730-BFE3-CAC6CAD5EF5E}*/)
		{
			/*
				Two mechanisms suppress the Windows 11 modern menu, and only one
				of them is a setting. Measured on this machine 2026-08-24,
				Windows 11 26200.8875 x64, by raising the desktop menu and
				looking at which window class appeared:

					TreatAs   priority   menu
					ours      1          classic (Shell)
					ours      0          classic (Shell)   <- the setting is inert
					absent    0          modern (Microsoft.UI.Content...)
					absent    1          classic (Shell)

				So `priority` does exactly what it says when there is no
				redirect, and nothing at all when there is one: COM substitutes
				Shell for the modern menu class, Shell's object does not
				implement the interface the modern menu wants, and Explorer
				falls back to the classic menu it was going to get anyway.

				That cannot be fixed from here, and it is worth being explicit
				about why rather than leaving the next reader to re-derive it.
				A per-call opt-out of TreatAs does not exist; refusing the
				redirected activation lands on the classic menu too, because
				COM does not fall back to the original class when a TreatAs
				substitute fails. Restoring the modern menu means removing the
				redirect - `shell.exe -unregister -treat` - which is machine-wide
				HKLM state and an elevated act, not something a config file read
				by every host process gets to do.

				What *is* fixable is the waste and the silence. When the
				redirect is ours the answer is already decided, so this stops
				building a Context and evaluating an expression on every
				activation to reach it - docs/refactor/01-takeover-contract.md
				section 9's "router de-dup". And `shell.exe -check` now says so,
				which is where somebody who has just written priority = 0 will
				find out that it did nothing.
			*/
			if(RegistryConfig::ModernMenuRedirectedToUsCached())
				return hr;

			if(cache->settings.priority)
			{
				Context context;
				Object obj = context.Eval(cache->settings.priority).move();
				if(obj.is_number())
					process = obj;
			}

			if(process)
				return hr;
		}
		else
		{
			/*auto xx = Guid(L"{f3d06e7c-1e45-4a26-847e-f9fcdee59be0}");

			if(xx.equals({riid,rclsid}))
			{
				MB(L"0000000000000");
			}*/

			//if(riid== IID_IExplorerCommandProvider)
			//	_log.info(L"IID_IExplorerCommandProvider %s", Guid(rclsid).to_string(2).c_str());

			//if(riid == IID_IExecuteCommand)
			//	_log.info(L"IID_IExecuteCommand %s", Guid(rclsid).to_string(2).c_str());
			/*
			if(riid == IID_IExplorerCommandState)
			{
				hr = _CoCreateInstance.invoke(rclsid, pUnkOuter, dwClsContext, riid, ppv);

				auto cc = (IExplorerCommand *)*ppv;

				IExplorerCommand *f = 0;
				cc->QueryInterface(IID_IExplorerCommand, (void**)&f);
				//printto;rotate90;rotate270;setwallpaper;slideshow

				LPWSTR p;
				f->GetTitle(0, &p);
				_log.info(L"IExplorerCommandState %s", p);
				return hr;
			}
		*/
			auto is_UWP = false;
			process = false;
			/*
			if(Guid(rclsid).equals(L"{E6950302-61F0-4FEB-97DB-855E30D4A991}"))
			{
				*ppv = new ExplorerCommandBase;
				return S_OK;
			}
			*/
			if(is_inproc_server)
				process = riid == IID_IContextMenu || riid == IID_IContextMenu2 || riid == IID_IContextMenu3 || riid == IID_IExplorerCommand;
			else
			{
				process = riid == IID_IExplorerCommand;
				is_UWP = is_local_server && process;
			}

			if(process)
			{
				auto timing = Keyboard::IsKeyDown(VK_MENU);

				/*
					The fast path. docs/refactor/01-takeover-contract.md
					section 9: "if(!policy->may_affect(rclsid)) return
					original(...)".

					This detour sees every in-process and local-server
					activation in the host. Everything below it - a Context, a
					stringified CLSID, a walk of the rule list evaluating
					`where` expressions - used to run for each one whose IID was
					one of the four, on a machine whose configuration names no
					CLSID at all. Sixteen bytes copied and compared against a
					list that is usually empty replaces all of it.

					The timing probe is deliberately *not* gated by this. It
					exists to time every activation and find the slow one, so
					narrowing it to the CLSIDs a blocklist already names would
					destroy the diagnostic while appearing to optimise it.
				*/
				ClsidKey key{};
				::memcpy(&key, &rclsid, sizeof(GUID));

				auto policy = current_com_policy();
				if(!timing && (!policy || !policy->may_affect(key)))
					return _CoCreateInstance.invoke(rclsid, pUnkOuter, dwClsContext, riid, ppv);

				Context context;
				this_item _this; context._this = &_this;
				_this.is_uwp = is_UWP;
				_this.clsid = Guid(rclsid).to_string(2);

				// Policy first, diagnostics second - see ComActivationPolicy.h.
				// The Alt-held timing probe used to sit in front of this loop and
				// return from it, so the blocklist did nothing while Alt was down.
				auto blocked = false;
				for(auto si : cache->statics)
				{
					if(si->clsid.empty() || (si->where && !context.eval_bool(si->where)))
						continue;

					if(si->has_clsid)
					{
						for(auto &id : si->clsid)
						{
							if(id.equals(rclsid))
							{
								blocked = true;
								break;
							}
						}
					}

					if(blocked)
						break;
				}

				switch(decide_com_activation(true, blocked, timing))
				{
					case ComActivationVerdict::Block:
						// Nothing was activated, so there is nothing to release.
						// Set the out parameter the way a failing CoCreateInstance
						// would rather than leaving the caller's uninitialised
						// pointer in place - the old code read *ppv here, which it
						// had no right to do on a path that never called through.
						// https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cocreateinstance
						if(ppv)
							*ppv = nullptr;
						return E_NOINTERFACE;

					case ComActivationVerdict::TimeAndReturn:
					{
						Timer t;
						t.start();
						hr = _CoCreateInstance.invoke(rclsid, pUnkOuter, dwClsContext, riid, ppv);
						t.stop();
						auto elapsed = (int)t.elapsed_milliseconds();
						_log.write(L"%d%s\t%s\t%s\r\n", elapsed, elapsed > 0 ? L"ms" : L"" , _this.clsid.c_str(), is_UWP ? L"UWP":L"");
						return hr;
					}

					case ComActivationVerdict::PassThrough:
						break;
				}
			}
		}
	}

	return _CoCreateInstance.invoke(rclsid, pUnkOuter, dwClsContext, riid, ppv);
}

#define TPM_SYSMENU	0x0200L

// Identifies the host process in a diagnostics record without keeping its name.
// Computed once - the module never moves - because this runs on the path
// between a right-click and the first pixel.
static uint32_t host_identity_hash()
{
	static const uint32_t hash = []
	{
		wchar_t path[MAX_PATH]{};
		auto length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
		if(length == 0 || length >= MAX_PATH)
			return 0u;

		// FNV-1a over the lowercased path. Not a security boundary - it exists
		// so a report can say "these forty menus were all the same host"
		// without carrying the path around.
		uint32_t h = 2166136261u;
		for(DWORD i = 0; i < length; i++)
		{
			auto c = path[i];
			if(c >= L'A' && c <= L'Z')
				c = static_cast<wchar_t>(c - L'A' + L'a');
			h ^= static_cast<uint32_t>(c & 0xFF);
			h *= 16777619u;
			h ^= static_cast<uint32_t>((c >> 8) & 0xFF);
			h *= 16777619u;
		}
		return h;
	}();
	return hash;
}


BOOL WINAPI TrackPopupMenuProc(HMENU hMenu, uint32_t uFlags, int x, int y, int nReserved, HWND hWnd, const RECT *prcRect);
BOOL WINAPI TrackPopupMenuExProc(HMENU hMenu, uint32_t uFlags, int x, int y, HWND hWnd, LPTPMPARAMS lptpm);

BOOL WINAPI NtUserTrackPopupMenu(HMENU hMenu, uint32_t uFlags, int x, int y, HWND hWnd, LPTPMPARAMS lptpm, int tfunc)
{
#ifdef _DIJA
	return (BOOL)XXX::OnContextMenu(hWnd, x, y, hMenu);;
#endif

	// COINIT_DISABLE_OLE1DDE is what Microsoft recommends for new code, and this
	// is the one apartment initialisation that had been left without it.
	// S_FALSE (the thread was already initialised) counts as success and still
	// needs its CoUninitialize.
	// https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex
	HRESULT hr_com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	bool need_uninit_com = SUCCEEDED(hr_com);

	auto result = FALSE;
	auto invoked = false;

	// Whether this click got as far as trying to build Shell's menu. Only then
	// is a fail-open a *failure* rather than a decision - an unregistered
	// process, a disabled shell, a bypass gesture and an open breaker all reach
	// the fallback deliberately and must not count against the host.
	auto attempted_takeover = false;

	// One diagnostics session per intercepted popup, always on. Phases recorded
	// anywhere below this land in it; session_end() in the __finally publishes
	// it into the process ring. Both are plain functions on plain data, which is
	// what an SEH function can hold.
	// uFlags as the host passed them, before anything below rewrites them.
	// Which half of complete_host_contract a host exercises turns on
	// TPM_RETURNCMD, and this is the only place the original value exists.
	perf::session_begin(host_identity_hash(), uFlags);

	// SEH function: cannot hold an object that requires unwinding.
	auto perf_pre_display = perf::menu_perf_begin();

	// Distinguishes a menu the user dismissed from a call that never showed one.
	// Both return zero once TPM_RETURNCMD is set, and the last-error code is what
	// tells them apart - measured, not documented. See Include/HostContract.h.
	auto track_error = static_cast<DWORD>(ERROR_SUCCESS);

	auto invoke = [&](HMENU hmenu, uint32_t flag, Point pt)
	{
		if(!invoked)
		{
			::SetLastError(ERROR_SUCCESS);
			if(tfunc == 0)
			{
				// Calling the real TrackPopupMenu here would re-enter this hook:
				// user32 forwards to win32u!NtUserTrackPopupMenuEx through user32's
				// import table, which is the very slot iathook_NtUserTrackPopupMenuEx
				// patched - so the whole hook body (menu build included) would run a
				// second time for a menu that is about to be discarded. Verified with
				// a probe against the real user32/win32u pair. Call the saved
				// original directly instead; NtUserTrackPopupMenuEx with a null
				// TPMPARAMS is what TrackPopupMenu does internally anyway.
				if(iathook_NtUserTrackPopupMenuEx.installed())
					result = iathook_NtUserTrackPopupMenuEx.invoke<decltype(TrackPopupMenuExProc)>(hmenu, flag, pt.x, pt.y, hWnd, nullptr);
				else
					result = ::TrackPopupMenu(hmenu, flag, pt.x, pt.y, 0, hWnd, nullptr);
			}
			else if(iathook_NtUserTrackPopupMenuEx.installed())
				result = iathook_NtUserTrackPopupMenuEx.invoke<decltype(TrackPopupMenuExProc)>(hmenu, flag, pt.x, pt.y, hWnd, lptpm);
			else
				result = ::TrackPopupMenuEx(hmenu, flag, pt.x, pt.y, hWnd, lptpm);
			track_error = ::GetLastError();
			invoked = true;
		}
	};

	__try
	{
		__try
		{

			if(!is_registered())
			{
				__trace(L"TrackPopupMenu unregistered");
				__leave;
			}

			//cs.lock();
			auto has_inited = 0;
			auto perf_onstate = perf::menu_perf_begin();

			// Classified once, from one read of the keyboard, and used by both
			// OnState and the bypass check below. Two reads could disagree -
			// the user is releasing keys while this runs - and the plan (QA-04)
			// requires that reload and bypass can never both fire from one
			// click. One classification of one snapshot is what makes that
			// true by construction. Include/TakeoverGesture.h.
			auto gesture = Initializer::classify_click(Nilesoft::Shell::in_taskbar());
			Initializer::OnState(gesture);
			perf::menu_perf_end(perf_onstate, L"popup.initializer_onstate");

			// "Windows menu, this time." Before any Shell work at all: no
			// context, no selection, no composition. __leave hands control to
			// the __finally, whose fail-open call tracks the host's own menu
			// with the host's own flags - which is exactly what a bypass is.
			// docs/refactor/01-takeover-contract.md section 7,
			// docs/refactor/05-capabilities.md section 2.
			if(gesture == Gesture::BypassOnce)
			{
				perf::session_decision(perf::TakeoverDecision::BypassOnce);
				__leave;
			}

			// The host has refused takeover often enough in a row that trying
			// again just costs the user latency on every click. Hand it the
			// menu it would have got anyway, without the attempt.
			// docs/refactor/01-takeover-contract.md section 7.
			if(!takeover_breaker().should_attempt())
			{
				takeover_breaker().record_skipped();
				perf::session_decision(perf::TakeoverDecision::Degraded);
				__leave;
			}

			if(!_initializer.Status.Disabled)
			{
				// is injected from explorer
				if(!_initializer.Status.Loaded)
				{
					if(_initializer.has_error() || has_inited)
						__leave; //goto skip;
					_initializer.init();
				}

				// From here on a failure to show Shell's menu is a takeover
				// failure rather than a decision not to try, and the breaker
				// counts it. Set before the call, so an exception inside it
				// counts too.
				attempted_takeover = true;

				// ...but only a failure. Initialize() also refuses on purpose -
				// for a window Shell does not handle, a configuration that hides
				// the menu here, or a generation that is not being served - and
				// counting those opened the breaker on hosts where Shell works.
				// Found by running the trace harness through the hook: three of
				// the probe's own plain popups were enough to switch takeover
				// off for the process, after which a shell-namespace menu that
				// Shell handles perfectly well was handed straight back.
				// A third-party file manager raising three of its own internal
				// popups would lose Shell for the rest of the session.
				// docs/refactor/01-takeover-contract.md section 7a.
				bool declined = false;

				auto perf_ctx = perf::menu_perf_begin();
				auto ctx = ContextMenu::CreateAndInitialize(hWnd, hMenu, { x, y }, _loader.explorer, ShellExtCapture::has(hMenu), &declined);
				perf::menu_perf_end(perf_ctx, L"popup.context_construct_initialize");

				if(!ctx && declined)
				{
					attempted_takeover = false;
					perf::session_decision(perf::TakeoverDecision::Declined);
				}
				if(ctx != nullptr)
				{

					// Add TPM_RETURNCMD, drop TPM_NONOTIFY. Both halves and the
					// reasons are in Include/HostContract.h; the short version
					// is that without RETURNCMD the tracked menu answers TRUE
					// instead of an identifier, so InvokeCommand matches nothing
					// and the user's own command silently does not run.
					Flag<uint32_t> flag(plan_host_track(uFlags).flags);
					//	flag.remove(TPM_NOANIMATION);

				//	flag.add(TPM_HORPOSANIMATION);

				//	BOOL v = 1;
				//	SystemParametersInfoW(SPI_SETMENUANIMATION, 0, &v, SPIF_SENDCHANGE);
				//	v = 0;
				//	SystemParametersInfoW(SPI_GETMENUFADE, 0, &v, SPIF_SENDCHANGE);
					
					if(ctx->is_layoutRTL == 1)
						flag.add(TPM_LAYOUTRTL);
					else if(ctx->is_layoutRTL == 0)
						flag.remove(TPM_LAYOUTRTL);

					auto z = ctx->_theme.border.size + ctx->_theme.border.size;

					if(ctx->Selected.Window.parent == WINDOW_TASKBAR)
					{
						HWND hTaskbar = hWnd;
						if(ctx->Selected.Window.id == WINDOW_START)
							hTaskbar = ::WindowFromPoint({ x, y });
						else
							hTaskbar = ::GetAncestor(hWnd, GA_ROOTOWNER);

						if(!hTaskbar)
							hTaskbar = hWnd;

						Monitor monitor(hTaskbar);
						monitor.info();

						Rect rcW = monitor.rcWork;
						Rect rcM = monitor.rcMonitor;
						Rect rc = hTaskbar;

						if(ctx->Selected.Window.id == WINDOW_START)
						{
							if(rc.top > 0)
								y = (rc.top + 5) - (z + ctx->_theme.border.padding.height());
							else if(rc.left == 0)
								y = rc.bottom;
							else
								x = rc.left;
						}
						else
						{
							if(rcM.top == rc.top && rcM.bottom == rc.bottom) // Vertical
							{
								if(rcM.left == rc.left)
									x = rc.right;
								else if(rcM.right == rc.right)
									x = rc.left - 20;
							}
							else if(rcM.left == rc.left && rcM.right == rc.right) // Horizontal
							{
								if(rcM.top == rc.top)
									y = rc.bottom;
								else if(rcM.bottom == rc.bottom)
									y = (rc.top + 5) - (z + ctx->_theme.border.padding.height());
								else
									y = rc.bottom;
							}
						}
					}
					else
					{
						y -= z;
					}
					
					flag.remove(TPM_HORIZONTAL);
					flag.add(TPM_VERTICAL);
					//flag.add(TPM_RIGHTBUTTON);

					if(ctx->Selected.Window.id == WINDOW_START)
					{
						flag.remove(TPM_TOPALIGN);
						flag.add(TPM_BOTTOMALIGN);
					}

					//invoke(hMenu, flag, { x, y });

					//if(ctx->is_menu_has_commands)
					{
						//flag.remove(TPM_RIGHTBUTTON);
						//flag.add(TPM_LEFTBUTTON);
						//flag.add(TPM_RETURNCMD);
					}

					// Everything above this line is latency the user pays before the
					// first pixel of the menu appears.
					perf::menu_perf_end(perf_pre_display, L"popup.total_pre_display");
					perf::session_decision(perf::TakeoverDecision::TakeOver);

					// A menu Shell composed and is about to track. Whatever the
					// run of failures before it, this host works.
					takeover_breaker().record_success();

					invoke(ctx->MenuHandle(), flag, { x, y });

					// v = 0;
					// SystemParametersInfoW(SPI_SETMENUANIMATION, 0, &v, SPIF_SENDCHANGE);

					// With TPM_RETURNCMD now always set, this is the identifier
					// of the chosen item rather than TRUE.
					auto selected = result;
					auto failed = selected == 0 && track_error != ERROR_SUCCESS;
					result = ctx->InvokeCommand(result);

					// ctx is gone - InvokeCommand deletes it - so everything the
					// host contract needs was already read out of it above.
					auto completion = complete_host_contract(uFlags, selected, result, failed);
					result = completion.result;

					if(completion.notify == HostNotification::Command)
					{
						// Posted, not sent: Windows posts this after the tracking
						// call returns, and a host that receives it before its own
						// call has returned is seeing a sequence the real API
						// never produces. Measured in
						// src\tests\hostprobe\fixtures\select.plain.classic.trace.
						// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenuex
						::PostMessageW(hWnd, WM_COMMAND,
									   MAKEWPARAM(completion.notify_id, 0), 0);
					}
					__leave;
				}
			}
		}
		except
		{
#ifdef _DEBUG
			_log.exception(__func__);
#endif
		}
	}
	__finally
	{
		// Only this menu. Another window's popup may still be open and holding a
		// capture of its own.
		ShellExtCapture::clear(hMenu);

		// Reaching here with the menu not yet tracked means every path above
		// declined or threw, and what happens next is the fail-open fallback:
		// the host's own menu, with the host's own flags, untouched. That is the
		// single most valuable safety property in this file and it is now
		// recorded rather than merely relied upon.
		// docs/refactor/01-takeover-contract.md section 2.
		if(!invoked)
		{
			// A bypass and an open breaker have already recorded what they
			// were; overwriting them with FailOpen would erase the only
			// evidence of why this menu was the host's own.
			if(perf::current_session().record.decision == perf::TakeoverDecision::Unknown)
				perf::session_decision(perf::TakeoverDecision::FailOpen);

			// Said once, by whichever thread crossed the threshold, rather than
			// on every menu from here on.
			// docs/refactor/01-takeover-contract.md section 7.
			if(attempted_takeover && takeover_breaker().record_failure())
				log_breaker_opened();
		}

		invoke(hMenu, uFlags, { x, y });
		// The flag used to be cleared here, which was the only place it was
		// cleared and not a place every taskbar menu passes through.
		// ScopedTaskbarOrigin in ShowTaskbarContextMenu owns it now.
		// The WIC factory is apartment-local and must not survive the
		// CoUninitialize that can close this thread's apartment.
		WIC::release();
		perf::session_end();
		if(need_uninit_com)
			::CoUninitialize();

		/*
			Hand back the last error the tracking call left, not whatever the
			teardown above happened to set.

			TrackPopupMenu documents the pairing: "If the function fails, the
			return value is zero. To get extended error information, call
			GetLastError."
			https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenu

			Everything between the tracking call and this line - InvokeCommand,
			complete_host_contract, PostMessage, WIC::release, session_end,
			CoUninitialize - can set the thread's last-error value, and several
			of them do. The host is then told a menu it never saw simply
			returned zero, with no way to tell a cancelled menu from a call that
			failed.

			That distinction is not hypothetical here: Include/HostContract.h
			depends on exactly this signal, because adding TPM_RETURNCMD makes
			both cases return zero. Shell was consuming a contract it destroyed
			for its own callers. Caught by running the trace harness through the
			hook - src\tests\hostprobe\fixtures\question.a_failed_track_sets_a_last_error.trace
			was the one scenario in twenty-three whose host-observable result
			differed.
		*/
		::SetLastError(track_error);
		return result;
	}
}

BOOL WINAPI TrackPopupMenuProc(HMENU hMenu, uint32_t uFlags, int x, int y, [[maybe_unused]] int nReserved, HWND hWnd, [[maybe_unused]] const RECT *prcRect)
{
	//_log.write(L"%s\n", Window::class_name(hWnd).c_str());
	return NtUserTrackPopupMenu(hMenu, uFlags, x, y, hWnd, nullptr, 0);
}

//win32u.dll win10 1607	Redstone 1 14393
BOOL WINAPI TrackPopupMenuExProc(HMENU hMenu, uint32_t uFlags, int x, int y, HWND hWnd, LPTPMPARAMS lptpm)
{
	//_log.write(L"%s\n", Window::class_name(hWnd).c_str());
	return NtUserTrackPopupMenu(hMenu, uFlags, x, y, hWnd, lptpm, 1);
}

bool ShowTaskbarContextMenu(HWND hTaskbar, const Point &pt, uint32_t uMsg)
{
	auto ret = false;
	//if(taskbar_t::is_allowed_area(hTaskbar, pt))
	{
		struct menu {
			HMENU handle{};
			menu() { handle = ::CreatePopupMenu(); }
			~menu() { if(handle) ::DestroyMenu(handle); }
		}_menu;

		if(_menu.handle)
		{
			// Restored on the way out of this block, so the branch below that
			// invokes the saved native target directly - and therefore never
			// reaches the hook's __finally - cannot leave it set.
			Nilesoft::Shell::ScopedTaskbarOrigin taskbar_origin(true);

			//::SetForegroundWindow(hTaskbar);
			auto uFlags = TPM_RETURNCMD | TPM_RIGHTBUTTON | (::GetSystemMetrics(SM_MIDEASTENABLED) ? TPM_LAYOUTRTL : 0);
			if(uMsg == WM_CONTEXTMENU)
			{
				//_log.info(L"************");
				TrackPopupMenuExProc(_menu.handle, uFlags, pt.x, pt.y, hTaskbar, nullptr);
			}
			else 
			{
				iathook_NtUserTrackPopupMenuEx.invoke<decltype(TrackPopupMenuExProc)>(_menu.handle, uFlags, 0, 0, hTaskbar, nullptr);
			}
			::PostMessageW(hTaskbar, WM_NULL, 0, 0); // send benign message to window to make sure the menu goes away.
			ret = true;
		}
	}
	return ret;
}

LRESULT __stdcall TaskbarProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	auto wndproc = (WNDPROC)::GetPropW(hWnd, UxSubclass);
	
	if(wndproc == nullptr)
		return ::DefWindowProcW(hWnd, uMsg, wParam, lParam);

	if(uMsg == WM_CONTEXTMENU)
	{
		auto pt = Point::CursorPos();
		if(taskbar_t::is_allowed_area(hWnd, pt))
		{
			if(ShowTaskbarContextMenu(hWnd, pt, wParam == 0 && lParam == 0 ? WM_RBUTTONDOWN : WM_CONTEXTMENU))
				return 0;
		}
	}
	else if(uMsg == WM_MOUSEACTIVATE)
	{
		auto msg = HIWORD(lParam);
		if(msg != WM_RBUTTONDOWN)
			return wndproc(hWnd, uMsg, wParam, lParam);

		auto pt = Point::CursorPos();

		if(taskbar_t::is_allowed_area(hWnd, pt))
		{
			int disabled_taskbar = 0;
			if(RegistryConfig::get(L"\\disable", L"taskbar", disabled_taskbar) && disabled_taskbar == 1)
				return wndproc(hWnd, uMsg, wParam, lParam);

			if(msg == WM_RBUTTONDOWN)
			{
				Keyboard kb;
				if(auto count = kb.get_keys_excloude_contextmenu(); count > 0)
				{
					if(kb.key_ctrl())
					{
						if(count == 2 && kb.key_win())
							return wndproc(hWnd, uMsg, wParam, lParam);

						if(count == 1 && !_initializer.has_error())
						{
							if(!_initializer.Status.Loaded)
								_initializer.OnState();
						}
					}
				}
			}

			if(_initializer.has_error())
			{
				_initializer.OnState();
				if(_initializer.has_error())
					return wndproc(hWnd, uMsg, wParam, lParam);
			}
			else
			{
				if(_initializer.Status.Disabled)
					return wndproc(hWnd, uMsg, wParam, lParam);
			}

			if(!is_registered())
			{
				return wndproc(hWnd, uMsg, wParam, lParam);
			}

			::SetFocus(hWnd);
			::SendMessageW(hWnd, WM_CONTEXTMENU, 0, 0);

			return MA_ACTIVATEANDEAT;
		}
	}
	else if(uMsg == WM_SETTINGCHANGE || uMsg == WM_DISPLAYCHANGE)
	{
		// Cached hit-test answers describe a layout that has just changed.
		taskbar_t::invalidate_hit_cache();

		if(uMsg == WM_SETTINGCHANGE && SPI_SETWORKAREA == wParam && ::GetSystemMetrics(SM_CMONITORS) > 1)
		{
			taskbar_t::hook_all(1000);
		}
	}
	else if(uMsg == WM_DESTROY)
	{
		taskbar_t::invalidate_hit_cache();
		taskbar_t::unhook(hWnd);
		taskbar_t::hook_all(1000);
	}

	return wndproc(hWnd, uMsg, wParam, lParam);
}

LRESULT __stdcall TaskbarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, [[maybe_unused]] UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	if(uMsg == WM_CONTEXTMENU)
	{
		auto hTaskbar = reinterpret_cast<HWND>(dwRefData);
		auto pt = Point::CursorPos();
		if(taskbar_t::is_allowed_area(hTaskbar, pt))
		{
			if(ShowTaskbarContextMenu(hTaskbar, pt, wParam == 0 && lParam == 0? WM_RBUTTONDOWN : WM_CONTEXTMENU))
				return 0;			
		}
	}
	else if(uMsg == WM_MOUSEACTIVATE)
	{
		auto lret = ::DefSubclassProc(hWnd, uMsg, wParam, lParam);
		auto msg = HIWORD(lParam);

		if(/*msg != WM_LBUTTONDOWN && */msg != WM_RBUTTONDOWN)
			return lret;

		auto hTaskbar = reinterpret_cast<HWND>(dwRefData);
		auto pt = Point::CursorPos();

		if(taskbar_t::is_allowed_area(hTaskbar, pt))
		{
			int disabled_taskbar = 0;
			if(RegistryConfig::get(L"\\disable", L"taskbar", disabled_taskbar) && disabled_taskbar == 1)
				return lret;

			if(msg == WM_RBUTTONDOWN)
			{
				Keyboard kb;
				if(auto count = kb.get_keys_excloude_contextmenu(); count > 0)
				{
					if(kb.key_ctrl())
					{
						if(count == 2 && kb.key_win())
							return lret;

						if(count == 1 && !_initializer.has_error())
						{
							if(!_initializer.Status.Loaded)
								_initializer.OnState();
						}
					}
				}
			}

			if(_initializer.has_error())
			{
				if(msg != WM_RBUTTONDOWN)
					return lret;

				_initializer.OnState();

				if(_initializer.has_error())
					return lret;
			}
			else
			{
				if(_initializer.Status.Disabled)
					return lret;
			}

			if(!is_registered())
			{
				_taskbar_mouse.unhook();
				taskbar_t::unhook(hWnd);
			//	for(auto &window : _window_taskbar)
			//		window.second.remove_prop(UxSubclass);
				return lret;
			}

			if(msg == WM_RBUTTONDOWN)
			{
				_taskbar_mouse.hook(WH_MOUSE, [](int nCode, WPARAM wParam, LPARAM lParam)->LRESULT
				{
					//auto lret = _taskbar_mouse.callnext(nCode, wParam, lParam);
					if(nCode == HC_ACTION && wParam == WM_RBUTTONUP)
					{
						auto mh = reinterpret_cast<MOUSEHOOKSTRUCT *>(lParam);
						Window window = ::GetParent(mh->hwnd);
						if(window.is_prop(UxSubclass))
						{
							_taskbar_mouse.unhook();
							::SetFocus(window);
							ShowTaskbarContextMenu(window, mh->pt, WM_CONTEXTMENU);
							return 1;
						}
					}
					return _taskbar_mouse.callnext(nCode, wParam, lParam);
				}, hWnd);
				
				return MA_ACTIVATEANDEAT;
			}

			//if(msg != WM_RBUTTONDOWN)
			//	::SetFocus(hWnd);

			/*
			if(msg == WM_RBUTTONDOWN)
			{
				::SetFocus(hWnd);
				//ShowTaskbarContextMenu(hWnd, pt, WM_RBUTTONDOWN);
				SendMessageW(hWnd, WM_CONTEXTMENU, 0, 0);
			}
			*/
		}
		return lret;
	}
	else if(uMsg == WM_SETTINGCHANGE || uMsg == WM_DISPLAYCHANGE)
	{
		// Cached hit-test answers describe a layout that has just changed.
		taskbar_t::invalidate_hit_cache();

		if(uMsg == WM_SETTINGCHANGE && SPI_SETWORKAREA == wParam && ::GetSystemMetrics(SM_CMONITORS) > 1)
		{
			taskbar_t::hook_all(1000);
		}
	}
	else if(uMsg == WM_DESTROY)
	{
		taskbar_t::invalidate_hit_cache();
		taskbar_t::unhook(hWnd);
		taskbar_t::hook_all(1000);
	}

	return ::DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void BootstrapOnce()
{
	static std::once_flag flag;
	std::call_once(flag, []()
	{
		auto &rt = Runtime();
		bool success = false;
		try
		{
			rt.loader.init();

			if(!rt.loader.explorer && !rt.loader.path.ends_with(L"\\shell.exe"))
			{
				int disabled_3rdparty = 0;
				if(RegistryConfig::get(L"\\disable", L"3rdparty", disabled_3rdparty) && disabled_3rdparty == 1)
					return;
			}

			if(rt.initializer.init(_hInstance))
			{
				rt.initializer.process.hModule = rt.loader.handle;
				rt.initializer.process.path = Path::Module(rt.loader.handle).move();
				rt.initializer.process.name = Path::Title(rt.initializer.process.path).move();
				rt.initializer.process.id = ::GetCurrentProcessId();
				rt.initializer.process.handle = ::GetCurrentProcess();

				if(!PinModule())
					return;

				ContextMenu::RegisterLayer();

				rt.ntuser_popup_hook
					.init(L"user32.dll", "win32u.dll", "NtUserTrackPopupMenuEx", TrackPopupMenuExProc)
					.install();

				auto hook = [&rt]()
				{
					__trace(L"hook all the modules in '%s' process", rt.initializer.process.name.c_str());

					auto user32 = "user32.dll";

					std::unordered_set<HMODULE> hooked;
					hooked.reserve(rt.popup_hooks.size() + 64);
					for(const auto &m : rt.popup_hooks)
						hooked.insert(m._hModule);

					for(auto hModule : Process::Modules(rt.initializer.process.handle))
					{
						if(_hInstance == hModule)
							continue;

						if(!hooked.insert(hModule).second)
							continue;

						rt.popup_hooks.emplace_back(hModule, user32, ::TrackPopupMenu, TrackPopupMenuProc).install();
						rt.popup_hooks.emplace_back(hModule, user32, ::TrackPopupMenuEx, TrackPopupMenuExProc).install();
					}
				};

				if(rt.loader.explorer)
				{
					// The only inline detour in the process, and it patches a function
					// Explorer calls from most of its threads - so every other thread
					// is enlisted in the transaction. See DetourTransaction.
					DetourTransaction transaction;
					if(transaction.begin())
					{
						rt.co_create_instance_hook.init(::CoCreateInstance, CoCreateInstanceHook).hook();

						if(!transaction.commit())
						{
							// Nothing was patched, whatever the attach reported.
							rt.co_create_instance_hook.forget();
							__trace(L"CoCreateInstance detour did not commit");
						}
					}

					if(!rt.ntuser_popup_hook.installed())
					{
						hook();
					}

					if(Windows::Version::Instance().IsWindows11OrGreater())
					{
						taskbar_t::hook_all(1000);
					}
				}
				else if(!rt.ntuser_popup_hook.install())
				{
					hook();
				}

				// Start the packaged-verb scan now, on its own thread. It is
				// registry and manifest I/O - 244 files and ~111 ms cold on the
				// machine this was measured on - and it used to run on the menu
				// thread, for whichever right-click happened to find the 30 s
				// TTL expired. Starting it here overlaps it with the rest of
				// bootstrap, so by the first menu the answer is usually already
				// published. See Include/PackageCatalogService.h.
				PackageCatalogService::instance().warm_async();

				success = true;
				rt.hooks_installed.store(true, std::memory_order_release);
				hooks_installed.store(true, std::memory_order_release);
			}
		}
		catch(...)
		{
#ifdef _DEBUG
			Logger::Instance().exception(__func__);
			Logger::Instance().close();
#endif
		}

		if(!success)
		{
			try
			{
				if(rt.ntuser_popup_hook.installed())
					rt.ntuser_popup_hook.uninstall();
				for(auto &h : rt.popup_hooks)
					h.uninstall();
				rt.popup_hooks.clear();
			}
			catch(...)
			{
			}
		}
	});
}

//integrate 
BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
	if(dwReason == DLL_PROCESS_ATTACH)
	{
		_hInstance = hInstance;
		::DisableThreadLibraryCalls(hInstance);
		return TRUE;
	}
	else if(dwReason == DLL_PROCESS_DETACH)
	{
		if(lpReserved != nullptr)
		{
			// Process termination: Microsoft DLL best practices recommend empty detach.
			return TRUE;
		}
		// Explicit unload: module is pinned once hooks are installed.
		return TRUE;
	}
	return TRUE;
}

//IID_FolderExtensions
_Check_return_
STDAPI DllGetClassObject(_In_ REFCLSID rclsid, [[maybe_unused]] _In_ REFIID riid, [[maybe_unused]] _Outptr_ LPVOID FAR *ppv)
{
	if(!ppv) return E_POINTER;
	*ppv = nullptr;

	BootstrapOnce();

	Guid iid = rclsid;
	if(iid.equals(IID_FolderExtensions))
		return E_NOTIMPL;

	if(!iid.equals({ IID_ContextMenu, IID_IconOverlay }))
		return CLASS_E_CLASSNOTAVAILABLE;

	if(Initializer::Status.Disabled.load(std::memory_order_relaxed))
		return CLASS_E_CLASSNOTAVAILABLE;

	if(!is_registered())
		return CLASS_E_CLASSNOTAVAILABLE;
	
	if(_initializer.has_error())
		return CLASS_E_CLASSNOTAVAILABLE;
	
	if(rclsid == IID_ContextMenu)
	{
		Selections::point.GetCursorPos();
	}

	if(!_initializer.Status.Loaded.load(std::memory_order_relaxed))
		_initializer.init();

	if(rclsid == IID_ContextMenu)
		return CreateShellExtFactory(riid, ppv);

	return CLASS_E_CLASSNOTAVAILABLE;
}

//DllCanUnloadNow. COM calls this function to determine whether the object is serving any clients. 
__control_entrypoint(DllExport)
STDAPI DllCanUnloadNow(void)
{
	if(com_object_count.load(std::memory_order_relaxed) > 0)
		return S_FALSE;

	if(ShellExtCapture::has_active_captures())
		return S_FALSE;

	if(Runtime().hooks_installed.load(std::memory_order_relaxed) || hooks_installed.load(std::memory_order_relaxed))
		return S_FALSE;

	if(!_loader.explorer || !is_registered())
		return S_OK;

	return S_FALSE;
}

/*
	Parse a configuration and report; publish nothing.

	This is what `shell.exe -check` calls. It exists as an export because the
	parser lives here and the manager EXE does not link it - see
	src/shared/ConfigCheck.h for the boundary and docs/refactor/03-config-safety.md
	section 1b step 4 for why the feature is worth having at all.

	Deliberately does *not* call BootstrapOnce(): loading this DLL to ask it a
	question must not install a hook into the asking process. DllMain does
	nothing but record the instance, so a plain LoadLibrary is inert.

	It does have to call `init(HINSTANCE)` though, and not doing so was a defect
	that made `shell.exe -check` with no argument answer "no configuration file
	was found" on *every* machine, however healthy. The path is derived in
	`Initializer::init(HINSTANCE)` - `application.ConfigPortable`, the
	`shell.nss` beside this DLL - and `Parser`'s default constructor gives up
	immediately when `Initializer::instance` is null, which is exactly the state
	skipping BootstrapOnce leaves behind. So the bare form could never work, and
	docs/refactor/03-config-safety.md section 1b's "empty means whatever this
	machine would load" was describing something that had never happened.

	`init(HINSTANCE)` is safe here in a way BootstrapOnce is not: it assigns
	paths, reads the DPI and asks whether this process is elevated. No hook, no
	COM, no window, no thread.

	Guarded on `instance` being null so this only ever *establishes* the paths.
	The export is callable from a host where Shell is live and has already
	bootstrapped, and re-running it there would reset `application.Config` to
	the portable path underneath a running menu - turning a read-only diagnostic
	into something that changes what it is diagnosing, which the comment above
	says this must never do.

	Named with __stdcall and exported by name through shell.def, so the same
	name works on x86, x64 and arm64 without decoration leaking into the
	contract.
*/
extern "C" __declspec(dllexport)
int __stdcall ShellCheckConfig(const wchar_t *path, Nilesoft::Shell::ConfigCheckResult *result)
{
	using namespace Nilesoft::Shell;

	// A caller compiled against a different version of the struct, or none at
	// all. Refusing is the only safe answer: every field below is written
	// through a pointer whose size this is the only evidence for.
	if(!result || result->cbSize != sizeof(ConfigCheckResult))
		return CONFIG_CHECK_UNUSABLE;

	if(Initializer::instance == nullptr)
		_initializer.init(_hInstance);

	return _initializer.check(path, *result);
}

// Register the COM server and the context menu handler.
HRESULT Register(bool reg)
{
	try
	{
		if(reg)
		{
			Path::Delete(_log.path());
			//_log.reset();
		}

		if(!ver->IsWindows7OrGreater())
		{
			//windows compatibility
			_log.error(L"%s", string::Extract(_hInstance, IDS_WINDOWS_COMPATIBILITY).c_str());
			return ERROR_EXE_MACHINE_TYPE_MISMATCH;//ERROR_DS_VERSION_CHECK_FAILURE
		}

		auto is_elevated = Security::Elevation::IsElevated();

		if(!is_elevated)
		{
			// Missing administrative privileges!
			string msg = string::Extract(_hInstance, IDS_ADMIN_PRIVILEGES).move();
			//You will need to provide administrator permission to run this Shell
			_log.error(L"%s", msg.c_str());
			return ERROR_ACCESS_DENIED;
		}

		string path = Path::Module(_hInstance).move();

		if(reg)
		{
			// The install-directory ACL widening that used to be here is gone;
			// see the matching comment in exe/src/Main.cpp. It granted Users
			// GENERIC_ALL on Program Files and only failed to do so because it
			// could not open a directory handle.
			//_log.reset();
			//IO::Path::Delete(_log.path());
		}

		string msg;

		if(reg)
		{
			//_log.create();
			REGOP regop{};
			regop.REGISTER = regop.CONTEXTMENU = regop.ICONOVERLAY = true;

			if(!RegistryConfig::Register(path, regop))
			{
				msg = string::Extract(_hInstance, IDS_REGISTER_NOT_SUCCESS).move();
				_log.error(L"%s", msg.c_str());
				return S_FALSE;
			}
			msg = string::Extract(_hInstance, IDS_REGISTER_SUCCESS).move();
		}
		else
		{
			if(!RegistryConfig::IsRegistered())
				return S_OK;

			if(!RegistryConfig::Unregister())
			{
				msg = string::Extract(_hInstance, IDS_UNREGISTER_NOT_SUCCESS).move();
				_log.error(L"%s", msg.c_str());
				return S_FALSE;;
			}
			msg = string::Extract(_hInstance, IDS_UNREGISTER_SUCCESS).move();
		}

		_log.info(L"%s", msg.c_str());

		_log.close();

		return S_OK;
	}
	catch(...)
	{
#ifdef _DEBUG
		_log.exception(__func__);
#endif
	}
	_log.close();
	return S_FALSE;
}
/*
//  Register the COM server and the context menu handler.
__control_entrypoint(DllExport)
STDAPI DllRegisterServer()
{
	return Register(true);
}

__control_entrypoint(DllExport)
STDAPI DllUnregisterServer()
{
	return Register(false);
}
*/
/*
STDAPI DllInstall(BOOL bInstall, _In_opt_ PCWSTR pszCmdLine)
{
	MBF(L"%d %s", bInstall, pszCmdLine);
	return E_NOTIMPL;
}
*/

/*
#include <Shldisp.h>

CoInitialize(NULL);
// Create an instance of the shell class
IShellDispatch4 *pShellDisp = NULL;
HRESULT sc = CoCreateInstance(CLSID_Shell, NULL, CLSCTX_SERVER, IID_IDispatch, (LPVOID *) &pShellDisp );
// Show the desktop
sc = pShellDisp->ToggleDesktop();
// Restore the desktop
sc = pShellDisp->ToggleDesktop();
pShellDisp->Release();
 */

#pragma endregion

uint32_t ImmersiveColor::GetColorByColorType(uint32_t colorType)
{
	auto colorSet = GetImmersiveUserColorSetPreference(false, false);
	return GetImmersiveColorFromColorSetEx(colorSet, colorType, false, 0);
}

uint32_t ImmersiveColor::GetColorByName(const wchar_t *name)
{
	auto colorSet = GetImmersiveUserColorSetPreference(false, false);
	auto colorType = GetImmersiveColorTypeFromName(name);
	return GetImmersiveColorFromColorSetEx(colorSet, colorType, false, 0);
}

uint32_t ImmersiveColor::GetImmersiveUserColorSetPreference(bool bForceCheckRegistry, bool bSkipCheckOnFail)
{
	return DLL::Invoke<uint32_t>(hUxTheme, 98, bForceCheckRegistry, bSkipCheckOnFail);
}

uint32_t ImmersiveColor::GetImmersiveColorFromColorSetEx(uint32_t dwImmersiveColorSet, uint32_t dwImmersiveColorType, bool bIgnoreHighContrast, uint32_t dwHighContrastCacheMode)
{
	return DLL::Invoke<uint32_t>(hUxTheme, 95, dwImmersiveColorSet, dwImmersiveColorType, bIgnoreHighContrast, dwHighContrastCacheMode);
}

uint32_t ImmersiveColor::GetImmersiveColorTypeFromName(const wchar_t *name)
{
	return DLL::Invoke<uint32_t>(hUxTheme, 96, name);
}

uint32_t ImmersiveColor::GetImmersiveColorSetCount()
{
	return DLL::Invoke<uint32_t>(hUxTheme, 94);
}

bool ImmersiveColor::IsSupported()
{
	return  ::IsCompositionActive() && DLL::IsFunc(hUxTheme, 95);
}
