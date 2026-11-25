#pragma once

/*
	Stop trying to take over a host that keeps refusing to be taken over.

	docs/refactor/01-takeover-contract.md section 7:

		consecutive_failures >= 3  -> mode = Core (no takeover) for process lifetime
		success resets counter; never persisted across processes

	The fail-open path already means a failed takeover costs the user the host's
	own menu rather than no menu at all, which is why this is a latency and
	log-noise feature rather than a correctness one. What it buys is that a host
	where Shell cannot work - a third-party file manager whose window Shell
	cannot subclass, an Explorer wedged by something else - stops paying for the
	attempt on every single right-click, and says so once instead of failing
	silently forever.

	Three rules, and each is here because the obvious alternative is worse:

	- **Consecutive, not cumulative.** A process that raises ten thousand menus
	  will collect occasional failures; a cumulative count would eventually trip
	  on a host that works. What says "this host is not going to work" is a run
	  of them with no success in between.

	- **Never persisted.** Audit 1 section 18's conservatism rule: crash
	  correlation uses session markers, not "explorer died so it must have been
	  Shell". A breaker that survived process exit could disable Shell across a
	  whole machine on the strength of three failures in one unlucky process,
	  and the user would have no idea why their menu had gone.

	- **It never re-arms itself.** Once open it stays open for the life of the
	  process, so a wedged host does not oscillate between working and not from
	  one click to the next. Explorer restarts, and every other host is
	  short-lived by comparison, so "for this process" is a bounded sentence.

	Deliberately free of Win32 and of any global state, so the state machine can
	be driven directly by a test rather than inferred from a running Explorer.
*/

#include <atomic>
#include <cstdint>

namespace Nilesoft
{
	namespace Shell
	{
		class TakeoverBreaker
		{
		public:
			// Consecutive failures that open the breaker.
			static constexpr uint32_t THRESHOLD = 3;

			// Whether takeover should be attempted at all. False means the
			// caller goes straight to the host's own menu.
			bool should_attempt() const noexcept
			{
				return !_open.load(std::memory_order_relaxed);
			}

			bool is_open() const noexcept
			{
				return _open.load(std::memory_order_relaxed);
			}

			uint32_t consecutive_failures() const noexcept
			{
				return _consecutive.load(std::memory_order_relaxed);
			}

			// How many menus have been given to the host since the breaker
			// opened. This is the number worth reporting - it says how much the
			// user has actually lost, which "the breaker is open" does not.
			uint64_t skipped() const noexcept
			{
				return _skipped.load(std::memory_order_relaxed);
			}

			// A menu Shell composed and tracked.
			void record_success() noexcept
			{
				_consecutive.store(0, std::memory_order_relaxed);
			}

			// A takeover that was attempted and did not happen. Returns true if
			// this failure is the one that opened the breaker, so the caller can
			// say so once rather than on every menu afterwards.
			bool record_failure() noexcept
			{
				auto count = _consecutive.fetch_add(1, std::memory_order_relaxed) + 1;
				if(count < THRESHOLD)
					return false;

				// exchange, not store: several threads can reach the threshold
				// together, and exactly one of them should be told it was the
				// one that opened it.
				return !_open.exchange(true, std::memory_order_relaxed);
			}

			// Called for each menu handed to the host while the breaker is open.
			void record_skipped() noexcept
			{
				_skipped.fetch_add(1, std::memory_order_relaxed);
			}

			// Tests only. Nothing in the shipping path closes a breaker.
			void reset() noexcept
			{
				_consecutive.store(0, std::memory_order_relaxed);
				_open.store(false, std::memory_order_relaxed);
				_skipped.store(0, std::memory_order_relaxed);
			}

		private:
			std::atomic<uint32_t> _consecutive{};
			std::atomic<bool> _open{};
			std::atomic<uint64_t> _skipped{};
		};
	}
}
