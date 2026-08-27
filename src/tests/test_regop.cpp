#include "test.h"

#include <windows.h>
#include "Resource.h"
#include "Globals.h"
#include "System.h"
#include "RegistryConfig.h"

using Nilesoft::REGOP;
using Nilesoft::unregister_if_present;

TEST(regop, every_option_defaults_to_false)
{
	// Deliberately use default initialization, matching the two call sites that
	// exposed the bug. Struct-level defaults make this safe even if a future
	// caller forgets braces.
	REGOP options;
	CHECK(!options.REGISTER);
	CHECK(!options.UNREGISTER);
	CHECK(!options.TREAT);
	CHECK(!options.CONTEXTMENU);
	CHECK(!options.ICONOVERLAY);
	CHECK(!options.RESTART);
	CHECK(!options.SILENT);
}

// Was selecting_ordinary_handlers_cannot_enable_folderextensions. REGOP no
// longer has a FOLDEREXTENSIONS field, so that is now structural rather than
// something a test can observe; what is still worth pinning is that naming a
// handler is not by itself a register or unregister operation.
TEST(regop, selecting_handlers_is_not_by_itself_a_register_operation)
{
	REGOP options{};
	options.CONTEXTMENU = true;
	options.ICONOVERLAY = true;

	CHECK(options.CONTEXTMENU);
	CHECK(options.ICONOVERLAY);
	CHECK(!options.REGISTER);
	CHECK(!options.UNREGISTER);
}

TEST(regop, repeated_unregister_is_an_idempotent_no_op)
{
	int remove_calls = 0;
	auto remove = [&]
	{
		remove_calls++;
		return false;
	};

	CHECK(unregister_if_present(false, remove));
	CHECK_EQ(remove_calls, 0);
}

TEST(regop, registered_state_propagates_the_delete_result)
{
	int remove_calls = 0;
	CHECK(unregister_if_present(true, [&]
		{
			remove_calls++;
			return true;
		}));
	CHECK_EQ(remove_calls, 1);

	CHECK(!unregister_if_present(true, [&]
		{
			remove_calls++;
			return false;
		}));
	CHECK_EQ(remove_calls, 2);
}

// No build can register FolderExtensions any more, but `shell.exe -register
// -force` could until upstream ed826e1 (2024-12-03), so a machine can still be
// carrying the CLSID key. If detection stopped answering for it, uninstall on
// such a machine would decide there was nothing to remove and skip the servicing
// deletes entirely - unregister_if_present short-circuits on IsRegistered().
//
// Seeded under HKCU\Software\Classes rather than written straight to HKCR: that
// is the branch of the merged view a non-elevated process owns, and reads
// through HKCR see it. https://learn.microsoft.com/en-us/windows/win32/sysinfo/merged-view-of-hkey-classes-root
//
// Seeded with a bare RegCreateKeyExW rather than Registry::CurrentUser, because
// the two do not address the same hive from a 32-bit process. Registry's static
// roots are built with KEY_WOW64_64KEY (Registry.cpp:660-662), so CreateSubKey
// writes the 64-bit view; Registry::Exists takes a `view` defaulting to 0 and
// DeleteSubKey passes no flag, so both use the caller's natural view, which
// under WOW64 is Software\Classes\WOW6432Node. Seeding through the roots made
// this test pass as x64 and fail as x86 - the key went somewhere the detection
// it is testing does not look. Detection is what production runs, so the seed
// follows detection.
//
// That divergence is real beyond this test: on 32-bit Shell every
// Registry::Exists-based check reads a different view than the Registry roots
// write. It is module-wide, so it is not fixed here.
// https://learn.microsoft.com/en-us/windows/win32/winprog64/accessing-an-alternate-registry-view
//
// The HKLM half of the servicing pair (Drive\shellex\FolderExtensions) is NOT
// covered here: writing it needs elevation the suite does not have, and calling
// Unregister() to observe the delete would tear down the real registration on
// the developer's machine. That half rests on the code reading as written.
TEST(regop, a_legacy_folderextensions_key_still_reports_as_registered)
{
	constexpr auto SEEDED = LR"(Software\Classes\CLSID\{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF3})";

	using Nilesoft::RegistryConfig;

	// A developer machine may already carry the real thing - this one does, in
	// HKLM, which is what makes the servicing path worth keeping - so record
	// what was there and put the machine back exactly as found. The disposition
	// out-parameter, not the earlier IsFolderExtensions() reading, is what says
	// whether this test owns the key: they disagree whenever detection is
	// answering for a key in a different hive than the one seeded here.
	const bool was_detected = RegistryConfig::IsFolderExtensions();

	HKEY seeded{};
	DWORD disposition = 0;
	const auto rc = ::RegCreateKeyExW(HKEY_CURRENT_USER, SEEDED, 0, nullptr,
									  REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
									  &seeded, &disposition);
	CHECK_MSG(rc == ERROR_SUCCESS, "the scratch CLSID key could be created under HKCU");
	if(seeded)
		::RegCloseKey(seeded);

	CHECK_MSG(RegistryConfig::IsFolderExtensions(),
			  "a legacy FolderExtensions CLSID is detected through the HKCR merged view");
	CHECK_MSG(RegistryConfig::IsRegistered(),
			  "IsRegistered() answers true for it, so Unregister() is allowed to run");

	if(disposition == REG_CREATED_NEW_KEY)
	{
		CHECK(ERROR_SUCCESS == ::RegDeleteTreeW(HKEY_CURRENT_USER, SEEDED));

		// Only meaningful when nothing else was answering for it already.
		if(!was_detected)
			CHECK_MSG(!RegistryConfig::IsFolderExtensions(),
					  "and stops reporting once the key is gone");
	}
}
