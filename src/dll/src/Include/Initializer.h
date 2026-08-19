#pragma once
#include "Include/Theme.h"

namespace Nilesoft
{
	namespace Shell
	{
		// A Direct2D/DirectWrite wrapper used to live here, backing an alternative
		// menu renderer. That renderer was unreachable (its only entry point sat
		// behind a hardcoded false), so the wrapper was deleted with it. Removing
		// it also dropped d2d1.dll and dwrite.dll from the import table, which
		// OPT:REF had not been stripping.

		class Initializer
		{
		private:
			uintptr_t _last_write_time{};

		public:
			struct {
				HMODULE	hModule{};
				HANDLE	handle{};
				DWORD	id{};
				string	name;
				string	path;
			} process;

			Application		application;
			// No COM_INITIALIZER member. As a member of a global it initialised
			// COM on the thread that loaded the DLL and then, at teardown, ran
			// CoUninitialize from whichever thread destroys globals, which is
			// not the thread that had incremented the count. COM is now brought
			// up per-thread in Initializer::init.
			bool			is_elevated{};
			CACHE *cache{};
			//Hooker			user32_TrackPopupMenu;
			//Hooker			user32_TrackPopupMenuEx;
			//Hooker			user32u_TrackPopupMenuEx;
			uint32_t		dpi = 96;

			Initializer() { instance = this; };
			~Initializer();

			bool query(int ch = 0);
			bool init(HINSTANCE hInstance);
			bool init();
			static void ensure_com();
			// Clean up resources allocated during initialization.
			bool uninit();

			//reloadOnChange
			//determine
			bool config_has_changed();
			bool has_error(bool detect_changes = false);
			void load_mui();

			struct STATUS { bool Loaded, Disabled, Refresh, Error; } inline static Status{};

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
				return Status.Loaded;
			}

			//static HRESULT Modern(int enabled);
			static bool OnState(bool istaskbar = false);
			static bool is_excluded(HWND hWnd = nullptr);
			static bool check_excluded();

			static void SetSubclass(HWND hWnd);
			inline static std::unordered_map<uint32_t, MUID> MAP_MUID;

			static MUID* get_muid(uint32_t hash)
			{
				for(auto &it : MAP_MUID)
				{
					if(it.first == hash || it.second.id == hash)
						return &it.second;
				}
				return nullptr;
			}

			static uint32_t get_hash(uint32_t id)
			{
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