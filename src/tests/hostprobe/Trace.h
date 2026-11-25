#pragma once

/*
	What a host actually observes while a menu is up.

	This records the message stream a menu's owner window receives, decoded into
	named fields and with every handle replaced by a stable alias, so two runs of
	the same scenario produce byte-identical text and can be diffed.

	That is the whole point. docs/refactor/01-takeover-contract.md proposes that
	Shell reproduce the host-observable sequence of an untouched TrackPopupMenu
	while substituting its own menu, and notes that "static analysis alone is not
	evidence here". A trace is the evidence: record what Windows does, record
	what Shell does, and diff.

	Normalisation rules, and why each exists:

	  - HMENU and HWND values differ on every run, so they are replaced by
	    aliases assigned in first-seen order: menu#1, menu#2, owner. An alias
	    also *carries information* a raw handle does not - "the UNINIT names the
	    same menu as the INIT" is visible at a glance.
	  - Times are not recorded at all. Ordering is the contract; duration is not.
	  - Pointers passed by message (MEASUREITEMSTRUCT, DRAWITEMSTRUCT) are read
	    at record time and reduced to the fields that identify the item. The
	    pointer is only valid for the duration of the call.
*/

#include <windows.h>
#include <string>
#include <vector>

namespace hostprobe
{
	// One observed message, already decoded. Kept as text because that is what
	// gets compared; the raw values stay alongside for a caller that wants to
	// assert on them directly.
	struct Event
	{
		UINT message{};
		WPARAM wparam{};
		LPARAM lparam{};
		std::wstring text;			// the normalised, diffable rendering
		bool has_reply{};
		LRESULT reply{};			// what the probe returned, where it answers
	};

	class Trace
	{
	public:
		void clear()
		{
			_events.clear();
			_menus.clear();
			_windows.clear();
		}

		const std::vector<Event> &events() const { return _events; }

		// Stable alias for a handle, assigned in first-seen order. Null keeps
		// its own spelling: "the menu was destroyed" and "menu#1" are different
		// observations and must not render the same.
		std::wstring menu_alias(HMENU h)
		{
			if(!h)
				return L"null";
			return alias(_menus, reinterpret_cast<void *>(h), L"menu#");
		}

		std::wstring window_alias(HWND h)
		{
			if(!h)
				return L"null";
			return alias(_windows, reinterpret_cast<void *>(h), L"window#");
		}

		void add(UINT message, WPARAM wparam, LPARAM lparam, std::wstring text)
		{
			Event e;
			e.message = message;
			e.wparam = wparam;
			e.lparam = lparam;
			e.text = std::move(text);
			_events.push_back(std::move(e));
		}

		void set_reply(LRESULT reply)
		{
			if(_events.empty())
				return;
			_events.back().has_reply = true;
			_events.back().reply = reply;
		}

		// The whole trace as one block of text, one event per line.
		//
		// Consecutive identical lines collapse to one. An owner-drawn item is
		// redrawn on every hover and repaint, so the *number* of WM_DRAWITEM
		// messages is a function of how the compositor felt rather than of the
		// contract; the fact that one arrived, and where in the order, is the
		// observation worth diffing. Distinct lines are never merged, so a
		// selection moving between items is fully preserved.
		std::wstring render(bool collapse_repeats = true) const
		{
			std::wstring out;
			const std::wstring *previous = nullptr;
			for(auto &e : _events)
			{
				if(collapse_repeats && previous && *previous == e.text && !e.has_reply)
					continue;
				previous = &e.text;

				out += e.text;
				if(e.has_reply)
				{
					wchar_t buf[64];
					::swprintf_s(buf, L"  -> 0x%08lX", static_cast<unsigned long>(e.reply));
					out += buf;
				}
				out += L'\n';
			}
			return out;
		}

		size_t count(UINT message) const
		{
			size_t n = 0;
			for(auto &e : _events)
				if(e.message == message)
					n++;
			return n;
		}

		// First recorded value of wParam for a message, for the assertions that
		// only need one field.
		bool first(UINT message, Event *out) const
		{
			for(auto &e : _events)
			{
				if(e.message == message)
				{
					if(out)
						*out = e;
					return true;
				}
			}
			return false;
		}

	private:
		static std::wstring alias(std::vector<void *> &seen, void *h, const wchar_t *prefix)
		{
			size_t index = 0;
			for(; index < seen.size(); index++)
			{
				if(seen[index] == h)
					break;
			}
			if(index == seen.size())
				seen.push_back(h);

			wchar_t buf[32];
			::swprintf_s(buf, L"%s%zu", prefix, index + 1);
			return buf;
		}

		std::vector<Event> _events;
		std::vector<void *> _menus;
		std::vector<void *> _windows;
	};

	// Message names. Only the ones a menu's owner can see; anything else is not
	// recorded, so an unrelated message cannot make two runs differ.
	inline const wchar_t *message_name(UINT message)
	{
		switch(message)
		{
		case WM_ENTERMENULOOP:   return L"WM_ENTERMENULOOP";
		case WM_EXITMENULOOP:    return L"WM_EXITMENULOOP";
		case WM_INITMENU:        return L"WM_INITMENU";
		case WM_INITMENUPOPUP:   return L"WM_INITMENUPOPUP";
		case WM_UNINITMENUPOPUP: return L"WM_UNINITMENUPOPUP";
		case WM_MENUSELECT:      return L"WM_MENUSELECT";
		case WM_MENUCHAR:        return L"WM_MENUCHAR";
		case WM_MEASUREITEM:     return L"WM_MEASUREITEM";
		case WM_DRAWITEM:        return L"WM_DRAWITEM";
		case WM_COMMAND:         return L"WM_COMMAND";
		case WM_MENUCOMMAND:     return L"WM_MENUCOMMAND";
		case WM_MENURBUTTONUP:   return L"WM_MENURBUTTONUP";
		case WM_NEXTMENU:        return L"WM_NEXTMENU";
		}
		return nullptr;
	}

	// The MF_* set WM_MENUSELECT reports in the high word, rendered as names.
	// https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menuselect
	inline std::wstring menu_flags(UINT flags)
	{
		struct { UINT bit; const wchar_t *name; } known[] = {
			{ MF_GRAYED,      L"GRAYED" },
			{ MF_DISABLED,    L"DISABLED" },
			{ MF_BITMAP,      L"BITMAP" },
			{ MF_CHECKED,     L"CHECKED" },
			{ MF_POPUP,       L"POPUP" },
			{ MF_HILITE,      L"HILITE" },
			{ MF_OWNERDRAW,   L"OWNERDRAW" },
			{ MF_SYSMENU,     L"SYSMENU" },
			{ MF_MOUSESELECT, L"MOUSESELECT" },
		};

		// 0xFFFF with a null menu is the documented "the system has closed the
		// menu" notification, not a flag combination.
		if(flags == 0xFFFF)
			return L"CLOSED";

		std::wstring out;
		UINT remaining = flags;
		for(auto &k : known)
		{
			// MF_GRAYED is 1 and MF_DISABLED is 2, so test the exact bits.
			if((flags & k.bit) == k.bit && k.bit != 0)
			{
				if(!out.empty())
					out += L'|';
				out += k.name;
				remaining &= ~k.bit;
			}
		}
		if(remaining)
		{
			wchar_t buf[32];
			::swprintf_s(buf, L"%s0x%04X", out.empty() ? L"" : L"|", remaining);
			out += buf;
		}
		return out.empty() ? L"none" : out;
	}
}
