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
	CHECK(!options.FOLDEREXTENSIONS);
	CHECK(!options.ICONOVERLAY);
	CHECK(!options.RESTART);
	CHECK(!options.SILENT);
}

TEST(regop, selecting_ordinary_handlers_cannot_enable_folderextensions)
{
	REGOP options{};
	options.CONTEXTMENU = true;
	options.ICONOVERLAY = true;

	CHECK(options.CONTEXTMENU);
	CHECK(options.ICONOVERLAY);
	CHECK(!options.FOLDEREXTENSIONS);
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
