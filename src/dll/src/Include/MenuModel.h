#pragma once

/*
	The items this menu composed, and where each of them came from.

	Seam step 6 of docs/refactor/04-code-health.md section 4, and the origin
	table docs/refactor/01-takeover-contract.md section 4 specified:

	  "`origins` replaces today's three parallel vectors
	   `_items_command`/`_items_popup`/`_main_popup` + linear `get_item` scan"

	Three vectors held one fact between them, and which vector an item landed in
	*was* the fact - a dynamic popup went in one, a dynamic command in another,
	a packaged verb in the same one as the dynamic command, and a mirrored
	native item in none. Nothing said so anywhere; it was encoded in the shape
	of an if/else at the composition site, and read back out by scanning both
	vectors and asking "is this pointer in here". `_main_popup` had stopped
	being read at all.

	What that cost, concretely: `owns_item` walked both vectors for every item,
	and `OnMenuChar` calls it once per item, so a keystroke in a thirty-item
	menu was nine hundred pointer comparisons. And there was nowhere to hang the
	thing docs/refactor/05-capabilities.md sections 6 and 7 both need - favorites
	wants an identity that survives the session, and the rule inspector wants to
	say which rule put an item there. Neither can be added to a vector whose
	membership is its only content.

	## What is deliberately NOT here

	**Mirrored native items.** Section 01.4's sketch lists `Native` as a kind,
	and it is absent here on purpose. This table is about *ownership*: these are
	the items Shell built and is answerable for. A native item is the host's -
	it keeps the host's own wID, Shell renders it and hands the identifier
	straight back, and `owns_item` has always answered false for one.

	The native side of section 01.4 is not missing, it is somewhere else:
	mapping a chosen identifier back to the host's original wID is what
	`Include/HostContract.h` does, and it landed with seam step 4. Putting
	natives in here as well would change what `owns_item` answers, which changes
	which items `OnMenuChar` will match a keystroke against - a behaviour
	change, in a commit whose whole rule is that it moves code without altering
	it (section 04.4: "move code, don't improve it in the same commit").

	Whether natives *should* become mnemonic candidates is a real question and
	is left open rather than settled here by accident. Note before investigating
	it that WM_MENUCHAR is only sent for a key "that does not correspond to any
	mnemonic or accelerator key"
	(https://learn.microsoft.com/en-us/windows/win32/menurc/wm-menuchar), so
	whatever Windows already matches never reaches Shell's handler at all - the
	answer depends on what Windows does with an MFT_OWNERDRAW item that also
	carries MIIM_STRING, and that is a measurement nobody has taken.

	## Why a template

	Same reason as `PopupStack<T>` in Include/PopupLifecycle.h: the logic is
	pure - a table, two lookups and an order - and templating it on the item
	type lets the unit suite drive the real code with a two-field fake instead
	of building a MENUITEMINFOW-derived object with a live COM pointer hanging
	off it. src/tests/test_menu_model.cpp does exactly that.

	## The two rules that had to be preserved exactly

	Both were implicit in the vectors and are now stated, because getting either
	wrong is silent:

	  1. **A popup is not a command.** `_items_command` never held a dynamic
	     popup, so an identifier lookup could never land on one. `command()`
	     skips popups for that reason and `test_menu_model` pins it.
	  2. **First match wins.** The old lookup was a forward scan that stopped at
	     the first hit, so if two entries ever carried one identifier the
	     earlier won. The index keeps that by refusing to overwrite, rather than
	     by accident of iteration order.
*/

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		// Where a composed item came from. Not a rendering distinction - both
		// kinds are owner-drawn the same way - but an *answerability* one, and
		// it is what sections 05.6 and 05.7 will key on.
		enum class ItemOrigin : uint8_t
		{
			// An NSS rule built it. Identity is the rule, and survives a
			// session because the configuration does.
			Custom,
			// A packaged IExplorerCommand. Identity is the CLSID, which is what
			// `shell.exe -quarantine:add` already takes.
			ExplorerCommand,
		};

		template<typename T>
		class MenuModel
		{
		public:
			struct Entry
			{
				T *item{};
				ItemOrigin origin{ ItemOrigin::Custom };
				uint32_t wid{};

				// Opens a submenu rather than running something. Kept as a
				// recorded fact rather than re-derived from `item`, so the
				// table can answer without dereferencing an item whose menu has
				// since been torn down.
				bool popup{};

				bool commandable() const { return !popup; }
			};

			void clear()
			{
				_entries.clear();
				_owned.clear();
				_by_id.clear();
			}

			void reserve(size_t count)
			{
				_entries.reserve(count);
				_owned.reserve(count);
				_by_id.reserve(count);
			}

			void add(T *item, ItemOrigin origin, uint32_t wid, bool popup)
			{
				if(!item)
					return;

				Entry entry;
				entry.item = item;
				entry.origin = origin;
				entry.wid = wid;
				entry.popup = popup;

				_entries.push_back(entry);
				_owned.insert(item);

				// emplace, not operator[]: first match wins, which is what the
				// forward scan this replaces did. See rule 2 in the header.
				if(entry.commandable())
					_by_id.emplace(wid, _entries.size() - 1);
			}

			// Is this pointer one of ours?
			//
			// The question is asked of a pointer read out of a menu item's
			// dwItemData, which for a borrowed host popup is whatever the host
			// put there - so this is a validity check before a dereference, not
			// a convenience. That is why it is a set membership test and not
			// anything that touches `item`.
			bool owns(const T *item) const
			{
				return item && _owned.find(const_cast<T *>(item)) != _owned.end();
			}

			// The item a chosen identifier refers to, or nullptr. Popups are
			// never candidates - see rule 1.
			T *command(uint32_t wid) const
			{
				auto found = _by_id.find(wid);
				if(found == _by_id.end())
					return nullptr;
				return _entries[found->second].item;
			}

			// Composition order, which is the order the user sees. Sections
			// 05.6 and 05.7 both need it; nothing does yet, and it costs
			// nothing to keep because the vector is the storage.
			const std::vector<Entry> &entries() const { return _entries; }
			size_t size() const { return _entries.size(); }

		private:
			std::vector<Entry> _entries;
			std::unordered_set<T *> _owned;
			std::unordered_map<uint32_t, size_t> _by_id;
		};
	}
}
