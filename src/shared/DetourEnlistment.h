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
		*/
		inline bool thread_still_present(const InlineDetourApi &api, DWORD pid, DWORD tid)
		{
			auto snapshot = api.create_snapshot(TH32CS_SNAPTHREAD, 0);
			if(snapshot == INVALID_HANDLE_VALUE)
				return true;		// cannot prove it left; treat it as present

			bool present = false;
			THREADENTRY32 entry{};
			entry.dwSize = sizeof(entry);

			if(api.thread_first(snapshot, &entry))
			{
				do
				{
					if(entry.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID)
									  + sizeof(entry.th32OwnerProcessID))
						continue;
					if(entry.th32OwnerProcessID == pid && entry.th32ThreadID == tid)
					{
						present = true;
						break;
					}
				}
				while(api.thread_next(snapshot, &entry));
			}

			api.close_handle(snapshot);
			return present;
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
				out.error = ::GetLastError();
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
				out.error = ::GetLastError();
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
					auto error = ::GetLastError();
					if(!thread_still_present(api, pid, entry.th32ThreadID))
						continue;		// it ended during the race; nothing to update

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
