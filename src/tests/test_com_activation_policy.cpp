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
