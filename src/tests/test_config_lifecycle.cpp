#include "test.h"

#include "Include/ConfigLifecycle.h"

// A typo in shell.nss used to remove the context menu from the whole desktop:
// Initializer::init() sets Status.Error on a failed parse, and query() refused
// every menu while that flag was set - even though init() builds into a fresh
// CACHE and publishes only on success, so the last good generation was still
// in memory, untouched. docs/refactor/03-config-safety.md sections 1 and 2.

using Nilesoft::Shell::ConfigVerdict;
using Nilesoft::Shell::decide_config_serve;

namespace
{
	// decide_config_serve(disabled, refresh, error, has_snapshot)
	constexpr ConfigVerdict verdict(bool disabled, bool refresh, bool error, bool snapshot)
	{
		return decide_config_serve(disabled, refresh, error, snapshot);
	}
}

TEST(config_lifecycle, a_failed_parse_keeps_serving_the_last_good_generation)
{
	CHECK_MSG(verdict(false, false, true, true) == ConfigVerdict::Serve,
			  "StaleWithError: the published snapshot is still correct");
}

TEST(config_lifecycle, a_failed_parse_with_nothing_loaded_still_refuses)
{
	// Nothing has ever parsed in this process, so there is genuinely nothing
	// to show. Half a menu would be worse than none.
	CHECK(verdict(false, false, true, false) == ConfigVerdict::Refuse);
}

TEST(config_lifecycle, a_reload_request_outranks_a_stale_snapshot)
{
	// Otherwise StaleWithError would be a trap: menus keep working, and the
	// fixed file never gets picked up because the stale snapshot always
	// answers first.
	CHECK(verdict(false, true, true, true) == ConfigVerdict::Reparse);
	CHECK(verdict(false, true, false, true) == ConfigVerdict::Reparse);
	CHECK(verdict(false, true, true, false) == ConfigVerdict::Reparse);
}

TEST(config_lifecycle, disabled_outranks_everything)
{
	CHECK(verdict(true, false, false, true) == ConfigVerdict::Refuse);
	CHECK_MSG(verdict(true, true, false, true) == ConfigVerdict::Refuse,
			  "a pending reload must not resurrect a shell the user turned off");
}

TEST(config_lifecycle, the_ordinary_paths_are_unchanged)
{
	// Loaded and healthy.
	CHECK(verdict(false, false, false, true) == ConfigVerdict::Serve);
	// First menu in a fresh process.
	CHECK(verdict(false, false, false, false) == ConfigVerdict::Reparse);
}

TEST(config_lifecycle, every_input_combination_has_a_verdict)
{
	// Sixteen states, no gaps: the switch in query() has no default, so a
	// combination nobody thought about must not fall through it.
	int served = 0, reparsed = 0, refused = 0;
	for(int i = 0; i < 16; i++)
	{
		auto v = verdict((i & 1) != 0, (i & 2) != 0, (i & 4) != 0, (i & 8) != 0);
		if(v == ConfigVerdict::Serve) served++;
		else if(v == ConfigVerdict::Reparse) reparsed++;
		else refused++;
	}
	CHECK_EQ(served + reparsed + refused, 16);
	CHECK_MSG(served > 0 && reparsed > 0 && refused > 0, "all three states reachable");
}
