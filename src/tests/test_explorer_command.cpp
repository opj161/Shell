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
