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

// ---------------------------------------------------------------------------
// Capture ownership.
//
// Three bugs live here in principle, and all three are invisible until two
// menus exist at once: a pending capture that belonged to the process rather
// than to the handler collecting it, a completed capture that any closing popup
// could destroy, and a match() that handed back raw pointers into the registry
// after dropping the lock.
// ---------------------------------------------------------------------------

namespace
{
	// A handler, driven the way a host drives it.
	struct Handler
	{
		IShellExtInit *init = nullptr;
		IContextMenu *cm = nullptr;

		Handler()
		{
			auto f = make_factory();
			f->CreateInstance(nullptr, IID_IShellExtInit, reinterpret_cast<void **>(&init));
			if(init)
				init->QueryInterface(IID_IContextMenu, reinterpret_cast<void **>(&cm));
			f->Release();
		}

		~Handler()
		{
			if(cm) cm->Release();
			if(init) init->Release();
		}

		bool valid() const { return init && cm; }
	};

	struct Pidl
	{
		PIDLIST_ABSOLUTE p = nullptr;
		explicit Pidl(REFKNOWNFOLDERID id) { ::SHGetKnownFolderIDList(id, 0, nullptr, &p); }
		~Pidl() { if(p) ::CoTaskMemFree(p); }
		explicit operator bool() const { return p != nullptr; }
	};

	struct Menu
	{
		HMENU h = ::CreatePopupMenu();
		~Menu() { if(h) ::DestroyMenu(h); }
	};
}

TEST(shellext, an_empty_capture_is_never_reported_as_a_match)
{
	ShellExtCapture::clear_all();
	Menu menu, other;

	Handler h;
	CHECK(h.valid());
	if(!h.valid()) return;

	// Neither a data object nor a folder: nothing to offer.
	h.init->Initialize(nullptr, nullptr, nullptr);
	h.cm->QueryContextMenu(menu.h, 0, 1, 0x7FFF, CMF_NORMAL);

	CHECK(!static_cast<bool>(ShellExtCapture::match(menu.h)));
	CHECK(!static_cast<bool>(ShellExtCapture::match(other.h)));
	CHECK(!static_cast<bool>(ShellExtCapture::match(nullptr)));
	CHECK(!ShellExtCapture::has(menu.h));

	ShellExtCapture::clear_all();
}

// The regression this exists to prevent: an earlier version validated only the
// items against the menu handle and let the folder and the background flag
// through raw. A background right-click in Explorer therefore left both set,
// and the next address-bar, inline-rename, title-bar or Win+X menu - none of
// which have an IShellBrowser, so all of which reach this path - was built as a
// background click in whatever folder had last been clicked in.
TEST(shellext, a_capture_belongs_to_one_menu_and_contributes_nothing_to_others)
{
	ShellExtCapture::clear_all();
	Menu mine, other;
	Pidl desktop(FOLDERID_Desktop);
	CHECK(static_cast<bool>(desktop));
	if(!desktop) return;

	Handler h;
	if(!h.valid()) return;

	h.init->Initialize(desktop.p, nullptr, nullptr);   // a background click
	h.cm->QueryContextMenu(mine.h, 0, 1, 0x7FFF, CMF_NORMAL);

	auto ok = ShellExtCapture::match(mine.h);
	CHECK(static_cast<bool>(ok));
	CHECK(ok.folder != nullptr);
	CHECK(ok.background == true);

	// Every field, not just the items.
	auto no = ShellExtCapture::match(other.h);
	CHECK(!static_cast<bool>(no));
	CHECK(!static_cast<bool>(no.items));
	CHECK(no.folder == nullptr);
	CHECK(no.background == false);

	auto none = ShellExtCapture::match(nullptr);
	CHECK(none.folder == nullptr);
	CHECK(none.background == false);

	ShellExtCapture::clear_all();
}

TEST(shellext, the_folder_id_list_is_cloned_and_released)
{
	ShellExtCapture::clear_all();
	Menu menu;
	Pidl desktop(FOLDERID_Desktop);
	if(!desktop) return;

	Handler h;
	if(!h.valid()) return;

	h.init->Initialize(desktop.p, nullptr, nullptr);
	h.cm->QueryContextMenu(menu.h, 0, 1, 0x7FFF, CMF_NORMAL);

	auto matched = ShellExtCapture::match(menu.h);
	CHECK(static_cast<bool>(matched));
	CHECK(matched.folder != nullptr);
	// The host owns the pidl it passes to Initialize and it is not valid
	// afterwards, so holding the same pointer would be a dangling read. The
	// caller's copy is its own clone again, not the registry's.
	CHECK(matched.folder.get() != desktop.p);

	ShellExtCapture::clear_all();
}

TEST(shellext, two_handlers_do_not_overwrite_each_other)
{
	ShellExtCapture::clear_all();
	Menu menu_a, menu_b;
	Pidl desktop(FOLDERID_Desktop);
	Pidl documents(FOLDERID_Documents);
	if(!desktop || !documents) return;

	Handler a, b;
	if(!a.valid() || !b.valid()) return;

	// A host is free to interleave these. With one process-wide pending slot,
	// B's Initialize erased A's selection and A's menu was built from B's.
	a.init->Initialize(desktop.p, nullptr, nullptr);
	b.init->Initialize(documents.p, nullptr, nullptr);

	a.cm->QueryContextMenu(menu_a.h, 0, 1, 0x7FFF, CMF_NORMAL);
	b.cm->QueryContextMenu(menu_b.h, 0, 1, 0x7FFF, CMF_NORMAL);

	auto ma = ShellExtCapture::match(menu_a.h);
	auto mb = ShellExtCapture::match(menu_b.h);

	CHECK(static_cast<bool>(ma));
	CHECK(static_cast<bool>(mb));
	CHECK(ma.folder != nullptr);
	CHECK(mb.folder != nullptr);

	// Different folders: each menu kept its own handler's capture.
	CHECK(ma.folder.get() != mb.folder.get());
	CHECK(!::ILIsEqual(ma.folder.get(), mb.folder.get()));

	ShellExtCapture::clear_all();
}

TEST(shellext, two_bound_menus_coexist_and_clear_is_per_menu)
{
	ShellExtCapture::clear_all();
	Menu menu_a, menu_b;
	Pidl desktop(FOLDERID_Desktop);
	if(!desktop) return;

	Handler a, b;
	if(!a.valid() || !b.valid()) return;

	a.init->Initialize(desktop.p, nullptr, nullptr);
	a.cm->QueryContextMenu(menu_a.h, 0, 1, 0x7FFF, CMF_NORMAL);
	b.init->Initialize(desktop.p, nullptr, nullptr);
	b.cm->QueryContextMenu(menu_b.h, 0, 1, 0x7FFF, CMF_NORMAL);

	CHECK_EQ(ShellExtCapture::bound_count(), size_t(2));

	// One popup closing must leave the other window's menu alone. The old
	// finaliser cleared the whole registry.
	ShellExtCapture::clear(menu_a.h);

	CHECK(!ShellExtCapture::has(menu_a.h));
	CHECK(ShellExtCapture::has(menu_b.h));
	CHECK(static_cast<bool>(ShellExtCapture::match(menu_b.h)));

	ShellExtCapture::clear_all();
	CHECK_EQ(ShellExtCapture::bound_count(), size_t(0));
}

TEST(shellext, a_match_outlives_the_registry_entry)
{
	ShellExtCapture::clear_all();
	Menu menu;
	Pidl desktop(FOLDERID_Desktop);
	if(!desktop) return;

	Handler h;
	if(!h.valid()) return;

	h.init->Initialize(desktop.p, nullptr, nullptr);
	h.cm->QueryContextMenu(menu.h, 0, 1, 0x7FFF, CMF_NORMAL);

	auto matched = ShellExtCapture::match(menu.h);
	CHECK(static_cast<bool>(matched));

	// match() used to return pointers into the registry, so this was a
	// use-after-free waiting for a second thread.
	ShellExtCapture::clear_all();

	CHECK(static_cast<bool>(matched));
	CHECK(matched.folder != nullptr);

	IShellItem *item = nullptr;
	CHECK(SUCCEEDED(::SHCreateItemFromIDList(matched.folder.get(), IID_PPV_ARGS(&item))));
	if(item) item->Release();
}

TEST(shellext, end_to_end_initialize_and_query_context_menu)
{
	ShellExtCapture::clear_all();
	Menu menu;
	Pidl desktop(FOLDERID_Desktop);
	CHECK(static_cast<bool>(desktop));
	if(!desktop) return;

	Handler h;
	CHECK(h.valid());
	if(!h.valid()) return;

	CHECK(h.init->Initialize(desktop.p, nullptr, nullptr) == S_OK);

	// QueryContextMenu binds the menu and returns 0 (zero items inserted).
	auto hr = h.cm->QueryContextMenu(menu.h, 0, 1, 0x7FFF, CMF_NORMAL);
	CHECK(SUCCEEDED(hr));
	CHECK_EQ(HRESULT_CODE(hr), 0);

	auto matched = ShellExtCapture::match(menu.h);
	CHECK(static_cast<bool>(matched));
	CHECK(matched.folder != nullptr);
	CHECK(matched.background == true);

	ShellExtCapture::clear_all();
}

TEST(shellext, a_default_only_query_binds_nothing)
{
	ShellExtCapture::clear_all();
	Menu menu;
	Pidl desktop(FOLDERID_Desktop);
	if(!desktop) return;

	Handler h;
	if(!h.valid()) return;

	h.init->Initialize(desktop.p, nullptr, nullptr);
	// A double-click asking for the default verb, not a menu anyone will see.
	h.cm->QueryContextMenu(menu.h, 0, 1, 0x7FFF, CMF_DEFAULTONLY);

	CHECK(!ShellExtCapture::has(menu.h));
	CHECK_EQ(ShellExtCapture::bound_count(), size_t(0));

	ShellExtCapture::clear_all();
}

TEST(shellext, end_to_end_selected_file_data_object_capture)
{
	ShellExtCapture::clear_all();
	Menu menu;
	Pidl desktop(FOLDERID_Desktop);
	if(!desktop) return;

	Handler h;
	if(!h.valid()) return;

	IShellItem *item = nullptr;
	if(SUCCEEDED(::SHCreateItemFromIDList(desktop.p, IID_PPV_ARGS(&item))))
	{
		IDataObject *dto = nullptr;
		if(SUCCEEDED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dto))))
		{
			CHECK(h.init->Initialize(nullptr, dto, 0) == S_OK);

			auto hr = h.cm->QueryContextMenu(menu.h, 0, 1, 0x7FFF, CMF_NORMAL);
			CHECK(SUCCEEDED(hr));
			CHECK_EQ(HRESULT_CODE(hr), 0);

			auto matched = ShellExtCapture::match(menu.h);
			CHECK(static_cast<bool>(matched));
			CHECK(static_cast<bool>(matched.items));
			CHECK(matched.folder == nullptr);
			CHECK(matched.background == false);

			// A usable interface, not just a non-null pointer.
			if(matched.items)
			{
				DWORD count = 0;
				CHECK(SUCCEEDED(matched.items->GetCount(&count)));
				CHECK(count > 0);
			}

			dto->Release();
		}
		item->Release();
	}

	ShellExtCapture::clear_all();
}

// Interface pointers must be marshaled when passed between apartments, and the
// registry is reachable from every thread in the process. A capture taken on
// one single-threaded apartment has to still be callable on another.
TEST(shellext, a_capture_taken_on_one_apartment_is_usable_on_another)
{
	ShellExtCapture::clear_all();
	Menu menu;

	struct Shared
	{
		HMENU menu;
		HANDLE captured_evt;
		HANDLE release_evt;
		bool captured;
		bool resolved;
		bool callable;
	} shared{ menu.h, ::CreateEventW(nullptr, TRUE, FALSE, nullptr),
			  ::CreateEventW(nullptr, TRUE, FALSE, nullptr), false, false, false };

	// Apartment A takes the capture and, like the host thread it stands in for,
	// stays alive while the menu is used. An apartment that shuts down takes its
	// objects with it, agile reference or not.
	auto producer = [](void *p) -> DWORD
	{
		auto *s = static_cast<Shared *>(p);
		if(FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
		{
			::SetEvent(s->captured_evt);
			return 0;
		}

		{
			Pidl desktop(FOLDERID_Desktop);
			IShellItem *item = nullptr;
			if(desktop && SUCCEEDED(::SHCreateItemFromIDList(desktop.p, IID_PPV_ARGS(&item))))
			{
				IDataObject *dto = nullptr;
				if(SUCCEEDED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dto))))
				{
					Handler h;
					if(h.valid())
					{
						h.init->Initialize(nullptr, dto, 0);
						h.cm->QueryContextMenu(s->menu, 0, 1, 0x7FFF, CMF_NORMAL);
						s->captured = ShellExtCapture::has(s->menu);
					}
					dto->Release();
				}
				item->Release();
			}
		}

		::SetEvent(s->captured_evt);

		// A single-threaded apartment has to pump while it waits, or the
		// cross-apartment call the consumer is about to make never arrives.
		for(;;)
		{
			DWORD index = 0;
			::SetLastError(ERROR_SUCCESS);
			auto hr = ::CoWaitForMultipleHandles(0, 20000, 1, &s->release_evt, &index);
			if(hr != RPC_S_CALLPENDING)
				break;
		}

		::CoUninitialize();
		return 0;
	};

	// Apartment B uses it.
	auto consumer = [](void *p) -> DWORD
	{
		auto *s = static_cast<Shared *>(p);
		if(FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
			return 0;

		{
			auto matched = ShellExtCapture::match(s->menu);
			s->resolved = static_cast<bool>(matched.items);
			if(matched.items)
			{
				DWORD count = 0;
				s->callable = SUCCEEDED(matched.items->GetCount(&count)) && count > 0;
			}
		}

		::CoUninitialize();
		return 0;
	};

	auto producer_thread = ::CreateThread(nullptr, 0, producer, &shared, 0, nullptr);
	::WaitForSingleObject(shared.captured_evt, 20000);

	CHECK(shared.captured);

	if(shared.captured)
	{
		if(auto t = ::CreateThread(nullptr, 0, consumer, &shared, 0, nullptr))
		{
			::WaitForSingleObject(t, 20000);
			::CloseHandle(t);
		}
		CHECK(shared.resolved);
		CHECK(shared.callable);
	}

	::SetEvent(shared.release_evt);
	if(producer_thread)
	{
		::WaitForSingleObject(producer_thread, 20000);
		::CloseHandle(producer_thread);
	}
	::CloseHandle(shared.captured_evt);
	::CloseHandle(shared.release_evt);

	ShellExtCapture::clear_all();
}

TEST(shellext, active_captures_reporting)
{
	ShellExtCapture::clear_all();
	CHECK(!ShellExtCapture::has_active_captures());

	Menu menu;
	Pidl desktop(FOLDERID_Desktop);
	if(!desktop) return;

	Handler h;
	if(!h.valid()) return;

	// A capture in progress belongs to the handler, so it is not yet something
	// that would keep the DLL loaded.
	CHECK(h.init->Initialize(desktop.p, nullptr, nullptr) == S_OK);
	CHECK(!ShellExtCapture::has_active_captures());

	h.cm->QueryContextMenu(menu.h, 0, 1, 0x7FFF, CMF_NORMAL);
	CHECK(ShellExtCapture::has_active_captures());

	ShellExtCapture::clear(menu.h);
	CHECK(!ShellExtCapture::has_active_captures());
}
