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
