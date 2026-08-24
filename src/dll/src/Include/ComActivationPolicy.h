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

#include <vector>
#include <cstddef>

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

		/*
			A CLSID reduced to sixteen bytes of plain data.

			Not a GUID, on purpose: this header stays free of COM so the set
			below can be driven directly by the test suite. The hook copies the
			bytes of the REFCLSID it was handed, which costs nothing and cannot
			fail - unlike Guid::to_string, which the hook used to call for every
			interesting activation in the process before it knew whether any
			rule mentioned the CLSID at all.
		*/
		struct ClsidKey
		{
			unsigned long long lo{};
			unsigned long long hi{};

			constexpr bool operator==(const ClsidKey &other) const
			{
				return lo == other.lo && hi == other.hi;
			}
		};

		/*
			The CLSIDs any configured rule could possibly name, compiled once
			when a configuration is published.

			docs/refactor/01-takeover-contract.md section 9: "At config publish
			time compile ComActivationPolicy{ exact_blocked_clsid_set,
			conditional_groups } from the same statics rules currently evaluated
			per activation", then "fast path: if(!policy->may_affect(rclsid))
			return original(...)", and "attach the detour only when policy is
			non-empty or the Win11-priority feature is on".

			What that replaces is a real cost. CoCreateInstanceHook is a
			process-wide detour: it sees every in-process and local-server
			activation in explorer.exe, and for each one whose IID was one of
			the four it cared about it built a Context, stringified the CLSID,
			and walked the rule list evaluating `where` expressions - on a
			machine whose configuration names no CLSID at all, which is the
			stock configuration.

			`may_affect` is a linear scan of a vector that is almost always
			empty and never large. That is deliberately not a hash set: the
			whole point is that this runs before anything allocates, and a
			handful of 16-byte comparisons beats hashing for the sizes a
			human-written blocklist reaches.
		*/
		class ComActivationPolicy
		{
		public:
			// A CLSID some rule names. `conditional` records that the rule
			// carries a `where`, which is not used by may_affect - a mentioned
			// CLSID is worth evaluating either way - but is what tells the
			// caller whether the answer can be cached. Kept because the
			// distinction is the plan's, and losing it here would lose it
			// everywhere.
			void mention(const ClsidKey &clsid, bool conditional)
			{
				for(auto &known : _mentioned)
				{
					if(known == clsid)
						return;
				}
				_mentioned.push_back(clsid);
				if(conditional)
					_conditional++;
			}

			// The Windows 11 modern-menu suppression is on. It is not keyed on
			// a CLSID the rules name - it matches one Windows CLSID - so it is
			// tracked separately and is on its own enough to need the hook.
			void set_suppresses_modern_menu(bool on) { _suppresses_modern = on; }
			bool suppresses_modern_menu() const { return _suppresses_modern; }

			size_t size() const { return _mentioned.size(); }
			size_t conditional() const { return _conditional; }

			// Nothing for the hook to do. Not the same as "no CLSIDs": the
			// modern-menu suppression needs the hook with no CLSID rules at all.
			bool empty() const { return _mentioned.empty() && !_suppresses_modern; }

			/*
				Is this activation worth looking at any further?

				False means: no configured rule can name this CLSID, so do not
				build a Context, do not stringify anything, just call through.
				True means only "maybe" - a rule mentions it, and whether it is
				actually blocked still depends on evaluating that rule's `where`
				against the current selection.
			*/
			bool may_affect(const ClsidKey &clsid) const
			{
				for(auto &known : _mentioned)
				{
					if(known == clsid)
						return true;
				}
				return false;
			}

			// Whether the detour is worth installing at all.
			bool needs_hook() const { return !empty(); }

		private:
			std::vector<ClsidKey> _mentioned;
			size_t _conditional{};
			bool _suppresses_modern{};
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
