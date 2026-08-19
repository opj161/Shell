// The context-menu handler is the first COM object this DLL has ever handed
// out, and DllCanUnloadNow now answers from com_object_count. Get that count
// wrong downward and COM may unload shell.dll while a host still holds an
// IContextMenu; wrong upward and the DLL is pinned forever in every process
// that ever raised a menu. Neither shows up in a build, and neither is
// comfortable to discover inside someone's Explorer, so the reference counting
// is exercised directly here.

#include <windows.h>
#include "test.h"
#include "../dll/src/Include/ShellExt.h"

using Nilesoft::Shell::com_object_count;
using Nilesoft::Shell::ShellExtCapture;
using Nilesoft::Shell::ShellExtFactory;

namespace
{
	long live() { return com_object_count.load(); }

	// Returns a factory holding exactly one reference.
	IClassFactory *make_factory()
	{
		auto f = new ShellExtFactory();
		IClassFactory *out = nullptr;
		f->QueryInterface(IID_IClassFactory, reinterpret_cast<void **>(&out));
		f->Release();                 // the QueryInterface above took its own
		return out;
	}
}

TEST(shellext, handler_answers_both_interfaces_and_refuses_others)
{
	auto before = live();
	auto factory = make_factory();
	CHECK(factory != nullptr);

	IShellExtInit *init = nullptr;
	CHECK(factory->CreateInstance(nullptr, IID_IShellExtInit,
								  reinterpret_cast<void **>(&init)) == S_OK);
	CHECK(init != nullptr);

	// The host QIs across from IShellExtInit to IContextMenu on the same object.
	IContextMenu *cm = nullptr;
	CHECK(init->QueryInterface(IID_IContextMenu, reinterpret_cast<void **>(&cm)) == S_OK);
	CHECK(cm != nullptr);

	// Anything else must be refused rather than handed something that faults on
	// first use.
	void *nope = nullptr;
	CHECK(init->QueryInterface(IID_IShellIconOverlayIdentifier, &nope) == E_NOINTERFACE);
	CHECK(nope == nullptr);

	cm->Release();
	init->Release();
	factory->Release();
	CHECK_EQ(live(), before);
}

TEST(shellext, aggregation_is_refused)
{
	auto factory = make_factory();
	IUnknown *outer = reinterpret_cast<IUnknown *>(1);   // only tested against null
	void *ppv = nullptr;
	CHECK(factory->CreateInstance(outer, IID_IShellExtInit, &ppv) == CLASS_E_NOAGGREGATION);
	CHECK(ppv == nullptr);
	factory->Release();
}

TEST(shellext, every_live_object_is_counted_and_the_count_returns_to_zero)
{
	auto before = live();
	auto factory = make_factory();
	CHECK_EQ(live(), before + 1);

	IShellExtInit *a = nullptr;
	IShellExtInit *b = nullptr;
	factory->CreateInstance(nullptr, IID_IShellExtInit, reinterpret_cast<void **>(&a));
	factory->CreateInstance(nullptr, IID_IShellExtInit, reinterpret_cast<void **>(&b));
	CHECK_EQ(live(), before + 3);

	// An extra reference must not change the object count - only the last
	// Release destroys the object.
	a->AddRef();
	CHECK_EQ(live(), before + 3);
	a->Release();
	CHECK_EQ(live(), before + 3);

	a->Release();
	CHECK_EQ(live(), before + 2);
	b->Release();
	CHECK_EQ(live(), before + 1);
	factory->Release();
	CHECK_EQ(live(), before);
}

TEST(shellext, lockserver_pins_and_unpins)
{
	auto before = live();
	auto factory = make_factory();
	factory->LockServer(TRUE);
	CHECK_EQ(live(), before + 2);
	factory->LockServer(FALSE);
	CHECK_EQ(live(), before + 1);
	factory->Release();
	CHECK_EQ(live(), before);
}

TEST(shellext, an_empty_capture_is_never_reported_as_a_match)
{
	ShellExtCapture::clear();
	auto menu = ::CreatePopupMenu();
	auto other = ::CreatePopupMenu();

	ShellExtCapture::capture(nullptr, false);
	ShellExtCapture::bind(menu);

	// Nothing was captured, so there is nothing to hand back even for the menu
	// it was bound to. The caller must fall through to its existing behaviour.
	CHECK(!static_cast<bool>(ShellExtCapture::match(menu)));
	CHECK(!static_cast<bool>(ShellExtCapture::match(other)));
	CHECK(!static_cast<bool>(ShellExtCapture::match(nullptr)));

	::DestroyMenu(menu);
	::DestroyMenu(other);
	ShellExtCapture::clear();
}

// The regression this exists to prevent: an earlier version validated only the
// items against the menu handle and let the folder and the background flag
// through raw. A background right-click in Explorer therefore left both set,
// and the next address-bar, inline-rename, title-bar or Win+X menu - none of
// which have an IShellBrowser, so all of which reach this path - was built as a
// background click in whatever folder had last been clicked in.
TEST(shellext, a_capture_belongs_to_one_menu_and_contributes_nothing_to_others)
{
	ShellExtCapture::clear();
	auto mine = ::CreatePopupMenu();
	auto other = ::CreatePopupMenu();

	PIDLIST_ABSOLUTE desktop = nullptr;
	if(SUCCEEDED(::SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &desktop)))
	{
		ShellExtCapture::capture(nullptr, true);      // a background click
		ShellExtCapture::capture_folder(desktop);
		ShellExtCapture::bind(mine);

		auto ok = ShellExtCapture::match(mine);
		CHECK(static_cast<bool>(ok));
		CHECK(ok.folder != nullptr);
		CHECK(ok.background == true);

		// Every field, not just the items.
		auto no = ShellExtCapture::match(other);
		CHECK(!static_cast<bool>(no));
		CHECK(no.items == nullptr);
		CHECK(no.folder == nullptr);
		CHECK(no.background == false);

		auto none = ShellExtCapture::match(nullptr);
		CHECK(none.folder == nullptr);
		CHECK(none.background == false);

		::CoTaskMemFree(desktop);
	}

	ShellExtCapture::clear();
	::DestroyMenu(mine);
	::DestroyMenu(other);
}

TEST(shellext, the_folder_id_list_is_cloned_and_released)
{
	ShellExtCapture::clear();
	auto menu = ::CreatePopupMenu();

	PIDLIST_ABSOLUTE desktop = nullptr;
	if(SUCCEEDED(::SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &desktop)))
	{
		ShellExtCapture::capture(nullptr, true);
		ShellExtCapture::capture_folder(desktop);
		ShellExtCapture::bind(menu);

		CHECK(ShellExtCapture::folder != nullptr);
		// The host owns the pidl it passes to Initialize and it is not valid
		// afterwards, so holding the same pointer would be a dangling read.
		CHECK(ShellExtCapture::folder != desktop);
		CHECK(ShellExtCapture::background == true);

		ShellExtCapture::clear();
		CHECK(ShellExtCapture::folder == nullptr);
		CHECK(ShellExtCapture::hmenu == nullptr);
		CHECK(ShellExtCapture::background == false);

		::CoTaskMemFree(desktop);
	}

	::DestroyMenu(menu);
}

TEST(shellext, end_to_end_initialize_and_query_context_menu)
{
	ShellExtCapture::clear();
	auto factory = make_factory();
	CHECK(factory != nullptr);

	IShellExtInit *init = nullptr;
	CHECK(factory->CreateInstance(nullptr, IID_IShellExtInit, reinterpret_cast<void **>(&init)) == S_OK);
	CHECK(init != nullptr);

	PIDLIST_ABSOLUTE desktop = nullptr;
	auto hr_desktop = ::SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &desktop);
	CHECK(SUCCEEDED(hr_desktop));
	if(SUCCEEDED(hr_desktop))
	{
		CHECK(init->Initialize(desktop, nullptr, nullptr) == S_OK);

		IContextMenu *cm = nullptr;
		CHECK(init->QueryInterface(IID_IContextMenu, reinterpret_cast<void **>(&cm)) == S_OK);
		CHECK(cm != nullptr);

		auto menu = ::CreatePopupMenu();
		// QueryContextMenu binds the menu and returns 0 (zero items inserted)
		auto hr = cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL);
		CHECK(SUCCEEDED(hr));
		CHECK_EQ(HRESULT_CODE(hr), 0);

		auto matched = ShellExtCapture::match(menu);
		CHECK(static_cast<bool>(matched));
		CHECK(matched.folder != nullptr);
		CHECK(matched.background == true);

		::DestroyMenu(menu);
		cm->Release();
		::CoTaskMemFree(desktop);
	}

	init->Release();
	factory->Release();
	ShellExtCapture::clear();
}

TEST(shellext, end_to_end_selected_file_data_object_capture)
{
	ShellExtCapture::clear();
	auto factory = make_factory();
	CHECK(factory != nullptr);

	IShellExtInit *init = nullptr;
	CHECK(factory->CreateInstance(nullptr, IID_IShellExtInit, reinterpret_cast<void **>(&init)) == S_OK);
	CHECK(init != nullptr);

	PIDLIST_ABSOLUTE desktop = nullptr;
	if(SUCCEEDED(::SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &desktop)))
	{
		IShellItem *item = nullptr;
		if(SUCCEEDED(::SHCreateItemFromIDList(desktop, IID_PPV_ARGS(&item))))
		{
			IDataObject *dto = nullptr;
			if(SUCCEEDED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dto))))
			{
				CHECK(init->Initialize(nullptr, dto, 0) == S_OK);

				IContextMenu *cm = nullptr;
				CHECK(init->QueryInterface(IID_IContextMenu, reinterpret_cast<void **>(&cm)) == S_OK);
				CHECK(cm != nullptr);

				auto menu = ::CreatePopupMenu();
				auto hr = cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL);
				CHECK(SUCCEEDED(hr));
				CHECK_EQ(HRESULT_CODE(hr), 0);

				auto matched = ShellExtCapture::match(menu);
				CHECK(static_cast<bool>(matched));
				CHECK(matched.items != nullptr);
				CHECK(matched.folder == nullptr);
				CHECK(matched.background == false);

				::DestroyMenu(menu);
				cm->Release();
				dto->Release();
			}
			item->Release();
		}
		::CoTaskMemFree(desktop);
	}

	init->Release();
	factory->Release();
	ShellExtCapture::clear();
}