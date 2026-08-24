#pragma once
#include "Include/ConfigLifecycle.h"
#include "Include/Theme.h"
#include "Include/TakeoverGesture.h"
#include "Include/ConfigWatcher.h"
#include "Include/ComActivationPolicy.h"
#include <ConfigCheck.h>
#include <mutex>
#include <memory>
#include <atomic>

namespace Nilesoft
{
	namespace Shell
	{
		// The compiled COM activation policy for this process, or null before
		// the first configuration has been published. Read by
		// CoCreateInstanceHook on every activation, so it is a lock-free
		// shared_ptr copy rather than anything that touches the config snapshot
		// mutex. docs/refactor/01-takeover-contract.md section 9.
		std::shared_ptr<const ComActivationPolicy> current_com_policy();

		class Initializer
		{
		private:
			uintptr_t _last_write_time{};
			mutable std::mutex _snapshot_mutex;
			std::mutex _reload_mutex;
			std::shared_ptr<const CACHE> _snapshot;
			std::atomic<uint64_t> _generation{ 0 };

		public:
			struct {
				HMODULE	hModule{};
				HANDLE	handle{};
				DWORD	id{};
				string	name;
				string	path;
			} process;

			Application		application;
			bool			is_elevated{};
			std::atomic<uint32_t> dpi{ 96 };

			Initializer() { instance = this; }
			~Initializer();

			bool query();
			bool init(HINSTANCE hInstance);
			bool init();
			bool uninit();

		private:
			// One parse into one new generation, published only on success.
			// Null path means the real configuration; non-null is the
			// last-known-good shadow. See Initializer.cpp.
			bool load_generation(const string *config_path);

			// Everything a Parser needs before Load() can be called: the CACHE
			// it builds into, and the variable maps its context points at.
			// Shared by the publishing path and by check(), which builds the
			// same thing and then throws it away.
			std::unique_ptr<Parser> prepare_parser(const string *config_path, CACHE *cache);

			// Point the process's watcher at the set that was just loaded.
			// docs/refactor/03-config-safety.md section 3.
			static void start_watching(const std::vector<std::wstring> &files);

			// What the watcher calls when one of those files is written.
			static void on_config_file_changed();

			// Compile the COM activation policy from a published generation.
			// docs/refactor/01-takeover-contract.md section 9.
			static void compile_com_policy(const CACHE *cache);

		public:

			// Parse and report; publish nothing, touch no generation, write no
			// shadow. This is what `shell.exe -check` is, and it is the reason
			// prepare_parser exists as its own step.
			// docs/refactor/03-config-safety.md section 1b
			int check(const wchar_t *config_path, ConfigCheckResult &result);


			bool config_has_changed();

			// "There is no configuration this process can serve." A failed parse
			// alone is not that: init() builds into a fresh CACHE and publishes
			// only on success, so the previous generation is still live and still
			// correct. See Include/ConfigLifecycle.h.
			bool has_error(bool detect_changes = false);

			bool has_snapshot() const
			{
				std::lock_guard<std::mutex> lock(_snapshot_mutex);
				return _snapshot != nullptr;
			}
			void load_mui(CACHE *new_cache);

			std::shared_ptr<const CACHE> acquire_snapshot() const
			{
				std::lock_guard<std::mutex> lock(_snapshot_mutex);
				return _snapshot;
			}

			std::shared_ptr<const CACHE> get_cache() const
			{
				return acquire_snapshot();
			}

			uint64_t current_generation() const
			{
				return _generation.load(std::memory_order_relaxed);
			}

			struct STATUS {
				std::atomic<bool> Loaded{ false };
				std::atomic<bool> Disabled{ false };
				std::atomic<bool> Refresh{ false };
				std::atomic<bool> Error{ false };

				// The published generation is older than the file on disk,
				// because the newest parse of that file failed. Menus keep
				// working; this is what a UI would report and what tells the
				// error surface to speak once rather than on every menu.
				std::atomic<bool> Stale{ false };

				STATUS() = default;
				STATUS(const STATUS &other)
					: Loaded(other.Loaded.load()), Disabled(other.Disabled.load()), Refresh(other.Refresh.load()),
					  Error(other.Error.load()), Stale(other.Stale.load()) {}
				STATUS &operator=(const STATUS &other)
				{
					if(this != &other)
					{
						Loaded.store(other.Loaded.load());
						Disabled.store(other.Disabled.load());
						Refresh.store(other.Refresh.load());
						Error.store(other.Error.load());
						Stale.store(other.Stale.load());
					}
					return *this;
				}
			};
			inline static STATUS Status{};

			struct LASTERROR 
			{ 
				TokenError code{};
				uint32_t line, col{};
			};
			inline static LASTERROR LastError{};

		public:
			inline static Initializer *instance{};
			inline static HINSTANCE HInstance{};

			static bool Inited()
			{
				return Status.Loaded.load(std::memory_order_relaxed);
			}

			// Reads the keyboard, classifies, and acts.
			static bool OnState(bool istaskbar = false);

			// The same, on a gesture somebody else has already classified. The
			// popup hook uses this so its own bypass check and this cannot
			// disagree about which keys were down.
			// See Include/TakeoverGesture.h.
			static bool OnState(Gesture gesture);

			// One snapshot of the keyboard, with the buttons that caused the
			// click discounted.
			static GestureState read_gesture(bool istaskbar);

			// read_gesture + classify_gesture: what this click means, decided
			// once so that every consumer of it agrees.
			static Gesture classify_click(bool istaskbar)
			{
				return classify_gesture(read_gesture(istaskbar));
			}
			static bool is_excluded(HWND hWnd = nullptr);
			static bool check_excluded();

			static void SetSubclass(HWND hWnd);
		};
	}
}