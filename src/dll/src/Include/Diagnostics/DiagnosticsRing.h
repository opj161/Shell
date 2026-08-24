#pragma once

/*
	Where a menu's time went, recorded whether or not anybody asked.

	MenuPerf is opt-in for a good reason: Logger opens, appends and closes the
	log file for every line, so leaving it on would put file I/O on the exact
	path it exists to measure. But that reason is about the *sink*, not about the
	measurement, and it has a cost of its own - the phase timings only exist on a
	machine where somebody has already reproduced the problem and set a registry
	value. Everything built on top of them (provider health, a regression budget,
	an answer to "why is my menu slow?") needs the numbers to be there the first
	time.

	So: keep the phases, change the sink. Timing goes into a fixed-size ring in
	memory, always. The log file is written only when the perf value is set, and
	only for phases that breach its floor, exactly as before.

	What that costs, per phase: two QueryPerformanceCounter calls and a store
	into a thread-local array. No lock, no allocation, no formatting. The only
	lock is taken once per menu, after it has closed, to copy one record into the
	process-wide ring.

	Measured rather than asserted, on this machine (Windows 11 26200.8875 x64,
	2026-08-24), against the budget of 1 us per phase record that
	docs/refactor/02-first-paint-latency.md section 7 sets:

		session_phase(), the store alone           2.7 ns
		a full phase, two QPC calls plus the store  47.8 ns
		a whole menu, twelve phases and publish    ~0.6 us

	So publishing costs about 70 ns, and an entire menu's worth of always-on
	timing costs less than one phase was budgeted. The ring itself is 30 KB of
	fixed storage - 600 bytes a record, fifty of them - allocated once.

	Concurrency. Menu sessions run on several threads at once - Explorer windows,
	each taskbar - so the ring is many-writer. Rather than guard it per phase,
	each session accumulates into thread-local storage, where it has exactly one
	writer and needs no synchronisation at all, and publishes once at the end.

	Nesting. A menu can open while another is up (TPM_RECURSE), and Shell's own
	hook can re-enter. Sessions therefore nest by depth: the inner ones fold
	their phases into the outer record and only the outermost publishes. Losing
	the inner boundary is better than either clobbering the outer session or
	allocating.

	Overflow is counted, not hidden. There are more distinct phase names in the
	menu path than the eight the plan sketched, and a session that recorded eight
	and silently dropped the rest would be worse than useless - the phases that
	go missing under load are exactly the slow ones. dropped_phases says how many
	did not fit.
*/

#include <windows.h>
#include <cstdint>
#include <mutex>

namespace Nilesoft
{
	namespace Shell
	{
		namespace Diagnostics
		{
			// How the hook resolved one interception. docs/refactor/01 section 2
			// requires every exit path to map to one of these.
			enum class TakeoverDecision : uint8_t
			{
				Unknown,
				TakeOver,
				BypassOnce,
				Degraded,
				FailOpen,

				// Shell looked and chose not to compose a menu here: not a
				// window it handles, a configuration that hides the menu in
				// this context, or no generation currently being served.
				//
				// Separate from FailOpen because the two need opposite
				// responses. A fail-open is evidence that takeover is broken
				// and feeds the circuit breaker; a decline is the normal
				// answer for most popups in a host that is not Explorer, and
				// counting it switched Shell off for the whole process.
				// docs/refactor/01-takeover-contract.md section 7a.
				Declined,
			};

			enum class ProviderResult : uint8_t
			{
				Ok,
				Pending,		// GetState answered E_PENDING
				Failed,			// activation or a metadata call failed
				Deferred,		// missed its first-paint deadline (docs/refactor/02 section 2a)

				// The user quarantined it (docs/refactor/05 section 1b). A
				// separate word from Deferred on purpose: "it has never once
				// been quick" and "you told me to stop asking" are different
				// answers, and only one of them is Shell's own judgement.
				Quarantined,
			};

			struct PhaseRecord
			{
				// A literal. Every phase name in the tree is a string literal
				// with static storage, so this is a pointer rather than a copy -
				// which is what makes recording a phase free of allocation.
				const wchar_t *name{};
				uint32_t microseconds{};
				int32_t count{ -1 };	// items, depth, whatever the phase counts
			};

			struct ProviderRecord
			{
				uint32_t clsid_hash{};
				uint32_t microseconds{};
				ProviderResult result{ ProviderResult::Ok };
			};

			// Sized from the phases that exist rather than from a round number:
			// the menu path currently names eleven, plus three SEH-safe marks in
			// the hook, and a nested session folds its own in on top.
			inline constexpr uint8_t MAX_PHASES = 24;
			inline constexpr uint8_t MAX_PROVIDERS = 16;
			inline constexpr size_t RING_CAPACITY = 50;

			struct MenuSessionRecord
			{
				uint64_t tick{};			// GetTickCount64 at session start
				uint32_t host_hash{};

				// The uFlags the host passed to its own tracking call, verbatim,
				// before Shell touched them.
				//
				// This is what says which half of complete_host_contract a host
				// exercises. A host that sets TPM_RETURNCMD gets an identifier
				// back; one that does not is told TRUE and notified separately,
				// and until a real third-party host was measured there was no way
				// to know which of those two paths had ever run outside a test.
				// docs/refactor/01-takeover-contract.md section 3.
				uint32_t host_flags{};
				uint32_t total_microseconds{};
				TakeoverDecision decision{ TakeoverDecision::Unknown };
				uint8_t phase_count{};
				uint8_t provider_count{};
				uint8_t dropped_phases{};
				uint8_t dropped_providers{};
				PhaseRecord phases[MAX_PHASES]{};
				ProviderRecord providers[MAX_PROVIDERS]{};

				void reset()
				{
					*this = MenuSessionRecord{};
				}

				void add_phase(const wchar_t *name, uint32_t microseconds, int32_t count)
				{
					if(phase_count >= MAX_PHASES)
					{
						if(dropped_phases < 255)
							dropped_phases++;
						return;
					}
					phases[phase_count].name = name;
					phases[phase_count].microseconds = microseconds;
					phases[phase_count].count = count;
					phase_count++;
				}

				void add_provider(uint32_t clsid_hash, uint32_t microseconds, ProviderResult result)
				{
					if(provider_count >= MAX_PROVIDERS)
					{
						if(dropped_providers < 255)
							dropped_providers++;
						return;
					}
					providers[provider_count].clsid_hash = clsid_hash;
					providers[provider_count].microseconds = microseconds;
					providers[provider_count].result = result;
					provider_count++;
				}
			};

			/*
				The process-wide ring. Fixed storage, one lock, taken only by
				publish() and by a reader - never on the measured path.

				Process lifetime and no destructor, like every other service here:
				the module is pinned for the life of the host, and a static whose
				destructor could run while a menu thread is still publishing is a
				crash waiting for an unlucky shutdown.
			*/
			class DiagnosticsRing
			{
			public:
				// Constructible on its own as well as through instance(): the
				// wrap and ordering rules are worth testing without reaching for
				// process-wide state a previous test may have written to.
				DiagnosticsRing() = default;

				DiagnosticsRing(const DiagnosticsRing &) = delete;
				DiagnosticsRing &operator=(const DiagnosticsRing &) = delete;

				static DiagnosticsRing &instance()
				{
					static DiagnosticsRing *ring = new DiagnosticsRing();
					return *ring;
				}

				void publish(const MenuSessionRecord &record)
				{
					std::lock_guard<std::mutex> lock(_mutex);
					_records[_next] = record;
					_next = (_next + 1) % RING_CAPACITY;
					if(_size < RING_CAPACITY)
						_size++;
					_published++;
				}

				// Copies out the most recent sessions, newest first. Returns how
				// many were written. The caller owns the buffer; nothing here
				// hands out a pointer into the ring, because a reader that walked
				// it while a menu closed would read a half-written record.
				size_t snapshot(MenuSessionRecord *out, size_t capacity) const
				{
					if(!out || capacity == 0)
						return 0;

					std::lock_guard<std::mutex> lock(_mutex);
					auto want = capacity < _size ? capacity : _size;
					for(size_t i = 0; i < want; i++)
					{
						// _next is one past the newest, so walk backwards from
						// there, wrapping.
						auto index = (_next + RING_CAPACITY - 1 - i) % RING_CAPACITY;
						out[i] = _records[index];
					}
					return want;
				}

				size_t size() const
				{
					std::lock_guard<std::mutex> lock(_mutex);
					return _size;
				}

				// Total ever published, which keeps counting after the ring wraps.
				uint64_t published() const
				{
					std::lock_guard<std::mutex> lock(_mutex);
					return _published;
				}

				void clear()
				{
					std::lock_guard<std::mutex> lock(_mutex);
					_next = 0;
					_size = 0;
					_published = 0;
				}

			private:
				mutable std::mutex _mutex;
				MenuSessionRecord _records[RING_CAPACITY]{};
				size_t _next{};
				size_t _size{};
				uint64_t _published{};
			};

			/*
				The current session on this thread.

				Thread-local, so the accumulate path has one writer and takes no
				lock. begin()/end() nest by depth; only the outermost end()
				publishes.
			*/
			struct SessionSlot
			{
				MenuSessionRecord record;
				int depth{};
				LARGE_INTEGER started{};
			};

			SessionSlot &current_session() noexcept;

			void session_begin(uint32_t host_hash, uint32_t host_flags) noexcept;
			void session_phase(const wchar_t *name, uint32_t microseconds, int32_t count) noexcept;
			void session_provider(uint32_t clsid_hash, uint32_t microseconds, ProviderResult result) noexcept;
			void session_decision(TakeoverDecision decision) noexcept;

			/*
				The title a provider gave itself, for the export's name
				directory.

				Deliberately not part of the session record. A record carries a
				hash so the measured path never copies a string and so a report
				can say "these forty menus were all the same handler"; the name
				belongs to the *provider*, not to the menu, so it is written
				once per distinct CLSID per process and read beside the records.
				docs/refactor/05-capabilities.md section 1.

				Costs a scan of at most 32 integers after the first sighting.
			*/
			void provider_name(uint32_t clsid_hash, const GUID &clsid, const wchar_t *name) noexcept;

			// Publishes if this was the outermost begin(). Safe to call without a
			// matching begin() - it does nothing, which is what a hook path that
			// bailed before starting a session needs.
			void session_end() noexcept;
		}
	}
}
