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

	**The watch set is already computed.** Parser::LoadedFiles() lists every
	file a parse read, root first, and Parser::LoadedStamps() carries the
	ConfigStamp.h stamp taken just before each was opened. The watcher takes
	both: the files say what to watch, the stamps say what the configuration
	being served actually looked like.

	**FindFirstChangeNotification, not ReadDirectoryChangesW.** The latter is
	the right call when you need to know *which* file changed; the former's own
	page says so: "This function does not indicate the change that satisfied
	the wait condition. To retrieve information about the specific change as
	part of the notification, use the ReadDirectoryChangesW function."

		https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstchangenotificationw

	But a reload re-parses the whole set regardless, so the filename would be
	discarded the moment it arrived. What the simpler primitive avoids is a
	caller-owned buffer, overlapped I/O, and the ERROR_NOTIFY_ENUM_DIR overflow
	path where the buffer filled and the notifications have to be reconstructed
	by rescanning - three failure modes bought for information this code does
	not want.

	**A wakeup is a hint, not an answer.** Watching directories rather than
	files means any change in the same folder wakes the thread. Every wakeup
	therefore re-reads the stamps of the files that were actually loaded, and
	does nothing unless one differs from the baseline. That comparison is also
	what makes the documented gap between the wait returning and
	FindNextChangeNotification re-arming harmless: a notification lost in that
	window is covered by the next one, and by the stamps not matching.

	**The baseline is the parser's, not the watcher's.** A watch that took its
	own baseline when it started would never notice a save that landed between
	the parser reading a file and the watch starting. So the baseline is
	whatever the caller stamped before reading, and a watch (new or re-pointed)
	compares it with the disk straight away.

	**One thread, re-pointed as data.** The reload callback is
	Initializer::init(), which re-points the watcher on every load, and other
	threads re-point it too (a keyboard reload). A caller that joined the
	watcher thread to do so would deadlock whenever that thread was inside the
	callback waiting for a lock the caller holds. So start() on a running
	watcher never joins anything: it stores the new set and signals the wake
	event, and the watcher thread swaps its own handles. Only stop() joins, and
	the callback must never call it.

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
#include <ConfigStamp.h>

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
			// The wait holds the stop event, the wake event, then one handle per
			// directory, and WaitForMultipleObjects takes at most
			// MAXIMUM_WAIT_OBJECTS. Well below it: a configuration spread across
			// more than fifteen directories is not a case worth widening the wait
			// for, and the cap failing loudly in a test beats the wait failing
			// silently on a user's machine.
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
				The wait the watch thread blocks in, as a seam.

				WAIT_FAILED cannot be provoked from outside: closing a handle
				another thread is already blocked on does not reliably return
				the wait, so a test written that way measures a race rather than
				the invariant. What the invariant *is* - that every exit from
				the watch loop leaves watching() false - is worth pinning
				precisely because nothing user-visible reads it yet, so a
				regression would be silent until something did.

				Set before start(); the thread reads it once per wait and never
				writes it.
			*/
			using WaitForObjects = DWORD (WINAPI *)(DWORD, const HANDLE *, BOOL, DWORD);

			void set_wait_for_testing(WaitForObjects wait) noexcept
			{
				_wait = wait ? wait : &::WaitForMultipleObjects;
			}

			/*
				Watch the directories covering `files`, calling `on_changed`
				when one of those files no longer matches its stamp.

				`stamps` is the baseline, parallel to `files`: ConfigStamp.h's
				stamp of each, taken by the reader before it read the file.

				Safe from any thread, including the watcher's own callback, and
				never joins a running watcher. If the thread is running this only
				records the new set and wakes it; `on_changed` is the one given
				when the thread was started and is not replaced. If it is not,
				a fresh watch is set up. The second case is serialised against
				stop() and itself, the first deliberately is not - it is reached
				from the callback while stop() may be joining.
			*/
			bool start(const std::vector<std::wstring> &files,
					   const std::vector<uint64_t> &stamps, Callback on_changed)
			{
				if(!on_changed || files.empty() || stamps.size() != files.size())
					return false;

				if(repoint(files, stamps))
					return true;

				std::lock_guard<std::mutex> control(_control_mutex);

				// Another caller may have started it while this one waited.
				if(repoint(files, stamps))
					return true;

				// Not running: the thread, if there was one, has exited and this
				// returns at once. Releases whatever it left behind.
				teardown();

				auto directories = config_watch_directories(files, MAX_DIRECTORIES);
				if(directories.empty())
					return false;

				// Manual-reset: once set it stays set, whichever wait sees it.
				HANDLE stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
				if(!stop_event)
					return false;

				// Auto-reset: one wake per request, and a request made while the
				// thread is busy in its callback is not lost -
				// "If no threads are waiting, the event object's state remains
				// signaled."
				// https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventw
				HANDLE wake_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
				if(!wake_event)
				{
					::CloseHandle(stop_event);
					return false;
				}

				std::vector<HANDLE> handles{ stop_event, wake_event };
				open_directories(directories, handles);

				if(handles.size() <= FIRST_DIRECTORY)
				{
					::CloseHandle(stop_event);
					::CloseHandle(wake_event);
					return false;
				}

				_stop = stop_event;
				{
					std::lock_guard<std::mutex> lock(_pending_mutex);
					_wake = wake_event;
					_repoint = false;
				}

				_watched = files;
				_stamps = stamps;
				_handles = std::move(handles);
				_callback = on_changed;
				_running.store(true, std::memory_order_release);

				_thread = std::thread(&ConfigWatcher::run, this);
				return true;
			}

			/*
				Stop watching and join the thread.

				Never from the reload callback: the callback runs on the thread
				being joined. Called there, this only signals the stop and
				returns; the handles are released by the next stop() or start().
			*/
			void stop()
			{
				if(_stop && _thread.get_id() == std::this_thread::get_id())
				{
					::SetEvent(_stop);
					return;
				}

				std::lock_guard<std::mutex> control(_control_mutex);
				teardown();
			}

			// Reloads this watcher has actually triggered, for reporting and
			// for tests that must wait for one rather than sleep for one.
			uint64_t reloads() const noexcept
			{
				return _reloads.load(std::memory_order_acquire);
			}

		private:
			// Positions in the wait. The stop event is first so that, when
			// several handles are signalled, it wins: "the function returns the
			// index of the first handle in the array whose object was signaled".
			// https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects
			static constexpr DWORD STOP_SLOT = 0;
			static constexpr DWORD WAKE_SLOT = 1;
			static constexpr DWORD FIRST_DIRECTORY = 2;

			static std::vector<uint64_t> write_times(const std::vector<std::wstring> &files)
			{
				std::vector<uint64_t> stamps;
				stamps.reserve(files.size());

				// The same value the parser stamped with. A deleted file stamps
				// as zero, which differs from whatever it stamped as before - so
				// losing an import counts as a change, which is what it is.
				for(const auto &file : files)
					stamps.push_back(config_file_stamp(file));

				return stamps;
			}

			// One unwatchable directory - a removable drive, a share that does
			// not notify - is not a reason to watch none of the others.
			static void open_directories(const std::vector<std::wstring> &directories,
										 std::vector<HANDLE> &handles)
			{
				for(const auto &directory : directories)
				{
					auto handle = ::FindFirstChangeNotificationW(
						directory.c_str(), FALSE,
						FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);

					if(handle != INVALID_HANDLE_VALUE)
						handles.push_back(handle);
				}
			}

			/*
				Hand a running watcher a new set. False when it is not running,
				which the caller answers by starting one.

				Takes only _pending_mutex, never _control_mutex, and joins
				nothing: the reload callback gets here on the watcher's own
				thread, possibly while another thread holds _control_mutex in
				stop() waiting for exactly that thread. The wake handle is only
				touched under the same mutex that closes it.
			*/
			bool repoint(const std::vector<std::wstring> &files,
						 const std::vector<uint64_t> &stamps)
			{
				if(!_running.load(std::memory_order_acquire))
					return false;

				{
					std::lock_guard<std::mutex> lock(_pending_mutex);
					if(!_wake)
						return false;

					_pending_files = files;
					_pending_stamps = stamps;
					_repoint = true;
					::SetEvent(_wake);
				}

				// The thread may have exited between the first check and the
				// signal, and then nothing will ever read the request.
				return _running.load(std::memory_order_acquire);
			}

			// Under _control_mutex. Joins the thread (unless that is this one),
			// then releases everything. The handles are closed only after the
			// join: closing one a wait is still using is undefined.
			// https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects
			void teardown()
			{
				if(_stop)
					::SetEvent(_stop);

				if(_thread.joinable() && _thread.get_id() != std::this_thread::get_id())
					_thread.join();

				_running.store(false, std::memory_order_release);

				// The stop and wake events are the first two handles; the rest
				// came from FindFirstChangeNotification and are closed by its own
				// function.
				for(size_t i = FIRST_DIRECTORY; i < _handles.size(); i++)
					::FindCloseChangeNotification(_handles[i]);
				_handles.clear();

				if(_stop)
				{
					::CloseHandle(_stop);
					_stop = nullptr;
				}

				{
					std::lock_guard<std::mutex> lock(_pending_mutex);
					if(_wake)
					{
						::CloseHandle(_wake);
						_wake = nullptr;
					}

					// A re-point the thread never got to is dropped rather than
					// carried into the next watch, which has its own set.
					_repoint = false;
					_pending_files.clear();
					_pending_stamps.clear();
				}

				_watched.clear();
				_stamps.clear();
				_callback = nullptr;
			}

			/*
				The watch thread. `_running` is what `watching()` answers, and
				it belongs to the thread rather than to the call that started
				it: every way out of watch_loop() below - a failed
				WaitForMultipleObjects, an out-of-range index, a failed
				FindNextChangeNotification, or a re-point that found nothing
				watchable - means this thread has gone and the configuration is
				no longer being watched.
			*/
			void run()
			{
				watch_loop();
				_running.store(false, std::memory_order_release);
			}

			// Stop, a failed wait, or an index outside the handle range: none of
			// them is something to spin on. WAIT_TIMEOUT is the caller's to test
			// first where a timeout is expected.
			bool ends_watch(DWORD index) const
			{
				return index == WAIT_OBJECT_0 + STOP_SLOT ||
					   index >= WAIT_OBJECT_0 + _handles.size();
			}

			void watch_loop()
			{
				// The first pass compares the baseline with the disk without
				// waiting: a save that landed after the caller stamped its files
				// and before this thread existed has no notification to wake on.
				bool due = true;

				for(;;)
				{
					if(!due)
					{
						auto index = _wait(static_cast<DWORD>(_handles.size()),
										   _handles.data(), FALSE, INFINITE);

						if(ends_watch(index))
							return;

						/*
							Debounce, for directory notifications. An editor
							saving a file commonly produces several, and every
							directory in the set can report the same save. Keep
							re-arming and re-waiting with a short timeout until
							things go quiet, so one save is one reload. A wake
							needs no debounce: it is a request, not a burst.
						*/
						if(index != WAIT_OBJECT_0 + WAKE_SLOT)
						{
							for(;;)
							{
								auto woken = index - WAIT_OBJECT_0;
								if(woken >= FIRST_DIRECTORY && !::FindNextChangeNotification(_handles[woken]))
									return;

								index = _wait(static_cast<DWORD>(_handles.size()),
											  _handles.data(), FALSE, DEBOUNCE_MS);

								if(index == WAIT_TIMEOUT)
									break;

								if(ends_watch(index))
									return;
							}
						}
					}
					due = false;

					// A request from start(), or the wake that announced it.
					// Nothing watchable in the new set ends the watch: stopping is
					// the honest outcome and the keyboard reload combos remain.
					if(!apply_repoint())
						return;

					// The wakeup said "something in one of these directories".
					// Only the files that were actually loaded matter.
					auto stamps = write_times(_watched);
					if(stamps == _stamps)
						continue;

					_stamps = std::move(stamps);

					if(_callback)
						_callback();

					// The callback re-points the watcher at whatever the reload
					// turned out to load.
					if(!apply_repoint())
						return;

					// Published after the callback and after the re-point, so a
					// test that waits for this to change is waiting for the
					// reload to have happened rather than for it to have
					// started.
					_reloads.fetch_add(1, std::memory_order_release);

					// The re-point's baseline can already be behind the disk.
					due = true;
				}
			}

			// On the watcher's thread. False only when a pending set could not
			// be watched at all.
			bool apply_repoint()
			{
				std::vector<std::wstring> files;
				std::vector<uint64_t> stamps;
				{
					std::lock_guard<std::mutex> lock(_pending_mutex);
					if(!_repoint)
						return true;

					_repoint = false;
					files.swap(_pending_files);
					stamps.swap(_pending_stamps);
				}

				return rearm(files, stamps);
			}

			/*
				Exchange the notification handles for ones covering `files`,
				from the watcher's own thread.

				The new handles are opened before the old ones are closed, so a
				failure part-way leaves the watcher watching what it already
				was rather than watching nothing.
			*/
			bool rearm(const std::vector<std::wstring> &files,
					   const std::vector<uint64_t> &stamps)
			{
				if(files.empty() || stamps.size() != files.size())
					return false;

				auto directories = config_watch_directories(files, MAX_DIRECTORIES);
				if(directories.empty())
					return false;

				std::vector<HANDLE> handles{ _handles[STOP_SLOT], _handles[WAKE_SLOT] };
				open_directories(directories, handles);

				if(handles.size() <= FIRST_DIRECTORY)
					return false;

				// The stop and wake events are shared with the new set, so they
				// are deliberately not closed here.
				for(size_t i = FIRST_DIRECTORY; i < _handles.size(); i++)
					::FindCloseChangeNotification(_handles[i]);

				_watched = files;
				_stamps = stamps;
				_handles = std::move(handles);
				return true;
			}

			// Serialises a fresh start against stop() and against other fresh
			// starts. Never taken by the running-thread path.
			std::mutex _control_mutex;

			std::thread _thread;
			HANDLE _stop{};
			std::vector<HANDLE> _handles;
			std::vector<std::wstring> _watched;
			std::vector<uint64_t> _stamps;
			Callback _callback{};
			std::atomic<bool> _running{};
			WaitForObjects _wait = &::WaitForMultipleObjects;
			std::atomic<uint64_t> _reloads{};

			// A re-point asked for while the thread runs, applied by the thread.
			// _wake and _repoint belong to _pending_mutex, which is also what
			// makes closing the wake handle safe against a concurrent start().
			std::mutex _pending_mutex;
			HANDLE _wake{};
			std::vector<std::wstring> _pending_files;
			std::vector<uint64_t> _pending_stamps;
			bool _repoint{};
		};
	}
}
