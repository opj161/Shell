#include "test.h"

#include <windows.h>
#include "..\dll\src\Include\ExplorerCommandCatalog.h"

using namespace Nilesoft::Shell;

TEST(explorer_command, star_matches_files_only)
{
	CHECK(explorer_command_type_matches(L"*", ExplorerCommandKind::File));
	CHECK(!explorer_command_type_matches(L"*", ExplorerCommandKind::Directory));
	CHECK(!explorer_command_type_matches(L"*", ExplorerCommandKind::DirectoryBackground));
	CHECK(!explorer_command_type_matches(L"*", ExplorerCommandKind::Drive));
}

TEST(explorer_command, directory_and_drive_types)
{
	CHECK(explorer_command_type_matches(L"Directory", ExplorerCommandKind::Directory));
	CHECK(explorer_command_type_matches(L"Directory\\Background", ExplorerCommandKind::DirectoryBackground));
	CHECK(explorer_command_type_matches(L"Directory/Background", ExplorerCommandKind::DirectoryBackground));
	CHECK(explorer_command_type_matches(L"Drive", ExplorerCommandKind::Drive));
	CHECK(!explorer_command_type_matches(L"Directory", ExplorerCommandKind::File));
}

TEST(explorer_command, leading_dot_type_matches_files)
{
	// desktop4:ItemType/@Type "must begin with a period or be the wildcard *"
	// https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-desktop4-itemtype
	CHECK(explorer_command_type_matches(L".zip", ExplorerCommandKind::File));
	CHECK(explorer_command_type_matches(L".7z", ExplorerCommandKind::File));
	CHECK(!explorer_command_type_matches(L".zip", ExplorerCommandKind::Directory));
}

TEST(explorer_command, parse_guid_accepts_braced_and_plain)
{
	GUID a{}, b{};
	CHECK(parse_guid(L"{CAE3F1D4-7765-4D98-A060-52CD14D56EAB}", a));
	CHECK(parse_guid(L"CAE3F1D4-7765-4D98-A060-52CD14D56EAB", b));
	CHECK(InlineIsEqualGUID(a, b));
}

TEST(explorer_command, parse_nanazip_style_manifest)
{
	const wchar_t *xml =
		L"<desktop4:Extension Category=\"windows.fileExplorerContextMenus\">"
		L"<desktop4:FileExplorerContextMenus>"
		L"<desktop4:ItemType Type=\"*\">"
		L"<desktop4:Verb Id=\"0000NanaZipShellExtension\" Clsid=\"CAE3F1D4-7765-4D98-A060-52CD14D56EAB\" />"
		L"</desktop4:ItemType>"
		L"<desktop5:ItemType Type=\"Directory\">"
		L"<desktop5:Verb Id=\"0000NanaZipShellExtension\" Clsid=\"CAE3F1D4-7765-4D98-A060-52CD14D56EAB\" />"
		L"</desktop5:ItemType>"
		L"<desktop10:ItemType Type=\"Drive\">"
		L"<desktop10:Verb Id=\"0000NanaZipShellExtension\" Clsid=\"CAE3F1D4-7765-4D98-A060-52CD14D56EAB\" />"
		L"</desktop10:ItemType>"
		L"</desktop4:FileExplorerContextMenus>"
		L"</desktop4:Extension>";

	std::vector<ExplorerCommandRegistration> regs;
	auto parsed = parse_file_explorer_context_menus(xml, regs);
	CHECK(parsed);
	if(!parsed || regs.empty())
		return;
	CHECK_EQ(regs.size(), size_t(1));
	CHECK_EQ(regs[0].types.size(), size_t(3));
	std::vector<ExplorerCommandKind> files{ ExplorerCommandKind::File };
	std::vector<ExplorerCommandKind> dirs{ ExplorerCommandKind::Directory };
	std::vector<ExplorerCommandKind> drives{ ExplorerCommandKind::Drive };
	std::vector<ExplorerCommandKind> bg{ ExplorerCommandKind::DirectoryBackground };
	CHECK(explorer_command_matches_any(regs[0], files));
	CHECK(explorer_command_matches_any(regs[0], dirs));
	CHECK(explorer_command_matches_any(regs[0], drives));
	CHECK(!explorer_command_matches_any(regs[0], bg));
}

TEST(explorer_command, type_attribute_is_not_itemtype_suffix)
{
	const wchar_t *xml =
		L"<FileExplorerContextMenus>"
		L"<ItemType Type=\"Directory\">"
		L"<Verb Clsid=\"{CAE3F1D4-7765-4D98-A060-52CD14D56EAB}\" />"
		L"</ItemType>"
		L"</FileExplorerContextMenus>";

	std::vector<ExplorerCommandRegistration> regs;
	auto parsed = parse_file_explorer_context_menus(xml, regs);
	CHECK(parsed);
	if(!parsed || regs.empty())
		return;
	CHECK_EQ(regs[0].types.size(), size_t(1));
	std::vector<ExplorerCommandKind> dirs{ ExplorerCommandKind::Directory };
	std::vector<ExplorerCommandKind> files{ ExplorerCommandKind::File };
	CHECK(explorer_command_matches_any(regs[0], dirs));
	CHECK(!explorer_command_matches_any(regs[0], files));
}

TEST(explorer_command, pretty_printed_type_with_spaces)
{
	const wchar_t *xml =
		L"<desktop4:FileExplorerContextMenus>\n"
		L"  <desktop4:ItemType Type = \"*\">\n"
		L"    <desktop4:Verb Clsid=\"CAE3F1D4-7765-4D98-A060-52CD14D56EAB\" />\n"
		L"  </desktop4:ItemType>\n"
		L"</desktop4:FileExplorerContextMenus>";

	std::vector<ExplorerCommandRegistration> regs;
	auto parsed = parse_file_explorer_context_menus(xml, regs);
	CHECK(parsed);
	if(!parsed || regs.empty())
		return;
	std::vector<ExplorerCommandKind> files{ ExplorerCommandKind::File };
	CHECK(explorer_command_matches_any(regs[0], files));
}

TEST(explorer_command, category_attribute_is_not_a_registration)
{
	std::vector<ExplorerCommandRegistration> regs;
	CHECK(!parse_file_explorer_context_menus(
		L"<desktop4:Extension Category=\"windows.fileExplorerContextMenus\"></desktop4:Extension>",
		regs));
	CHECK(regs.empty());
}

TEST(explorer_command, missing_section_is_not_a_registration)
{
	std::vector<ExplorerCommandRegistration> regs;
	CHECK(!parse_file_explorer_context_menus(L"<Package></Package>", regs));
	CHECK(regs.empty());
}

TEST(explorer_command, same_hash_and_type_is_already_represented)
{
	ExplorerCommandIdentity have;
	have.hash = 0x1234;
	have.type = 1;
	std::vector<ExplorerCommandIdentity> accepted{ have };

	ExplorerCommandIdentity candidate;
	candidate.hash = 0x1234;
	candidate.type = 1;
	CHECK(explorer_command_already_represented(candidate, accepted));
}

TEST(explorer_command, same_hash_different_type_is_not_a_duplicate)
{
	ExplorerCommandIdentity have;
	have.hash = 0x1234;
	have.type = 0;
	std::vector<ExplorerCommandIdentity> accepted{ have };

	ExplorerCommandIdentity candidate;
	candidate.hash = 0x1234;
	candidate.type = 1;
	CHECK(!explorer_command_already_represented(candidate, accepted));
}

TEST(explorer_command, clsid_match_skips_before_title)
{
	GUID clsid{};
	CHECK(parse_guid(L"{CAE3F1D4-7765-4D98-A060-52CD14D56EAB}", clsid));

	ExplorerCommandIdentity have;
	have.clsid = clsid;
	std::vector<ExplorerCommandIdentity> accepted{ have };

	ExplorerCommandIdentity candidate;
	candidate.clsid = clsid;
	CHECK(explorer_command_already_represented(candidate, accepted));
}

TEST(explorer_command, guid_null_is_not_an_identity)
{
	ExplorerCommandIdentity have;
	have.clsid = GUID_NULL;
	have.canonical = GUID_NULL;
	have.hash = 0;
	std::vector<ExplorerCommandIdentity> accepted{ have };

	ExplorerCommandIdentity candidate;
	candidate.clsid = GUID_NULL;
	candidate.canonical = GUID_NULL;
	CHECK(!explorer_command_already_represented(candidate, accepted));
}

TEST(explorer_command, canonical_guid_skips_when_usable)
{
	GUID canonical{};
	CHECK(parse_guid(L"{01234567-89AB-CDEF-0123-456789ABCDEF}", canonical));

	ExplorerCommandIdentity have;
	have.canonical = canonical;
	std::vector<ExplorerCommandIdentity> accepted{ have };

	ExplorerCommandIdentity candidate;
	candidate.canonical = canonical;
	CHECK(explorer_command_already_represented(candidate, accepted));
	CHECK(explorer_command_guid_usable(canonical));
	CHECK(!explorer_command_guid_usable(GUID_NULL));
}

// ---- the provider cache, and the two references nobody could count --------
//
// Include/ProviderCache.h exists because both of the defects below are
// reference-counting defects and a reference count was precisely what could not
// be observed: the cache was a `static thread_local std::vector` and three free
// functions in an anonymous namespace inside ExplorerCommand.cpp.
//
// The fake here never has to be a real IExplorerCommand. The cache only stores
// the pointer and passes it to the injected table, so a counter standing in for
// an object is enough - and keeps COM, and a desktop, out of the suite.

#include "..\dll\src\Include\ProviderCache.h"

namespace
{
	struct FakeProvider
	{
		GUID clsid{};
		long refs{};
	};

	struct ProviderWorld
	{
		std::vector<FakeProvider *> objects;
		int activations = 0;
		bool activation_fails = false;

		~ProviderWorld() { reset(); }

		void reset()
		{
			for(auto *o : objects)
				delete o;
			objects.clear();
			activations = 0;
			activation_fails = false;
		}

		FakeProvider *find(const GUID &clsid)
		{
			for(auto *o : objects)
			{
				if(::IsEqualGUID(o->clsid, clsid))
					return o;
			}
			return nullptr;
		}

		// Every reference held by anybody, which is the number both defects
		// were about.
		long outstanding() const
		{
			long total = 0;
			for(auto *o : objects)
				total += o->refs;
			return total;
		}
	};

	ProviderWorld g_world;

	IExplorerCommand *fake_activate(const GUID &clsid)
	{
		if(g_world.activation_fails)
			return nullptr;

		g_world.activations++;
		auto *object = new FakeProvider{ clsid, 1 };	// CoCreateInstance returns one
		g_world.objects.push_back(object);
		return reinterpret_cast<IExplorerCommand *>(object);
	}

	void fake_add_ref(IExplorerCommand *cmd)
	{
		reinterpret_cast<FakeProvider *>(cmd)->refs++;
	}

	void fake_release(IExplorerCommand *cmd)
	{
		reinterpret_cast<FakeProvider *>(cmd)->refs--;
	}

	const ProviderComApi &fake_provider_api()
	{
		static const ProviderComApi api{ &fake_activate, &fake_add_ref, &fake_release };
		return api;
	}

	GUID provider_guid(unsigned char tag)
	{
		GUID g{ 0x1FA0E654, 0xC9F2, 0x4A1F, { 0x98, 0x00, 0xB9, 0xA7, 0x5D, 0x74, 0x4B, tag } };
		return g;
	}
}

TEST(provider_cache, a_borrowed_provider_releases_the_callers_reference)
{
	// The defect that shipped, and the one that mattered most because it was on
	// the path that *succeeds*: acquire handed out two references - "one for
	// the cache, one for the caller" - and the resolution loop released the
	// caller's half only when the provider declined or failed. Every provider
	// that actually contributed an item leaked one reference per menu, on every
	// host including Explorer's long-lived menu thread.
	g_world.reset();
	ProviderCache cache(fake_provider_api());

	auto clsid = provider_guid(0x01);
	{
		auto borrowed = cache.borrow(clsid);
		CHECK(borrowed);
		CHECK_EQ(g_world.find(clsid)->refs, 2L);	// the cache's, and this scope's
	}

	// Only the cache's remains. There is no Release for a caller to forget,
	// which is the point of returning a handle rather than a pointer.
	CHECK_EQ(g_world.find(clsid)->refs, 1L);
}

TEST(provider_cache, a_hundred_menus_do_not_accumulate_references)
{
	// The same defect stated the way it would have been noticed: as growth.
	g_world.reset();
	ProviderCache cache(fake_provider_api());

	auto clsid = provider_guid(0x02);
	for(int menu = 0; menu < 100; menu++)
	{
		auto borrowed = cache.borrow(clsid);
		CHECK(borrowed);
	}

	CHECK_EQ(g_world.find(clsid)->refs, 1L);
	CHECK_MSG(g_world.activations == 1,
			  "and the object is still being reused, which is why the cache exists");
}

TEST(provider_cache, a_released_cache_holds_nothing)
{
	// D9: a host that raises menus on transient threads left every one of them
	// holding providers until process exit.
	g_world.reset();
	ProviderCache cache(fake_provider_api());

	for(unsigned char i = 0; i < 5; i++)
	{
		auto borrowed = cache.borrow(provider_guid(0x10 + i));
		CHECK(borrowed);
	}
	CHECK_EQ(cache.size(), (size_t)5);
	CHECK_EQ(g_world.outstanding(), 5L);

	cache.release_all();

	CHECK_EQ(cache.size(), (size_t)0);
	CHECK_MSG(g_world.outstanding() == 0L,
			  "every reference this thread held is gone, not just the vector");
}

TEST(provider_cache, a_third_party_hosts_thread_keeps_nothing_between_menus)
{
	g_world.reset();
	ProviderCache cache(fake_provider_api());

	{
		auto borrowed = cache.borrow(provider_guid(0x20));
		CHECK(borrowed);
	}
	cache.end_of_menu(ProviderLifetime::ThisMenuOnly);

	CHECK_EQ(cache.size(), (size_t)0);
	CHECK_EQ(g_world.outstanding(), 0L);
}

TEST(provider_cache, explorers_thread_keeps_them_because_that_is_the_measured_win)
{
	// The over-correction guard. Releasing on every menu everywhere would
	// satisfy the test above and would put a warm menu back from ~41 ms to
	// ~170 ms on the host that raises almost all of them.
	g_world.reset();
	ProviderCache cache(fake_provider_api());

	auto clsid = provider_guid(0x21);
	for(int menu = 0; menu < 3; menu++)
	{
		{
			auto borrowed = cache.borrow(clsid);
			CHECK(borrowed);
		}
		cache.end_of_menu(ProviderLifetime::AcrossMenus);
	}

	CHECK_EQ(cache.size(), (size_t)1);
	CHECK_MSG(g_world.activations == 1, "three menus, one activation");
	CHECK_EQ(g_world.find(clsid)->refs, 1L);
}

TEST(provider_cache, forgetting_a_provider_drops_the_caches_reference)
{
	// The path a failed call takes: this thread stops reusing an object whose
	// author never expected it to be reused from that state.
	g_world.reset();
	ProviderCache cache(fake_provider_api());

	auto clsid = provider_guid(0x30);
	{
		auto borrowed = cache.borrow(clsid);
		CHECK(borrowed);
	}
	CHECK_EQ(g_world.find(clsid)->refs, 1L);

	cache.forget(clsid);
	CHECK_EQ(cache.size(), (size_t)0);
	CHECK_EQ(g_world.find(clsid)->refs, 0L);

	// And the next menu activates a fresh one rather than serving the dropped
	// object again.
	{
		auto borrowed = cache.borrow(clsid);
		CHECK(borrowed);
	}
	CHECK_EQ(g_world.activations, 2);
}

TEST(provider_cache, an_activation_that_failed_caches_nothing)
{
	g_world.reset();
	g_world.activation_fails = true;
	ProviderCache cache(fake_provider_api());

	auto borrowed = cache.borrow(provider_guid(0x40));
	CHECK(!borrowed);
	CHECK_EQ(cache.size(), (size_t)0);
	CHECK_EQ(g_world.outstanding(), 0L);
}

TEST(provider_cache, a_moved_handle_names_one_reference_and_not_two)
{
	g_world.reset();
	ProviderCache cache(fake_provider_api());

	auto clsid = provider_guid(0x50);
	{
		auto first = cache.borrow(clsid);
		CHECK_EQ(g_world.find(clsid)->refs, 2L);

		auto second = std::move(first);
		CHECK(second);
		CHECK(!first);
		CHECK_MSG(g_world.find(clsid)->refs == 2L,
				  "a move transfers the reference, it does not duplicate it");
	}
	CHECK_EQ(g_world.find(clsid)->refs, 1L);
}

// ---- where a provider's cost stops being counted ---------------------------
//
// W3 of docs/refactor/12-closure-plan.md. GetCanonicalName used to be called
// after health.record and Diagnostics::session_provider, so every remembered
// and every reported provider cost excluded it, while the whole-menu
// ProviderBudget charged it anyway - the gap section 09 2.1 read as Shell's
// own pre-paint work.
//
// The fake below is a real IExplorerCommand, unlike the reference-counting one
// above: this is about a method being *called*, so it has to be reachable
// through the vtable. Its clock is a counter rather than a sleep, so the
// assertion is exact instead of merely probable.

#include "..\dll\src\Include\ProviderCall.h"

namespace
{
	// The menu's elapsed budget, advanced only by the calls that pretend to
	// take time. Nothing here reads a real clock.
	uint32_t g_elapsed_us = 0;

	inline constexpr uint32_t kFillCostUs = 700;
	inline constexpr uint32_t kCanonicalCostUs = 900;

	class CountedExplorerCommand : public IExplorerCommand
	{
	public:
		int canonical_calls = 0;
		HRESULT canonical_hr = S_OK;

		// IUnknown. Not reference-counted: the test owns the object by scope,
		// and ProviderCall.h neither retains nor releases what it is handed.
		IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv) override
		{
			if(!ppv)
				return E_POINTER;
			if(::IsEqualIID(riid, IID_IUnknown) || ::IsEqualIID(riid, IID_IExplorerCommand))
			{
				*ppv = static_cast<IExplorerCommand *>(this);
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
		IFACEMETHODIMP_(ULONG) Release() override { return 1; }

		IFACEMETHODIMP GetCanonicalName(GUID *guid) override
		{
			canonical_calls++;
			g_elapsed_us += kCanonicalCostUs;		// the handler taking its time
			if(FAILED(canonical_hr))
				return canonical_hr;
			if(guid)
				*guid = provider_guid(0x77);
			return S_OK;
		}

		// The rest of the interface exists so the class is concrete. W3 is
		// about one method's place in the measured span; the others are made
		// by fill_menuitem_from_explorer_command, which the fill callable
		// stands in for.
		IFACEMETHODIMP GetTitle(IShellItemArray *, LPWSTR *) override { return E_NOTIMPL; }
		IFACEMETHODIMP GetIcon(IShellItemArray *, LPWSTR *) override { return E_NOTIMPL; }
		IFACEMETHODIMP GetToolTip(IShellItemArray *, LPWSTR *) override { return E_NOTIMPL; }
		IFACEMETHODIMP GetState(IShellItemArray *, BOOL, EXPCMDSTATE *) override { return E_NOTIMPL; }
		IFACEMETHODIMP Invoke(IShellItemArray *, IBindCtx *) override { return E_NOTIMPL; }
		IFACEMETHODIMP GetFlags(EXPCMDFLAGS *) override { return E_NOTIMPL; }
		IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand **) override { return E_NOTIMPL; }
	};

	uint32_t fake_clock() { return g_elapsed_us; }

	// What the GetState/GetTitle/GetIcon sequence costs, as one number.
	bool fill_that_costs(bool shown)
	{
		g_elapsed_us += kFillCostUs;
		return shown;
	}
}

TEST(provider_call, the_canonical_name_read_is_inside_the_providers_cost)
{
	// The regression itself. Before W3 the recorded cost was kFillCostUs
	// alone, because the read happened after health.record had already been
	// given a number.
	g_elapsed_us = 0;
	CountedExplorerCommand cmd;
	GUID canonical = GUID_NULL;

	auto measured = measure_provider_call(g_elapsed_us, &fake_clock, &cmd,
										  [] { return fill_that_costs(true); },
										  &canonical);

	CHECK(measured.shown);
	CHECK_EQ(cmd.canonical_calls, 1);
	CHECK_MSG(measured.cost_us == kFillCostUs + kCanonicalCostUs,
			  "the span has to cover every synchronous call made before the "
			  "item is published, not just the fill");
}

TEST(provider_call, activation_stays_inside_the_cost)
{
	// spent_before is sampled by the caller *before* it borrows from the
	// cache, so CoCreateInstance - ~2 ms per provider even fully warm - is
	// charged to the provider that needed it. Moving the first sample into
	// measure_provider_call would drop it from every provider's cost, which is
	// the same defect pointing the other way.
	g_elapsed_us = 0;
	const uint32_t spent_before = g_elapsed_us;

	g_elapsed_us += 2000;					// the activation

	CountedExplorerCommand cmd;
	GUID canonical = GUID_NULL;
	auto measured = measure_provider_call(spent_before, &fake_clock, &cmd,
										  [] { return fill_that_costs(true); },
										  &canonical);

	CHECK_EQ(measured.cost_us, 2000u + kFillCostUs + kCanonicalCostUs);
}

TEST(provider_call, a_provider_that_shows_nothing_is_not_asked_its_name)
{
	// Hidden is a live provider declining this selection and happens
	// constantly. Asking a handler for a name it will not be shown under is
	// work the user waits for and nothing consumes.
	g_elapsed_us = 0;
	CountedExplorerCommand cmd;
	GUID canonical = GUID_NULL;

	auto measured = measure_provider_call(g_elapsed_us, &fake_clock, &cmd,
										  [] { return fill_that_costs(false); },
										  &canonical);

	CHECK(!measured.shown);
	CHECK_EQ(cmd.canonical_calls, 0);
	CHECK_EQ(measured.cost_us, kFillCostUs);
	CHECK(::IsEqualGUID(canonical, GUID_NULL));
}

TEST(provider_call, a_failed_name_read_is_still_charged_and_yields_no_identity)
{
	// "A pointer to a value that, when this method returns successfully,
	// receives the command's GUID" - so on failure the out-parameter holds
	// nothing the caller may use, and GUID_NULL is the value that says so.
	// The time it took is still the user's.
	g_elapsed_us = 0;
	CountedExplorerCommand cmd;
	cmd.canonical_hr = E_FAIL;
	GUID canonical = provider_guid(0x11);		// deliberately not null going in

	auto measured = measure_provider_call(g_elapsed_us, &fake_clock, &cmd,
										  [] { return fill_that_costs(true); },
										  &canonical);

	CHECK(::IsEqualGUID(canonical, GUID_NULL));
	CHECK_EQ(measured.cost_us, kFillCostUs + kCanonicalCostUs);
}
