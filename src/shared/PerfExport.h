#pragma once

/*
	Reading the diagnostics ring from outside the process that filled it.

	docs/refactor/06-phases-and-tests.md section 4 names `shell.exe -report perf`
	and docs/refactor/05-capabilities.md section 1 (the Reliability Center) needs
	it; docs/refactor/08-handoff.md section 3.2 explains why nothing else will
	do. The ring
	(src/dll/src/Include/Diagnostics/DiagnosticsRing.h) is process-local and the
	interesting process is somebody else's explorer.exe, which nobody can attach
	a debugger to. The documented substitute - the `perf` registry value, which
	writes breaching phases to shell.log - was measured on 2026-08-24 producing
	nothing at all from explorer.exe while the same DLL logged freely from
	another host in the same session. So the numbers exist and there has been no
	way to look at them.

	This is the channel. Every process that loads Shell publishes its last
	sessions into a small named section; a reader enumerates the processes,
	opens whichever of those sections it is allowed to, and formats what it
	finds.

	Four decisions, each with a plausible opposite:

	**A section per process, named by process id - not one shared block with a
	slot per process.** The shared block is the smaller allocation and it is the
	wrong shape. It would make every host in the session a writer into memory
	every other host can write too, needing slot claiming, a cross-process lock
	and a story for a host that dies mid-write. Per-process, each block has
	exactly one writer for its whole life, and the security descriptor is the
	creating process's own: a section created by an elevated host is simply not
	openable by a medium-integrity reader, which is the correct answer rather
	than a shared writable surface anything in the session can corrupt. The cost
	is that a reader has to enumerate processes to find the blocks, and "all
	users have read access to the list of processes in the system"
	(https://learn.microsoft.com/windows/win32/procthread/process-enumeration),
	so that cost is a few hundred OpenFileMapping calls that fail immediately.

	**A sequence counter, not a mutex.** A cross-process mutex would let a
	reader block a host's menu thread, and a host killed while holding it leaves
	it abandoned. Here the writer never waits for anything: it bumps a counter
	odd, writes, bumps it even. A reader that sees an odd counter, or a
	different one after copying, discards what it read and tries again. Torn
	reads are the only failure and they are detected rather than prevented.

	**The block is a ring of its own, written one record at a time.** Mirroring
	the whole 50-record in-process ring on every publish would memcpy 30 KB per
	menu for the benefit of a reader that is almost never there. One record is
	about a kilobyte, and it is copied after the menu has closed - `publish()`
	is already off the measured path.

	**Everything a reader takes out of the block is clamped and re-terminated.**
	The block was written by another process, possibly at another integrity
	level, and the reader may be the one that matters. Same rule
	docs/refactor/02-first-paint-latency.md section 1 states for the catalog
	cache: parse it as untrusted input, fail closed. Nothing read from here
	steers anything; it is formatted and printed.

	Layout is fixed-width and pointer-free so a 32-bit shell.exe can read a
	64-bit host's block and the reverse. PhaseRecord's name is a `const wchar_t *`
	in the ring - a pointer into that process's own image, which is exactly why
	the export copies the characters instead.

	Contracts this depends on:

	- "To share memory that is not associated with a file, a process must use
	  the CreateFileMapping function and specify INVALID_HANDLE_VALUE as the
	  hFile parameter... You must specify a size greater than zero"
	  (https://learn.microsoft.com/windows/win32/memory/sharing-files-and-memory).
	- "The name can have a 'Global' or 'Local' prefix to explicitly create the
	  object in the global or session namespace. The remainder of the name can
	  contain any character except the backslash character... Creating a file
	  mapping object in the global namespace from a session other than session
	  zero requires the SeCreateGlobalPrivilege privilege"
	  (https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-createfilemappingw).
	  Local it is - a host and the shell.exe reading it are the same user in the
	  same session, and asking for a privilege to print a timing report would be
	  absurd.
	- "If the object exists before the function call, the function returns a
	  handle to the existing object (with its current size, not the specified
	  size), and GetLastError returns ERROR_ALREADY_EXISTS" (same page). Which
	  is why the header is validated after mapping rather than assumed from the
	  size that was asked for.
*/

#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstddef>
#include <atomic>

namespace Nilesoft
{
	namespace Shell
	{
		namespace Diagnostics
		{
			// 'NSPF' - Nilesoft Shell PerF. Little-endian, so it reads
			// backwards in a hex dump; that is fine, it is a number.
			inline constexpr uint32_t PERF_EXPORT_MAGIC = 0x4650534Eu;

			// Bumped whenever the layout changes. A reader refuses anything it
			// does not recognise rather than guessing, because the alternative
			// is printing a newer build's memory as if it were this one's.
			//
			// 2: `reserved` in PerfExportRecord became `host_flags`. Same size,
			// which is exactly why the version had to move - record_size alone
			// would not have caught it, and a version-1 host's zeroed reserved
			// field would have read as "this host passes no TPM flags at all".
			inline constexpr uint32_t PERF_EXPORT_VERSION = 2;

			// Caps. Deliberately smaller than the in-process ring: this is a
			// window onto recent activity, not an archive, and every byte here
			// is mapped into a host process that did not ask for it.
			// 40 fits the longest phase name in the tree,
			// `popup.context_construct_initialize` at 34, with room to grow.
			// A name is truncated rather than dropped, but a truncated name
			// reads as a different phase to anyone skimming, so the buffer is
			// sized from the names that exist rather than from a round number.
			inline constexpr size_t PERF_EXPORT_NAME = 40;		// phase name, wchar_t
			inline constexpr size_t PERF_EXPORT_HOST = 64;		// host image file name

			// 16 phases against the ring's 24. The menu path names eleven and
			// the hook adds three, so this is the normal case plus headroom -
			// and `dropped_phases` says when it was not enough, which is the
			// rule the ring's own header sets: overflow is counted, not hidden.
			inline constexpr uint8_t PERF_EXPORT_PHASES = 16;
			inline constexpr uint8_t PERF_EXPORT_PROVIDERS = 8;

			// 16 sessions, ~24 KB of shared commit. This is mapped into every
			// process that raises a context menu - two dozen on a normal
			// desktop - so it is a window onto recent activity, not an archive.
			inline constexpr uint32_t PERF_EXPORT_RECORDS = 16;

			// Deliberately not "%s%u" at every call site: the prefix contains a
			// backslash and the remainder must not, which is a rule worth
			// stating once.
			inline constexpr wchar_t PERF_EXPORT_PREFIX[] = L"Local\\Nilesoft.Shell.Perf.";


#pragma pack(push, 8)

			struct PerfExportPhase
			{
				uint32_t microseconds;
				int32_t count;					// items, depth, or -1 for none
				wchar_t name[PERF_EXPORT_NAME];	// truncated, always terminated
			};

			struct PerfExportProvider
			{
				uint32_t clsid_hash;
				uint32_t microseconds;
				uint32_t result;				// Diagnostics::ProviderResult
			};

			struct PerfExportRecord
			{
				uint64_t tick;					// GetTickCount64 at session start
				uint32_t host_hash;
				uint32_t total_microseconds;
				uint32_t decision;				// Diagnostics::TakeoverDecision
				uint32_t phase_count;
				uint32_t provider_count;
				uint32_t dropped_phases;
				uint32_t dropped_providers;

				// The TPM_* flags the host passed to its own tracking call,
				// verbatim. Which half of complete_host_contract a host
				// exercises turns entirely on TPM_RETURNCMD, and nothing could
				// say which one a real third-party host takes.
				// docs/refactor/01-takeover-contract.md section 3.
				uint32_t host_flags;
				PerfExportPhase phases[PERF_EXPORT_PHASES];
				PerfExportProvider providers[PERF_EXPORT_PROVIDERS];
			};

			struct PerfExportHeader
			{
				uint32_t magic;
				uint32_t version;
				uint32_t record_size;			// sizeof(PerfExportRecord) as written
				uint32_t capacity;				// records that follow
				uint32_t process_id;
				uint32_t architecture;			// IMAGE_FILE_MACHINE_* of the writer

				// Odd while a record is being written. See the file comment.
				uint32_t sequence;

				uint32_t next;					// slot the next record goes in
				uint32_t count;					// slots filled, <= capacity
				uint32_t reserved;
				uint64_t published;				// sessions ever, keeps counting past a wrap
				wchar_t host[PERF_EXPORT_HOST];	// image file name, not the full path
			};

			struct PerfExportBlock
			{
				PerfExportHeader header;
				PerfExportRecord records[PERF_EXPORT_RECORDS];
			};

#pragma pack(pop)

			// Layout is a wire format shared between architectures, so it is
			// asserted rather than assumed. If one of these fires, bump
			// PERF_EXPORT_VERSION - do not adjust the number to match.
			static_assert(sizeof(PerfExportPhase) == 8 + PERF_EXPORT_NAME * sizeof(wchar_t),
						  "PerfExportPhase gained padding");
			static_assert(sizeof(PerfExportProvider) == 12, "PerfExportProvider gained padding");
			static_assert(sizeof(PerfExportHeader) == 48 + PERF_EXPORT_HOST * sizeof(wchar_t),
						  "PerfExportHeader gained padding");
			static_assert(std::atomic<uint32_t>::is_always_lock_free,
						  "the sequence counter must not be emulated with a lock - "
						  "a lock in shared memory is exactly what this avoids");

			// Writes `Local\Nilesoft.Shell.Perf.<pid>` into `out`. Returns false
			// rather than truncating: a truncated name would silently address a
			// different process's block.
			inline bool perf_export_name(uint32_t process_id, wchar_t *out, size_t capacity)
			{
				if(!out || capacity == 0)
					return false;

				out[0] = L'\0';

				// 10 digits is the widest a uint32_t gets.
				constexpr size_t digits = 10;
				constexpr size_t prefix = sizeof(PERF_EXPORT_PREFIX) / sizeof(wchar_t) - 1;
				if(capacity < prefix + digits + 1)
					return false;

				size_t at = 0;
				for(size_t i = 0; i < prefix; i++)
					out[at++] = PERF_EXPORT_PREFIX[i];

				wchar_t number[digits + 1];
				size_t length = 0;
				auto value = process_id;
				do
				{
					number[length++] = static_cast<wchar_t>(L'0' + (value % 10));
					value /= 10;
				} while(value != 0);

				while(length > 0)
					out[at++] = number[--length];

				out[at] = L'\0';
				return true;
			}

			/*
				Copies one session into the block, in the shape a reader can
				trust: counts clamped to the caps, names truncated and
				terminated.

				Split out from the writer so the part that can be wrong - the
				clamping, the ring arithmetic, the sequence protocol - can be
				driven by a test against a plain struct, with no section, no
				second process and no timing.
			*/
			inline void perf_export_store(PerfExportBlock &block, const PerfExportRecord &record)
			{
				auto capacity = block.header.capacity;
				if(capacity == 0 || capacity > PERF_EXPORT_RECORDS)
					return;

				std::atomic_ref<uint32_t> sequence(block.header.sequence);

				// Odd: a reader that arrives now knows what it sees may tear.
				sequence.fetch_add(1, std::memory_order_acq_rel);

				auto slot = block.header.next % capacity;
				block.records[slot] = record;

				block.header.next = (slot + 1) % capacity;
				if(block.header.count < capacity)
					block.header.count++;
				block.header.published++;

				// Even again, and the record must be visible before the counter
				// says so - which is what release ordering here buys.
				sequence.fetch_add(1, std::memory_order_acq_rel);
			}

			/*
				Runs between the copy and the second sequence read, so a test
				can stage the case the second read exists for: a writer
				completing a whole record while the reader was copying.

				There is no other way to pin it. A concurrency test cannot
				assert its own premise here - measured on this machine, a writer
				storing in a tight loop makes *every* read torn (a 16-record
				copy is microseconds and a store is a fraction of one), so the
				test either observes no clean reads or, if the writer is
				throttled, no torn ones. Deleting the second read leaves every
				other test in the file passing, which was checked.

				Production passes nothing and pays a null check, on a path that
				runs a few times per report.
			*/
			using PerfExportInterpose = void (*)(void *);

			/*
				The other half: copy `want` most-recent records out, newest
				first, and say whether the block changed underneath.

				Returns the number written. `torn` is set when the sequence
				counter was odd on entry or moved during the copy - the caller
				retries, and after a few attempts reports the process as busy
				rather than printing a record that may be half of two.
			*/
			inline size_t perf_export_load(const PerfExportBlock &block,
										   PerfExportRecord *out, size_t want, bool &torn,
										   PerfExportInterpose interpose = nullptr,
										   void *interpose_context = nullptr)
			{
				// A caller that asked for nothing got everything it asked for.
				// Reporting that as torn would make the retry loop above spin
				// three times and then call the host busy.
				torn = false;
				if(!out || want == 0)
					return 0;

				torn = true;

				// const_cast because C++20's atomic_ref does not accept a
				// const-qualified type (P3323 adds that in C++26), and the
				// block a reader holds is genuinely const - it is mapped
				// FILE_MAP_READ, so a write here would fault rather than
				// corrupt. Only load() is called.
				std::atomic_ref<uint32_t> sequence(const_cast<uint32_t &>(block.header.sequence));

				auto before = sequence.load(std::memory_order_acquire);
				if((before & 1u) != 0)
					return 0;

				auto capacity = block.header.capacity;
				auto count = block.header.count;
				auto next = block.header.next;

				// Everything below is arithmetic on numbers another process
				// wrote. A capacity larger than the array is the one that would
				// read past the block, so it is refused rather than clamped.
				if(capacity == 0 || capacity > PERF_EXPORT_RECORDS)
				{
					torn = false;
					return 0;
				}
				if(count > capacity)
					count = capacity;
				if(next >= capacity)
					next = 0;

				auto written = want < count ? want : count;
				for(size_t i = 0; i < written; i++)
				{
					// next is one past the newest, so walk back from there.
					auto index = (next + capacity - 1 - static_cast<uint32_t>(i)) % capacity;
					out[i] = block.records[index];
				}

				if(interpose)
					interpose(interpose_context);

				// The fence, not the acquire on the load, is what keeps the
				// copy above from being sunk below this read. An acquire load
				// orders what comes *after* it; a seqlock needs what came
				// before it held in place, which is what an acquire fence
				// does - "no reads or writes in the current thread can be
				// reordered before this fence"
				// (https://en.cppreference.com/w/cpp/atomic/atomic_thread_fence).
				// Free on x86, and the reason this is not merely lucky.
				std::atomic_thread_fence(std::memory_order_acquire);

				auto after = sequence.load(std::memory_order_relaxed);
				torn = (after != before);
				return torn ? 0 : written;
			}

			// A record straight out of a block is not yet safe to print: the
			// counts and the name buffers are whatever the writer put there.
			// Nothing calls this before formatting.
			inline void perf_export_sanitize(PerfExportRecord &record)
			{
				if(record.phase_count > PERF_EXPORT_PHASES)
					record.phase_count = PERF_EXPORT_PHASES;
				if(record.provider_count > PERF_EXPORT_PROVIDERS)
					record.provider_count = PERF_EXPORT_PROVIDERS;

				for(auto &phase : record.phases)
					phase.name[PERF_EXPORT_NAME - 1] = L'\0';
			}

			inline const wchar_t *perf_export_decision_name(uint32_t decision)
			{
				switch(decision)
				{
					case 1: return L"takeover";
					case 2: return L"bypass";
					case 3: return L"degraded";
					case 4: return L"fail-open";
					case 5: return L"declined";
					default: return L"unknown";
				}
			}

			inline const wchar_t *perf_export_result_name(uint32_t result)
			{
				switch(result)
				{
					case 1: return L"pending";
					case 2: return L"failed";
					case 3: return L"deferred";
					default: return L"ok";
				}
			}

			/*
				The TPM_* flags a host passed, as words.

				Only the ones that change what Shell has to do on the way back
				out. TPM_RETURNCMD first because it is the one that decides
				which half of complete_host_contract runs; the alignment and
				button flags are noise in a report about contracts.

				Always writes something - "(none)" for a host that passed no
				flags at all, which is a real answer and the commonest one.
			*/
			inline void perf_export_flag_names(uint32_t flags, wchar_t *out, size_t capacity)
			{
				if(!out || capacity == 0)
					return;
				out[0] = L'\0';

				// The SDK's own constants, not the numbers. Written from memory
				// first and two of them were wrong - TPM_RIGHTBUTTON is 0x0002,
				// not 0x0004, so a report would have called a right-click menu
				// centre-aligned. The header is included here anyway.
				struct { uint32_t bit; const wchar_t *name; } known[] = {
					{ TPM_RETURNCMD, L"RETURNCMD" },
					{ TPM_NONOTIFY, L"NONOTIFY" },
					{ TPM_RIGHTBUTTON, L"RIGHTBUTTON" },
					{ TPM_VERTICAL, L"VERTICAL" },
					{ TPM_RECURSE, L"RECURSE" },
				};

				size_t at = 0;
				for(auto &k : known)
				{
					if((flags & k.bit) == 0)
						continue;
					if(at != 0 && at + 1 < capacity)
						out[at++] = L'|';
					for(size_t i = 0; k.name[i] && at + 1 < capacity; i++)
						out[at++] = k.name[i];
				}

				if(at == 0)
				{
					const wchar_t none[] = L"(none)";
					for(size_t i = 0; none[i] && at + 1 < capacity; i++)
						out[at++] = none[i];
				}
				out[at] = L'\0';
			}

			inline uint32_t perf_export_architecture()
			{
#if defined(_M_ARM64)
				return IMAGE_FILE_MACHINE_ARM64;
#elif defined(_M_AMD64)
				return IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_IX86)
				return IMAGE_FILE_MACHINE_I386;
#else
				return IMAGE_FILE_MACHINE_UNKNOWN;
#endif
			}

			inline const wchar_t *perf_export_architecture_name(uint32_t machine)
			{
				switch(machine)
				{
					case IMAGE_FILE_MACHINE_I386: return L"x86";
					case IMAGE_FILE_MACHINE_AMD64: return L"x64";
					case IMAGE_FILE_MACHINE_ARM64: return L"arm64";
					default: return L"?";
				}
			}

			/*
				The writer side, one per process, created on the first menu that
				publishes rather than at bootstrap - a process that loads Shell
				and never raises a menu should not carry the mapping.

				Process lifetime and no destructor, for the same reason
				DiagnosticsRing has none: the module is pinned for the life of
				the host, and a static whose destructor could run while a menu
				thread is publishing is a crash waiting for an unlucky shutdown.
			*/
			class PerfExportWriter
			{
			public:
				PerfExportWriter() = default;
				PerfExportWriter(const PerfExportWriter &) = delete;
				PerfExportWriter &operator=(const PerfExportWriter &) = delete;

				static PerfExportWriter &instance()
				{
					static PerfExportWriter *writer = new PerfExportWriter();
					return *writer;
				}

				// Idempotent, and never retried after a failure: a process
				// whose first attempt was refused will be refused every time,
				// and retrying once a menu would put a CreateFileMapping call
				// on every publish for nothing.
				bool open()
				{
					if(_block)
						return true;
					if(_attempted)
						return false;
					_attempted = true;

					wchar_t name[128]{};
					if(!perf_export_name(::GetCurrentProcessId(), name, ARRAYSIZE(name)))
						return false;

					// INVALID_HANDLE_VALUE plus a non-zero size: memory backed
					// by the paging file rather than by a file, per
					// https://learn.microsoft.com/windows/win32/memory/sharing-files-and-memory
					auto mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
														PAGE_READWRITE, 0,
														sizeof(PerfExportBlock), name);
					if(!mapping)
						return false;

					// A block already under this process id can only be a
					// leftover from a previous process that reused the id and
					// has not yet closed - which cannot happen while it holds
					// the handle - or another module in this process that got
					// there first. Either way, mapping it and re-stamping the
					// header is correct; the size is whatever the first creator
					// asked for, so it is checked rather than assumed
					// (CreateFileMappingW: "the function returns a handle to
					// the existing object (with its current size, not the
					// specified size)").
					auto view = ::MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0,
												sizeof(PerfExportBlock));
					if(!view)
					{
						::CloseHandle(mapping);
						return false;
					}

					_mapping = mapping;
					_block = static_cast<PerfExportBlock *>(view);

					// A fresh pagefile-backed section reads as zeroes, so an
					// existing block is the one that needs the sequence left
					// alone: bumping it to odd and never back would make every
					// reader think it was mid-write forever.
					auto sequence = _block->header.sequence;

					_block->header.magic = PERF_EXPORT_MAGIC;
					_block->header.version = PERF_EXPORT_VERSION;
					_block->header.record_size = sizeof(PerfExportRecord);
					_block->header.capacity = PERF_EXPORT_RECORDS;
					_block->header.process_id = ::GetCurrentProcessId();
					_block->header.architecture = perf_export_architecture();
					_block->header.sequence = (sequence & 1u) ? sequence + 1 : sequence;

					fill_host(_block->header.host, PERF_EXPORT_HOST);
					return true;
				}

				void store(const PerfExportRecord &record)
				{
					if(!open())
						return;
					perf_export_store(*_block, record);
				}

			private:
				// The image file name, not the whole path: this is a label in a
				// report, and the full path of every host on the machine is
				// more than a timing report needs to hand out.
				static void fill_host(wchar_t *out, size_t capacity)
				{
					if(!out || capacity == 0)
						return;
					out[0] = L'\0';

					wchar_t path[MAX_PATH]{};
					auto length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
					if(length == 0 || length >= MAX_PATH)
						return;

					auto start = path;
					for(auto p = path; *p; p++)
					{
						if(*p == L'\\' || *p == L'/')
							start = p + 1;
					}

					size_t i = 0;
					for(; start[i] && i + 1 < capacity; i++)
						out[i] = start[i];
					out[i] = L'\0';
				}

				HANDLE _mapping{};
				PerfExportBlock *_block{};
				bool _attempted{};
			};

			// What a reader learned about one process.
			struct PerfExportSource
			{
				uint32_t process_id{};
				uint32_t architecture{};
				uint64_t published{};
				wchar_t host[PERF_EXPORT_HOST]{};
			};

			enum class PerfExportStatus
			{
				Ok,
				NotPresent,		// no block under that process id, or not readable
				Unsupported,	// a block, but a version or layout this build does not know
				Busy,			// kept tearing - the host is publishing faster than this reads
			};

			/*
				Open one process's block and copy its most recent records out.

				`written` is set on Ok. Everything the block says about itself is
				checked before it is used, and every record that comes back has
				been sanitized, because the process that wrote it is not this
				one.
			*/
			inline PerfExportStatus perf_export_read(uint32_t process_id, PerfExportSource &source,
													 PerfExportRecord *out, size_t want,
													 size_t &written)
			{
				written = 0;

				wchar_t name[128]{};
				if(!perf_export_name(process_id, name, ARRAYSIZE(name)))
					return PerfExportStatus::NotPresent;

				auto mapping = ::OpenFileMappingW(FILE_MAP_READ, FALSE, name);
				if(!mapping)
					return PerfExportStatus::NotPresent;

				auto view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(PerfExportBlock));
				if(!view)
				{
					// A block smaller than this build's - an older Shell in a
					// host that has not been restarted. Readable in principle,
					// but not by a mapping this size, and guessing at the
					// layout is how a reader prints another build's memory.
					::CloseHandle(mapping);
					return PerfExportStatus::Unsupported;
				}

				auto &block = *static_cast<const PerfExportBlock *>(view);
				auto status = PerfExportStatus::Busy;

				if(block.header.magic != PERF_EXPORT_MAGIC
				   || block.header.version != PERF_EXPORT_VERSION
				   || block.header.record_size != sizeof(PerfExportRecord))
				{
					status = PerfExportStatus::Unsupported;
				}
				else
				{
					source = PerfExportSource{};
					source.process_id = block.header.process_id;
					source.architecture = block.header.architecture;
					source.published = block.header.published;
					for(size_t i = 0; i + 1 < PERF_EXPORT_HOST; i++)
						source.host[i] = block.header.host[i];
					source.host[PERF_EXPORT_HOST - 1] = L'\0';

					// Three attempts, because a torn read means the host
					// published while this was copying and the next attempt
					// almost certainly succeeds. A host publishing faster than
					// three copies is reported as busy rather than waited for.
					//
					// The budget is set against the real cadence, and the
					// pathological case was measured rather than guessed: a
					// writer storing in a tight loop tears *every* read, since
					// copying sixteen records takes microseconds and a store
					// takes a fraction of one. No number of retries helps
					// there. But a host publishes once per menu the user
					// opens - seconds apart - so an overlap is a coincidence
					// and two in a row is not something to design for.
					for(int attempt = 0; attempt < 3; attempt++)
					{
						bool torn = true;
						auto count = perf_export_load(block, out, want, torn);
						if(!torn)
						{
							for(size_t i = 0; i < count; i++)
								perf_export_sanitize(out[i]);
							written = count;
							status = PerfExportStatus::Ok;
							break;
						}
					}
				}

				::UnmapViewOfFile(view);
				::CloseHandle(mapping);
				return status;
			}

			/*
				Which processes to ask.

				There is no registry of hosts that have loaded Shell, and
				building one would mean a process-wide list that every host
				writes to - the shared-writable surface this design exists to
				avoid. So the reader asks the process list instead, and tries to
				open a block under each id. "All users have read access to the
				list of processes in the system"
				(https://learn.microsoft.com/windows/win32/procthread/process-enumeration),
				and the two or three hundred OpenFileMapping calls that follow
				fail immediately for every process that is not a host.

				Toolhelp rather than EnumProcesses because it hands back the
				executable name in the same pass, which is what makes the report
				readable when a block's own header cannot be opened.
			*/
			inline size_t perf_export_enumerate(uint32_t *out, size_t capacity)
			{
				if(!out || capacity == 0)
					return 0;

				auto snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
				if(snapshot == INVALID_HANDLE_VALUE)
					return 0;

				PROCESSENTRY32W entry{};
				entry.dwSize = sizeof(entry);

				size_t written = 0;
				if(::Process32FirstW(snapshot, &entry))
				{
					do
					{
						if(entry.th32ProcessID == 0)
							continue;
						if(written >= capacity)
							break;
						out[written++] = entry.th32ProcessID;
					} while(::Process32NextW(snapshot, &entry));
				}

				::CloseHandle(snapshot);
				return written;
			}
		}
	}
}
