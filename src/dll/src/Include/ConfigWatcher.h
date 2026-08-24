#pragma once

/*
	Notice that shell.nss changed, without waiting to be right-clicked.

	docs/refactor/03-config-safety.md section 3. Today the only ways a running
	process picks up an edit are the keyboard combos in Initializer::OnState -
	Ctrl+right-click or Shift+Ctrl+right-click - because the timestamp poll that
	was meant to be the automatic route is unreachable dead code (section 1
	records why: every caller of has_error() takes the default
	detect_changes = false). So editing a configuration means editing, then
	remembering a key combination, then right-clicking. This removes the middle
	step.

	Three departures from the plan's sketch, each because the tree turned out to
	be further along than it assumed.

	**The watch set is already computed.** The plan expected to reach into the
	parser's `_imports` stack, or to extend the parser to record import paths.
	Parser::LoadedFiles() has done exactly that since the last-known-good shadow
	landed - root first, every import after it - so the watcher takes the same
	list the shadow does and needs no parser change at all.

	**FindFirstChangeNotification, not ReadDirectoryChangesW.** The plan named
	the latter. It is the right call when you need to know *which* file changed;
	its own page says so of the former: "This function does not indicate the
	change that satisfied the wait condition. To retrieve information about the
	specific change as part of the notification, use the ReadDirectoryChangesW
	function."

		https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstchangenotificationw

	But a reload re-parses the whole set regardless, so the filename would be
	discarded the moment it arrived. What the simpler primitive avoids is a
	caller-owned buffer, overlapped I/O, and the ERROR_NOTIFY_ENUM_DIR overflow
	path where the buffer filled and the notifications have to be reconstructed
	by rescanning - three failure modes bought for information this code does
	not want.

	**A wakeup is a hint, not an answer.** Watching directories rather than
	files means any change in the same folder wakes the thread. Every wakeup
	therefore re-reads the write times of the files that were actually loaded,
	and does nothing unless one of them moved. That check is also what makes the
	documented gap between the wait returning and FindNextChangeNotification
	re-arming harmless: a notification lost in that window is covered by the
	next one, and by the timestamps not matching.

	Best-effort, deliberately. The same page: "Notifications may not be returned
	when calling FindFirstChangeNotification for a remote file system." A
	configuration on a network share may never notify, so the keyboard combos
	stay exactly as they are and nothing here is allowed to be the only way
	back.
*/

#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		/*
			The distinct directories that have to be watched to cover a set of
			files, in the order the files were loaded.

			Pure, so the rule can be tested without touching a disk: dedupe is
			case-insensitive because the file system is, the cap exists because
			the wait below is a WaitForMultipleObjects and a configuration that
			imported from a hundred directories would otherwise fail to watch
			any of them, and a path with no directory part contributes nothing
			rather than an empty string that FindFirstChangeNotification would
			reject ("This cannot be a relative path or an empty string").
		*/
		inline std::vector<std::wstring> config_watch_directories(
			const std::vector<std::wstring> &files, size_t cap)
		{
			std::vector<std::wstring> directories;
			if(cap == 0)
				return directories;

			for(const auto &file : files)
			{
				auto slash = file.find_last_of(L"\\/");
				if(slash == std::wstring::npos || slash == 0)
					continue;

				auto directory = file.substr(0, slash);

				bool seen = false;
				for(const auto &known : directories)
				{
					if(known.size() == directory.size() &&
					   ::CompareStringOrdinal(known.c_str(), static_cast<int>(known.size()),
											  directory.c_str(), static_cast<int>(directory.size()),
											  TRUE) == CSTR_EQUAL)
					{
						seen = true;
						break;
					}
				}
				if(seen)
					continue;

				directories.push_back(std::move(directory));
				if(directories.size() >= cap)
					break;
			}

			return directories;
		}

		class ConfigWatcher
		{
		public:
			// One slot of the wait is the stop event, and WaitForMultipleObjects
			// takes at most MAXIMUM_WAIT_OBJECTS handles. Well below it: a
			// configuration spread across more than fifteen directories is not
			// a case worth widening the wait for, and the cap failing loudly in
			// a test beats the wait failing silently on a user's machine.
			static constexpr size_t MAX_DIRECTORIES = 15;

			// Long enough that a save which writes, truncates and rewrites
			// produces one reload rather than three; short enough that it still
			// feels immediate.
			static constexpr DWORD DEBOUNCE_MS = 250;

			using Callback = void (*)();

			ConfigWatcher() = default;
			ConfigWatcher(const ConfigWatcher &) = delete;
			ConfigWatcher &operator=(const ConfigWatcher &) = delete;
			~ConfigWatcher() { stop(); }

			bool watching() const noexcept
			{
				return _running.load(std::memory_order_relaxed);
			}

			/*
				Watch the directories covering `files`, calling `on_changed`
				when one of those files is written.

				Restartable: the set of imports changes with the configuration,
				so every successful load re-points the watcher. Stopping first
				is what makes that safe - the thread must be gone before its
				handles are.
			*/
			bool start(const std::vector<std::wstring> &files, Callback on_changed)
			{
				stop();

				if(!on_changed || files.empty())
					return false;

				auto directories = config_watch_directories(files, MAX_DIRECTORIES);
				if(directories.empty())
					return false;

				_stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
				if(!_stop)
					return false;

				std::vector<HANDLE> handles;
				handles.push_back(_stop);

				for(const auto &directory : directories)
				{
					auto handle = ::FindFirstChangeNotificationW(
						directory.c_str(), FALSE,
						FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);

					// One unwatchable directory - a removable drive, a share
					// that does not notify - is not a reason to watch none of
					// the others.
					if(handle != INVALID_HANDLE_VALUE)
						handles.push_back(handle);
				}

				if(handles.size() <= 1)
				{
					::CloseHandle(_stop);
					_stop = nullptr;
					return false;
				}

				_watched = files;
				_stamps = write_times(files);
				_handles = std::move(handles);
				_callback = on_changed;
				_running.store(true, std::memory_order_release);

				_thread = std::thread(&ConfigWatcher::run, this);
				return true;
			}

			void stop()
			{
				if(_stop)
					::SetEvent(_stop);

				if(_thread.joinable())
					_thread.join();

				_running.store(false, std::memory_order_release);

				// The stop event is _handles[0]; everything after it came from
				// FindFirstChangeNotification and is closed by its own function.
				for(size_t i = 1; i < _handles.size(); i++)
					::FindCloseChangeNotification(_handles[i]);
				_handles.clear();

				if(_stop)
				{
					::CloseHandle(_stop);
					_stop = nullptr;
				}

				_watched.clear();
				_stamps.clear();
				_callback = nullptr;
			}

			// Reloads this watcher has actually triggered, for reporting and
			// for tests that must wait for one rather than sleep for one.
			uint64_t reloads() const noexcept
			{
				return _reloads.load(std::memory_order_acquire);
			}

		private:
			static std::vector<uint64_t> write_times(const std::vector<std::wstring> &files)
			{
				std::vector<uint64_t> stamps;
				stamps.reserve(files.size());

				for(const auto &file : files)
				{
					WIN32_FILE_ATTRIBUTE_DATA data{};
					uint64_t stamp = 0;
					if(::GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data))
					{
						stamp = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
								data.ftLastWriteTime.dwLowDateTime;

						// A file that changed size within the same write-time
						// tick is still a change. Cheap, and it costs nothing
						// to fold in.
						stamp ^= (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
					}

					// A file that has been deleted stamps as zero, which
					// differs from whatever it stamped as before - so losing an
					// import counts as a change, which is what it is.
					stamps.push_back(stamp);
				}

				return stamps;
			}

			void run()
			{
				for(;;)
				{
					auto index = ::WaitForMultipleObjects(static_cast<DWORD>(_handles.size()),
														  _handles.data(), FALSE, INFINITE);

					// Index 0 is the stop event; a failed wait is not something
					// to spin on.
					if(index == WAIT_OBJECT_0 || index == WAIT_FAILED)
						return;

					if(index < WAIT_OBJECT_0 || index >= WAIT_OBJECT_0 + _handles.size())
						return;

					/*
						Debounce. An editor saving a file commonly produces
						several notifications, and every directory in the set
						can report the same save. Keep re-arming and re-waiting
						with a short timeout until things go quiet, so one save
						is one reload.
					*/
					for(;;)
					{
						auto woken = index - WAIT_OBJECT_0;
						if(woken > 0 && !::FindNextChangeNotification(_handles[woken]))
							return;

						index = ::WaitForMultipleObjects(static_cast<DWORD>(_handles.size()),
														 _handles.data(), FALSE, DEBOUNCE_MS);

						if(index == WAIT_OBJECT_0 || index == WAIT_FAILED)
							return;

						if(index == WAIT_TIMEOUT)
							break;
					}

					// The wakeup said "something in one of these directories".
					// Only the files that were actually loaded matter.
					auto stamps = write_times(_watched);
					if(stamps == _stamps)
						continue;

					_stamps = std::move(stamps);

					if(_callback)
						_callback();

					// Published after the callback, so a test that waits for
					// this to change is waiting for the reload to have
					// happened rather than for it to have started.
					_reloads.fetch_add(1, std::memory_order_release);
				}
			}

			std::thread _thread;
			HANDLE _stop{};
			std::vector<HANDLE> _handles;
			std::vector<std::wstring> _watched;
			std::vector<uint64_t> _stamps;
			Callback _callback{};
			std::atomic<bool> _running{};
			std::atomic<uint64_t> _reloads{};
		};
	}
}
