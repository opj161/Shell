#pragma once

/*
	Typing a letter in the menu.

	Shell draws every item itself, so Windows cannot match mnemonics for it -
	there is no text in the HMENU to match against. What Windows does instead is
	ask: "Sent when a menu is active and the user presses a key that does not
	correspond to any mnemonic or accelerator key. This message is sent to the
	window that owns the menu."
	https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menuchar

	Nothing answered it. The subclass swallowed the character, DefWindowProc
	returned MNC_IGNORE, and a keypress produced a beep - so a menu full of items
	whose titles carry mnemonics ("E&dit with Adobe Acrobat", "&Move to
	OneDrive", straight out of Explorer and out of packaged verb handlers) could
	not be driven by the keyboard at all.

	The reply is one of four actions in the high word:

		MNC_IGNORE   0   discard the character and beep
		MNC_CLOSE    1   close the menu
		MNC_EXECUTE  2   choose the item named in the low word
		MNC_SELECT   3   select the item named in the low word

	and the low word is where this gets dangerous. The WM_MENUCHAR page says only
	"the item specified in the low-order word"; *Using Menus* is the page that
	says what that means: "the low-order word of the return value contains the
	zero-based index of the menu item".
	https://learn.microsoft.com/en-us/windows/win32/menurc/using-menus

	An index, not an identifier - and Shell's own identifiers start at
	0x0fffffff, so returning one would either select an unrelated item or lose
	the keystroke entirely. That mattered enough to measure rather than trust:
	src/tests/hostprobe/fixtures/question.menuchar_low_word_is_an_index.trace
	replies with index 2 against a menu whose identifiers are 5001..5005 and gets
	5003; question.menuchar_low_word_is_not_an_identifier replies with 5003 and
	gets *nothing*, the menu simply closing. So an out-of-range index is refused
	rather than reinterpreted, and the failure mode of getting this wrong is a
	silently dead keyboard.

	This header is the decision, kept free of the menu engine so it can be tested
	directly. The caller supplies what is on screen; this says what to answer.
*/

#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace Nilesoft
{
	namespace Shell
	{
		// Case folding for a single character, through the user's locale -
		// mnemonics are text a person reads, so matching them linguistically is
		// correct here in a way it would not be for an identifier.
		//
		// "If the high-order word of this parameter is zero, the low-order word
		// must contain a single character to be converted... the return value is
		// a 32-bit value whose high-order word is zero, and low-order word
		// contains the converted character."
		// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-charupperw
		inline wchar_t upper_char(wchar_t c) noexcept
		{
			if(!c)
				return c;
			auto converted = ::CharUpperW(reinterpret_cast<LPWSTR>(static_cast<UINT_PTR>(c)));
			return static_cast<wchar_t>(reinterpret_cast<UINT_PTR>(converted) & 0xFFFF);
		}

		/*
			The mnemonic a title declares, or 0.

			"&" marks the next character; "&&" is a literal ampersand and marks
			nothing. Only the first marker counts, which is what Windows does
			with a title that carries more than one.

			Anything after a tab is the accelerator column ("Rename\tF2") and is
			not part of the label, so a marker there is not a mnemonic.
		*/
		inline wchar_t mnemonic_of(const wchar_t *title) noexcept
		{
			if(!title)
				return 0;

			for(size_t i = 0; title[i]; i++)
			{
				if(title[i] == L'\t' || title[i] == L'\b')
					break;

				if(title[i] != L'&')
					continue;

				auto next = title[i + 1];
				if(next == L'&')
				{
					i++;			// a literal ampersand; skip both
					continue;
				}
				if(next == L'\0' || next == L'\t')
					break;			// a trailing marker names nothing

				return upper_char(next);
			}
			return 0;
		}

		// One item as it appears in the popup: where it is, and what it answers
		// to. `position` is the item's zero-based index in the HMENU, which is
		// the currency WM_MENUCHAR's reply is denominated in.
		struct MnemonicItem
		{
			int position{};
			wchar_t mnemonic{};
			bool selectable{ true };	// a separator, or a disabled item, is not
		};

		struct MnemonicReply
		{
			UINT action{ MNC_IGNORE };
			UINT position{};

			// The value to return from the window procedure.
			LRESULT to_lresult() const noexcept
			{
				return MAKELRESULT(position, action);
			}
		};

		/*
			What to answer, given what is on screen.

			pressed          the character from LOWORD(wParam)
			items            the popup's items in display order
			count            how many
			current_position the highlighted item's index, or -1

			The rules are Windows' own, and the difference between them is the
			whole behaviour a user feels:

			  no match          ignore it, and let Windows beep. Deliberately not
			                    "close the menu" - a mistyped letter should not
			                    dismiss what the user was reading.
			  exactly one       execute it. This is what makes a mnemonic a
			                    shortcut rather than a way to move the highlight.
			  more than one     select the next one after the current highlight,
			                    wrapping. Pressing the letter again moves on, so
			                    duplicated mnemonics cycle instead of one of them
			                    silently winning forever.
		*/
		inline MnemonicReply choose_mnemonic(wchar_t pressed, const MnemonicItem *items,
											 size_t count, int current_position) noexcept
		{
			MnemonicReply reply;
			if(!pressed || !items || count == 0)
				return reply;

			auto wanted = upper_char(pressed);
			if(!wanted)
				return reply;

			size_t matches = 0;
			size_t first = 0;
			size_t next_after_current = count;	// count means "none found yet"

			for(size_t i = 0; i < count; i++)
			{
				if(!items[i].selectable || items[i].mnemonic != wanted)
					continue;

				if(matches == 0)
					first = i;
				matches++;

				if(next_after_current == count && items[i].position > current_position)
					next_after_current = i;
			}

			if(matches == 0)
				return reply;

			if(matches == 1)
			{
				reply.action = MNC_EXECUTE;
				reply.position = static_cast<UINT>(items[first].position);
				return reply;
			}

			// Wrap to the first match when the highlight is already past the last
			// one - or when there is no highlight at all.
			auto chosen = next_after_current == count ? first : next_after_current;
			reply.action = MNC_SELECT;
			reply.position = static_cast<UINT>(items[chosen].position);
			return reply;
		}
	}
}
