#pragma once

// What Initializer::query() does when a menu is about to be built.
//
// The three-state model from docs/refactor/03-config-safety.md section 2:
//
//   Loaded          newest configuration valid            -> serve it
//   StaleWithError  newest attempt failed, a previously
//                   published generation is still live    -> serve that
//   Disabled        explicit user disable                 -> refuse
//
// Before this existed, a failed parse set Status.Error and query() refused
// every menu while it was set - even though init() builds into a fresh CACHE
// and publishes only on success, so the last good snapshot was still sitting
// in memory, untouched and correct. One typo in shell.nss therefore removed
// the context menu from the whole desktop until the user found the file, fixed
// it, and knew to press Shift+Ctrl+right-click.
//
// Split out as a pure function so the test suite drives the real decision:
// Initializer itself cannot be linked into the test binary, because its
// translation unit's closure is the entire menu engine.

namespace Nilesoft
{
	namespace Shell
	{
		enum class ConfigVerdict
		{
			// A snapshot is available: build the menu from it.
			Serve,

			// Nothing usable in memory, or the user asked for a reload: parse.
			Reparse,

			// Do not build a menu. Either the user disabled Shell, or no
			// configuration has ever loaded in this process and the newest
			// attempt failed - there is genuinely nothing to show.
			Refuse,
		};

		// `disabled`     - Status.Disabled: the user turned Shell off.
		// `refresh`      - Status.Refresh: an explicit reload was requested, by
		//                  a key combo or by the NSS reload function. It outranks
		//                  a stale snapshot, which is the only way out of
		//                  StaleWithError without restarting the host.
		// `error`        - Status.Error: the newest parse attempt failed.
		// `has_snapshot` - a generation is published and can be served.
		constexpr ConfigVerdict decide_config_serve(bool disabled, bool refresh,
													bool error, bool has_snapshot)
		{
			if(disabled)
				return ConfigVerdict::Refuse;
			if(refresh)
				return ConfigVerdict::Reparse;
			if(error)
				return has_snapshot ? ConfigVerdict::Serve : ConfigVerdict::Refuse;
			return has_snapshot ? ConfigVerdict::Serve : ConfigVerdict::Reparse;
		}
	}
}
