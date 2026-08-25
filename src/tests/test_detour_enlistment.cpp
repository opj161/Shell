#include "test.h"

#include "..\shared\DetourEnlistment.h"

// Whether the one inline detour in this product may be committed.
//
// Detours rewrites the first instructions of a live function, and is explicit
// about what that costs threads the transaction does not know about:
//
//     "Threads not enlisted in the transaction are not updated when the
//      transaction commits. As a result, they may attempt to execute an illegal
//      combination of old and new code."
//     https://github.com/microsoft/Detours/wiki/DetourUpdateThread
//
// The wrapper quoted that page and then did the opposite: enlist() returned
// void, gave up silently when CreateToolhelp32Snapshot failed, skipped every
// thread OpenThread or DetourUpdateThread refused - and begin() returned true
// regardless, so Main.cpp patched CoCreateInstance inside explorer.exe against
// an unknown set of threads. docs/refactor/09-remediation-plan.md finding U.
//
// None of that could be reached from a test while the calls were named
// directly, which is the other half of why it survived. Every one of them is
// injected now, so each failure shape below is a run rather than an argument.

using namespace Nilesoft::Shell;

namespace
{
	// The process and threads the fake snapshot describes. Thread 1 is the
	// caller; 2 and 3 are the ones that have to be enlisted.
	constexpr DWORD PID = 4242;
	constexpr DWORD SELF = 1;
	constexpr DWORD OTHER_A = 2;
	constexpr DWORD OTHER_B = 3;
	constexpr DWORD FOREIGN_PID = 99;

	// Enough of a fake to be driven from a static, because the API table is
	// plain function pointers - which is what makes it linkable without
	// detours.lib.
	struct Fake
	{
		// what the snapshot walks
		std::vector<std::pair<DWORD, DWORD>> entries;	// {pid, tid}
		std::vector<std::pair<DWORD, DWORD>> second_entries;
		bool use_second_snapshot_next = false;

		bool snapshot_fails = false;
		bool first_fails = false;
		DWORD open_fails_for = 0;
		DWORD update_fails_for = 0;

		LONG begin_result = NO_ERROR;
		LONG commit_result = NO_ERROR;

		// One cursor per outstanding snapshot, not one shared cursor. The
		// liveness re-check takes a *second* snapshot while the first walk is
		// mid-iteration, so a fake with a single cursor would have the inner
		// walk silently advance the outer one - and the outer loop's behaviour
		// after a skipped thread is precisely what these tests are about.
		size_t cursor[2] = { 0, 0 };
		int snapshots_taken = 0;
		int aborts = 0;
		int commits = 0;
		int opened = 0;
		int closed = 0;
		int updated = 0;
	};

	Fake g;

	constexpr intptr_t SNAPSHOT_BASE = 0x5AFE0;

	int slot_of(HANDLE h) { return (int)((intptr_t)h - SNAPSHOT_BASE); }
	bool is_snapshot(HANDLE h) { return slot_of(h) == 0 || slot_of(h) == 1; }

	HANDLE WINAPI fake_snapshot(DWORD, DWORD)
	{
		if(g.snapshot_fails)
		{
			g.snapshots_taken++;
			return INVALID_HANDLE_VALUE;
		}
		// Slot 0 is the enlistment walk; slot 1 is the liveness re-check after
		// a failed OpenThread, which may describe a process one thread lighter.
		int slot = g.snapshots_taken == 0 ? 0 : 1;
		g.snapshots_taken++;
		g.cursor[slot] = 0;
		return (HANDLE)(SNAPSHOT_BASE + slot);
	}

	const std::vector<std::pair<DWORD, DWORD>> &entries_for(int slot)
	{
		return (slot == 1 && g.use_second_snapshot_next) ? g.second_entries : g.entries;
	}

	bool fill(int slot, LPTHREADENTRY32 entry)
	{
		const auto &list = entries_for(slot);
		if(g.cursor[slot] >= list.size())
			return false;
		entry->dwSize = sizeof(THREADENTRY32);
		entry->th32OwnerProcessID = list[g.cursor[slot]].first;
		entry->th32ThreadID = list[g.cursor[slot]].second;
		g.cursor[slot]++;
		return true;
	}

	BOOL WINAPI fake_first(HANDLE h, LPTHREADENTRY32 entry)
	{
		int slot = slot_of(h);
		if(g.first_fails && slot == 0)
			return FALSE;
		g.cursor[slot] = 0;
		return fill(slot, entry) ? TRUE : FALSE;
	}

	BOOL WINAPI fake_next(HANDLE h, LPTHREADENTRY32 entry)
	{
		return fill(slot_of(h), entry) ? TRUE : FALSE;
	}

	BOOL WINAPI fake_close(HANDLE h)
	{
		if(!is_snapshot(h))
			g.closed++;
		return TRUE;
	}

	HANDLE WINAPI fake_open(DWORD, BOOL, DWORD tid)
	{
		if(g.open_fails_for == tid)
		{
			::SetLastError(ERROR_ACCESS_DENIED);
			return nullptr;
		}
		g.opened++;
		return (HANDLE)(intptr_t)(0x1000 + tid);
	}

	LONG WINAPI fake_update(HANDLE h)
	{
		auto tid = (DWORD)((intptr_t)h - 0x1000);
		if(g.update_fails_for == tid)
			return ERROR_NOT_ENOUGH_MEMORY;
		g.updated++;
		return NO_ERROR;
	}

	LONG WINAPI fake_begin() { return g.begin_result; }
	LONG WINAPI fake_abort() { g.aborts++; return NO_ERROR; }
	LONG WINAPI fake_commit(PVOID **failed)
	{
		g.commits++;
		if(failed)
			*failed = g.commit_result == NO_ERROR ? nullptr : (PVOID *)(intptr_t)0xBAD;
		return g.commit_result;
	}
	BOOL WINAPI fake_ignore_too_small(BOOL) { return TRUE; }

	const InlineDetourApi &fake_api()
	{
		static const InlineDetourApi api
		{
			&fake_snapshot, &fake_first, &fake_next, &fake_close,
			&fake_open, &fake_update,
			&fake_begin, &fake_abort, &fake_commit, &fake_ignore_too_small,
		};
		return api;
	}

	void reset()
	{
		g = Fake{};
		g.entries = { { PID, SELF }, { PID, OTHER_A }, { FOREIGN_PID, 77 }, { PID, OTHER_B } };
	}
}

TEST(detour_enlistment, every_other_thread_of_this_process_is_enlisted)
{
	reset();
	auto e = enlist_process_threads(fake_api(), PID, SELF);

	CHECK(e.ok());
	CHECK_EQ(e.threads.size(), (size_t)2);
	CHECK_EQ(g.updated, 2);

	// The caller stays out - DetourUpdateThread's page: a real handle to the
	// current thread "is currently unsupported and will result in application
	// hangs" - and so does another process's thread.
	CHECK_EQ(g.opened, 2);
}

TEST(detour_enlistment, a_failed_snapshot_is_a_failed_enlistment)
{
	reset();
	g.snapshot_fails = true;
	auto e = enlist_process_threads(fake_api(), PID, SELF);

	CHECK(!e.ok());
	CHECK(e.result == EnlistmentResult::SnapshotFailed);
	CHECK(e.threads.empty());
}

// The subtler half: the snapshot exists but cannot be walked. The old code read
// that as "no threads to enlist" and carried on - and this thread is in there,
// so it was never an empty process.
TEST(detour_enlistment, a_snapshot_that_cannot_be_walked_is_a_failed_enlistment)
{
	reset();
	g.first_fails = true;
	auto e = enlist_process_threads(fake_api(), PID, SELF);

	CHECK(!e.ok());
	CHECK(e.result == EnlistmentResult::EnumerationFailed);
}

TEST(detour_enlistment, a_live_thread_that_cannot_be_opened_fails_the_enlistment)
{
	reset();
	g.open_fails_for = OTHER_B;
	g.use_second_snapshot_next = true;
	g.second_entries = g.entries;			// still there: not the benign race

	auto e = enlist_process_threads(fake_api(), PID, SELF);

	CHECK(!e.ok());
	CHECK(e.result == EnlistmentResult::ThreadUnavailable);
	CHECK_EQ(e.thread_id, OTHER_B);
	CHECK_EQ(e.error, (DWORD)ERROR_ACCESS_DENIED);
}

// The one OpenThread failure that may be ignored, and the reason the check is a
// second snapshot rather than an error code: "The identifiers are valid from
// the time the thread is created until the thread has been terminated"
// (https://learn.microsoft.com/windows/win32/procthread/thread-handles-and-identifiers),
// so an id the process no longer lists names a thread that ended between the
// first snapshot and the open. OpenThread's own page documents no error code
// for it.
TEST(detour_enlistment, a_thread_that_ended_during_the_snapshot_race_is_ignored)
{
	reset();
	g.open_fails_for = OTHER_B;
	g.use_second_snapshot_next = true;
	g.second_entries = { { PID, SELF }, { PID, OTHER_A } };		// OTHER_B is gone

	auto e = enlist_process_threads(fake_api(), PID, SELF);

	CHECK(e.ok());
	CHECK_EQ(e.threads.size(), (size_t)1);
}

TEST(detour_enlistment, a_thread_detours_will_not_record_fails_the_enlistment)
{
	reset();
	g.update_fails_for = OTHER_A;
	auto e = enlist_process_threads(fake_api(), PID, SELF);

	CHECK(!e.ok());
	CHECK(e.result == EnlistmentResult::UpdateFailed);
	CHECK_EQ(e.thread_id, OTHER_A);
	CHECK_EQ(e.error, (DWORD)ERROR_NOT_ENOUGH_MEMORY);

	// The handle it did open is not leaked into the caller's set.
	CHECK(e.threads.empty());
	CHECK_EQ(g.closed, 1);
}

// ---- the transaction on top of it ---------------------------------------
//
// These run against the *real* process id and thread id, because the
// transaction asks the OS for them; the fake snapshot answers about PID, so no
// thread matches and the enlistment is trivially complete. What is being pinned
// here is what begin() and commit() do with each result, not the walk.

namespace
{
	void reset_for_transaction()
	{
		reset();
		g.entries = { { PID, SELF } };		// nothing belonging to this process
	}
}

TEST(detour_enlistment, begin_refuses_when_the_snapshot_failed)
{
	reset_for_transaction();
	g.snapshot_fails = true;

	InlineDetourTransaction t(fake_api());
	CHECK(!t.begin());

	// Refusing has to leave the transaction closed, or the next one cannot open.
	CHECK_EQ(g.aborts, 1);
	CHECK_EQ(g.commits, 0);
	CHECK(t.enlistment().result == EnlistmentResult::SnapshotFailed);
}

TEST(detour_enlistment, begin_refuses_when_detours_would_not_open_the_transaction)
{
	reset_for_transaction();
	g.begin_result = ERROR_INVALID_OPERATION;

	InlineDetourTransaction t(fake_api());
	CHECK(!t.begin());
	CHECK_EQ(g.commits, 0);
}

TEST(detour_enlistment, a_clean_begin_commits)
{
	reset_for_transaction();

	InlineDetourTransaction t(fake_api());
	CHECK(t.begin());
	CHECK(t.commit());
	CHECK_EQ(g.commits, 1);
	CHECK_EQ(g.aborts, 0);
	CHECK(t.failed_pointer() == nullptr);
}

// DetourTransactionCommitEx rather than DetourTransactionCommit, so a failed
// attach names "the target pointer passed to the DetourAttach ... call that
// caused the latest transaction to fail" instead of only the fact that one did.
// https://github.com/microsoft/Detours/wiki/DetourTransactionCommitEx
TEST(detour_enlistment, a_failed_commit_reports_which_target_failed)
{
	reset_for_transaction();
	g.commit_result = ERROR_INVALID_DATA;

	InlineDetourTransaction t(fake_api());
	CHECK(t.begin());
	CHECK(!t.commit());
	CHECK(t.failed_pointer() != nullptr);
}

TEST(detour_enlistment, aborting_twice_is_harmless_and_reopening_is_refused)
{
	reset_for_transaction();

	InlineDetourTransaction t(fake_api());
	CHECK(t.begin());
	CHECK(!t.begin());		// already open
	t.abort();
	t.abort();
	CHECK_EQ(g.aborts, 1);
	CHECK(!t.commit());		// nothing open to commit
	CHECK_EQ(g.commits, 0);
}
