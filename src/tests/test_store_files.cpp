#include "test.h"

#include <windows.h>

#include "..\shared\StoreFile.h"
#include "..\shared\Favorites.h"
#include "..\shared\ProviderQuarantine.h"
#include "Include/FavoritesStore.h"
#include "Include/ProviderQuarantineStore.h"

#include <atomic>
#include <thread>

// favorites.txt and quarantine.txt, and the holders that cache them.
//
// Neither holder was included by any test file. R6.4 asked for direct
// real-file tests of both and delivered the mutex fix without them, so the
// three defects below shipped with the class that was supposed to have been
// reviewed:
//
//   - a failed read was indistinguishable from an empty list, so a
//     read-modify-write wrote a one-entry file over the user's history;
//   - a failed *save* was swapped into process state anyway, and refreshed the
//     stamp, so refresh_if_stale never re-read and the divergence was
//     permanent rather than transient;
//   - content and write time came from two separate calls, so a rewrite
//     between them cached old data under a new stamp.
//
// These use real files in %TEMP%, not a mock, because every one of the three is
// about what the file system does between two calls.

using namespace Nilesoft::Shell;

namespace
{
	std::wstring temp_path(const wchar_t *leaf)
	{
		wchar_t directory[MAX_PATH]{};
		if(::GetTempPathW(MAX_PATH, directory) == 0)
			return {};
		std::wstring out = directory;
		out += L"nilesoft-store-tests\\";
		::CreateDirectoryW(out.c_str(), nullptr);
		out += leaf;
		::DeleteFileW(out.c_str());
		return out;
	}

	void write_raw(const std::wstring &path, const void *bytes, DWORD count)
	{
		auto file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
								  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if(file == INVALID_HANDLE_VALUE)
			return;
		DWORD written = 0;
		::WriteFile(file, bytes, count, &written, nullptr);
		::CloseHandle(file);
	}

	// A handle that keeps a file open with no sharing at all, which is what a
	// host in the middle of the old CREATE_ALWAYS save looked like to everybody
	// else.
	struct ExclusiveHold
	{
		HANDLE handle{ INVALID_HANDLE_VALUE };

		explicit ExclusiveHold(const std::wstring &path)
		{
			handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
								   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		}
		~ExclusiveHold() { release(); }
		void release()
		{
			if(handle != INVALID_HANDLE_VALUE)
			{
				::CloseHandle(handle);
				handle = INVALID_HANDLE_VALUE;
			}
		}
		bool held() const { return handle != INVALID_HANDLE_VALUE; }
	};

	// The other half of the pair, and the one that isolates a *failed save*.
	//
	// Sharing READ but not DELETE means StoreFile::read succeeds - its own
	// share mode permits this handle's access - while ReplaceFile cannot take
	// the DELETE it needs on the target. So the store gets a good list, edits
	// it, and cannot write it. With an exclusive hold the read fails first and
	// the save is never reached, which makes that test pass without ever
	// exercising the thing it is named for.
	struct SharedReadHold
	{
		HANDLE handle{ INVALID_HANDLE_VALUE };

		explicit SharedReadHold(const std::wstring &path)
		{
			handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
								   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		}
		~SharedReadHold()
		{
			if(handle != INVALID_HANDLE_VALUE)
				::CloseHandle(handle);
		}
		bool held() const { return handle != INVALID_HANDLE_VALUE; }
	};

	// A real identity, not a hand-filled struct: Identity::valid() requires a
	// kind and a signature, and record_use refuses anything else - which is the
	// right behaviour and made the first version of these tests fail for a
	// reason that had nothing to do with the file system.
	MenuIdentity::Identity identity_of(uint32_t marker)
	{
		wchar_t signature[64]{};
		::swprintf_s(signature, L"probe/store/%08x", marker);
		return MenuIdentity::make(MenuIdentity::Kind::Item, signature);
	}
}

// ---- StoreFile ----------------------------------------------------------

TEST(store_file, a_missing_file_is_missing_rather_than_failed)
{
	auto path = temp_path(L"missing.txt");
	auto got = StoreFile::read(path, 64 * 1024);

	CHECK(got.state == StoreFile::LoadState::Missing);
	CHECK(got.usable());
	CHECK(got.text.empty());
}

TEST(store_file, a_file_that_will_not_open_is_failed_rather_than_empty)
{
	// The distinction the whole header exists for. Before it, this and the
	// case above produced the same value, and a caller writing back turned the
	// difference into data loss.
	auto path = temp_path(L"locked.txt");
	CHECK(StoreFile::replace(path, L"one\r\n"));

	ExclusiveHold hold(path);
	CHECK(hold.held());

	auto got = StoreFile::read(path, 64 * 1024);
	CHECK(got.state == StoreFile::LoadState::Failed);
	CHECK(!got.usable());
}

TEST(store_file, a_reader_does_not_block_a_replace)
{
	// FILE_SHARE_DELETE, and the reason it is not decoration: replace() renames
	// over the destination, and a rename cannot replace a file another process
	// holds open without it. Omitting it would move the sharing violation from
	// the writer to the reader rather than removing it.
	auto path = temp_path(L"shared.txt");
	CHECK(StoreFile::replace(path, L"first\r\n"));

	auto reader = ::CreateFileW(path.c_str(), GENERIC_READ,
								FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
								nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	CHECK(reader != INVALID_HANDLE_VALUE);

	CHECK(StoreFile::replace(path, L"second\r\n"));

	if(reader != INVALID_HANDLE_VALUE)
		::CloseHandle(reader);

	auto got = StoreFile::read(path, 64 * 1024);
	CHECK(got.loaded());
	CHECK(got.text == L"second\r\n");
}

TEST(store_file, the_content_and_the_stamp_come_from_one_handle)
{
	auto path = temp_path(L"stamped.txt");
	CHECK(StoreFile::replace(path, L"alpha\r\n"));

	auto first = StoreFile::read(path, 64 * 1024);
	CHECK(first.loaded());
	CHECK(first.write_time != 0);

	// A file system timestamp has coarse resolution, so the content is what
	// makes this meaningful rather than the number: the two must describe the
	// same file, and they cannot if they were fetched by two separate calls
	// with a write in between.
	::Sleep(40);
	CHECK(StoreFile::replace(path, L"beta and then some more\r\n"));

	auto second = StoreFile::read(path, 64 * 1024);
	CHECK(second.loaded());
	CHECK(second.text == L"beta and then some more\r\n");
	CHECK(second.write_time != first.write_time);
}

TEST(store_file, an_oversized_file_is_failed_rather_than_truncated_or_empty)
{
	auto path = temp_path(L"huge.txt");
	std::wstring big(4096, L'x');
	CHECK(StoreFile::replace(path, big));

	// The cap exists because these are parsed on the way to a menu. A file
	// above it is a machine in a state nobody should write back over.
	auto got = StoreFile::read(path, 64);
	CHECK(got.state == StoreFile::LoadState::Failed);
	CHECK(!got.usable());
}

TEST(store_file, an_odd_sized_file_after_a_utf16_mark_is_failed)
{
	// Marked UTF-16, so an odd byte count is not a short read, it is a file
	// that is not what it claims to be.
	auto path = temp_path(L"odd.txt");
	const char bytes[] = { '\xFF', '\xFE', 'a' };
	write_raw(path, bytes, 3);

	auto got = StoreFile::read(path, 64 * 1024);
	CHECK(got.state == StoreFile::LoadState::Failed);
}

// ---- encoding on read ---------------------------------------------------

namespace
{
	std::string bytes_of(std::initializer_list<int> values)
	{
		std::string out;
		for(int v : values)
			out.push_back(static_cast<char>(v));
		return out;
	}

	std::wstring decoded(const std::string &bytes, bool &ok)
	{
		std::wstring out;
		ok = StoreFile::decode(bytes, out);
		return out;
	}
}

TEST(store_file, an_editors_encodings_are_all_read)
{
	bool ok = false;

	// UTF-16LE with a mark: what Shell itself writes.
	CHECK(decoded(bytes_of({ 0xFF, 0xFE, 'a', 0, 'b', 0 }), ok) == L"ab" && ok);
	// UTF-8 with a mark.
	CHECK(decoded(bytes_of({ 0xEF, 0xBB, 0xBF, 'a', 'b' }), ok) == L"ab" && ok);
	// UTF-16LE without one.
	CHECK(decoded(bytes_of({ 'a', 0, 'b', 0 }), ok) == L"ab" && ok);

	// UTF-8 without one - Notepad's default - at both parities, which is what
	// the old "even means UTF-16" rule could not tell apart.
	CHECK(decoded(bytes_of({ 'a', 'b' }), ok) == L"ab" && ok);
	CHECK(decoded(bytes_of({ 'a', 'b', 'c' }), ok) == L"abc" && ok);
	CHECK(decoded(bytes_of({ 'c', 'a', 'f', 0xC3, 0xA9 }), ok) == L"café" && ok);
	CHECK(decoded(bytes_of({ 'c', 'a', 'f', 0xC3, 0xA9, '!' }), ok) == L"café!" && ok);
}

TEST(store_file, bytes_that_are_not_text_are_refused_rather_than_repaired)
{
	bool ok = true;

	// An invalid sequence would come back as U+FFFD without
	// MB_ERR_INVALID_CHARS, and the file would be written back that way.
	decoded(bytes_of({ 'a', 0xC3, 0x28 }), ok);
	CHECK(!ok);
	decoded(bytes_of({ 0xFF, 'a' }), ok);
	CHECK(!ok);

	// UTF-16BE is not guessed at.
	decoded(bytes_of({ 0xFE, 0xFF, 0, 'a' }), ok);
	CHECK(!ok);

	// A NUL in what would have to be UTF-8 is a binary file.
	decoded(bytes_of({ 'a', 0, 'b' }), ok);
	CHECK(!ok);
}

TEST(store_file, a_utf8_file_is_read_from_disk)
{
	auto path = temp_path(L"utf8.txt");
	auto bytes = bytes_of({ 0xEF, 0xBB, 0xBF, '#', ' ', 'c', 'a', 'f', 0xC3, 0xA9, '\r', '\n' });
	write_raw(path, bytes.data(), static_cast<DWORD>(bytes.size()));

	auto got = StoreFile::read(path, 64 * 1024);
	CHECK(got.loaded());
	CHECK(got.text == L"# café\r\n");
}

TEST(store_file, an_undecodable_file_is_failed_and_never_written_back)
{
	auto path = temp_path(L"favorites-invalid.txt");
	auto bytes = bytes_of({ '#', ' ', 0xC3, 0x28, '\r', '\n' });
	write_raw(path, bytes.data(), static_cast<DWORD>(bytes.size()));

	CHECK(StoreFile::read(path, 64 * 1024).state == StoreFile::LoadState::Failed);

	std::vector<Favorites::Entry> entries;
	CHECK(Favorites::record_use(entries, identity_of(0x01010101)));
	CHECK(!Favorites::save(path, entries));

	std::vector<Quarantine::Entry> held;
	Quarantine::Entry e;
	CHECK(Quarantine::parse_guid(L"{11111111-2222-3333-4444-555555555555}", e.clsid));
	e.hash = Quarantine::hash_clsid(e.clsid);
	held.push_back(e);
	CHECK(!Quarantine::save(path, held));

	// Byte for byte what was there.
	auto file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
							  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	CHECK(file != INVALID_HANDLE_VALUE);
	if(file != INVALID_HANDLE_VALUE)
	{
		char back[64]{};
		DWORD got = 0;
		::ReadFile(file, back, sizeof(back), &got, nullptr);
		::CloseHandle(file);
		CHECK(std::string(back, got) == bytes);
	}
}

// ---- replace() when ReplaceFile fails -----------------------------------

namespace
{
	// What a real race cannot be made to produce on demand. Each fake leaves
	// the disk as the ReplaceFileW page says that error leaves it.
	// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew
	BOOL WINAPI fake_1176(LPCWSTR replaced, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID)
	{
		// "the replaced file no longer exists and the replacement file exists
		// under its original name"
		::DeleteFileW(replaced);
		::SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT);
		return FALSE;
	}

	BOOL WINAPI fake_1177(LPCWSTR replaced, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID)
	{
		// "The file to be replaced still exists with a different name."
		std::wstring aside = std::wstring(replaced) + L".old";
		::MoveFileExW(replaced, aside.c_str(), MOVEFILE_REPLACE_EXISTING);
		::SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
		return FALSE;
	}

	BOOL WINAPI fake_1175(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID)
	{
		::SetLastError(ERROR_UNABLE_TO_REMOVE_REPLACED);	// both names kept
		return FALSE;
	}

	BOOL WINAPI fake_invalid_parameter(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID)
	{
		::SetLastError(ERROR_INVALID_PARAMETER);			// both names kept
		return FALSE;
	}

	// The destination is gone *and* cannot be recreated: a directory now
	// stands at the path, which MoveFileEx will not replace.
	BOOL WINAPI fake_1176_then_blocked(LPCWSTR replaced, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID)
	{
		::DeleteFileW(replaced);
		::CreateDirectoryW(replaced, nullptr);
		::SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT);
		return FALSE;
	}

	struct FakeReplaceFile
	{
		StoreFile::ReplaceFileFn saved;
		explicit FakeReplaceFile(StoreFile::ReplaceFileFn fake)
			: saved(StoreFile::replace_file) { StoreFile::replace_file = fake; }
		~FakeReplaceFile() { StoreFile::replace_file = saved; }
	};

	// Every *.tmp beside `path`: what replace() stages into.
	std::vector<std::wstring> staged_beside(const std::wstring &path)
	{
		std::vector<std::wstring> out;
		auto directory = path.substr(0, path.find_last_of(L'\\') + 1);
		WIN32_FIND_DATAW found{};
		auto search = ::FindFirstFileW((directory + L"*.tmp").c_str(), &found);
		if(search == INVALID_HANDLE_VALUE)
			return out;
		do
			out.push_back(directory + found.cFileName);
		while(::FindNextFileW(search, &found));
		::FindClose(search);
		return out;
	}
}

TEST(store_file, a_replace_that_lost_the_destination_still_lands_the_new_content)
{
	// 1176: the only copy is the staged file, and deleting it - which this did
	// - left favorites.txt Missing, i.e. an empty list that is safe to write
	// over.
	auto path = temp_path(L"lost-1176.txt");
	CHECK(StoreFile::replace(path, L"old\r\n"));

	{
		FakeReplaceFile fake(&fake_1176);
		CHECK(StoreFile::replace(path, L"new\r\n"));
	}

	auto got = StoreFile::read(path, 64 * 1024);
	CHECK(got.loaded());
	CHECK(got.text == L"new\r\n");
	CHECK(staged_beside(path).empty());
}

TEST(store_file, a_replace_that_moved_the_destination_aside_still_lands_the_new_content)
{
	auto path = temp_path(L"lost-1177.txt");
	CHECK(StoreFile::replace(path, L"old\r\n"));

	{
		FakeReplaceFile fake(&fake_1177);
		CHECK(StoreFile::replace(path, L"new\r\n"));
	}

	auto got = StoreFile::read(path, 64 * 1024);
	CHECK(got.loaded());
	CHECK(got.text == L"new\r\n");
	CHECK(staged_beside(path).empty());
	::DeleteFileW((path + L".old").c_str());
}

TEST(store_file, a_replace_that_changed_nothing_keeps_the_old_content_and_tidies_up)
{
	// 1175 and everything else: "the replaced and replacement files retain
	// their original file names", so the staged copy is only litter.
	StoreFile::ReplaceFileFn fakes[] = { &fake_1175, &fake_invalid_parameter };
	for(auto fake : fakes)
	{
		auto path = temp_path(L"kept-other.txt");
		CHECK(StoreFile::replace(path, L"old\r\n"));

		{
			FakeReplaceFile scoped(fake);
			CHECK(!StoreFile::replace(path, L"new\r\n"));
		}

		auto got = StoreFile::read(path, 64 * 1024);
		CHECK(got.loaded());
		CHECK(got.text == L"old\r\n");
		CHECK(staged_beside(path).empty());
	}
}

TEST(store_file, the_staged_file_is_kept_when_it_is_the_only_copy_and_cannot_be_moved)
{
	auto path = temp_path(L"blocked-1176.txt");
	CHECK(StoreFile::replace(path, L"old\r\n"));

	{
		FakeReplaceFile fake(&fake_1176_then_blocked);
		CHECK(!StoreFile::replace(path, L"new\r\n"));
	}
	::RemoveDirectoryW(path.c_str());

	auto staged = staged_beside(path);
	CHECK_EQ(staged.size(), (size_t)1);
	for(const auto &file : staged)
	{
		auto got = StoreFile::read(file, 64 * 1024);
		CHECK(got.loaded());
		CHECK(got.text == L"new\r\n");
		::DeleteFileW(file.c_str());
	}
}

TEST(store_file, a_failed_replace_leaves_the_previous_file_intact)
{
	auto path = temp_path(L"kept.txt");
	CHECK(StoreFile::replace(path, L"original\r\n"));

	// Held with no sharing, so the rename cannot land.
	{
		ExclusiveHold hold(path);
		CHECK(hold.held());
		CHECK(!StoreFile::replace(path, L"replacement\r\n"));
	}

	auto got = StoreFile::read(path, 64 * 1024);
	CHECK(got.loaded());
	CHECK(got.text == L"original\r\n");
}

TEST(store_file, a_failed_replace_does_not_leave_its_temporary_behind)
{
	auto path = temp_path(L"tidy.txt");
	CHECK(StoreFile::replace(path, L"original\r\n"));

	{
		ExclusiveHold hold(path);
		CHECK(!StoreFile::replace(path, L"replacement\r\n"));
	}

	// One staging file per attempt, in the same directory, would accumulate
	// silently in %LocalAppData% forever.
	auto cut = path.find_last_of(L'\\');
	std::wstring pattern = path.substr(0, cut + 1) + L"*.tmp";
	WIN32_FIND_DATAW found{};
	auto search = ::FindFirstFileW(pattern.c_str(), &found);
	CHECK(search == INVALID_HANDLE_VALUE);
	if(search != INVALID_HANDLE_VALUE)
		::FindClose(search);
}

// ---- StoreFile::Lock ----------------------------------------------------

TEST(store_file, a_held_lock_refuses_a_second_and_releasing_it_removes_the_file)
{
	auto path = temp_path(L"locked-store.txt");
	auto lock_file = path + L".lock";

	{
		StoreFile::Lock first(path);
		CHECK(first);

		// Short timeout: the point is that it gives up rather than waits forever.
		StoreFile::Lock second(path, 50);
		CHECK_MSG(!second, "an exclusive file is exclusive within a process too");

		// A move hands the lock over rather than releasing it.
		StoreFile::Lock moved(std::move(first));
		CHECK(moved);
		StoreFile::Lock third(path, 50);
		CHECK(!third);
	}

	// DELETE_ON_CLOSE: nothing left beside the store, and the next taker wins.
	CHECK(::GetFileAttributesW(lock_file.c_str()) == INVALID_FILE_ATTRIBUTES);
	StoreFile::Lock again(path);
	CHECK(again);
}

TEST(store_file, locked_read_modify_write_from_two_threads_loses_no_update)
{
	/*
		The real shape of a writer - read, change, save, all under the lock -
		run by two threads that each add their own distinct entries. They share
		a process, so this does not need two processes to show the lost update:
		without the lock both read the same list and one entry is overwritten,
		and a read landing between ReplaceFileW's two renames starts from an
		empty list and drops everything.
	*/
	auto path = temp_path(L"lost-update.txt");
	constexpr uint32_t PerThread = 100;
	std::atomic<uint32_t> failed{ 0 };

	auto worker = [&](uint32_t base)
	{
		for(uint32_t i = 0; i < PerThread; i++)
		{
			StoreFile::Lock lock(path, 5000);
			if(!lock)
			{
				failed++;
				continue;
			}

			auto loaded = Favorites::read(path);
			if(!loaded.usable())
			{
				failed++;
				continue;
			}

			auto entries = std::move(loaded.entries);
			if(!Favorites::record_use(entries, identity_of(base + i))
			   || !Favorites::save(path, entries))
				failed++;
		}
	};

	std::thread a(worker, 0xA0000000u);
	std::thread b(worker, 0xB0000000u);
	a.join();
	b.join();

	CHECK_EQ(failed.load(), 0u);

	auto back = Favorites::read(path);
	CHECK(back.loaded());
	CHECK_EQ(back.entries.size(), static_cast<size_t>(2 * PerThread));
}

// ---- the file formats on top of it --------------------------------------

TEST(store_file, favorites_round_trip_through_the_atomic_write)
{
	auto path = temp_path(L"favorites.txt");

	std::vector<Favorites::Entry> entries;
	CHECK(Favorites::record_use(entries, identity_of(0xAABBCCDD)));
	CHECK(Favorites::save(path, entries));

	auto back = Favorites::read(path);
	CHECK(back.state == StoreFile::LoadState::Loaded);
	CHECK_EQ(back.entries.size(), (size_t)1);
	if(!back.entries.empty())
		CHECK_EQ(back.entries[0].uses, 1u);
}

TEST(store_file, a_favorites_read_that_failed_is_not_an_empty_list)
{
	auto path = temp_path(L"favorites-locked.txt");

	std::vector<Favorites::Entry> entries;
	CHECK(Favorites::record_use(entries, identity_of(0x11112222)));
	CHECK(Favorites::save(path, entries));

	ExclusiveHold hold(path);
	CHECK(hold.held());

	auto back = Favorites::read(path);
	CHECK(!back.usable());
	CHECK(back.entries.empty());

	// And this is the shape that made it destructive: load() answers with the
	// same empty vector it answers for a machine with no favourites at all.
	CHECK(Favorites::load(path).empty());
}

// ---- FavoritesStore -----------------------------------------------------

TEST(favorites_store, a_use_is_counted_and_visible_to_the_next_menu)
{
	auto path = temp_path(L"store-favorites.txt");
	auto &store = FavoritesStore::instance();
	store.set_path_for_testing(path);

	CHECK_EQ(store.size(), (size_t)0);
	CHECK(store.record_use(identity_of(0x0BADF00D)));

	auto ranks = store.ranks();
	CHECK_EQ(ranks.size(), (size_t)1);
	if(!ranks.empty())
	{
		CHECK_EQ(ranks[0].hash, identity_of(0x0BADF00D).hash);
		CHECK_EQ(ranks[0].uses, 1u);
	}

	store.set_path_for_testing({});
}

TEST(favorites_store, a_failed_save_does_not_become_process_state)
{
	/*
		The sticky-divergence defect, end to end.

		record_use swapped the unsaved entries into _entries *and* refreshed
		_stamp, so refresh_if_stale saw a matching timestamp and never re-read.
		This process then served a count that is not in the file, for as long
		as it lived.
	*/
	auto path = temp_path(L"store-failed-save.txt");
	auto &store = FavoritesStore::instance();
	store.set_path_for_testing(path);

	CHECK(store.record_use(identity_of(0x1234ABCD)));
	CHECK_EQ(store.size(), (size_t)1);

	{
		// Shared for reading but not for deleting, so the read succeeds and it
		// is the *save* that fails - which is the defect this test is named
		// for. An exclusive hold would fail the read first and never reach it.
		SharedReadHold hold(path);
		CHECK(hold.held());
		CHECK(!store.record_use(identity_of(0x5678EF01)));
	}

	// Nothing of the failed attempt survived, in memory or on disk.
	auto on_disk = Favorites::read(path);
	CHECK(on_disk.loaded());
	CHECK_EQ(on_disk.entries.size(), (size_t)1);

	auto ranks = store.ranks();
	CHECK_EQ(ranks.size(), (size_t)1);
	if(!ranks.empty())
		CHECK_EQ(ranks[0].hash, identity_of(0x1234ABCD).hash);

	store.set_path_for_testing({});
}

TEST(favorites_store, a_failed_read_never_writes_a_fresh_list_over_the_file)
{
	// The data-loss case named in docs/refactor/12-closure-plan.md §A.1 F7:
	// A truncates and holds -> B's load fails -> B gets {} -> A finishes ->
	// B saves a one-entry list over the complete history.
	auto path = temp_path(L"store-not-clobbered.txt");

	// A file this process cannot read but could perfectly well overwrite: past
	// Favorites::MaxFileBytes, so StoreFile::read refuses it while nothing
	// stands in the way of a save. A lock would fail both and prove nothing
	// about which of the two defences did the work.
	std::wstring oversized(Favorites::MaxFileBytes, L'x');
	CHECK(StoreFile::replace(path, oversized));

	auto before = StoreFile::read(path, Favorites::MaxFileBytes * 4);
	CHECK(before.loaded());
	auto original_size = before.text.size();

	auto &store = FavoritesStore::instance();
	store.set_path_for_testing(path);

	CHECK_MSG(!store.record_use(identity_of(0xDEADBEEF)),
			  "a file that could not be read is not a file with no favourites in it");

	auto after = StoreFile::read(path, Favorites::MaxFileBytes * 4);
	CHECK(after.loaded());
	CHECK_MSG(after.text.size() == original_size,
			  "a failed read must never become a one-entry file written over the history");

	store.set_path_for_testing({});
}

TEST(favorites_store, an_edit_by_another_host_is_picked_up)
{
	auto path = temp_path(L"store-external-edit.txt");
	auto &store = FavoritesStore::instance();
	store.set_path_for_testing(path);

	CHECK(store.record_use(identity_of(0x00000101)));
	CHECK_EQ(store.size(), (size_t)1);

	// Somebody else's process writes two entries.
	std::vector<Favorites::Entry> theirs;
	CHECK(Favorites::record_use(theirs, identity_of(0x00000201)));
	CHECK(Favorites::record_use(theirs, identity_of(0x00000202)));
	::Sleep(40);
	CHECK(Favorites::save(path, theirs));

	// reload() rather than waiting out CheckIntervalMs: the interval is a
	// throttle on stat calls, not the property under test.
	store.reload();
	CHECK_EQ(store.size(), (size_t)2);

	store.set_path_for_testing({});
}

TEST(favorites_store, concurrent_uses_in_one_process_all_land)
{
	// The same-process half of the race. Without the transaction under one
	// lock, two threads read the same list, each increments its own copy, and
	// one of the two writes is lost.
	auto path = temp_path(L"store-concurrent.txt");
	auto &store = FavoritesStore::instance();
	store.set_path_for_testing(path);

	std::vector<std::thread> threads;
	for(uint32_t i = 0; i < 8; i++)
		threads.emplace_back([&store, i] { store.record_use(identity_of(0xC0000000 + i)); });
	for(auto &t : threads)
		t.join();

	store.reload();
	CHECK_EQ(store.size(), (size_t)8);

	store.set_path_for_testing({});
}

// ---- ProviderQuarantineStore --------------------------------------------

TEST(quarantine_store, a_quarantined_hash_is_found_and_an_absent_one_is_not)
{
	auto path = temp_path(L"store-quarantine.txt");
	auto &store = ProviderQuarantineStore::instance();

	GUID clsid{ 0x1FA0E654, 0xC9F2, 0x4A1F, { 0x98, 0x00, 0xB9, 0xA7, 0x5D, 0x74, 0x4B, 0x00 } };
	std::vector<Quarantine::Entry> entries;
	Quarantine::Entry e;
	e.clsid = clsid;
	e.hash = Quarantine::hash_clsid(clsid);
	entries.push_back(e);
	CHECK(Quarantine::save(path, entries));

	store.set_path_for_testing(path);
	CHECK(store.contains(e.hash));
	CHECK(!store.contains(e.hash ^ 0x1u));

	store.set_path_for_testing({});
}

TEST(quarantine_store, a_read_that_failed_keeps_the_previous_list)
{
	/*
		The half that matters for the menu: a quarantined provider must not
		quietly come back because the file could not be read for a moment.
		reload() used to swap in the empty vector Quarantine::load returns for
		every failure, and stamp it fresh.
	*/
	auto path = temp_path(L"store-quarantine-locked.txt");

	GUID clsid{ 0x2FA0E654, 0xC9F2, 0x4A1F, { 0x98, 0x00, 0xB9, 0xA7, 0x5D, 0x74, 0x4B, 0x01 } };
	std::vector<Quarantine::Entry> entries;
	Quarantine::Entry e;
	e.clsid = clsid;
	e.hash = Quarantine::hash_clsid(clsid);
	entries.push_back(e);
	CHECK(Quarantine::save(path, entries));

	auto &store = ProviderQuarantineStore::instance();
	store.set_path_for_testing(path);
	CHECK(store.contains(e.hash));

	{
		ExclusiveHold hold(path);
		CHECK(hold.held());
		store.reload();
		CHECK_MSG(store.contains(e.hash),
				  "a file that would not open is not a file that says nothing is quarantined");
	}

	store.set_path_for_testing({});
}

TEST(quarantine_store, a_missing_file_really_is_an_empty_list)
{
	// The over-correction guard for the test above: refusing every reload
	// would satisfy it and would mean releasing a quarantine never took
	// effect. A file that is not there is a real answer.
	auto path = temp_path(L"store-quarantine-released.txt");

	GUID clsid{ 0x3FA0E654, 0xC9F2, 0x4A1F, { 0x98, 0x00, 0xB9, 0xA7, 0x5D, 0x74, 0x4B, 0x02 } };
	std::vector<Quarantine::Entry> entries;
	Quarantine::Entry e;
	e.clsid = clsid;
	e.hash = Quarantine::hash_clsid(clsid);
	entries.push_back(e);
	CHECK(Quarantine::save(path, entries));

	auto &store = ProviderQuarantineStore::instance();
	store.set_path_for_testing(path);
	CHECK(store.contains(e.hash));

	::DeleteFileW(path.c_str());
	store.reload();
	CHECK(!store.contains(e.hash));

	store.set_path_for_testing({});
}

TEST(quarantine_store, an_empty_saved_list_releases_everything)
{
	auto path = temp_path(L"store-quarantine-empty.txt");

	GUID clsid{ 0x4FA0E654, 0xC9F2, 0x4A1F, { 0x98, 0x00, 0xB9, 0xA7, 0x5D, 0x74, 0x4B, 0x03 } };
	std::vector<Quarantine::Entry> entries;
	Quarantine::Entry e;
	e.clsid = clsid;
	e.hash = Quarantine::hash_clsid(clsid);
	entries.push_back(e);
	CHECK(Quarantine::save(path, entries));

	auto &store = ProviderQuarantineStore::instance();
	store.set_path_for_testing(path);
	CHECK(store.contains(e.hash));

	CHECK(Quarantine::save(path, {}));
	store.reload();
	CHECK(!store.contains(e.hash));

	store.set_path_for_testing({});
}
