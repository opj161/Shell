#include "test.h"

#include <windows.h>
#include "Include/Diagnostics/DiagnosticsRing.h"

#include <thread>
#include <vector>

// The always-on phase ring.
//
// Three things here are easy to get subtly wrong and expensive to notice later,
// so each has a test that fails loudly rather than quietly losing data:
//
//   - Wrapping. A ring that reads back in the wrong order, or that reports more
//     entries than it holds, produces a plausible-looking report of the wrong
//     menus.
//   - Overflow. There are more phase names in the menu path than the plan's
//     sketch allowed for, and the phases that go missing under load are exactly
//     the slow ones. Dropping has to be counted.
//   - Nesting. A menu can open while another is up, and Shell's own hook can
//     re-enter. An inner session that published on its own would truncate the
//     outer one at the moment it started.

using namespace Nilesoft::Shell::Diagnostics;

namespace
{
	// Phase names are stored as pointers, not copies - that is what makes
	// recording free of allocation - so they have to be literals.
	const wchar_t *PHASE_A = L"test.alpha";
	const wchar_t *PHASE_B = L"test.bravo";

	MenuSessionRecord one(uint32_t host, uint32_t total)
	{
		MenuSessionRecord r;
		r.host_hash = host;
		r.total_microseconds = total;
		return r;
	}
}

TEST(diagnostics_ring, an_empty_ring_offers_nothing)
{
	DiagnosticsRing ring;
	MenuSessionRecord out[4];

	CHECK_EQ(ring.size(), size_t(0));
	CHECK_EQ(ring.snapshot(out, 4), size_t(0));
	CHECK_EQ(ring.published(), 0ull);
}

TEST(diagnostics_ring, the_newest_session_comes_back_first)
{
	DiagnosticsRing ring;
	ring.publish(one(1, 100));
	ring.publish(one(2, 200));
	ring.publish(one(3, 300));

	MenuSessionRecord out[3];
	CHECK_EQ(ring.snapshot(out, 3), size_t(3));
	CHECK_EQ(out[0].host_hash, 3u);
	CHECK_EQ(out[1].host_hash, 2u);
	CHECK_EQ(out[2].host_hash, 1u);
}

TEST(diagnostics_ring, asking_for_more_than_it_holds_gives_what_it_has)
{
	DiagnosticsRing ring;
	ring.publish(one(7, 1));

	MenuSessionRecord out[8];
	CHECK_EQ(ring.snapshot(out, 8), size_t(1));
	CHECK_EQ(out[0].host_hash, 7u);
}

TEST(diagnostics_ring, wrapping_keeps_the_newest_and_forgets_the_oldest)
{
	DiagnosticsRing ring;

	// One and a half times round, so every slot has been written twice for the
	// first half - the case where an off-by-one in the read order shows up.
	const uint32_t total = static_cast<uint32_t>(RING_CAPACITY) + RING_CAPACITY / 2;
	for(uint32_t i = 1; i <= total; i++)
		ring.publish(one(i, i));

	CHECK_EQ(ring.size(), RING_CAPACITY);
	CHECK_EQ(ring.published(), static_cast<uint64_t>(total));

	std::vector<MenuSessionRecord> out(RING_CAPACITY);
	CHECK_EQ(ring.snapshot(out.data(), out.size()), RING_CAPACITY);

	// Newest first, counting down, and the oldest survivor is exactly the one
	// that capacity allows.
	CHECK_EQ(out[0].host_hash, total);
	CHECK_EQ(out[1].host_hash, total - 1);
	CHECK_EQ(out[RING_CAPACITY - 1].host_hash, total - (RING_CAPACITY - 1));
}

TEST(diagnostics_ring, clearing_forgets_everything_including_the_running_total)
{
	DiagnosticsRing ring;
	ring.publish(one(1, 1));
	ring.publish(one(2, 2));
	ring.clear();

	MenuSessionRecord out[2];
	CHECK_EQ(ring.size(), size_t(0));
	CHECK_EQ(ring.published(), 0ull);
	CHECK_EQ(ring.snapshot(out, 2), size_t(0));
}

TEST(diagnostics_ring, phases_are_recorded_in_order_with_their_counts)
{
	MenuSessionRecord r;
	r.add_phase(PHASE_A, 1500, 12);
	r.add_phase(PHASE_B, 400, -1);

	CHECK_EQ(r.phase_count, 2);
	CHECK_EQ(r.dropped_phases, 0);
	CHECK(r.phases[0].name == PHASE_A);
	CHECK_EQ(r.phases[0].microseconds, 1500u);
	CHECK_EQ(r.phases[0].count, 12);
	CHECK(r.phases[1].name == PHASE_B);
	CHECK_EQ(r.phases[1].count, -1);
}

TEST(diagnostics_ring, a_phase_that_does_not_fit_is_counted_not_dropped_silently)
{
	MenuSessionRecord r;
	for(int i = 0; i < MAX_PHASES + 5; i++)
		r.add_phase(PHASE_A, static_cast<uint32_t>(i), i);

	CHECK_EQ(r.phase_count, MAX_PHASES);
	CHECK_EQ(r.dropped_phases, 5);

	// What did fit is the *first* phases, not the last: overwriting would lose
	// the pre-display work, which is the part anybody reading this cares about.
	CHECK_EQ(r.phases[0].count, 0);
	CHECK_EQ(r.phases[MAX_PHASES - 1].count, MAX_PHASES - 1);
}

TEST(diagnostics_ring, a_provider_that_does_not_fit_is_counted_too)
{
	MenuSessionRecord r;
	for(int i = 0; i < MAX_PROVIDERS + 3; i++)
		r.add_provider(static_cast<uint32_t>(i), 10, ProviderResult::Ok);

	CHECK_EQ(r.provider_count, MAX_PROVIDERS);
	CHECK_EQ(r.dropped_providers, 3);
}

TEST(diagnostics_ring, provider_results_survive_the_round_trip)
{
	MenuSessionRecord r;
	r.add_provider(0xABCD, 186000, ProviderResult::Deferred);

	CHECK_EQ(r.provider_count, 1);
	CHECK_EQ(r.providers[0].clsid_hash, 0xABCDu);
	CHECK_EQ(r.providers[0].microseconds, 186000u);
	CHECK(r.providers[0].result == ProviderResult::Deferred);
}

TEST(diagnostics_ring, reset_leaves_nothing_of_the_previous_session)
{
	MenuSessionRecord r;
	r.host_hash = 99;
	r.decision = TakeoverDecision::FailOpen;
	r.add_phase(PHASE_A, 1, 1);
	r.add_provider(1, 1, ProviderResult::Failed);
	r.reset();

	CHECK_EQ(r.host_hash, 0u);
	CHECK_EQ(r.phase_count, 0);
	CHECK_EQ(r.provider_count, 0);
	CHECK(r.decision == TakeoverDecision::Unknown);
	CHECK(r.phases[0].name == nullptr);
}

TEST(diagnostics_ring, a_session_publishes_once_and_carries_its_phases)
{
	DiagnosticsRing::instance().clear();

	session_begin(0x1234, TPM_RETURNCMD);
	session_phase(PHASE_A, 900, 3);
	session_decision(TakeoverDecision::TakeOver);
	session_end();

	CHECK_EQ(DiagnosticsRing::instance().published(), 1ull);

	MenuSessionRecord out[1];
	CHECK_EQ(DiagnosticsRing::instance().snapshot(out, 1), size_t(1));
	CHECK_EQ(out[0].host_hash, 0x1234u);
	CHECK_EQ(out[0].phase_count, 1);
	CHECK(out[0].phases[0].name == PHASE_A);
	CHECK(out[0].decision == TakeoverDecision::TakeOver);
	CHECK(out[0].tick != 0);
}

TEST(diagnostics_ring, a_nested_session_folds_into_the_one_around_it)
{
	DiagnosticsRing::instance().clear();

	session_begin(0xAAAA, 0);
	session_phase(PHASE_A, 100, -1);

	// A menu opening while another is up. It must not publish on its own, or
	// the outer session is truncated at the moment the inner one started.
	session_begin(0xBBBB, 0);
	session_phase(PHASE_B, 200, -1);
	session_end();

	CHECK_EQ(DiagnosticsRing::instance().published(), 0ull);

	session_end();
	CHECK_EQ(DiagnosticsRing::instance().published(), 1ull);

	MenuSessionRecord out[1];
	DiagnosticsRing::instance().snapshot(out, 1);
	CHECK_EQ(out[0].host_hash, 0xAAAAu);
	CHECK_EQ(out[0].phase_count, 2);
	CHECK(out[0].phases[1].name == PHASE_B);
}

TEST(diagnostics_ring, the_outermost_decision_is_the_one_recorded)
{
	DiagnosticsRing::instance().clear();

	session_begin(1, 0);
	session_decision(TakeoverDecision::TakeOver);
	session_begin(2, 0);
	// A nested menu that failed open says nothing about how the menu around it
	// resolved.
	session_decision(TakeoverDecision::FailOpen);
	session_end();
	session_end();

	MenuSessionRecord out[1];
	DiagnosticsRing::instance().snapshot(out, 1);
	CHECK(out[0].decision == TakeoverDecision::TakeOver);
}

TEST(diagnostics_ring, recording_outside_a_session_is_ignored_rather_than_lost_somewhere)
{
	DiagnosticsRing::instance().clear();

	// Phases genuinely do run outside a hook session - the taskbar hit test is
	// one - and they must not accumulate into whatever session happens to start
	// next on this thread.
	session_phase(PHASE_A, 50, -1);
	session_provider(1, 50, ProviderResult::Ok);

	session_begin(5, 0);
	session_end();

	MenuSessionRecord out[1];
	DiagnosticsRing::instance().snapshot(out, 1);
	CHECK_EQ(out[0].phase_count, 0);
	CHECK_EQ(out[0].provider_count, 0);
}

TEST(diagnostics_ring, an_unbalanced_end_does_not_swallow_the_next_session)
{
	DiagnosticsRing::instance().clear();

	// A hook path that bails before starting a session still runs its __finally.
	// If that drove depth negative, the next real session would never publish.
	session_end();
	session_end();

	session_begin(11, 0);
	session_end();

	CHECK_EQ(DiagnosticsRing::instance().published(), 1ull);
}

TEST(diagnostics_ring, sessions_on_different_threads_do_not_share_a_slot)
{
	DiagnosticsRing::instance().clear();

	// The accumulate path takes no lock, which is only safe because each session
	// has one writer. If the slot were shared, these phases would interleave and
	// the counts below would be wrong.
	auto body = [](uint32_t host, const wchar_t *name, int repeats)
	{
		session_begin(host, 0);
		for(int i = 0; i < repeats; i++)
			session_phase(name, 1, i);
		session_end();
	};

	std::thread a(body, 0xA1u, PHASE_A, 3);
	std::thread b(body, 0xB2u, PHASE_B, 5);
	a.join();
	b.join();

	CHECK_EQ(DiagnosticsRing::instance().published(), 2ull);

	MenuSessionRecord out[2];
	CHECK_EQ(DiagnosticsRing::instance().snapshot(out, 2), size_t(2));

	for(auto &r : out)
	{
		if(r.host_hash == 0xA1u)
			CHECK_EQ(r.phase_count, 3);
		else if(r.host_hash == 0xB2u)
			CHECK_EQ(r.phase_count, 5);
		else
			CHECK_MSG(false, "a published session belonged to neither thread");
	}
}

TEST(diagnostics_ring, the_hosts_own_tracking_flags_are_kept_verbatim)
{
	// Which half of complete_host_contract a host exercises turns entirely on
	// TPM_RETURNCMD, and the hook rewrites uFlags before tracking - so the
	// value the *host* passed exists in exactly one place, at session_begin.
	// docs/refactor/01-takeover-contract.md section 3.
	DiagnosticsRing::instance().clear();

	session_begin(0x77, TPM_RETURNCMD | TPM_RIGHTBUTTON);
	session_end();

	MenuSessionRecord out[1];
	DiagnosticsRing::instance().snapshot(out, 1);
	CHECK_EQ(out[0].host_flags, uint32_t(TPM_RETURNCMD | TPM_RIGHTBUTTON));

	// And a host that passes none is a real answer, not a missing one - it is
	// the commonest case, and the one whose replay path had never run outside
	// a test.
	DiagnosticsRing::instance().clear();
	session_begin(0x78, 0);
	session_end();
	DiagnosticsRing::instance().snapshot(out, 1);
	CHECK_EQ(out[0].host_flags, 0u);
}
