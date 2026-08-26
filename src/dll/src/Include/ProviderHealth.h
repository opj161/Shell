#pragma once

/*
	How long each packaged verb handler takes, and what to do about the slow ones.

	Measured first, because the numbers decide the design. On this machine
	(Windows 11 26200.8875 x64, 2026-08-24), with 23 registered
	windows.fileExplorerContextMenus handlers and a single-file selection, doing
	exactly what fill_menuitem_from_explorer_command does - CoCreateInstance,
	GetState, GetTitle, GetFlags, GetIcon - on the thread between the right-click
	and the first pixel:

		first menu in a process   ~700 ms
		every menu after that     ~170 ms
		of which CoCreateInstance   ~46 ms   (about 2 ms x 23, warm)
		of which icon extraction    ~32 ms
		worst single provider        ~62 ms   warm, every single time

	170 ms, on every right-click, before anything is drawn. That is eleven times
	the 15 ms first-paint budget in docs/refactor/06 section 4, and it dwarfs the
	111 ms packaged-verb scan that 7ee2433 moved off this thread.

	Three things follow, and the measurements say which order they matter in.

	1. Reuse the provider objects. Activation is the single largest line and it
	   is the one call in the sequence that does not depend on the selection at
	   all - GetState, GetTitle and GetIcon each take an IShellItemArray
	   parameter, so a live IExplorerCommand is designed to be asked again.
	   Measured: keeping them alive drops a warm menu from ~170 ms to ~41 ms.

	2. Bound the total. Even with reuse, ~41 ms of third-party calls remain, and
	   nothing bounds a handler that decides to take longer. The interface
	   documentation is guidance, not enforcement: "None of the methods of this
	   interface should communicate with network resources. These methods are
	   called on the UI thread, so communication with network resources could
	   cause the UI to stop responding."
	   https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorercommand

	3. Remember which ones are slow. A whole-menu budget alone makes the *tail*
	   of the list pay for the head; remembering means a provider that has never
	   once been fast is skipped before it is called, and the ones that are fast
	   keep their place.

	This header is the third part, plus the bookkeeping the first two need.

	What is deliberately NOT here: moving the calls to a worker thread. The plan
	(docs/refactor/02 section 2a) proposed resolving providers on the worker
	under a deadline, which would bound even a single handler that blocks for
	seconds. The documentation quoted above says these methods "are called on the
	UI thread" - that is a statement about the environment handlers are written
	against, and calling them from a thread that owns no windows and is not the
	one holding the browser is a divergence with no way to test its blast radius
	on this machine. So a single pathological provider still blocks a menu once;
	after that it is remembered and skipped. Stated rather than hidden.
*/

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		/*
			How much selection a provider was asked about.

			docs/refactor/02-first-paint-latency.md section 2a specifies the
			key for a provider's remembered cost as `(clsid, selection_shape)`.
			The first implementation dropped the shape and keyed on the CLSID
			alone, and that was measured to matter on 2026-08-25: with 200 files
			selected in a real Explorer, one handler ("Convert to Adobe PDF")
			cost **209 ms** and the whole menu **634 ms**, three times running -
			against ~10 ms for the same menu over a single file.

			Nothing condemned that handler, because judgement is on a provider's
			*best ever* time and its best was measured on a one-file selection.
			A cost measured against one file is not a prediction about two
			hundred, so remembering them in the same slot means the budget
			admits providers it cannot afford and the deferral rule never fires.

			Four buckets rather than an exact count, because the question is
			"roughly how much work is this" and an exact key would remember
			every selection size separately and so learn nothing about any of
			them. The boundaries are where behaviour changes rather than round
			numbers: a background click has no items at all, one item is the
			overwhelmingly common case and the one every handler is optimised
			for, and past a dozen or so a handler that inspects each item starts
			to dominate.
		*/
		enum class SelectionShape : uint8_t
		{
			Background,		// no items - a right-click on the view's background
			Single,			// exactly one
			Few,			// 2..16
			Many,			// more than 16
		};

		inline SelectionShape selection_shape(size_t count) noexcept
		{
			if(count == 0)
				return SelectionShape::Background;
			if(count == 1)
				return SelectionShape::Single;
			return count <= 16 ? SelectionShape::Few : SelectionShape::Many;
		}

		struct ProviderTiming
		{
			uint32_t clsid_hash{};

			// Part of the key, not of the payload: one provider has one entry
			// per shape it has been asked about.
			SelectionShape shape{ SelectionShape::Single };

			uint32_t best_us{ UINT32_MAX };	// the fastest it has ever answered
			uint32_t last_us{};
			uint32_t worst_us{};
			uint16_t samples{};
			uint16_t deferrals{};
			uint16_t failures{};
			uint16_t since_probe{};			// menus skipped since it was last tried

			// Consecutive menus this provider was refused because its predicted
			// cost did not fit, as opposed to because it was judged slow.
			//
			// A separate counter, and not a reuse of either of the two above.
			// `deferrals` is incremented by both kinds and is a reporting total;
			// `since_probe` carries the slow-provider accounting and is
			// deliberately *not* touched by a budget deferral, because a
			// provider the menu could not afford has not had its turn. Neither
			// can answer "has this one been refused so often that it needs a
			// forced turn", which is the question that keeps it alive.
			//
			// Reset by record() and by note_reprobe_started: any answer at all,
			// forced or not, ends the streak.
			//
			// In-process only. PerfExportProvider is a separate layout, so this
			// does not touch PERF_EXPORT_VERSION.
			uint16_t budget_deferrals{};
		};

		/*
			Thresholds, all derived from the measurement above rather than
			chosen.

			SLOW_PROVIDER_US separates the population cleanly: warm, every
			provider on this machine answers in 1-5 ms except one outlier at
			~62 ms. 25 ms sits in the gap with room on both sides.

			MENU_BUDGET_US is set just above the measured steady-state total with
			reuse (~41 ms), so in normal operation it never bites - it exists for
			the first menu in a process, where everything is cold and the true
			cost is ~700 ms, and for a machine with far more handlers than this
			one.

			REPROBE_AFTER stops a deferral being a life sentence - an application
			update can make a slow handler fast, and nothing would ever find out.
			It is deliberately large. A re-probe means paying that provider's
			full cost again, so for a genuinely pathological one the interval is
			what decides how often the user sees a slow menu: at 20 it would be
			every twentieth right-click, which is often enough to notice. At 200
			it is a couple of seconds spread across a day's use, and one fast
			answer restores the provider permanently because the judgement is on
			its *best* time.

			MIN_SAMPLES_TO_JUDGE is the one that stops this design eating itself,
			and it is not obvious. On the first menu in a process *everything* is
			cold and slow - the measurements above are 700 ms across 23 handlers,
			so nearly all of them look pathological. Judge on one sample and the
			second menu defers every provider that got sampled; the budget means
			only a couple are sampled per menu, so each menu quietly marks two
			more as slow, and after a dozen right-clicks the menu has no packaged
			verbs in it at all. Permanently, from the user's point of view.

			Requiring two samples costs one extra slow menu for a provider that
			really is pathological, and buys the guarantee that a provider is
			never condemned on its cold time. By the second sample the object is
			cached and activation is not being charged to it, so what is measured
			is what it will cost from then on.
		*/
		inline constexpr uint32_t SLOW_PROVIDER_US = 25000;
		inline constexpr uint32_t MENU_BUDGET_US = 50000;
		inline constexpr uint16_t REPROBE_AFTER = 200;
		inline constexpr uint16_t MIN_SAMPLES_TO_JUDGE = 2;

		/*
			The same idea as REPROBE_AFTER, for the other way a provider can
			stop being called - and an order of magnitude smaller, because it is
			buying something an order of magnitude cheaper.

			REPROBE_AFTER is calibrated for a provider whose probe *costs the
			user a slow menu*: it is known slow, that is why it was deferred, so
			200 keeps the price to a couple of seconds spread over a day's use.

			A budget-deferred provider is in the opposite position. It is not
			judged slow - its best time is under SLOW_PROVIDER_US, which is why
			it was classified Known rather than condemned - so the belief is
			that it is cheap and one sample was unlucky. Forcing its turn costs
			whatever it actually costs now, which is expected to be small, and
			the answer immediately corrects the estimate that got it refused.
			There is no reason to make it wait 200 menus to find that out, and
			every reason not to: until it answers, its `last_us` cannot come
			down, so the estimate that refused it cannot change on its own.

			20 is roughly "within one sitting" rather than "eventually".
		*/
		inline constexpr uint16_t BUDGET_REPROBE_AFTER = 20;

		/*
			Process-lifetime, shared across menu threads, guarded by one lock.
			The lock is taken twice per provider per menu - once to decide, once
			to record - against work measured in milliseconds, so it is not on
			any path that cares.
		*/
		class ProviderHealth
		{
		public:
			static ProviderHealth &instance()
			{
				static ProviderHealth *health = new ProviderHealth();
				return *health;
			}

			ProviderHealth() = default;
			ProviderHealth(const ProviderHealth &) = delete;
			ProviderHealth &operator=(const ProviderHealth &) = delete;

			/*
				What is remembered about a provider, without deciding anything
				and without moving anything.

				This class used to expose consider(), which answered "try it or
				not" and mutated as it went - advancing since_probe, resetting it
				when a re-probe was granted, counting a deferral. That is correct
				for a caller that asks once per provider in the order it intends
				to call them, and wrong for one that wants to *plan* first: a
				planning pass that asked consider() for every registration would
				advance every slow provider's re-probe counter once per menu
				whether or not it ever got called, and count deferrals for
				providers it later decided to call after all.

				So planning reads, and the decisions that change state are named:
				note_slow_deferral, note_reprobe_started, note_budget_deferral.
				Include/ProviderSchedule.h.

				consider() itself is gone. It had had no caller in the product
				since the two-phase scheduler landed, and twenty-nine in
				test_provider_health.cpp - two divergent admission policies, of
				which only the unused one was under test. Those tests now drive
				the shipping sequence through a helper that transcribes
				ExplorerCommand.cpp's loop.
			*/
			bool classify(uint32_t clsid_hash, SelectionShape shape,
						  ProviderTiming *out) const
			{
				return lookup(clsid_hash, shape, out);
			}

			// This provider is judged slow and it is not its turn: charge the
			// deferral and move it one menu closer to a re-probe.
			void note_slow_deferral(uint32_t clsid_hash, SelectionShape shape)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				if(auto *timing = find(clsid_hash, shape))
				{
					if(timing->since_probe < UINT16_MAX)
						timing->since_probe++;
					if(timing->deferrals < UINT16_MAX)
						timing->deferrals++;
				}
			}

			// A re-probe was granted. Reset the counter here rather than in
			// record(), so a probe that is started and then refused by the live
			// budget does not leave the provider due forever.
			void note_reprobe_started(uint32_t clsid_hash, SelectionShape shape)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				if(auto *timing = find(clsid_hash, shape))
				{
					timing->since_probe = 0;
					timing->budget_deferrals = 0;
				}
			}

			// The menu ran out before this one was reached. Not a judgement
			// about the provider, so it does not touch since_probe: a provider
			// the menu could not afford has not had its turn.
			//
			// It does advance budget_deferrals, which is what makes that
			// sentence true rather than aspirational. Without a count of its
			// own, "has not had its turn" was a state with no exit: nothing
			// else in this class moves while a provider is being refused, so a
			// refusal that was going to repeat forever looked exactly like one
			// that was not.
			void note_budget_deferral(uint32_t clsid_hash, SelectionShape shape)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				if(auto *timing = find(clsid_hash, shape))
				{
					if(timing->deferrals < UINT16_MAX)
						timing->deferrals++;
					if(timing->budget_deferrals < UINT16_MAX)
						timing->budget_deferrals++;
				}
			}

			void record(uint32_t clsid_hash, SelectionShape shape,
						uint32_t microseconds, bool succeeded)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				auto *timing = find(clsid_hash, shape);
				if(!timing)
				{
					if(_timings.size() >= MAX_TRACKED)
						return;
					_timings.push_back(ProviderTiming{});
					timing = &_timings.back();
					timing->clsid_hash = clsid_hash;
					timing->shape = shape;
				}

				timing->last_us = microseconds;
				if(microseconds < timing->best_us)
					timing->best_us = microseconds;
				if(microseconds > timing->worst_us)
					timing->worst_us = microseconds;
				if(timing->samples < UINT16_MAX)
					timing->samples++;
				timing->since_probe = 0;

				// It answered, so it is not being starved, whatever the answer
				// was. Both streaks end here.
				timing->budget_deferrals = 0;

				if(!succeeded && timing->failures < UINT16_MAX)
					timing->failures++;
			}

			/*
				Advances once per menu, and is the whole of exploration fairness.

				A provider with fewer than MIN_SAMPLES_TO_JUDGE samples cannot be
				predicted, so it cannot be ordered by cost - it has to be tried.
				If trying always began at the head of registration order, an
				expensive cold provider near the front could consume the menu's
				allowance every time and an unknown tail would never be sampled
				at all: never sampled means never judged, and never judged means
				never schedulable. Rotating the starting point removes that fixed
				point without needing any per-provider state.

				Process-wide and monotonic; wrapping is harmless, since only
				`% count` is used. Include/ProviderSchedule.h.
			*/
			uint64_t next_exploration_cursor() noexcept
			{
				return _exploration_cursor.fetch_add(1, std::memory_order_relaxed);
			}

			bool lookup(uint32_t clsid_hash, SelectionShape shape, ProviderTiming *out) const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				for(const auto &t : _timings)
				{
					if(t.clsid_hash == clsid_hash && t.shape == shape)
					{
						if(out)
							*out = t;
						return true;
					}
				}
				return false;
			}

			std::vector<ProviderTiming> snapshot() const
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return _timings;
			}

			void clear()
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_timings.clear();
			}

		private:
			// One entry per registered handler *per selection shape*. This
			// machine has 23 handlers and there are four shapes, so 92 in the
			// worst case; the cap is a bound on a list nothing else bounds,
			// not a considered limit.
			static constexpr size_t MAX_TRACKED = 256;

			ProviderTiming *find(uint32_t clsid_hash, SelectionShape shape)
			{
				for(auto &t : _timings)
				{
					if(t.clsid_hash == clsid_hash && t.shape == shape)
						return &t;
				}
				return nullptr;
			}

			mutable std::mutex _mutex;
			std::vector<ProviderTiming> _timings;
			std::atomic<uint64_t> _exploration_cursor{ 0 };
		};

		// FNV-1a over the raw GUID bytes. An identity for a timing table and a
		// diagnostics record, not a security boundary.
		inline uint32_t provider_hash(const GUID &clsid) noexcept
		{
			auto bytes = reinterpret_cast<const unsigned char *>(&clsid);
			uint32_t h = 2166136261u;
			for(size_t i = 0; i < sizeof(GUID); i++)
			{
				h ^= bytes[i];
				h *= 16777619u;
			}
			return h;
		}

		/*
			The whole-menu allowance, as a plain countdown.

			Deliberately not a scope object: it is read between providers to
			decide whether the next one gets asked, so what it needs is a
			remaining() rather than a destructor.
		*/
		struct ProviderBudget
		{
			LARGE_INTEGER start{};
			uint32_t total_us{ MENU_BUDGET_US };

			static ProviderBudget begin(uint32_t total_us = MENU_BUDGET_US) noexcept
			{
				ProviderBudget b;
				b.total_us = total_us;
				::QueryPerformanceCounter(&b.start);
				return b;
			}

			uint32_t spent_us() const noexcept
			{
				LARGE_INTEGER now{};
				::QueryPerformanceCounter(&now);
				LARGE_INTEGER frequency{};
				if(!::QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
					return 0;
				auto ticks = now.QuadPart - start.QuadPart;
				if(ticks <= 0)
					return 0;
				return static_cast<uint32_t>((ticks * 1000000LL) / frequency.QuadPart);
			}

			uint32_t remaining_us() const noexcept
			{
				auto spent = spent_us();
				return spent >= total_us ? 0u : total_us - spent;
			}
		};
	}
}
