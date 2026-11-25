#pragma once

/*
	The stack of popup windows a menu session currently has open, and the
	lifecycle rules that keep it honest.

	docs/refactor/01-takeover-contract.md section 6 asked for a six-state
	machine here - Unknown, Created, Prepared, Visible, Closing, Dead - on the
	grounds that SetWinEventHook "documents that callbacks can reenter and
	complete out of order":

	  "While a hook function processes an event, additional events may be
	   triggered, which may cause the hook function to reenter before the
	   processing for the original event is finished. The problem with
	   reentrancy in hook functions is that events are completed out of
	   sequence unless the hook function handles this situation."
	  https://learn.microsoft.com/en-us/windows/win32/winauto/guarding-against-reentrancy-in-hook-functions

	That hazard is real and documented. What was never established is which
	shape of it this code is actually exposed to, so it was measured before
	anything was built. Probe run 2026-08-24, Windows 11 26200.8875 x64,
	a four-level cascade driven by posted keystrokes, reproducing the calls
	OnMenuCreate makes that can pump a message queue (SetWindowLongPtr,
	SetClassLongPtr, DwmSetWindowAttribute, PostMessage):

	  | shape                                   | escape | select | siblings |
	  | EVENT_OBJECT_CREATE twice for one HWND  |   0    |   0    |    0     |
	  | EVENT_OBJECT_SHOW inside a CREATE       |   no   |   no   |    no    |
	  | pop_back() removed the closing window   |  yes   |  yes   |   yes    |

	"siblings" walks across three sibling submenus, which interleaves creation
	and destruction (create B, destroy B, create C, ...) and is the shape most
	likely to desynchronise a positional stack. It did not.

	So the six-state machine is *not* built: there is no measured defect behind
	it, and inventing states nothing drives would be scope with no evidence.
	What is built is the part that is unsound whether or not the events
	misbehave, which is the reason this file exists at all:

	  ContextMenu::MenuSubClassProc's WM_NCDESTROY used to erase the WND from
	  the map by handle and then pop the *last* entry off the level stack. Those
	  are two different keys for the same removal. While they agree the code is
	  fine; the first time they disagree the stack keeps a pointer to a WND that
	  the map has already destroyed, and the very next popup dereferences it at
	  `_level[_level.size() - 2]->x`. Nothing would report it - it reads as a
	  misplaced submenu, or a crash somewhere else entirely.

	Removing by handle costs the same as popping by position for a stack this
	size (never more than a handful of entries) and makes the disagreement
	impossible rather than merely unobserved. The duplicate-CREATE and
	unknown-window guards are here for the same reason: cheap, and the
	documentation says the events may do it even though this machine did not.
*/

#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		// What the caller should do about a lifecycle event. Returned rather
		// than acted upon so the decision is pure and testable, and so the
		// caller keeps ownership of the Win32 work.
		enum class PopupAction : uint8_t
		{
			// Not ours, already known, or arrived after the window went away.
			Ignore,
			// First CREATE for this window: prepare and subclass it.
			Track,
			// First SHOW for a window we are tracking: reveal its layers.
			Show,
			// The window we were tracking is going away: tear its state down.
			Release,
		};

		/*
			An ordered stack of the popups a session has open, each carrying the
			caller's own per-window payload.

			Order matters: a submenu's placement is computed against the popup
			that opened it, which is the entry below it. Entries are therefore
			appended on create and removed *by handle* on destroy - which for a
			well-behaved cascade is the last entry, and for a misbehaving one is
			still the right one.
		*/
		template<class T>
		class PopupStack
		{
			struct Entry
			{
				HWND handle{};
				T *payload{};
				bool shown{};
			};

			std::vector<Entry> _open;

		public:
			static constexpr size_t npos = static_cast<size_t>(-1);

			size_t size() const noexcept { return _open.size(); }
			bool empty() const noexcept { return _open.empty(); }

			size_t index_of(HWND handle) const noexcept
			{
				for(size_t i = 0; i < _open.size(); i++)
					if(_open[i].handle == handle)
						return i;
				return npos;
			}

			bool contains(HWND handle) const noexcept { return index_of(handle) != npos; }

			T *at(size_t i) const noexcept { return i < _open.size() ? _open[i].payload : nullptr; }
			T *front() const noexcept { return _open.empty() ? nullptr : _open.front().payload; }
			T *back() const noexcept { return _open.empty() ? nullptr : _open.back().payload; }

			// The popup that opened the one on top, which is what a submenu is
			// positioned against. Null at the root, where there is no such thing -
			// the caller used to express this as `size() == 1`, which said the
			// same thing in a way that only worked while the stack was exact.
			T *parent_of_top() const noexcept
			{
				return _open.size() >= 2 ? _open[_open.size() - 2].payload : nullptr;
			}

			// Iteration is over payloads, oldest first: the screenshot path
			// composites root-to-leaf and depends on that order.
			template<class F>
			void for_each(F &&fn) const
			{
				for(auto &e : _open)
					if(e.payload)
						fn(e.payload);
			}

			/*
				EVENT_OBJECT_CREATE.

				A second CREATE for a window already on the stack answers Ignore
				rather than pushing again. A duplicate push would leave the stack
				one entry too deep: the root would stop answering "I am the root",
				so it would be positioned against itself, and the single removal
				that eventually arrives would leave a dangling entry behind.
			*/
			PopupAction on_create(HWND handle, T *payload)
			{
				if(handle == nullptr || contains(handle))
					return PopupAction::Ignore;

				_open.push_back({ handle, payload, false });
				return PopupAction::Track;
			}

			/*
				EVENT_OBJECT_SHOW.

				Only for a window this session is tracking, and only once. A SHOW
				for a window that was never created here belongs to somebody
				else's menu; a second SHOW would re-run work that is already done.
			*/
			PopupAction on_show(HWND handle)
			{
				auto i = index_of(handle);
				if(i == npos || _open[i].shown)
					return PopupAction::Ignore;

				_open[i].shown = true;
				return PopupAction::Show;
			}

			/*
				WM_NCDESTROY.

				Removes the entry for *this* window wherever it sits, and answers
				Ignore for one that was never tracked - so a stray destroy cannot
				pop an unrelated popup off the stack.
			*/
			PopupAction on_destroy(HWND handle)
			{
				auto i = index_of(handle);
				if(i == npos)
					return PopupAction::Ignore;

				_open.erase(_open.begin() + static_cast<ptrdiff_t>(i));
				return PopupAction::Release;
			}

			void clear() noexcept { _open.clear(); }
		};
	}
}
