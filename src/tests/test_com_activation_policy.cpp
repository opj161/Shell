#include "test.h"

#include "Include/ComActivationPolicy.h"

// The rule under test is an ordering rule, and the defect it pins was an
// ordering defect: CoCreateInstanceHook checked for a held Alt key first and
// returned the timed activation from inside that branch, so the configured
// CLSID blocklist was skipped entirely whenever Alt was down while a context
// menu opened. docs/refactor/04-code-health.md section 9.
//
// decide_com_activation() is the only place that answers "block or not", and
// it cannot be called without having evaluated the blocklist, which is what
// keeps the two from being reordered again.

using Nilesoft::Shell::ComActivationVerdict;
using Nilesoft::Shell::decide_com_activation;
using Nilesoft::Shell::ComActivationPolicy;
using Nilesoft::Shell::ClsidKey;

TEST(com_activation_policy, blocked_clsid_stays_blocked_with_alt_held)
{
	CHECK(decide_com_activation(true, true, false) == ComActivationVerdict::Block);
	CHECK_MSG(decide_com_activation(true, true, true) == ComActivationVerdict::Block,
			  "holding Alt is a request to measure an activation, not to permit one");
}

TEST(com_activation_policy, alt_times_only_activations_policy_allowed)
{
	CHECK(decide_com_activation(true, false, true) == ComActivationVerdict::TimeAndReturn);
	CHECK(decide_com_activation(true, false, false) == ComActivationVerdict::PassThrough);
}

TEST(com_activation_policy, uninteresting_activations_are_never_touched)
{
	// Neither the blocklist nor the diagnostic applies to a CLSID/IID pair the
	// hook decided not to look at: it must call through unchanged.
	CHECK(decide_com_activation(false, false, false) == ComActivationVerdict::PassThrough);
	CHECK(decide_com_activation(false, false, true) == ComActivationVerdict::PassThrough);
	CHECK_MSG(decide_com_activation(false, true, true) == ComActivationVerdict::PassThrough,
			  "a rule that did not match cannot block by accident");
}

TEST(com_activation_policy, the_decision_is_a_constant_expression)
{
	// Compile-time evaluable, so the fast path costs nothing at runtime and the
	// ordering is fixed in the type system rather than in a comment.
	static_assert(decide_com_activation(true, true, true) == ComActivationVerdict::Block,
				  "policy must win over the timing probe");
	static_assert(decide_com_activation(true, false, true) == ComActivationVerdict::TimeAndReturn, "");
	CHECK(true);
}

/*
	The compiled policy - docs/refactor/01-takeover-contract.md section 9.

	CoCreateInstanceHook is a process-wide inline detour: it sees every
	in-process and local-server activation in explorer.exe, and for each one
	whose IID was one of the four it cared about it built a Context, stringified
	the CLSID and walked the rule list evaluating `where` expressions - on a
	machine whose configuration names no CLSID at all, which is the stock
	configuration. may_affect is what replaces that with sixteen bytes compared
	against a list that is usually empty.

	The property that matters is the direction of the mistake. Answering true
	for a CLSID no rule names costs the old slow path and nothing else.
	Answering *false* for one a rule does name silently disables the blocklist,
	which is the defect docs/refactor/04-code-health.md section 9 already caught
	once in a different form.
*/

namespace
{
	ClsidKey key(unsigned long long lo, unsigned long long hi = 0)
	{
		return ClsidKey{ lo, hi };
	}
}

TEST(com_activation_policy, an_empty_policy_needs_no_hook_and_affects_nothing)
{
	ComActivationPolicy policy;

	CHECK(policy.empty());
	CHECK(!policy.needs_hook());
	CHECK_EQ(policy.size(), (size_t)0);
	CHECK(!policy.may_affect(key(1)));
}

TEST(com_activation_policy, a_mentioned_clsid_is_affected_and_others_are_not)
{
	ComActivationPolicy policy;
	policy.mention(key(0x1111, 0x2222), false);

	CHECK(policy.may_affect(key(0x1111, 0x2222)));
	CHECK(!policy.may_affect(key(0x1111, 0x3333)));
	CHECK(!policy.may_affect(key(0x9999, 0x2222)));
	CHECK(!policy.empty());
	CHECK(policy.needs_hook());
}

// Both halves of the key have to be compared. Comparing only one would make
// every CLSID sharing a first half look blocked, or - worse - make a genuinely
// blocked one look unmentioned.
TEST(com_activation_policy, both_halves_of_the_key_decide)
{
	ComActivationPolicy policy;
	policy.mention(key(0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull), false);

	CHECK(policy.may_affect(key(0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull)));
	CHECK(!policy.may_affect(key(0xAAAAAAAAAAAAAAAAull, 0)));
	CHECK(!policy.may_affect(key(0, 0xBBBBBBBBBBBBBBBBull)));
}

// Two rules naming the same CLSID is ordinary in a hand-written configuration.
TEST(com_activation_policy, the_same_clsid_mentioned_twice_is_stored_once)
{
	ComActivationPolicy policy;
	policy.mention(key(7), false);
	policy.mention(key(7), false);
	policy.mention(key(7), true);

	CHECK_EQ(policy.size(), (size_t)1);
	CHECK(policy.may_affect(key(7)));
}

// A rule carrying a `where` still mentions its CLSID: only evaluating the rule
// can say whether it blocks right now, and that evaluation is exactly what
// may_affect exists to gate.
TEST(com_activation_policy, a_conditional_rule_still_mentions_its_clsid)
{
	ComActivationPolicy policy;
	policy.mention(key(3), true);

	CHECK(policy.may_affect(key(3)));
	CHECK_EQ(policy.conditional(), (size_t)1);
}

TEST(com_activation_policy, conditional_and_unconditional_rules_are_counted_apart)
{
	ComActivationPolicy policy;
	policy.mention(key(1), false);
	policy.mention(key(2), true);
	policy.mention(key(3), true);

	CHECK_EQ(policy.size(), (size_t)3);
	CHECK_EQ(policy.conditional(), (size_t)2);
}

// The Win11 suppression matches one Windows CLSID rather than a configured
// one, so it needs the hook even with no blocklist at all. A policy that
// reported itself empty here would have the detour skipped and the feature
// silently stop working.
TEST(com_activation_policy, the_modern_menu_suppression_needs_the_hook_on_its_own)
{
	ComActivationPolicy policy;
	CHECK(policy.empty());

	policy.set_suppresses_modern_menu(true);

	CHECK(!policy.empty());
	CHECK(policy.needs_hook());
	CHECK(policy.suppresses_modern_menu());
	CHECK_MSG(policy.size() == 0, "and it names no CLSID of its own");
}

// A CLSID is sixteen bytes, and the key is the whole of it.
TEST(com_activation_policy, a_key_is_exactly_the_size_of_a_guid)
{
	static_assert(sizeof(ClsidKey) == 16, "a CLSID key is the GUID's sixteen bytes");
	CHECK_EQ(sizeof(ClsidKey), (size_t)16);
}
