#pragma once

// What CoCreateInstanceHook does with one activation request.
//
// This exists to state one ordering rule in a place where it can be tested:
// policy is decided before diagnostics, never after. The hook used to check
// for a held Alt key first and return the timed activation immediately, so
// holding Alt while a context menu opened bypassed the entire CLSID blocklist
// for every activation in that window - silently, since a suppressed extension
// simply reappeared. See docs/refactor/04-code-health.md section 9.
//
// Deliberately free of <windows.h>, COM and Context: the decision is three
// booleans, so the test suite drives the real function rather than a copy.
// The policy compile described in docs/refactor/01-takeover-contract.md
// section 9 lands here too, which is why the file is named for it.

namespace Nilesoft
{
	namespace Shell
	{
		enum class ComActivationVerdict
		{
			// Not this hook's business, or nothing to say about it: call the
			// original CoCreateInstance and return whatever it returns.
			PassThrough,

			// A configured rule names this CLSID: fail the activation with
			// E_NOINTERFACE without calling through. Nothing was created, so
			// nothing is released and *ppv is never read.
			Block,

			// Policy allowed the activation; the caller is holding Alt, so time
			// the call and log it. A diagnostic wrapped around a decision that
			// has already been made - not a decision of its own.
			TimeAndReturn,
		};

		// `interesting` - the hook looked at the CLSID/IID pair at all.
		// `blocked`     - a configured rule matched it. Evaluating this requires
		//                 walking the rule list, which is precisely why it is a
		//                 parameter: the caller cannot reach a verdict without
		//                 having done that work first.
		// `timing`      - the Alt-held timing probe is active.
		constexpr ComActivationVerdict decide_com_activation(bool interesting, bool blocked, bool timing)
		{
			if(!interesting)
				return ComActivationVerdict::PassThrough;
			if(blocked)
				return ComActivationVerdict::Block;
			return timing ? ComActivationVerdict::TimeAndReturn : ComActivationVerdict::PassThrough;
		}
	}
}
