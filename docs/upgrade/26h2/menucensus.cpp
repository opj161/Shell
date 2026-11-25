// Menu census probe - a reproducible "before" reading for the 26H2 upgrade.
//
// Two kinds of census, because they answer different questions:
//
//   live    the menu a host actually shows, read out of the popup window's own
//           HMENU. The only reading that proves Shell composed anything. Three
//           surfaces: the desktop background, the taskbar, and a folder window's
//           background. Also times the composition end to end.
//             MN_GETHMENU: "Retrieves the menu handle for the specified menu
//             window." https://learn.microsoft.com/en-us/windows/win32/menurc/mn-gethmenu
//             WM_CONTEXTMENU is what a host answers by calling TrackPopupMenu,
//             which is the call Shell hooks, so no synthetic input is needed.
//             https://learn.microsoft.com/en-us/windows/win32/menurc/wm-contextmenu
//
//   static  what the registered shell extensions contribute through
//           IContextMenu::QueryContextMenu, with no UI at all. Deterministic and
//           repeatable. Shell contributes zero items this way by design, so this
//           measures the *input* to Shell's composition: exactly the set that
//           26H2's new File Explorer verbs would land in.
//             IShellFolder::CreateViewObject gives a folder's background menu,
//             GetUIObjectOf gives one or more items' menu.
//             https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellfolder-createviewobject
//             https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellfolder-getuiobjectof
//
// Output is one tab-separated row per item so two runs diff cleanly. Command ids
// are printed but must not be diffed: Shell renumbers its own on every
// invocation. census.ps1 drops the column.

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <objbase.h>
#include <stdio.h>
#include <vector>

#ifndef MN_GETHMENU
#define MN_GETHMENU 0x01E1
#endif

static const wchar_t kSep = 0x5C; // backslash, spelled so no escape can be eaten

// ---------------------------------------------------------------- menu walking

static void print_row(int depth, int pos, UINT id, const wchar_t *kind,
                      const wchar_t *title, const wchar_t *verb)
{
    wprintf(L"%d\t%d\t%u\t%s\t%s\t%s\n", depth, pos, id, kind,
            title ? title : L"", verb ? verb : L"");
}

// GetMenuItemInfo with a fixed buffer truncates silently; size from the first
// call's cch and read into a buffer that size plus the terminator.
// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmenuiteminfow
static bool read_title(HMENU menu, int pos, wchar_t **out)
{
    *out = nullptr;

    MENUITEMINFOW mi {};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_STRING;
    mi.dwTypeData = nullptr;
    mi.cch = 0;
    if(!::GetMenuItemInfoW(menu, static_cast<UINT>(pos), TRUE, &mi))
        return false;
    if(mi.cch == 0)
        return false;

    const UINT needed = mi.cch + 1;
    wchar_t *buffer = static_cast<wchar_t *>(::calloc(needed, sizeof(wchar_t)));
    if(!buffer)
        return false;

    mi.dwTypeData = buffer;
    mi.cch = needed;
    if(!::GetMenuItemInfoW(menu, static_cast<UINT>(pos), TRUE, &mi))
    {
        ::free(buffer);
        return false;
    }

    *out = buffer;
    return true;
}

static void walk(HMENU menu, IContextMenu *cm, int depth, int max_depth, int *total)
{
    // "If the function fails, the return value is -1", so this is held in an int.
    // https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmenuitemcount
    const int count = menu ? ::GetMenuItemCount(menu) : -1;
    if(count <= 0)
        return;

    for(int i = 0; i < count; i++)
    {
        MENUITEMINFOW mi {};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU;
        if(!::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mi))
            continue;

        const wchar_t *kind = L"item";
        if(mi.fType & MFT_SEPARATOR)
            kind = L"sep";
        else if(mi.hSubMenu)
            kind = L"submenu";
        else if(mi.fType & MFT_OWNERDRAW)
            kind = L"ownerdraw";

        wchar_t *title = nullptr;
        read_title(menu, i, &title);

        // The verb identifies a handler's command across releases, so a new 26H2
        // command stays recognisable even if its display name changed.
        wchar_t verb[260] {};
        if(cm && mi.wID != 0 && !(mi.fType & MFT_SEPARATOR))
        {
            char narrow[260] {};
            if(SUCCEEDED(cm->GetCommandString(mi.wID - 1, GCS_VERBW, nullptr,
                                              reinterpret_cast<LPSTR>(verb), 260)))
            {
                // already wide
            }
            else if(SUCCEEDED(cm->GetCommandString(mi.wID - 1, GCS_VERBA, nullptr,
                                                   narrow, 260)))
            {
                ::MultiByteToWideChar(CP_ACP, 0, narrow, -1, verb, 260);
            }
            else
            {
                verb[0] = 0;
            }
        }

        if(!(mi.fType & MFT_SEPARATOR))
            (*total)++;
        print_row(depth, i, mi.wID, kind, title, verb);

        if(title)
            ::free(title);

        if(mi.hSubMenu && depth < max_depth)
            walk(mi.hSubMenu, cm, depth + 1, max_depth, total);
    }
}

// ---------------------------------------------------------------- static census

static int census_static(int argc, wchar_t **argv)
{
    const bool background = (::lstrcmpiW(argv[0], L"bg") == 0);

    HRESULT hr = S_OK;
    IContextMenu *cm = nullptr;

    if(background)
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        hr = ::SHParseDisplayName(argv[1], nullptr, &pidl, 0, nullptr);
        if(FAILED(hr)) { wprintf(L"# SHParseDisplayName 0x%08X\n", hr); return 2; }

        IShellFolder *folder = nullptr;
        hr = ::SHBindToObject(nullptr, pidl, nullptr, IID_IShellFolder,
                              reinterpret_cast<void **>(&folder));
        if(FAILED(hr)) { wprintf(L"# SHBindToObject 0x%08X\n", hr); return 2; }

        hr = folder->CreateViewObject(nullptr, IID_IContextMenu,
                                      reinterpret_cast<void **>(&cm));
        folder->Release();
        ::CoTaskMemFree(pidl);
        if(FAILED(hr)) { wprintf(L"# CreateViewObject 0x%08X\n", hr); return 2; }
    }
    else
    {
        const int n = argc - 1;
        if(n < 1) { wprintf(L"# no paths\n"); return 2; }

        PIDLIST_ABSOLUTE first = nullptr;
        hr = ::SHParseDisplayName(argv[1], nullptr, &first, 0, nullptr);
        if(FAILED(hr)) { wprintf(L"# SHParseDisplayName 0x%08X\n", hr); return 2; }

        IShellFolder *parent = nullptr;
        PCUITEMID_CHILD child = nullptr;
        hr = ::SHBindToParent(first, IID_IShellFolder,
                              reinterpret_cast<void **>(&parent), &child);
        if(FAILED(hr)) { wprintf(L"# SHBindToParent 0x%08X\n", hr); return 2; }

        std::vector<PCUITEMID_CHILD> children;
        std::vector<PIDLIST_ABSOLUTE> absolutes;
        children.push_back(child);
        absolutes.push_back(first);

        for(int i = 1; i < n; i++)
        {
            PIDLIST_ABSOLUTE p = nullptr;
            if(FAILED(::SHParseDisplayName(argv[1 + i], nullptr, &p, 0, nullptr)))
                continue;
            absolutes.push_back(p);
            children.push_back(::ILFindLastID(p));
        }

        hr = parent->GetUIObjectOf(nullptr, static_cast<UINT>(children.size()),
                                   children.data(), IID_IContextMenu, nullptr,
                                   reinterpret_cast<void **>(&cm));
        parent->Release();
        if(FAILED(hr)) { wprintf(L"# GetUIObjectOf 0x%08X\n", hr); return 2; }
    }

    HMENU menu = ::CreatePopupMenu();
    hr = cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL);
    if(FAILED(hr)) { wprintf(L"# QueryContextMenu 0x%08X\n", hr); return 2; }

    int total = 0;
    wprintf(L"# depth\tpos\tid\tkind\ttitle\tverb\n");
    walk(menu, cm, 0, 2, &total);
    wprintf(L"# top-level items: %d\n", ::GetMenuItemCount(menu));

    ::DestroyMenu(menu);
    cm->Release();
    return 0;
}

// ------------------------------------------------------------------ live census

// One live reading: post WM_CONTEXTMENU at a point, wait for the popup, read its
// HMENU, wait for the item count to stop moving, walk it, dismiss it.
//
// The timings are the point of the waiting loops as much as the census is. A
// 26H2 change to what Explorer contributes shows up as composition taking
// longer, and a fixed Sleep would hide exactly that.
static int census_live_at(HWND target, POINT pt, const wchar_t *label)
{
    if(!target)
    {
        wprintf(L"# %s: no target window\n", label);
        return 2;
    }

    const auto t0 = ::GetTickCount64();

    ::PostMessageW(target, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(target),
                   MAKELPARAM(pt.x, pt.y));

    HWND popup = nullptr;
    for(int i = 0; i < 160 && !popup; i++)
    {
        popup = ::FindWindowW(L"#32768", nullptr);
        if(!popup)
            ::Sleep(25);
    }
    if(!popup)
    {
        wprintf(L"# %s: no popup window appeared\n", label);
        return 3;
    }
    const auto t_popup = ::GetTickCount64();

    HMENU menu = nullptr;
    for(int i = 0; i < 160 && !menu; i++)
    {
        DWORD_PTR handle = 0;
        ::SendMessageTimeoutW(popup, MN_GETHMENU, 0, 0, SMTO_ABORTIFHUNG, 2000, &handle);
        menu = reinterpret_cast<HMENU>(handle);
        if(!menu)
            ::Sleep(25);
    }
    if(!menu)
    {
        wprintf(L"# %s: MN_GETHMENU gave nothing\n", label);
        ::PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0);
        return 3;
    }
    const auto t_hmenu = ::GetTickCount64();

    // Items can still be appended after the handle is readable, so wait for the
    // count to hold still rather than guessing a delay.
    int count = ::GetMenuItemCount(menu);
    int stable = 0;
    for(int i = 0; i < 120 && stable < 6; i++)
    {
        ::Sleep(25);
        const int now = ::GetMenuItemCount(menu);
        if(now == count)
            stable++;
        else
        {
            count = now;
            stable = 0;
        }
    }
    const auto t_settled = ::GetTickCount64();

    int total = 0;
    wprintf(L"# depth\tpos\tid\tkind\ttitle\tverb\n");
    walk(menu, nullptr, 0, 1, &total);
    wprintf(L"# top-level items: %d\n", ::GetMenuItemCount(menu));
    wprintf(L"# timing-ms popup=%llu hmenu=%llu count-stable=%llu\n",
            static_cast<unsigned long long>(t_popup - t0),
            static_cast<unsigned long long>(t_hmenu - t0),
            static_cast<unsigned long long>(t_settled - t0));

    // Dismiss it rather than leaving a menu open on the user's screen.
    ::PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0);
    ::Sleep(120);
    ::PostMessageW(popup, WM_CLOSE, 0, 0);
    return 0;
}

// Read whatever popup menu is open right now, without opening one.
//
// This is the way round the taskbar being a XAML surface that ignores a posted
// WM_CONTEXTMENU: a person right-clicks it and leaves the menu up, and this
// reads the menu they opened. It works for any host, so it is also the way to
// census a folder or a selection that cannot be driven from here.
static int census_read_open(int wait_seconds)
{
    if(wait_seconds < 1)
        wait_seconds = 20;

    const int polls = wait_seconds * 20; // 50 ms each
    HWND popup = nullptr;
    for(int i = 0; i < polls && !popup; i++)
    {
        popup = ::FindWindowW(L"#32768", nullptr);
        if(!popup)
            ::Sleep(50);
    }
    if(!popup)
    {
        wprintf(L"# read-open: no menu was open within %d s\n", wait_seconds);
        return 3;
    }

    // Record what was found, because the obvious ways to identify the surface do
    // not work and it is worth not trying them again. Measured 2026-11-25 while
    // a desktop menu was genuinely on screen: the #32768 window reports
    // IsWindowVisible = 0 and GW_OWNER = NULL, so neither tells you which host
    // tracked it, or even that the menu is showing.
    // https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindow
    //
    // What does identify the surface is the content: a taskbar menu has "Task
    // Manager" and "Taskbar Settings" in it, a desktop menu has "View", "Sort by"
    // and "Refresh". Check that before trusting a reading.
    //
    // Stale windows are at least not a problem: with no menu open, FindWindow
    // finds no #32768 at all, so a successful read means a menu really was up.
    {
        HWND owner = ::GetWindow(popup, GW_OWNER);
        wchar_t owner_cls[128] {};
        if(owner)
            ::GetClassNameW(owner, owner_cls, 128);
        wprintf(L"# read-open: popup %p visible=%d owner=%p class=%s\n",
                static_cast<void *>(popup), ::IsWindowVisible(popup) ? 1 : 0,
                static_cast<void *>(owner),
                owner_cls[0] ? owner_cls : L"(none)");
    }

    HMENU menu = nullptr;
    for(int i = 0; i < 40 && !menu; i++)
    {
        DWORD_PTR handle = 0;
        ::SendMessageTimeoutW(popup, MN_GETHMENU, 0, 0, SMTO_ABORTIFHUNG, 2000, &handle);
        menu = reinterpret_cast<HMENU>(handle);
        if(!menu)
            ::Sleep(25);
    }
    if(!menu)
    {
        wprintf(L"# read-open: MN_GETHMENU gave nothing\n");
        return 3;
    }

    // Wait for the menu to stop changing before walking it. Without this, a
    // reading taken the instant the handle became readable caught the menu
    // mid-composition: 27 rows both times, but a different 27 - Shell's items
    // present and Explorer's Paste and New+ not yet. Measured 2026-11-25.
    int count = ::GetMenuItemCount(menu);
    int stable = 0;
    for(int i = 0; i < 120 && stable < 6; i++)
    {
        ::Sleep(25);
        const int now = ::GetMenuItemCount(menu);
        if(now == count)
            stable++;
        else
        {
            count = now;
            stable = 0;
        }
    }

    int total = 0;
    wprintf(L"# depth\tpos\tid\tkind\ttitle\tverb\n");
    walk(menu, nullptr, 0, 1, &total);
    wprintf(L"# top-level items: %d\n", ::GetMenuItemCount(menu));
    // The menu belongs to whoever opened it, so it is left exactly as found.
    return 0;
}

static POINT centre_of(HWND hwnd)
{
    RECT rc {};
    ::GetWindowRect(hwnd, &rc);
    POINT pt { rc.left + (rc.right - rc.left) / 2,
               rc.top + (rc.bottom - rc.top) / 2 };
    return pt;
}

static HWND find_desktop_view(void)
{
    // The desktop list view lives under Progman, or under a WorkerW once a
    // wallpaper slideshow has reparented it.
    HWND progman = ::FindWindowW(L"Progman", nullptr);
    HWND view = progman ? ::FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)
                        : nullptr;
    if(!view)
    {
        HWND worker = nullptr;
        while((worker = ::FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr)
        {
            view = ::FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr);
            if(view)
                break;
        }
    }
    if(!view)
        return nullptr;

    HWND list = ::FindWindowExW(view, nullptr, L"SysListView32", nullptr);
    return list ? list : view;
}

static int census_live_desktop(void)
{
    HWND target = find_desktop_view();
    return census_live_at(target, centre_of(target), L"desktop");
}

// ------------------------------------------------------------------------- main

int wmain(int argc, wchar_t **argv)
{
    ::SetConsoleOutputCP(CP_UTF8);
    (void)kSep;

    if(argc < 2)
    {
        wprintf(L"usage: menucensus <mode> [args]\n"
                L"  live-desktop            the desktop background menu Explorer shows\n"
                L"  read-open [seconds]     read a menu someone already opened, and leave it\n"
                L"                          (the taskbar and folder windows cannot be driven\n"
                L"                          from an agent shell; right-click them by hand)\n"
                L"  bg <folder>             handler contributions, folder background\n"
                L"  item <path> [<path>...] handler contributions, one or more items\n");
        return 1;
    }

    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if(FAILED(hr))
    {
        wprintf(L"# CoInitializeEx 0x%08X\n", hr);
        return 1;
    }

    int rc = 1;
    if(::lstrcmpiW(argv[1], L"live-desktop") == 0)
        rc = census_live_desktop();
    else if(::lstrcmpiW(argv[1], L"read-open") == 0)
        rc = census_read_open(argc > 2 ? ::_wtoi(argv[2]) : 20);
    else
        rc = census_static(argc - 1, argv + 1);

    ::CoUninitialize();
    return rc;
}
