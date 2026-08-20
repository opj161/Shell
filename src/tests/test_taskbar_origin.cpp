// "This popup came from the taskbar" is a fact about one popup on one thread.
//
// It used to be a namespace-scope bool in Main.cpp, set by
// ShowTaskbarContextMenu and cleared in the __finally of the TrackPopupMenu
// hook. Explorer raises menus from more than one UI thread, so that is a data
// race - and, separately, one of the two branches of ShowTaskbarContextMenu
// invokes the saved native target directly and never reaches the hook, so the
// flag simply stayed true afterwards and the next ordinary popup was treated as
// a taskbar popup.
//
// These pin the two properties the fix depends on: threads cannot see each
// other's answer, and a scope always restores what it found - including the
// scope that never runs a hook at all.

#include "test.h"

#include <windows.h>
#include <atomic>
#include <thread>

#include "Include/TaskbarOrigin.h"

using Nilesoft::Shell::in_taskbar;
using Nilesoft::Shell::ScopedTaskbarOrigin;

TEST(taskbar_origin, defaults_to_false)
{
	CHECK(!in_taskbar());
}

TEST(taskbar_origin, a_scope_restores_what_it_found)
{
	CHECK(!in_taskbar());
	{
		ScopedTaskbarOrigin origin(true);
		CHECK(in_taskbar());
	}
	CHECK_MSG(!in_taskbar(), "the flag leaked out of its scope");
}

// The branch that bypasses the hook is exactly this shape: enter the scope, do
// something that never touches the hook's __finally, leave.
TEST(taskbar_origin, a_scope_that_never_reaches_the_hook_still_restores)
{
	{
		ScopedTaskbarOrigin origin(true);
		CHECK(in_taskbar());
		// no hook, no __finally, nothing clears it explicitly
	}
	CHECK(!in_taskbar());
}

TEST(taskbar_origin, scopes_nest_both_ways)
{
	{
		ScopedTaskbarOrigin outer(true);
		CHECK(in_taskbar());
		{
			ScopedTaskbarOrigin inner(false);
			CHECK(!in_taskbar());
		}
		CHECK_MSG(in_taskbar(), "the inner scope must restore true, not clear it");
	}
	CHECK(!in_taskbar());
}

// Two threads holding opposite answers at the same time. With the old global
// this could not hold by construction.
TEST(taskbar_origin, threads_do_not_see_each_others_state)
{
	std::atomic<bool> ready{ false };
	std::atomic<bool> taskbar_thread_saw{ true };
	std::atomic<bool> plain_thread_saw{ false };
	std::atomic<int> arrived{ 0 };

	std::thread taskbar([&]
	{
		ScopedTaskbarOrigin origin(true);
		arrived++;
		while(!ready.load()) { ::Sleep(0); }
		taskbar_thread_saw = in_taskbar();
	});

	std::thread plain([&]
	{
		arrived++;
		while(!ready.load()) { ::Sleep(0); }
		plain_thread_saw = in_taskbar();
	});

	// Both threads are inside their respective states before either reads.
	while(arrived.load() < 2) { ::Sleep(0); }
	ready = true;

	taskbar.join();
	plain.join();

	CHECK_MSG(taskbar_thread_saw.load(), "the taskbar thread must see its own scope");
	CHECK_MSG(!plain_thread_saw.load(), "a thread with no scope must not see another's");
	CHECK_MSG(!in_taskbar(), "and neither must the thread that started them");
}
