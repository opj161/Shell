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
			std::mutex _cache_mutex;
			std::shared_ptr<CACHE> _cache_snapshot;

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
			CACHE			*cache{};
			std::atomic<uint32_t> dpi{ 96 };

			Initializer() { instance = this; }
			~Initializer();

			bool query(int ch = 0);
			bool init(HINSTANCE hInstance);
			bool init();
			bool uninit();

			bool config_has_changed();
			bool has_error(bool detect_changes = false);
			void load_mui();

			std::shared_ptr<const CACHE> get_cache()
			{
				std::lock_guard<std::mutex> lock(_cache_mutex);
				return _cache_snapshot;
			}

			std::shared_ptr<CACHE> get_mutable_cache()
			{
				std::lock_guard<std::mutex> lock(_cache_mutex);
				return _cache_snapshot;
			}

			CACHE *get_raw_cache()
			{
				std::lock_guard<std::mutex> lock(_cache_mutex);
				return _cache_snapshot.get();
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
			} inline static Status{};

			struct LASTERROR 
			{ 
				TokenError code{};
				uint32_t line, col{};
			} inline static LastError{};

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
			inline static std::mutex MUTEX_MUID;
			inline static std::unordered_map<uint32_t, MUID> MAP_MUID;

			static MUID* get_muid(uint32_t hash)
			{
				std::lock_guard<std::mutex> lock(MUTEX_MUID);
				for(auto &it : MAP_MUID)
				{
					if(it.first == hash || it.second.id == hash)
						return &it.second;
				}
				return nullptr;
			}

			static uint32_t get_hash(uint32_t id)
			{
				std::lock_guard<std::mutex> lock(MUTEX_MUID);
				for(auto &it : MAP_MUID)
				{
					if(it.first == id)
						return it.second.id;
				}
				return 0;
			}
		};
	}
}