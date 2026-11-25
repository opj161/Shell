#pragma once

/*
	Typing a word in the menu, not just a letter.

	docs/refactor/05-capabilities.md section 4, Stage 2. Stage 1 answered
	WM_MENUCHAR with Windows' own mnemonic rules (Include/Mnemonics.h), which
	made "E&dit with Adobe Acrobat" reachable by pressing D. That covers the
	items whose titles happen to declare a mnemonic. It does nothing for the
	ones that do not - which, in a menu built from packaged verbs and NSS rules,
	is most of them - and nothing for a menu long enough that the user knows the
	name of what they want but not which letter it hid under.

	So: buffer the characters, and select the first item whose visible label
	starts with what has been typed. One second of idle time and the buffer
	clears, which is the same rule Explorer's own list views use.

	Three decisions, none of them forced by documentation:

	**A prefix match selects; it does not execute.** A unique *mnemonic*
	executes, because that is what Windows does and what makes a mnemonic a
	shortcut. A unique prefix must not: menus contain Delete, and somebody
	typing "de" to look for "Deselect" would have executed it before they saw
	it. Type-ahead moves the highlight; Enter is what chooses.

	**Mnemonics keep precedence on the first character.** A single keypress is
	tried as a mnemonic first and falls through to a prefix only when no
	mnemonic matched, so nothing Stage 1 shipped changes behaviour.

	**A character that matches nothing is not added to the buffer.** Otherwise
	one typo poisons the buffer for a whole second and every subsequent
	keystroke matches nothing either. Rejecting it leaves the last good prefix
	in place, so the next character continues the word the user was typing.

	Deliberately free of the menu engine and of the clock, so the state machine
	can be driven directly by a test.
*/

#include <windows.h>
#include <cstddef>
#include <cstdint>

#include "Include/Mnemonics.h"

namespace Nilesoft
{
	namespace Shell
	{
		/*
			Does `title` display a label beginning with `prefix`?

			Compared against the title as it is stored rather than against a
			label built from it, so nothing is allocated on a keystroke. Two
			things have to be skipped as it walks:

			  - mnemonic markers. "&Open" displays as "Open", so a single "&"
			    contributes nothing and "&&" contributes one literal ampersand.
			  - the accelerator column. "Rename\tF2" displays "Rename"; matching
			    into the F2 would let a user type a shortcut key and land
			    somewhere they did not name.

			Case folding goes through the user's locale, the same as mnemonic
			matching, because these are labels a person reads rather than
			identifiers.
		*/
		inline bool title_starts_with(const wchar_t *title, const wchar_t *prefix,
									  size_t prefix_length) noexcept
		{
			if(!title || !prefix || prefix_length == 0)
				return false;

			size_t matched = 0;
			for(size_t i = 0; title[i] && matched < prefix_length; i++)
			{
				auto c = title[i];

				if(c == L'\t' || c == L'\b')
					return false;			// the label ended before the prefix did

				if(c == L'&')
				{
					if(title[i + 1] == L'&')
						i++;				// a literal ampersand: compare the second
					else
						continue;			// a marker: displays as nothing
					c = title[i];
				}

				if(upper_char(c) != upper_char(prefix[matched]))
					return false;

				matched++;
			}

			return matched == prefix_length;
		}

		// One item as it appears in the popup. Mirrors MnemonicItem, plus the
		// title, because a prefix has to be matched against the whole label
		// rather than against one marked character.
		struct TypeAheadItem
		{
			int position{};
			const wchar_t *title{};
			bool selectable{ true };
		};

		/*
			The typed prefix, and when it stops being current.

			Fixed storage: a menu label nobody would type past, and no
			allocation on a keystroke. A prefix longer than the buffer stops
			growing rather than wrapping or truncating from the front, because
			by then it has almost certainly already selected what the user
			wanted.
		*/
		class TypeAheadBuffer
		{
		public:
			// docs/refactor/05-capabilities.md section 4: "buffer chars for 1 s".
			static constexpr uint64_t TIMEOUT_MS = 1000;
			static constexpr size_t CAPACITY = 32;

			size_t length() const noexcept { return _length; }
			const wchar_t *text() const noexcept { return _text; }
			bool empty() const noexcept { return _length == 0; }

			void clear() noexcept
			{
				_length = 0;
				_text[0] = L'\0';
				_popup = nullptr;
			}

			/*
				Drop the buffer if it has gone stale, or if the popup under the
				cursor is not the one it was typed into.

				The second rule matters as much as the first: a submenu opening
				is a new list, and carrying "de" into it would select something
				the user never typed towards.
			*/
			void refresh(uint64_t now, const void *popup) noexcept
			{
				if(_length == 0)
					return;

				if(popup != _popup || now < _typed_at || (now - _typed_at) > TIMEOUT_MS)
					clear();
			}

			// Speculatively append, without committing. The caller commits only
			// if the longer prefix actually matches something - see accept().
			bool would_be(wchar_t pressed, wchar_t *out, size_t capacity,
						  size_t &out_length) const noexcept
			{
				if(!pressed || capacity == 0)
					return false;

				auto wanted = _length + 1;
				if(wanted >= capacity || wanted > CAPACITY)
					return false;

				for(size_t i = 0; i < _length; i++)
					out[i] = _text[i];
				out[_length] = pressed;
				out[wanted] = L'\0';
				out_length = wanted;
				return true;
			}

			// Commit a prefix that matched.
			void accept(const wchar_t *text, size_t length, uint64_t now, const void *popup) noexcept
			{
				if(!text || length == 0 || length > CAPACITY)
					return;

				for(size_t i = 0; i < length; i++)
					_text[i] = text[i];
				_text[length] = L'\0';
				_length = length;
				_typed_at = now;
				_popup = popup;
			}

		private:
			wchar_t _text[CAPACITY + 1]{};
			size_t _length{};
			uint64_t _typed_at{};
			const void *_popup{};
		};

		/*
			Which item a prefix names, or none.

			The first match in display order wins. Not "the next one after the
			current highlight", which is what a repeated *mnemonic* does: a
			prefix that has just grown by a character describes a smaller set
			than it did a keystroke ago, and cycling through that set would move
			the highlight away from the item the user is narrowing towards.
		*/
		inline MnemonicReply choose_by_prefix(const wchar_t *prefix, size_t prefix_length,
											  const TypeAheadItem *items, size_t count) noexcept
		{
			MnemonicReply reply;
			if(!prefix || prefix_length == 0 || !items || count == 0)
				return reply;

			for(size_t i = 0; i < count; i++)
			{
				if(!items[i].selectable)
					continue;

				if(title_starts_with(items[i].title, prefix, prefix_length))
				{
					reply.action = MNC_SELECT;
					reply.position = static_cast<UINT>(items[i].position);
					return reply;
				}
			}

			return reply;
		}
	}
}
