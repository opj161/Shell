#pragma once

/*
	How the taskbar hit-test was answered, counted whether or not anybody asked.

	docs/refactor/02-first-paint-latency.md section 5 sets the acceptance for
	this path, and it is deliberately not "make the wait faster":

		"The bounded CoWaitForMultipleHandles path remains as the fallback for a
		 cold layout, and the ring records how often it is taken; that
		 frequency, not the wait itself, is the number to drive to zero."

	Nothing recorded it. The wait costs a measured ~2-3 ms when UI Automation
	answers, which is not worth engineering against - but when it does *not*
	answer inside the budget, TaskbarUiaWorker::query returns false and Windows
	handles the click, so Shell's menu silently does not appear. That is a
	user-visible failure with no trace anywhere, and it is the outcome worth
	counting.

	Why this is not a MenuSessionRecord phase: the hit-test decides whether
	there will *be* a menu, so it runs before any session begins, and
	session_phase() on a thread with no open session is dropped by design
	(Include/Diagnostics/DiagnosticsRing.h). These are process-lifetime counters
	instead, read by whoever is asking - the log, a report, eventually the
	Reliability Center (docs/refactor/05 section 1).

	Cost: one relaxed atomic increment per right-click on the taskbar. No lock,
	no allocation. Relaxed is the right ordering because these counters are
	statistics - nothing else is published through them, so there is no
	happens-before relationship for an acquire to establish.

		https://learn.microsoft.com/en-us/cpp/standard-library/atomic
*/

#include <atomic>
#include <cstdint>

namespace Nilesoft
{
	namespace Shell
	{
		class TaskbarHitStats
		{
		public:
			enum class Outcome
			{
				// A previous answer for this bucket was still fresh. The wait
				// was not taken; this is the outcome to drive towards.
				CacheHit,

				// The worker answered inside the budget.
				Answered,

				// The budget elapsed, or what was on the answer slot belonged to
				// an earlier request. Windows handled the click and Shell's menu
				// did not appear.
				TimedOut,

				// The worker could not be started at all, so there was nothing
				// to wait for.
				Unavailable,
			};

			struct Snapshot
			{
				uint64_t cache_hits{};
				uint64_t answered{};
				uint64_t timed_out{};
				uint64_t unavailable{};

				// Over the waits that were actually taken - answered and timed
				// out alike. A cache hit contributes no time.
				uint64_t waited_microseconds{};
				uint32_t worst_microseconds{};

				uint64_t waits() const noexcept { return answered + timed_out; }
				uint64_t total() const noexcept { return cache_hits + answered + timed_out + unavailable; }
			};

			void record(Outcome outcome, uint32_t microseconds) noexcept
			{
				switch(outcome)
				{
					case Outcome::CacheHit:    _cache_hits.fetch_add(1, std::memory_order_relaxed); return;
					case Outcome::Unavailable: _unavailable.fetch_add(1, std::memory_order_relaxed); return;
					case Outcome::Answered:    _answered.fetch_add(1, std::memory_order_relaxed); break;
					case Outcome::TimedOut:    _timed_out.fetch_add(1, std::memory_order_relaxed); break;
				}

				// Only a wait that happened contributes to the timings.
				_waited_us.fetch_add(microseconds, std::memory_order_relaxed);

				// Compare-exchange rather than a plain store: two taskbar threads
				// can be waiting at once, and a store would let the slower one
				// lose to whichever wrote last rather than to whichever was worse.
				auto worst = _worst_us.load(std::memory_order_relaxed);
				while(microseconds > worst &&
					  !_worst_us.compare_exchange_weak(worst, microseconds,
													   std::memory_order_relaxed))
				{
					// worst has been reloaded with the current value; loop.
				}
			}

			Snapshot snapshot() const noexcept
			{
				Snapshot s;
				s.cache_hits = _cache_hits.load(std::memory_order_relaxed);
				s.answered = _answered.load(std::memory_order_relaxed);
				s.timed_out = _timed_out.load(std::memory_order_relaxed);
				s.unavailable = _unavailable.load(std::memory_order_relaxed);
				s.waited_microseconds = _waited_us.load(std::memory_order_relaxed);
				s.worst_microseconds = _worst_us.load(std::memory_order_relaxed);
				return s;
			}

			void reset() noexcept
			{
				_cache_hits.store(0, std::memory_order_relaxed);
				_answered.store(0, std::memory_order_relaxed);
				_timed_out.store(0, std::memory_order_relaxed);
				_unavailable.store(0, std::memory_order_relaxed);
				_waited_us.store(0, std::memory_order_relaxed);
				_worst_us.store(0, std::memory_order_relaxed);
			}

		private:
			std::atomic<uint64_t> _cache_hits{};
			std::atomic<uint64_t> _answered{};
			std::atomic<uint64_t> _timed_out{};
			std::atomic<uint64_t> _unavailable{};
			std::atomic<uint64_t> _waited_us{};
			std::atomic<uint32_t> _worst_us{};
		};
	}
}
