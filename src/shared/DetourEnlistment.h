#pragma once

/*
	Enlisting this process's threads in an inline detour transaction, as
	something a test can drive.

	Detours rewrites the first instructions of the target function while the
	process runs, and is explicit about the cost to threads that are not part of
	the transaction:

		"Threads not enlisted in the transaction are not updated when the
		 transaction commits. As a result, they may attempt to execute an
		 illegal combination of old and new code."
		 https://github.com/microsoft/Detours/wiki/DetourUpdateThread

	That is the whole premise of the wrapper this backs. The one function this
	codebase detours inline is CoCreateInstance, inside explorer.exe, which has
	dozens of threads and calls it from most of them.

	The defect: enlistment used to be a void function that returned early when
	CreateToolhelp32Snapshot failed and silently skipped every thread OpenThread
	or DetourUpdateThread refused - and then begin() returned true and the
	caller committed the patch anyway. So the premise above was asserted in a
	comment and abandoned in the code, in exactly the case it was written for.
	docs/refactor/09-remediation-plan.md finding U.

	Every Win32 and Detours call is taken through an injected table so the four
	failure shapes can be exercised without a process to break. That is not a
	test-only affordance bolted on: a transaction whose failure paths cannot be
	run is a transaction whose failure paths are not known to work, and this one
	rewrites live code in Explorer.
*/

#include <windows.h>
#include <tlhelp32.h>

#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		/*
			Everything the transaction reaches outside itself. Defaults are set
			by default_inline_detour_api() in Library/detours.h, which is the
			only place that names the real Detours symbols - so a translation
			unit can include this header without linking detours.lib.
		*/
		struct InlineDetourApi
		{
			HANDLE (WINAPI *create_snapshot)(DWORD, DWORD);
			BOOL (WINAPI *thread_first)(HANDLE, LPTHREADENTRY32);
			BOOL (WINAPI *thread_next)(HANDLE, LPTHREADENTRY32);
			BOOL (WINAPI *close_handle)(HANDLE);
			HANDLE (WINAPI *open_thread)(DWORD, BOOL, DWORD);

			// Routed rather than called directly, because the difference
			// between "the walk finished" and "the walk broke" is *only* in
			// the last error - Thread32Next returns FALSE for both. Leaving
			// ::GetLastError named inline is what made the failed-walk case
			// impossible to express in a test, and therefore impossible to
			// notice.
			DWORD (WINAPI *last_error)();

			LONG (WINAPI *update_thread)(HANDLE);

			LONG (WINAPI *transaction_begin)();
			LONG (WINAPI *transaction_abort)();
			LONG (WINAPI *transaction_commit_ex)(PVOID **);
			BOOL (WINAPI *set_ignore_too_small)(BOOL);
		};

		enum class EnlistmentResult
		{
			Enlisted,			// every live thread but the caller is in the transaction
			SnapshotFailed,		// CreateToolhelp32Snapshot said no
			EnumerationFailed,	// the snapshot exists but Thread32First did not walk it
			ThreadUnavailable,	// a thread that is still there could not be opened
			UpdateFailed,		// DetourUpdateThread refused a thread it was given
		};

		struct Enlistment
		{
			EnlistmentResult result = EnlistmentResult::SnapshotFailed;
			std::vector<HANDLE> threads;
			DWORD error = 0;
			DWORD thread_id = 0;	// the one that failed, when one did

			bool ok() const noexcept { return result == EnlistmentResult::Enlisted; }
		};

		// The access DetourUpdateThread needs: Detours suspends each enlisted
		// thread and reads and rewrites its context at commit time.
		inline constexpr DWORD kEnlistAccess = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
											   | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION;

		/*
			Is `tid` still a thread of `pid`?

			Asked only when OpenThread has already failed, and asked this way on
			purpose. The obvious test - whether GetLastError() is
			ERROR_INVALID_PARAMETER - is not documented: OpenThread's page says
			only "If the function fails, the return value is NULL." What *is*
			documented is that a thread identifier is valid exactly as long as
			the thread is:

				"The identifiers are valid from the time the thread is created
				 until the thread has been terminated."
				 https://learn.microsoft.com/windows/win32/procthread/thread-handles-and-identifiers

			so a fresh snapshot that no longer lists the id proves the thread
			ended between the first snapshot and the open - a benign race, and
			the only OpenThread failure this code is entitled to ignore. Anything
			else is a thread that exists and will not be updated, which is the
			case Detours warns about.

			Which makes the answer three-valued, not two. "Did not find it" and
			"could not look" are different facts, and only the first of them is
			evidence the thread is gone. The snapshot-failure path already got
			this right - it returned "present", deliberately, because it could
			not prove otherwise - while the two enumeration paths below it did
			not: a failed Thread32First, or a walk that stopped early, both
			produced `present = false` and the caller then skipped a thread that
			may well still be running. That is exactly the outcome the whole
			file exists to prevent, reached by the code that was meant to
			prevent it.

			Present and Unknown are both refused by the caller. Only Gone is
			ignored.
		*/
		enum class ThreadPresence
		{
			Present,	// the snapshot lists it; it is still running
			Gone,		// the snapshot was walked to the end and did not list it
			Unknown,	// could not look, or could not finish looking
		};

		inline ThreadPresence thread_presence(const InlineDetourApi &api,
											  DWORD pid, DWORD tid)
		{
			auto snapshot = api.create_snapshot(TH32CS_SNAPTHREAD, 0);
			if(snapshot == INVALID_HANDLE_VALUE)
				return ThreadPresence::Unknown;

			THREADENTRY32 entry{};
			entry.dwSize = sizeof(entry);

			if(!api.thread_first(snapshot, &entry))
			{
				// A thread snapshot that will not open on its first entry is
				// not an empty machine - this thread is in it.
				api.close_handle(snapshot);
				return ThreadPresence::Unknown;
			}

			auto answer = ThreadPresence::Unknown;
			do
			{
				if(entry.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID)
								  + sizeof(entry.th32OwnerProcessID))
					continue;
				if(entry.th32OwnerProcessID == pid && entry.th32ThreadID == tid)
				{
					answer = ThreadPresence::Present;
					break;
				}
			}
			while(api.thread_next(snapshot, &entry));

			// Not found - but only "gone" if the walk actually reached the end.
			// Thread32Next returns FALSE for exhaustion and for failure alike;
			// ERROR_NO_MORE_FILES is the documented way to tell them apart.
			if(answer != ThreadPresence::Present
			   && api.last_error() == ERROR_NO_MORE_FILES)
				answer = ThreadPresence::Gone;

			api.close_handle(snapshot);
			return answer;
		}

		/*
			Open and enlist every thread of `pid` except `self`.

			`self` stays out deliberately, and by id rather than by comparing
			handles: DetourUpdateThread documents that the current thread's
			pseudo handle is a no-op, and that "Calling DetourUpdateThread with
			a non-pseudo handle to the current thread is currently unsupported
			and will result in application hangs."

			On failure the caller gets the handles opened so far so it can close
			them; nothing is enlisted that the transaction will not abort.
		*/
		inline Enlistment enlist_process_threads(const InlineDetourApi &api,
												 DWORD pid, DWORD self)
		{
			Enlistment out;

			auto snapshot = api.create_snapshot(TH32CS_SNAPTHREAD, 0);
			if(snapshot == INVALID_HANDLE_VALUE)
			{
				out.result = EnlistmentResult::SnapshotFailed;
				out.error = api.last_error();
				return out;
			}

			THREADENTRY32 entry{};
			entry.dwSize = sizeof(entry);

			if(!api.thread_first(snapshot, &entry))
			{
				// A snapshot that cannot be walked is not an empty process -
				// this thread is in it. Treating it as "no threads to enlist"
				// is what made the old code commit against an unknown set.
				out.result = EnlistmentResult::EnumerationFailed;
				out.error = api.last_error();
				api.close_handle(snapshot);
				return out;
			}

			out.result = EnlistmentResult::Enlisted;

			do
			{
				// The snapshot covers every process, so entries have to be
				// filtered - and th32OwnerProcessID is only present when the
				// entry reaches it.
				if(entry.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID)
								  + sizeof(entry.th32OwnerProcessID))
					continue;

				if(entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
					continue;

				auto thread = api.open_thread(kEnlistAccess, FALSE, entry.th32ThreadID);
				if(!thread)
				{
					auto error = api.last_error();
					if(thread_presence(api, pid, entry.th32ThreadID)
					   == ThreadPresence::Gone)
						continue;		// it ended during the race; nothing to update

					// Present *or* Unknown. "Could not prove it left" must
					// never be read as "proved gone" - the thread would run
					// through a rewritten prologue it was never told about.
					out.result = EnlistmentResult::ThreadUnavailable;
					out.error = error;
					out.thread_id = entry.th32ThreadID;
					break;
				}

				// The only documented failure is ERROR_NOT_ENOUGH_MEMORY, which
				// means Detours could not record the thread's identity - so the
				// thread would run unupdated.
				auto rc = api.update_thread(thread);
				if(rc != NO_ERROR)
				{
					api.close_handle(thread);
					out.result = EnlistmentResult::UpdateFailed;
					out.error = static_cast<DWORD>(rc);
					out.thread_id = entry.th32ThreadID;
					break;
				}

				// Held open until after the commit, because that is when
				// Detours reads and rewrites the thread contexts.
				out.threads.push_back(thread);
			}
			while(api.thread_next(snapshot, &entry));

			// The walk ended. Whether it ended because there was nothing left
			// or because it broke is not in the return value - Thread32Next
			// answers FALSE for both - and until now the difference was
			// discarded: `out.result` was already Enlisted, so a snapshot that
			// failed halfway through produced a *successful* enlistment of
			// however many threads had been reached, and begin() committed the
			// patch against the rest.
			//
			//     "The ERROR_NO_MORE_FILES error value is returned by the
			//      GetLastError function if no threads exist or the snapshot
			//      does not contain thread information."
			//     https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-thread32next
			//
			// so that value, and only that value, is exhaustion.
			//
			// Guarded by `result == Enlisted` because the loop's other exits
			// are `break`s that have already recorded a more specific failure,
			// and those must not be relabelled.
			//
			// The last error is read without clearing it first, which was
			// checked rather than assumed: measured on Windows 11 26200 x64,
			// MSVC 14.44.35207, a full 6,386-entry walk ended with
			// ERROR_NO_MORE_FILES on three consecutive runs *even with a
			// succeeding call inside the loop body stamping ERROR_SUCCESS*, so
			// Thread32Next sets its own error rather than leaving a stale one.
			// A module snapshot - one that genuinely holds no thread
			// information - failed Thread32First with ERROR_NO_MORE_FILES,
			// matching the sentence above. Were that ever not so, the reading
			// falls the safe way: an unexpected error fails the enlistment and
			// the detour is simply not applied.
			if(out.result == EnlistmentResult::Enlisted
			   && api.last_error() != ERROR_NO_MORE_FILES)
			{
				out.result = EnlistmentResult::EnumerationFailed;
				out.error = api.last_error();
			}

			api.close_handle(snapshot);
			return out;
		}

		/*
			One inline detour transaction over that enlistment.

			begin() now answers false when the process could not be brought into
			a known state, and the caller must not attach anything. Failing open
			- no detour, no policy hook, Shell's other interception route
			unaffected - is the existing contract for every other way this can
			go wrong, and is strictly better than patching live code that some
			threads have not been told about.
		*/
		class InlineDetourTransaction
		{
		public:
			explicit InlineDetourTransaction(const InlineDetourApi &api) : _api(api) {}
			~InlineDetourTransaction() { abort(); }

			InlineDetourTransaction(const InlineDetourTransaction &) = delete;
			InlineDetourTransaction &operator=(const InlineDetourTransaction &) = delete;

			bool begin()
			{
				if(_open)
					return false;

				_api.set_ignore_too_small(TRUE);
				if(_api.transaction_begin() != NO_ERROR)
					return false;

				_open = true;

				_enlistment = enlist_process_threads(_api, ::GetCurrentProcessId(),
													 ::GetCurrentThreadId());
				if(!_enlistment.ok())
				{
					abort();
					return false;
				}

				return true;
			}

			bool commit()
			{
				if(!_open)
					return false;

				// Ex, so a failed attach names the target pointer that caused
				// it rather than only the fact that something did.
				// https://github.com/microsoft/Detours/wiki/DetourTransactionCommitEx
				PVOID *failed = nullptr;
				auto rc = _api.transaction_commit_ex(&failed);
				_open = false;
				_failed_pointer = failed;
				release();
				return rc == NO_ERROR;
			}

			void abort()
			{
				if(_open)
				{
					_api.transaction_abort();
					_open = false;
				}
				release();
			}

			const Enlistment &enlistment() const noexcept { return _enlistment; }
			PVOID *failed_pointer() const noexcept { return _failed_pointer; }
			size_t enlisted() const noexcept { return _enlistment.threads.size(); }

		private:
			void release()
			{
				for(auto thread : _enlistment.threads)
					_api.close_handle(thread);
				_enlistment.threads.clear();
			}

			const InlineDetourApi &_api;
			Enlistment _enlistment;
			PVOID *_failed_pointer = nullptr;
			bool _open = false;
		};
	}
}
