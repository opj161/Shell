// Noticing that a configuration file changed.
//
// docs/refactor/03-config-safety.md section 3. Two halves are tested here for
// two different reasons.
//
// The watch-set rule is pure, so it is tested directly: which directories cover
// a set of files, deduped the way a case-insensitive file system needs, capped
// so the WaitForMultipleObjects behind it cannot be handed more handles than it
// takes. The cap failing loudly here beats the wait failing silently on a
// user's machine.
//
// The watcher itself is tested against real files on a real disk, because the
// property that matters - a save produces exactly one reload, and an unrelated
// file in the same directory produces none - is a property of Windows'
// notifications and of the debounce, and a mock of either would be testing the
// mock. The repo convention is real objects over mocks; this follows
// test_parser_imports.cpp.

#include "test.h"

#include "..\dll\src\Include\ConfigWatcher.h"

#include <atomic>
#include <string>
#include <vector>

using namespace Nilesoft::Shell;

namespace
{
	std::vector<std::wstring> dirs(const std::vector<std::wstring> &files, size_t cap = 15)
	{
		return config_watch_directories(files, cap);
	}

	// A directory of files that removes itself.
	struct TempDir
	{
		std::wstring dir;
		std::vector<std::wstring> written;

		TempDir()
		{
			wchar_t base[MAX_PATH]{};
			::GetTempPathW(MAX_PATH, base);

			wchar_t unique[MAX_PATH]{};
			::swprintf_s(unique, L"%snss_watch_%lu_%llu\\", base,
						 ::GetCurrentProcessId(), (unsigned long long)::GetTickCount64());
			dir = unique;
			::CreateDirectoryW(dir.c_str(), nullptr);
		}

		~TempDir()
		{
			for(const auto &name : written)
				::DeleteFileW((dir + name).c_str());
			::RemoveDirectoryW(dir.c_str());
		}

		std::wstring write(const wchar_t *name, const char *text)
		{
			auto full = dir + name;
			HANDLE h = ::CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
									 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if(h != INVALID_HANDLE_VALUE)
			{
				DWORD wrote = 0;
				::WriteFile(h, text, (DWORD)::strlen(text), &wrote, nullptr);
				::CloseHandle(h);
			}

			bool known = false;
			for(const auto &seen : written)
			{
				if(seen == name) { known = true; break; }
			}
			if(!known)
				written.push_back(name);

			return full;
		}
	};

	std::atomic<int> g_reloads{ 0 };
	void count_reload() { g_reloads.fetch_add(1, std::memory_order_release); }

	// Waits for the watcher to have finished a reload, rather than sleeping for
	// a duration and hoping. Returns false on timeout, which is a failure the
	// test reports rather than a flake it absorbs.
	bool wait_for_reloads(const ConfigWatcher &watcher, uint64_t want, DWORD timeout_ms)
	{
		auto deadline = ::GetTickCount64() + timeout_ms;
		while(::GetTickCount64() < deadline)
		{
			if(watcher.reloads() >= want)
				return true;
			::Sleep(15);
		}
		return watcher.reloads() >= want;
	}
}

// ---- the watch-set rule -------------------------------------------------

TEST(config_watcher, one_file_is_covered_by_its_own_directory)
{
	auto d = dirs({ L"C:\\config\\shell.nss" });
	CHECK_EQ(d.size(), (size_t)1);
	CHECK(d[0] == L"C:\\config");
}

TEST(config_watcher, files_in_one_directory_produce_one_watch)
{
	auto d = dirs({ L"C:\\config\\shell.nss",
					L"C:\\config\\imports\\..\\a.nss",
					L"C:\\config\\b.nss" });
	CHECK_EQ(d.size(), (size_t)2);
	CHECK(d[0] == L"C:\\config");
	CHECK(d[1] == L"C:\\config\\imports\\..");
}

// The file system does not distinguish these, so neither may the watch set -
// otherwise one directory is watched twice and the cap is spent on duplicates.
TEST(config_watcher, directories_differing_only_in_case_are_one_directory)
{
	auto d = dirs({ L"C:\\Config\\shell.nss", L"c:\\CONFIG\\other.nss" });
	CHECK_EQ(d.size(), (size_t)1);
}

TEST(config_watcher, forward_slashes_are_separators_too)
{
	auto d = dirs({ L"C:/config/shell.nss" });
	CHECK_EQ(d.size(), (size_t)1);
	CHECK(d[0] == L"C:/config");
}

// FindFirstChangeNotification's page: the path "cannot be a relative path or an
// empty string". A bare filename has no directory to contribute.
TEST(config_watcher, a_path_with_no_directory_contributes_nothing)
{
	CHECK_EQ(dirs({ L"shell.nss" }).size(), (size_t)0);
	CHECK_EQ(dirs({ L"" }).size(), (size_t)0);
	CHECK_EQ(dirs({ L"\\shell.nss" }).size(), (size_t)0);
}

TEST(config_watcher, the_cap_is_honoured)
{
	std::vector<std::wstring> files;
	for(int i = 0; i < 40; i++)
		files.push_back(L"C:\\dir" + std::to_wstring(i) + L"\\a.nss");

	CHECK_EQ(dirs(files, 15).size(), (size_t)15);
	CHECK_EQ(dirs(files, 1).size(), (size_t)1);
	CHECK_EQ(dirs(files, 0).size(), (size_t)0);
}

TEST(config_watcher, the_shipping_cap_leaves_room_for_the_stop_event)
{
	// A compile-time fact, asserted at compile time: the wait takes the stop
	// event plus one handle per directory, and MAXIMUM_WAIT_OBJECTS is the
	// ceiling. CHECK() on a constant is a warning MSVC treats as an error here,
	// and rightly - this never varies at run time.
	static_assert(ConfigWatcher::MAX_DIRECTORIES + 1 <= MAXIMUM_WAIT_OBJECTS,
				  "the watch set plus the stop event must fit in one wait");
	CHECK_EQ(ConfigWatcher::MAX_DIRECTORIES, (size_t)15);
}

// ---- the watcher, against real files ------------------------------------

TEST(config_watcher, a_watcher_with_nothing_to_watch_does_not_start)
{
	ConfigWatcher watcher;
	CHECK(!watcher.start({}, &count_reload));
	CHECK(!watcher.start({ L"shell.nss" }, &count_reload));

	TempDir tmp;
	auto file = tmp.write(L"shell.nss", "item(title='A')\r\n");
	CHECK(!watcher.start({ file }, nullptr));
	CHECK(!watcher.watching());
}

TEST(config_watcher, writing_a_watched_file_triggers_exactly_one_reload)
{
	TempDir tmp;
	auto file = tmp.write(L"shell.nss", "item(title='A')\r\n");

	ConfigWatcher watcher;
	g_reloads.store(0);
	CHECK(watcher.start({ file }, &count_reload));
	CHECK(watcher.watching());

	// A save that writes more than once, as editors do.
	tmp.write(L"shell.nss", "item(title='B')\r\n");
	tmp.write(L"shell.nss", "item(title='BB')\r\n");

	CHECK_MSG(wait_for_reloads(watcher, 1, 8000), "a save must reach the watcher");

	// The debounce is what makes several writes one reload. Give it long enough
	// that a second reload would have arrived if it were coming.
	::Sleep(ConfigWatcher::DEBOUNCE_MS * 4);
	CHECK_MSG(watcher.reloads() == 1, "one save is one reload, not one per write");

	watcher.stop();
	CHECK(!watcher.watching());
}

// The watcher watches directories, so it is woken by files it does not care
// about. It must not reload for them - a reload re-parses the whole
// configuration.
TEST(config_watcher, an_unrelated_file_in_the_same_directory_reloads_nothing)
{
	TempDir tmp;
	auto file = tmp.write(L"shell.nss", "item(title='A')\r\n");

	ConfigWatcher watcher;
	CHECK(watcher.start({ file }, &count_reload));

	tmp.write(L"notes.txt", "nothing to do with the configuration");
	tmp.write(L"other.nss", "item(title='NotImported')\r\n");

	::Sleep(ConfigWatcher::DEBOUNCE_MS * 6);
	CHECK_MSG(watcher.reloads() == 0,
			  "only the files that were loaded may cause a reload");

	// And the watcher is still live afterwards, rather than having consumed its
	// one notification on a file it ignored.
	tmp.write(L"shell.nss", "item(title='C')\r\n");
	CHECK_MSG(wait_for_reloads(watcher, 1, 8000),
			  "a real change still reaches it after an ignored one");

	watcher.stop();
}

// An import lives in its own directory; losing it is a change to the
// configuration just as much as editing it.
TEST(config_watcher, deleting_a_watched_file_is_a_change)
{
	TempDir tmp;
	auto root = tmp.write(L"shell.nss", "import 'part.nss'\r\n");
	auto part = tmp.write(L"part.nss", "item(title='Imported')\r\n");

	ConfigWatcher watcher;
	CHECK(watcher.start({ root, part }, &count_reload));

	::DeleteFileW(part.c_str());

	CHECK_MSG(wait_for_reloads(watcher, 1, 8000), "a deleted import is a change");
	watcher.stop();
}

// Every successful load re-points the watcher, so start-on-top-of-running has
// to be safe and has to leave exactly one thread behind.
TEST(config_watcher, restarting_replaces_the_previous_watch)
{
	TempDir tmp;
	auto first = tmp.write(L"one.nss", "item(title='One')\r\n");
	auto second = tmp.write(L"two.nss", "item(title='Two')\r\n");

	ConfigWatcher watcher;
	CHECK(watcher.start({ first }, &count_reload));
	CHECK(watcher.start({ second }, &count_reload));
	CHECK(watcher.watching());

	// The count belongs to the new watch; the old one's is gone with it.
	CHECK_EQ(watcher.reloads(), 0ull);

	tmp.write(L"two.nss", "item(title='TwoAgain')\r\n");
	CHECK_MSG(wait_for_reloads(watcher, 1, 8000), "the new watch is live");

	watcher.stop();
}

/*
	The reload callback re-points the watcher, and that must not kill it.

	This is the shape the shipping code actually has and the one every other
	test in this file misses. `Initializer::on_config_file_changed` calls
	`init()`, a successful load calls `start_watching(parser->LoadedFiles())` -
	because an edit can add or remove an import - and that lands in `start()`
	**on the watcher's own thread**.

	`start()` used to call `stop()`, which joins that same thread. The standard
	makes joining the current thread `resource_deadlock_would_occur`, so
	`join()` throws; but `stop()` had already signalled the stop event, so the
	run loop returned on its next wait. The exception was caught by
	`load_generation`'s `catch(...)` and the watcher was simply gone -
	`watching()` still answering true, no log line, nothing.

	The symptom was one reload per Explorer lifetime. Found in a real Explorer
	on 2026-08-25, by editing the installed shell.nss twice and noticing only
	the first edit reached the menu; every test here called `start()` from the
	test's own thread, which is the one caller the old code was correct for.

	Two reloads is the whole assertion. One is what the defect produced.
*/
namespace
{
	ConfigWatcher *g_repointing_watcher = nullptr;
	std::vector<std::wstring> g_repointing_files;
	std::atomic<bool> g_repoint_threw{ false };

	void reload_and_repoint()
	{
		g_reloads.fetch_add(1, std::memory_order_release);
		if(!g_repointing_watcher)
			return;

		// Wrapped for the same reason Initializer::load_generation wraps its
		// whole body in catch(...): without it the throw described below
		// reaches the top of the watcher thread and takes the process with it,
		// so the suite would terminate instead of reporting - the flaw
		// docs/refactor/08-handoff.md section 1 rule 2 exists to catch. In the
		// DLL that catch is what made the defect invisible; here it is what
		// makes it legible.
		try
		{
			g_repointing_watcher->start(g_repointing_files, &reload_and_repoint);
		}
		catch(...)
		{
			g_repoint_threw.store(true, std::memory_order_release);
		}
	}
}

TEST(config_watcher, a_callback_that_re_points_the_watcher_does_not_kill_it)
{
	TempDir tmp;
	auto file = tmp.write(L"shell.nss", "item(title='A')\r\n");

	ConfigWatcher watcher;
	g_reloads.store(0);
	g_repoint_threw.store(false);
	g_repointing_watcher = &watcher;
	g_repointing_files = { file };

	CHECK(watcher.start({ file }, &reload_and_repoint));

	// Every write here is a different length on purpose. The stamp folds the
	// file size into the write time, so two same-sized writes landing inside
	// one file-time tick stamp identically and read as "nothing changed" -
	// which is a flake in the test rather than a finding about the watcher, and
	// it cost this test its first run.
	tmp.write(L"shell.nss", "item(title='BB')\r\n");
	CHECK_MSG(wait_for_reloads(watcher, 1, 8000), "the first save reaches the watcher");

	// The one that used to be impossible. Long enough after the first that the
	// debounce cannot fold the two together.
	::Sleep(ConfigWatcher::DEBOUNCE_MS * 4);
	tmp.write(L"shell.nss", "item(title='CCC')\r\n");
	CHECK_MSG(wait_for_reloads(watcher, 2, 8000),
			  "a second save must reach it too - the callback re-pointing the "
			  "watcher must not stop it");

	// And a third, so the fix is a working loop rather than an off-by-one that
	// bought exactly one more reload.
	::Sleep(ConfigWatcher::DEBOUNCE_MS * 4);
	tmp.write(L"shell.nss", "item(title='DDDD')\r\n");
	CHECK_MSG(wait_for_reloads(watcher, 3, 8000), "and so must every one after it");

	CHECK_MSG(!g_repoint_threw.load(),
			  "re-pointing from the callback must not throw - joining the "
			  "calling thread is resource_deadlock_would_occur");

	g_repointing_watcher = nullptr;
	watcher.stop();
	CHECK(!watcher.watching());
}

TEST(config_watcher, a_callback_can_re_point_the_watcher_at_a_different_file)
{
	// The reason the reload re-points at all: an edit can add or remove an
	// import, so the set of files to watch changes with the configuration.
	TempDir tmp;
	auto first = tmp.write(L"one.nss", "item(title='One')\r\n");
	auto second = tmp.write(L"two.nss", "item(title='Two')\r\n");

	ConfigWatcher watcher;
	g_reloads.store(0);
	g_repointing_watcher = &watcher;
	g_repointing_files = { second };		// what the "reload" decides to watch

	CHECK(watcher.start({ first }, &reload_and_repoint));

	tmp.write(L"one.nss", "item(title='OneAgain')\r\n");
	CHECK_MSG(wait_for_reloads(watcher, 1, 8000), "the original watch is live");

	::Sleep(ConfigWatcher::DEBOUNCE_MS * 4);

	// Now only two.nss is watched, so editing it reloads and editing one.nss
	// does not.
	tmp.write(L"two.nss", "item(title='TwoAgain')\r\n");
	CHECK_MSG(wait_for_reloads(watcher, 2, 8000), "the re-pointed watch is live");

	g_repointing_watcher = nullptr;
	watcher.stop();
}

TEST(config_watcher, stopping_twice_is_harmless)
{
	TempDir tmp;
	auto file = tmp.write(L"shell.nss", "item(title='A')\r\n");

	ConfigWatcher watcher;
	CHECK(watcher.start({ file }, &count_reload));
	watcher.stop();
	watcher.stop();
	CHECK(!watcher.watching());
}

/*
	Every exit from the watch thread must leave watching() false.

	Four of the five ways out of the loop used to leave it true: a failed
	WaitForMultipleObjects, an index outside the handle range, a failed
	FindNextChangeNotification, and - before it was given its own store - a
	re-point that found nothing watchable. The watcher was gone; watching() said
	it was there. Nothing user-visible reads it yet, which is exactly why the
	regression would have been silent until something did, and is the same shape
	as the re-entrant start() in docs/refactor/03-config-safety.md section 3b.

	The wait is injected rather than provoked. Closing a handle another thread is
	already blocked on does not reliably return that thread's wait, so a test
	written that way would measure a race instead of the invariant.
*/
namespace
{
	DWORD WINAPI wait_that_fails(DWORD, const HANDLE *, BOOL, DWORD)
	{
		return WAIT_FAILED;
	}

	// Inside neither the stop slot nor any notification slot. The loop treats
	// this as unusable and returns, which it should - and used to do while
	// still claiming to be watching.
	DWORD WINAPI wait_that_answers_out_of_range(DWORD, const HANDLE *, BOOL, DWORD)
	{
		return WAIT_OBJECT_0 + MAXIMUM_WAIT_OBJECTS;
	}

	bool wait_until_not_watching(const ConfigWatcher &watcher, DWORD timeout_ms)
	{
		auto deadline = ::GetTickCount64() + timeout_ms;
		while(::GetTickCount64() < deadline)
		{
			if(!watcher.watching())
				return true;
			::Sleep(10);
		}
		return !watcher.watching();
	}
}

TEST(config_watcher, a_failed_wait_ends_the_watch_and_says_so)
{
	TempDir tmp;
	auto file = tmp.write(L"shell.nss", "item(title='A')\r\n");

	ConfigWatcher watcher;
	watcher.set_wait_for_testing(&wait_that_fails);
	CHECK(watcher.start({ file }, &count_reload));

	CHECK_MSG(wait_until_not_watching(watcher, 4000),
			  "the thread returned on WAIT_FAILED, so watching() must be false");

	watcher.stop();
	CHECK(!watcher.watching());
}

TEST(config_watcher, an_out_of_range_wait_result_ends_the_watch_and_says_so)
{
	TempDir tmp;
	auto file = tmp.write(L"shell.nss", "item(title='A')\r\n");

	ConfigWatcher watcher;
	watcher.set_wait_for_testing(&wait_that_answers_out_of_range);
	CHECK(watcher.start({ file }, &count_reload));

	CHECK_MSG(wait_until_not_watching(watcher, 4000),
			  "the thread returned on an unusable index, so watching() must be false");

	watcher.stop();
	CHECK(!watcher.watching());
}

TEST(config_watcher, a_re_point_with_nothing_watchable_ends_the_watch_and_says_so)
{
	// The fifth exit, and the only one reachable without the seam: the reload
	// callback re-points at a set with no watchable directory in it, rearm()
	// refuses it, and the thread stops. "Best-effort" has always meant the
	// keyboard combos remain.
	//
	// A bare filename rather than an empty list, because start() rejects an
	// empty list before it reaches the re-entrancy check and would therefore
	// never ask for the re-point at all. config_watch_directories answers
	// nothing for a path with no directory part - FindFirstChangeNotification's
	// page: the path "cannot be a relative path or an empty string".
	TempDir tmp;
	auto file = tmp.write(L"shell.nss", "item(title='A')\r\n");

	ConfigWatcher watcher;
	g_reloads.store(0);
	g_repointing_watcher = &watcher;
	g_repointing_files = { L"nothing-watchable.nss" };

	CHECK(watcher.start({ file }, &reload_and_repoint));

	tmp.write(L"shell.nss", "item(title='BB')\r\n");

	// Not wait_for_reloads: ConfigWatcher::reloads() is published *after* the
	// re-point is applied, and this re-point is the one that ends the thread -
	// so the counter never reaches one and waiting on it would time out
	// whatever the watcher did. The callback ran; watching() going false is the
	// observable that this test is actually about.
	CHECK_MSG(wait_until_not_watching(watcher, 12000),
			  "nothing left to watch, so watching() must be false");
	CHECK_MSG(g_reloads.load(std::memory_order_acquire) >= 1,
			  "and the save did reach the callback first");

	g_repointing_watcher = nullptr;
	watcher.stop();
}
