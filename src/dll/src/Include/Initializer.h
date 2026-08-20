#pragma once
#include "Include/Theme.h"
#include <mutex>
#include <memory>
#include <atomic>

namespace Nilesoft
{
	namespace Shell
	{
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

			bool query(int ch = 0);
			bool init(HINSTANCE hInstance);
			bool init();
			bool uninit();

			bool config_has_changed();
			bool has_error(bool detect_changes = false);
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

				STATUS() = default;
				STATUS(const STATUS &other)
					: Loaded(other.Loaded.load()), Disabled(other.Disabled.load()), Refresh(other.Refresh.load()), Error(other.Error.load()) {}
				STATUS &operator=(const STATUS &other)
				{
					if(this != &other)
					{
						Loaded.store(other.Loaded.load());
						Disabled.store(other.Disabled.load());
						Refresh.store(other.Refresh.load());
						Error.store(other.Error.load());
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

			static bool OnState(bool istaskbar = false);
			static bool is_excluded(HWND hWnd = nullptr);
			static bool check_excluded();

			static void SetSubclass(HWND hWnd);
		};
	}
}