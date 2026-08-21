#include <pch.h>
#include <unordered_set>
#include "Include/ContextMenu.h"
#include "Include/ShellExt.h"
#include "Include/Diagnostics/MenuPerf.h"
#include "Include/TaskbarHitCache.h"
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
#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
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
	Detours<decltype(::DllGetClassObject)> dll_get_class_object_hook;
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
#define _DllGetClassObject (Runtime().dll_get_class_object_hook)
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

	// Called on the taskbar's UI thread. Returns the cached answer when there is
	// one, otherwise hands the point to the worker and waits out the budget.
	bool query(HWND taskbar, POINT pt, bool secondary)
	{
		auto now = ::GetTickCount64();
		if(auto hit = _cache.lookup(taskbar, pt, now); hit)
			return *hit;

		if(!ensure_started())
			return false;

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

		DWORD index = 0;
		::SetLastError(ERROR_SUCCESS);
		auto hr = ::CoWaitForMultipleHandles(0, BUDGET_MS, 1, &_done, &index);

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
			return false;
		}

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

		for(;;)
		{
			::WaitForSingleObject(_work, INFINITE);

			Request request;
			{
				std::lock_guard<std::mutex> lock(_mutex);
				request = _request;
			}

			auto allowed = uia ? evaluate(uia, request) : false;

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

	// Everything UI Automation touches stays inside this function, on this
	// thread. No element pointer is stored or returned.
	static bool evaluate(IComPtr<IUIAutomation> &uia, const Request &request)
	{
		IComPtr<IUIAutomationElement> element;
		if(FAILED(uia->ElementFromPoint(request.pt, element)) || !element)
			return false;

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
		element->get_CurrentAutomationId(&elem.id);
		element->get_CurrentClassName(&elem.type);
		element->get_CurrentName(&elem.name);

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
				Context context;
				this_item _this; context._this = &_this;
				_this.is_uwp = is_UWP;
				_this.clsid = Guid(rclsid).to_string(2);

				bool test = Keyboard::IsKeyDown(VK_MENU);
				if(test)
				{
					Timer t;
					t.start();
					hr = _CoCreateInstance.invoke(rclsid, pUnkOuter, dwClsContext, riid, ppv);
					t.stop();
					auto elapsed = (int)t.elapsed_milliseconds();
					_log.write(L"%d%s\t%s\t%s\r\n", elapsed, elapsed > 0 ? L"ms" : L"" , _this.clsid.c_str(), is_UWP ? L"UWP":L"");
					return hr;
				}

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
								if(test && *ppv)
								{
									((IUnknown *)*ppv)->Release();
									*ppv = nullptr;
								}
								return E_NOINTERFACE;
							}
						}
					}
				}

				if(test)
					return hr;
			}
		}
	}

	return _CoCreateInstance.invoke(rclsid, pUnkOuter, dwClsContext, riid, ppv);
}

STDAPI WINAPI DllGetClassObjectHook(REFCLSID rclsid, _In_ REFIID riid, LPVOID *ppv)
{
	auto hr = CLASS_E_CLASSNOTAVAILABLE;
	if(IID_FileExplorerContextMenu == rclsid/* || rclsid == IID_FileExplorerCommandBar*/)
		return hr;
	hr = _DllGetClassObject.invoke(rclsid, riid, ppv);
	return hr;
}

#define TPM_SYSMENU	0x0200L


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

	// SEH function: cannot hold an object that requires unwinding.
	auto perf_pre_display = perf::menu_perf_begin();

	auto invoke = [&](HMENU hmenu, uint32_t flag, Point pt)
	{
		if(!invoked)
		{
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
			Initializer::OnState(Nilesoft::Shell::in_taskbar());
			perf::menu_perf_end(perf_onstate, L"popup.initializer_onstate");

			if(!_initializer.Status.Disabled)
			{
				// is injected from explorer
				if(!_initializer.Status.Loaded)
				{
					if(_initializer.has_error() || has_inited)
						__leave; //goto skip;
					_initializer.init();
				}

				auto perf_ctx = perf::menu_perf_begin();
				auto ctx = ContextMenu::CreateAndInitialize(hWnd, hMenu, { x, y }, _loader.explorer, ShellExtCapture::has(hMenu));
				perf::menu_perf_end(perf_ctx, L"popup.context_construct_initialize");
				if(ctx != nullptr)
				{

					Flag<uint32_t> flag(uFlags);
					//Selections::point = { x, y };
					//	flag.remove(TPM_RETURNCMD);
					flag.remove(TPM_NONOTIFY);
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

					invoke(ctx->MenuHandle(), flag, { x, y });
				
					// v = 0;
					// SystemParametersInfoW(SPI_SETMENUANIMATION, 0, &v, SPIF_SENDCHANGE);
					result = ctx->InvokeCommand(result);
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
		invoke(hMenu, uFlags, { x, y });
		// The flag used to be cleared here, which was the only place it was
		// cleared and not a place every taskbar menu passes through.
		// ScopedTaskbarOrigin in ShowTaskbarContextMenu owns it now.
		// The WIC factory is apartment-local and must not survive the
		// CoUninitialize that can close this thread's apartment.
		WIC::release();
		if(need_uninit_com)
			::CoUninitialize();
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
