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
#include <mutex>

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
			// 3: PERF_EXPORT_PHASES 16 -> 24.
			// 4: the block gained a provider-name directory, and the header's
			//    `reserved` became `directory_count`.
			// 5: a directory entry carries the provider's CLSID as well as its
			//    hash, so the report prints the identity `-quarantine:add`
			//    takes. A hash is not something a user can act on.
			// 6: the block gained `interception` - which of the two popup
			//    mechanisms is carrying this host, and whether its thunk still
			//    pointed at Shell when it last published.
			inline constexpr uint32_t PERF_EXPORT_VERSION = 6;

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

			// The same 24 the ring holds.
			//
			// This was 16, on the reasoning that the menu path names eleven and
			// the hook adds three. Measured in a real Explorer, an ordinary
			// takeover records fifteen and the first menu in a process records
			// more - so the cap was being hit in normal use, and the phases it
			// dropped were the ones after popup.total_pre_display, which is
			// exactly where the interesting late work happens. `dropped_phases`
			// said so, which is the only reason it was noticed.
			//
			// Costs 8 more phases per record: 704 bytes a record, 11 KB a host.
			inline constexpr uint8_t PERF_EXPORT_PHASES = 24;
			inline constexpr uint8_t PERF_EXPORT_PROVIDERS = 8;

			// 16 sessions, ~24 KB of shared commit. This is mapped into every
			// process that raises a context menu - two dozen on a normal
			// desktop - so it is a window onto recent activity, not an archive.
			inline constexpr uint32_t PERF_EXPORT_RECORDS = 16;

			/*
				The provider-name directory.

				A record carries a provider's CLSID hash, deliberately:
				docs/refactor/05-capabilities.md section 1 wants a report to be
				able to say "these forty menus were all the same handler"
				without carrying strings around the measured path. But a hash is
				not something a user can act on - `provider e345019d 186 ms` names
				nothing to quarantine - and the Reliability Center's whole value
				is naming the extension that is slow.

				So names live *beside* the records rather than in them: one entry
				per distinct provider this host has ever activated, written once
				when it is first seen and never again. Records stay hash-only and
				the measured path stays string-free.

				The reader cannot resolve these itself. The names come from
				IExplorerCommand::GetTitle, which takes the selection and is
				answered by the handler - so it exists only inside a host that
				has actually built a menu. Reconstructing it from the registry
				would mean duplicating the manifest scan in shell.exe.

				32 entries at 40 characters is 2.6 KB on top of a ~36 KB block.
				This machine registers 23 handlers; a host that somehow exceeds
				32 gets hashes for the overflow and `directory_dropped` says so,
				which is the same shape as dropped_phases.
			*/
			inline constexpr uint32_t PERF_EXPORT_DIRECTORY = 32;
			inline constexpr size_t PERF_EXPORT_PROVIDER_NAME = 40;

			/*
				How a host is intercepting popup menus.

				Part of the wire format rather than of the DLL, because the
				reader has to name it in a report and the two live in different
				binaries. The live value is in
				src/dll/src/Include/InterceptionStatus.h.

				Shell has exactly two mechanisms and they are *not*
				interchangeable - the primary sees strictly more than the
				fallback does, for reasons that follow from the PE format and
				are set out in docs/refactor/01-takeover-contract.md section 9c.
				That difference is the whole reason this is worth reporting:
				"the fallback is carrying this process" is a real and
				actionable statement, and nothing could make it before.
			*/
			inline constexpr uint32_t PERF_EXPORT_INTERCEPTION_NONE = 0;

			// user32's own import of win32u!NtUserTrackPopupMenuEx: one thunk,
			// in one module, covering every caller in the process that reaches
			// a popup through user32.
			inline constexpr uint32_t PERF_EXPORT_INTERCEPTION_WIN32U = 1;

			// Every loaded module's import of user32!TrackPopupMenu(Ex),
			// patched one module at a time. Strictly weaker; see section 9c.
			inline constexpr uint32_t PERF_EXPORT_INTERCEPTION_PERMODULE = 2;

			// The thunk still pointed at Shell when this was last refreshed.
			inline constexpr uint32_t PERF_EXPORT_INTERCEPTION_HEALTHY = 0x8000u;

			inline constexpr uint32_t PERF_EXPORT_INTERCEPTION_BACKEND_MASK = 0x00FFu;

			inline const wchar_t *perf_export_interception_name(uint32_t interception)
			{
				switch(interception & PERF_EXPORT_INTERCEPTION_BACKEND_MASK)
				{
					case PERF_EXPORT_INTERCEPTION_WIN32U: return L"win32u import";
					case PERF_EXPORT_INTERCEPTION_PERMODULE: return L"per-module imports";
					default: return L"none";
				}
			}

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

			/*
				One directory entry: the hash a record carries, the CLSID that
				hash was computed from, and the title the handler gave the first
				time this host activated it.

				The CLSID is here so the report can print the thing
				`shell.exe -quarantine:add` accepts. Without it a user reads
				`provider e345019d` and has no way to act on it, which would
				leave the diagnosis and the treatment speaking different
				languages. GUID is a plain 16-byte POD, so it costs the layout
				nothing across architectures.
			*/
			struct PerfExportName
			{
				uint32_t clsid_hash;
				GUID clsid;
				wchar_t name[PERF_EXPORT_PROVIDER_NAME];	// truncated, always terminated
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

				// Directory entries filled, <= PERF_EXPORT_DIRECTORY. This was
				// `reserved`, which is what a reserved field is for; the
				// version moved with it so an older reader refuses the block
				// rather than reading a directory it does not know is there.
				uint32_t directory_count;

				uint64_t published;				// sessions ever, keeps counting past a wrap
				wchar_t host[PERF_EXPORT_HOST];	// image file name, not the full path
			};

			struct PerfExportBlock
			{
				PerfExportHeader header;
				PerfExportRecord records[PERF_EXPORT_RECORDS];

				// Distinct providers seen, however many menus they appeared in.
				// Grows only; an entry is never rewritten, so a reader that
				// copies it mid-append sees either the old count or the new
				// one, and both are consistent.
				PerfExportName directory[PERF_EXPORT_DIRECTORY];

				// Providers that did not fit. A report says so rather than
				// quietly showing a hash and letting somebody wonder why one
				// extension has a name and another does not.
				uint32_t directory_dropped;

				/*
					How this host is intercepting popups, and whether that was
					still true when it last published.

					One of PERF_EXPORT_INTERCEPTION_*, optionally with
					_HEALTHY. Refreshed on every session publish rather than
					fixed at creation, because a displacement can happen at any
					point in a process's life.

					In the block rather than the header for two reasons: a
					header field here would sit between `directory_count` and a
					uint64, costing four bytes of padding, and this is a value
					that is rewritten like the tables are rather than a fact
					settled when the block was made.

					docs/refactor/01-takeover-contract.md section 9c has what
					this can and cannot tell you - in particular that a host
					whose interception is gone entirely stops publishing, so
					the honest signal there is the menu count, not this word.
				*/
				uint32_t interception;
			};

#pragma pack(pop)

			// Layout is a wire format shared between architectures, so it is
			// asserted rather than assumed. If one of these fires, bump
			// PERF_EXPORT_VERSION - do not adjust the number to match.
			static_assert(sizeof(PerfExportPhase) == 8 + PERF_EXPORT_NAME * sizeof(wchar_t),
						  "PerfExportPhase gained padding");
			static_assert(sizeof(PerfExportProvider) == 12, "PerfExportProvider gained padding");
			static_assert(sizeof(PerfExportName) == 4 + sizeof(GUID) + PERF_EXPORT_PROVIDER_NAME * sizeof(wchar_t),
						  "PerfExportName gained padding");
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
			/*
				A test seam. Declared here because the directory append needs it
				too; the long explanation of why a seam is the only way to pin
				either ordering is on perf_export_load below.
			*/
			using PerfExportInterpose = void (*)(void *);

			// The directory entry for a hash, or null when this host's menu has
			// not considered that provider - or when it did, but past the cap.
			inline const PerfExportName *perf_export_find_provider(const PerfExportBlock &block,
																   uint32_t clsid_hash)
			{
				auto count = block.header.directory_count;
				if(count > PERF_EXPORT_DIRECTORY)
					count = PERF_EXPORT_DIRECTORY;

				for(uint32_t i = 0; i < count; i++)
				{
					if(block.directory[i].clsid_hash == clsid_hash)
						return &block.directory[i];
				}
				return nullptr;
			}

			/*
				Look a provider's name up in the directory. Null when no
				activation has yet produced a title for it - which is normal on
				the first menu, and permanent for a handler that returns none.

				Null here does *not* mean the provider is unknown: identity is
				recorded for every candidate. Use perf_export_find_clsid for
				the question a report needs to answer.
			*/
			inline const wchar_t *perf_export_find_name(const PerfExportBlock &block, uint32_t clsid_hash)
			{
				auto entry = perf_export_find_provider(block, clsid_hash);
				if(!entry || !entry->name[0])
					return nullptr;
				return entry->name;
			}

			// The CLSID a report prints and `-quarantine:add` takes. Known for
			// every provider this host's menu considered, whether or not it
			// was ever activated and whether or not it has a name.
			inline const GUID *perf_export_find_clsid(const PerfExportBlock &block, uint32_t clsid_hash)
			{
				auto entry = perf_export_find_provider(block, clsid_hash);
				return entry ? &entry->clsid : nullptr;
			}

			/*
				Record a provider in the directory: its identity always, its
				name when the host happens to know it.

				`name` is optional, and that is the whole point of this
				function's shape. Identity and presentation are learned at
				different moments and one of them may never be learned at all:

				  - The CLSID is a property of the *registration*. It is known
					before anything is activated, it never changes, and every
					provider the menu considered has one - including the ones
					it deliberately did not ask, because they were quarantined
					or deferred.
				  - The name comes from IExplorerCommand::GetTitle, which takes
					the selection and is answered by the handler. It exists
					only after a successful activation, and a handler is free
					to return nothing.

				Coupling them cost this feature its most useful line: a
				provider that cost 70 ms of a 78 ms menu reported a bare hash,
				because its title was empty, and `-quarantine:add` takes a
				CLSID. docs/refactor/05-capabilities.md section 1c.

				Ordering, and what a concurrent reader can see. The identifying
				half - hash and CLSID - is written before the count is raised,
				so a reader in another process either does not see the entry at
				all or sees those two fields complete. The name is the one
				field that may be filled in after publication, and that is
				safe for a reason worth stating rather than assuming: the block
				is zero-filled when it is created, a name is only ever written
				into an entry that has none, and the writer fills characters
				from index 0 upward. So a reader copying mid-write sees a
				*prefix* of the name followed by the zeros that were already
				there - a truncated name, never a torn one, and never a name
				belonging to some other provider.

				Returns true when a new entry was added.

				`interpose` runs between the entry being written and the count
				being raised, which is the only way to observe the ordering this
				function exists for. A single-threaded test cannot otherwise
				tell entry-then-count from count-then-entry: both leave the same
				state behind once the call returns, so a test that inspects the
				result afterwards passes either way. Same seam, and the same
				reason, as perf_export_load's. Production passes nothing and pays
				a null check.
			*/
			inline bool perf_export_note_provider(PerfExportBlock &block, uint32_t clsid_hash,
												  const GUID &clsid, const wchar_t *name = nullptr,
												  PerfExportInterpose interpose = nullptr,
												  void *interpose_context = nullptr)
			{
				auto count = block.header.directory_count;
				if(count > PERF_EXPORT_DIRECTORY)
				{
					// Another writer, or a corrupted block. Refuse rather than
					// clamp: writing at a clamped index would overwrite a real
					// entry somebody is about to read.
					return false;
				}

				for(uint32_t i = 0; i < count; i++)
				{
					if(block.directory[i].clsid_hash != clsid_hash)
						continue;

					// Known already. Fill in the name if this is the first
					// activation that produced one - the entry keeps its
					// identity and gains a label, and never the other way
					// round, so nothing a reader already copied becomes wrong.
					if(name && *name && !block.directory[i].name[0])
					{
						auto &known = block.directory[i];
						size_t at = 0;
						for(; name[at] && at + 1 < PERF_EXPORT_PROVIDER_NAME; at++)
							known.name[at] = name[at];
						known.name[at] = L'\0';
					}
					return false;
				}

				if(count >= PERF_EXPORT_DIRECTORY)
				{
					block.directory_dropped++;
					return false;
				}

				auto &entry = block.directory[count];
				entry.clsid_hash = clsid_hash;
				entry.clsid = clsid;

				size_t at = 0;
				if(name)
					for(; name[at] && at + 1 < PERF_EXPORT_PROVIDER_NAME; at++)
						entry.name[at] = name[at];
				entry.name[at] = L'\0';

				// Last: the entry must be complete before the count admits it.
				// A reader copies the directory outside the sequence protocol,
				// so the count is the only thing telling it how far the table
				// is valid. Raising it first would let a reader in another
				// process copy a CLSID that is still being written.
				if(interpose)
					interpose(interpose_context);

				std::atomic_thread_fence(std::memory_order_release);
				block.header.directory_count = count + 1;
				return true;
			}

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
					case 4: return L"quarantined";
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

				/*
					Serialised, and it has to be.

					The block's sequence counter is a seqlock, which gives a
					*reader* a way to notice it copied a half-written record.
					It gives no protection at all against two writers: two menu
					threads storing at once each bump the counter twice, so it
					can read even while a write is in flight, and
					`header.next` is a plain read-modify-write that would put
					two records in one slot. A reader would then print a record
					that is half of two, with nothing to say so - which is worse
					than the missing record, because it looks like a
					measurement.

					One host really does raise menus on several threads: every
					Explorer window has its own, and the taskbar has another.
					The lock is process-local and uncontended in the normal
					case, and `store` runs after the menu has closed.

					The file comment's "exactly one writer for its whole life"
					is about one *process* per block, which is what makes the
					security descriptor the right boundary. It was never a
					claim about threads.
				*/
				void store(const PerfExportRecord &record)
				{
					if(!open())
						return;

					std::lock_guard<std::mutex> lock(_writer);
					perf_export_store(*_block, record);
				}

				/*
					A provider the menu considered: its CLSID always, and the
					name the handler gave itself when there is one.

					`name` may be null, and callers use both forms. The
					identity is recorded for every candidate, before the
					quarantine and health checks that may skip it; the name is
					recorded later, by whichever activation first produced a
					title. See perf_export_note_provider for why those are two
					moments rather than one, and what a reader can observe in
					between.

					Unlike `store`, this one *is* on the measured path - it is
					called while the menu is being built, once per candidate
					provider. What it costs there is an uncontended lock and a
					scan of at most 32 integers, and only the first sighting of
					a provider also copies a CLSID and up to 40 characters. Set
					against the 3-60 ms the GetState/GetTitle/GetIcon sequence
					next to it costs, that is noise - but it is not zero, and
					saying it is off the path would be wrong.

					The directory is deliberately *not* under the block's
					sequence counter. Bumping that here would put a seqlock
					write on the menu path and make a reader retry whenever a
					provider appeared mid-report, in exchange for ordering an
					append-only table already gets from writing the entry before
					the count.
				*/
				void note_provider(uint32_t clsid_hash, const GUID &clsid, const wchar_t *name = nullptr)
				{
					if(!open())
						return;

					std::lock_guard<std::mutex> lock(_writer);
					perf_export_note_provider(*_block, clsid_hash, clsid, name);
				}

				/*
					Which mechanism is carrying this host's popups, refreshed.

					A plain word-sized store of a value that is almost always
					the same one, so it is deliberately not under the writer
					lock: there is nothing to tear, and a reader that catches
					the moment it changes sees either the old value or the new
					one. Taking the lock here would put a menu thread behind
					another menu thread's publish for no gain.
				*/
				void note_interception(uint32_t interception)
				{
					if(!open())
						return;

					_block->interception = interception;
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
				std::mutex _writer;
			};

			// What a reader learned about one process.
			struct PerfExportSource
			{
				uint32_t process_id{};
				uint32_t architecture{};
				uint64_t published{};
				wchar_t host[PERF_EXPORT_HOST]{};

				// One of PERF_EXPORT_INTERCEPTION_*, optionally _HEALTHY, as
				// of this host's last publish.
				uint32_t interception{};

				// Copied out rather than pointed at: the view is unmapped
				// before perf_export_read returns, so a pointer into the
				// block would dangle at exactly the moment it gets printed.
				uint32_t directory_count{};
				uint32_t directory_dropped{};
				PerfExportName directory[PERF_EXPORT_DIRECTORY]{};

				// The handler's own title for a provider, or null when this
				// host has not activated it yet or the directory was full.
				const wchar_t *name_for(uint32_t clsid_hash) const
				{
					auto *entry = entry_for(clsid_hash);
					return entry && entry->name[0] ? entry->name : nullptr;
				}

				// The CLSID that hash was computed from - what the report
				// prints and what `shell.exe -quarantine:add` accepts.
				const GUID *clsid_for(uint32_t clsid_hash) const
				{
					auto *entry = entry_for(clsid_hash);
					return entry ? &entry->clsid : nullptr;
				}

				const PerfExportName *entry_for(uint32_t clsid_hash) const
				{
					auto count = directory_count > PERF_EXPORT_DIRECTORY
						? PERF_EXPORT_DIRECTORY : directory_count;
					for(uint32_t i = 0; i < count; i++)
					{
						if(directory[i].clsid_hash == clsid_hash)
							return &directory[i];
					}
					return nullptr;
				}
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

					/*
						The directory, copied and re-terminated like everything
						else that came out of another process's memory.

						Read outside the sequence protocol on purpose. The
						directory is append-only and the writer completes an
						entry before raising the count, so the worst a reader
						can see is the count from before an append - one name
						short, never a half-written one. Folding it into the
						seqlock would mean a provider appearing mid-report made
						the whole read tear, which is a worse trade for a table
						that changes once per provider per process lifetime.
					*/
					auto directory = block.header.directory_count;
					if(directory > PERF_EXPORT_DIRECTORY)
						directory = PERF_EXPORT_DIRECTORY;
					source.directory_count = directory;
					source.directory_dropped = block.directory_dropped;

					// Word-sized and written without the sequence counter, for
					// the same reason it is read without one: there is nothing
					// to tear, so a reader sees the value from before or after
					// a refresh and both are true statements about this host.
					source.interception = block.interception;
					for(uint32_t i = 0; i < directory; i++)
					{
						source.directory[i].clsid_hash = block.directory[i].clsid_hash;
						source.directory[i].clsid = block.directory[i].clsid;
						for(size_t c = 0; c + 1 < PERF_EXPORT_PROVIDER_NAME; c++)
							source.directory[i].name[c] = block.directory[i].name[c];
						source.directory[i].name[PERF_EXPORT_PROVIDER_NAME - 1] = L'\0';
					}

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
