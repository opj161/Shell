// Menu census probe - a reproducible "before" reading for the 26H2 upgrade.
//
// Two independent censuses, because they answer different questions:
//
//   static  - what the registered shell extensions contribute through
//             IContextMenu::QueryContextMenu, read out of process with no UI at
//             all. Deterministic and repeatable. Shell itself contributes zero
//             items this way by design, so this measures the *input* to Shell's
//             composition: exactly the set that 26H2's new File Explorer verbs
//             would land in.
//               IShellFolder::CreateViewObject gives a folder's background menu,
//               GetUIObjectOf gives one or more items' menu.
//               https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellfolder-createviewobject
//               https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellfolder-getuiobjectof
//
//   live    - what Explorer actually shows. WM_CONTEXTMENU is posted to the
//             desktop view, then the popup's own window is asked for its HMENU
//             with MN_GETHMENU and the menu is read with the documented two-call
//             GetMenuItemInfo pattern. This is the only reading that proves
//             Shell composed anything.
//               MN_GETHMENU: "Retrieves the menu handle for the specified menu
//               window." https://learn.microsoft.com/en-us/windows/win32/menurc/mn-gethmenu
//               GetMenuItemCount returns -1 on failure, so the count is held in
//               an int. https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmenuitemcount
//
// Output is one tab-separated row per item so two runs diff cleanly.

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <objbase.h>
#include <stdio.h>

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

        // The verb is what identifies a handler's command across releases, so a
        // new 26H2 command is recognisable even if its display name changed.
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
    // argv[0] is "bg" or "item"; the rest are paths.
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
        // One or more items that share a parent folder.
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

        PCUITEMID_CHILD *children = static_cast<PCUITEMID_CHILD *>(
            ::calloc(static_cast<size_t>(n), sizeof(PCUITEMID_CHILD)));
        PIDLIST_ABSOLUTE *absolutes = static_cast<PIDLIST_ABSOLUTE *>(
            ::calloc(static_cast<size_t>(n), sizeof(PIDLIST_ABSOLUTE)));
        children[0] = child;
        absolutes[0] = first;

        int have = 1;
        for(int i = 1; i < n; i++)
        {
            PIDLIST_ABSOLUTE p = nullptr;
            if(FAILED(::SHParseDisplayName(argv[1 + i], nullptr, &p, 0, nullptr)))
                continue;
            absolutes[have] = p;
            children[have] = ::ILFindLastID(p);
            have++;
        }

        hr = parent->GetUIObjectOf(nullptr, static_cast<UINT>(have), children,
                                   IID_IContextMenu, nullptr,
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

    const int top = ::GetMenuItemCount(menu);
    wprintf(L"# top-level items: %d\n", top);

    ::DestroyMenu(menu);
    cm->Release();
    return 0;
}

// ------------------------------------------------------------------ live census

static HWND find_desktop_view()
{
    // The desktop list view lives under Progman, or under a WorkerW when
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

static int census_live(void)
{
    HWND target = find_desktop_view();
    if(!target)
    {
        wprintf(L"# no desktop view found\n");
        return 2;
    }

    RECT rc {};
    ::GetWindowRect(target, &rc);
    // A point well inside the desktop but away from any icon column.
    const int x = rc.left + (rc.right - rc.left) / 2;
    const int y = rc.top + (rc.bottom - rc.top) / 2;

    // Explorer answers WM_CONTEXTMENU by calling TrackPopupMenu on its own
    // thread, which is the call Shell hooks - so no synthetic input is needed.
    ::PostMessageW(target, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(target),
                   MAKELPARAM(x, y));

    HWND popup = nullptr;
    for(int i = 0; i < 120 && !popup; i++)
    {
        popup = ::FindWindowW(L"#32768", nullptr);
        if(!popup)
            ::Sleep(25);
    }
    if(!popup)
    {
        wprintf(L"# no popup window appeared\n");
        return 3;
    }

    ::Sleep(250); // let the whole menu be built before reading it

    DWORD_PTR handle = 0;
    ::SendMessageTimeoutW(popup, MN_GETHMENU, 0, 0, SMTO_ABORTIFHUNG, 2000, &handle);
    HMENU menu = reinterpret_cast<HMENU>(handle);
    if(!menu)
    {
        wprintf(L"# MN_GETHMENU gave nothing\n");
        ::PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0);
        return 3;
    }

    int total = 0;
    wprintf(L"# depth\tpos\tid\tkind\ttitle\tverb\n");
    walk(menu, nullptr, 0, 1, &total);
    wprintf(L"# top-level items: %d\n", ::GetMenuItemCount(menu));

    // Dismiss it again rather than leaving a menu open on the user's desktop.
    ::PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0);
    ::Sleep(100);
    ::PostMessageW(popup, WM_CLOSE, 0, 0);
    return 0;
}

// ------------------------------------------------------------------------- main

int wmain(int argc, wchar_t **argv)
{
    ::SetConsoleOutputCP(CP_UTF8);
    (void)kSep;

    if(argc < 2)
    {
        wprintf(L"usage: menucensus bg <folder> | item <path> [<path>...] | live-desktop\n");
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
        rc = census_live();
    else
        rc = census_static(argc - 1, argv + 1);

    ::CoUninitialize();
    return rc;
}
