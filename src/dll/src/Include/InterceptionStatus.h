#pragma once

/*
  Which mechanism is currently carrying this process's popup menus.

  docs/refactor/00-master-plan.md's backlog item 8 and section 01.9 asked for a
  `PopupInterceptionBackend` interface - `install()`, `healthy()`,
  `uninstall()` - with the two mechanisms behind it and a health check on every
  hook entry. Section 01.9c records why that shape was declined and this one
  built instead. The two reasons, in short, because they are the reasons this
  file looks the way it does:

  1. The two mechanisms are NOT interchangeable implementations of one
     contract, so an interface asserting that they are would be a lie the type
     system enforces. The primary patches ONE thunk - user32's own import of
     win32u!NtUserTrackPopupMenuEx - and that covers every caller in the
     process that reaches a popup through user32. The fallback patches EVERY
     loaded module's import of user32!TrackPopupMenu(Ex), one module at a time,
     and the PE format is explicit that this is per-image: "The import
     directory table consists of an array of import directory entries, one
     entry for each DLL to which the image refers"
     (https://learn.microsoft.com/windows/win32/debug/pe-format#the-idata-section).
     So the fallback misses, by construction, every module loaded after
     bootstrap, every call made through GetProcAddress, every delay-loaded
     import, and every module that imports the API set name rather than
     user32 - TrackPopupMenuEx documents itself as living in both, "DLL:
     User32.dll" and "API set: ext-ms-win-ntuser-menu-l1-1-1"
     (https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenuex).

     Which mechanism is active is therefore a *fact worth reporting*, not an
     implementation detail worth hiding. That is what this file is for.

  2. A health check on hook entry cannot fail. `IATHook::installed()` compares
     `_thunk->u1.Function == _detour`; if that were false the thunk would not
     have routed the call here, so the hook body would not be running to ask.
     The check has to happen somewhere the answer can still be no.

  What this can and cannot tell you, stated because the difference matters:

  - It CAN say "the fallback is carrying this process", which means the primary
    failed to install and the coverage gaps above are live. That is the
    actionable case and nothing could report it before.
  - It CANNOT report a host whose interception has been displaced outright,
    because such a host stops publishing sessions altogether. The honest signal
    there is the menu count and the age of the last one, both of which the
    report already prints.
*/

#include <atomic>
#include <cstdint>

#include "PerfExport.h"

namespace Nilesoft
{
	namespace Shell
	{
		namespace Interception
		{
			/*
				The wire constants, renamed into this namespace.

				Not decoration: `Nilesoft::Diagnostics` and
				`Nilesoft::Shell::Diagnostics` both exist, and at global scope
				in Main.cpp - which is where the only caller lives - an
				unqualified `Diagnostics` is ambiguous (C2872). AGENTS.md names
				this trap. Spelling the whole path at each call site would work
				and would read badly; these do the same job once.
			*/
			inline constexpr uint32_t None = Diagnostics::PERF_EXPORT_INTERCEPTION_NONE;
			inline constexpr uint32_t Win32uImport = Diagnostics::PERF_EXPORT_INTERCEPTION_WIN32U;
			inline constexpr uint32_t PerModuleImports = Diagnostics::PERF_EXPORT_INTERCEPTION_PERMODULE;

			/*
				The published state, as one word.

				A word so it can be stored and loaded atomically and handed to
				the export as-is, and relaxed because nothing else is ordered
				against it: a reader that catches the moment it changes sees
				either the old value or the new one, and both are true
				statements about this process.
			*/
			inline std::atomic<uint32_t> &state() noexcept
			{
				static std::atomic<uint32_t> value{ Diagnostics::PERF_EXPORT_INTERCEPTION_NONE };
				return value;
			}

			inline uint32_t current() noexcept
			{
				return state().load(std::memory_order_relaxed);
			}

			// `backend` is one of PERF_EXPORT_INTERCEPTION_*; `healthy` is
			// whatever the cheap thunk compare said at the moment of the call.
			inline void publish(uint32_t backend, bool healthy) noexcept
			{
				auto value = (backend & Diagnostics::PERF_EXPORT_INTERCEPTION_BACKEND_MASK)
					| (healthy ? Diagnostics::PERF_EXPORT_INTERCEPTION_HEALTHY : 0u);
				state().store(value, std::memory_order_relaxed);
			}

			inline uint32_t backend_of(uint32_t value) noexcept
			{
				return value & Diagnostics::PERF_EXPORT_INTERCEPTION_BACKEND_MASK;
			}

			inline bool healthy_of(uint32_t value) noexcept
			{
				return (value & Diagnostics::PERF_EXPORT_INTERCEPTION_HEALTHY) != 0;
			}
		}
	}
}
