#pragma once

/*
	Which menu items this user reaches for, and which ones they have pinned.

	docs/refactor/05-capabilities.md section 6. The identities are
	MenuIdentity's - see that header for why a wID cannot be persisted and why
	an NSS item is signed by its title rather than by the line it was written
	on.

	## The shape of a line

	    pin 42 verb:{CAE3F1D4-7765-4D98-A060-52CD14D56EAB}
	    use  7 item:tools/terminal

	Two fixed tokens and then the identity, which is the whole rest of the line.
	That is the inverse of quarantine.txt, where the identifier comes first -
	and deliberately, for a reason quarantine does not have: an identity
	contains spaces, because menu titles do. Putting it last is what lets a
	title stay a title instead of being percent-encoded into something nobody
	can read or edit.

	`pin` and `use` are the whole vocabulary. `pin` is the user's decision and
	nothing but the user changes it; `use` is a count Shell keeps. Promoting
	something by hand means changing one word.

	## Why counts are persisted at all

	Section 6 asks for "a pinned section with usage counters", and the counter
	is what makes the feature work without the user doing anything: `favorites`
	set to a number promotes the items this person actually uses, ordered by
	how often, with anything explicitly pinned first. Without the count the
	feature is manual-only and most people would never reach it.

	## Bounds, and what they are protecting

	The file is read on the menu path. Both caps below exist so a file that has
	grown - or been made to grow - cannot turn a right-click into a long walk.
	The entry cap is generous against any plausible real list; the byte cap is
	checked before parsing.

	Nothing here can make an item appear that Shell would not otherwise have
	composed. A favorite is matched against items already in the menu and
	reorders them; an identity naming something absent matches nothing. That is
	the same fail-safe direction as quarantine (which only ever removes) and it
	is what makes a per-user file the right place for this: the worst a tampered
	file can do is reorder a menu the user can see.

	## Writing

	Not atomic, for the same reason quarantine.txt is not: a torn favorites file
	loses a count, which the next use restores. The last-known-good config
	shadow gets a MoveFileEx swap because a torn *config* is the failure it
	exists to survive. This is not that.

	The menu path never writes. Recording a use happens after a command has been
	chosen, which is after the menu is down.
*/

#include <windows.h>
#include <shlobj.h>
#include <cstdint>
#include <string>
#include <vector>

#include <MenuIdentity.h>
#include <StoreFile.h>

namespace Nilesoft
{
	namespace Shell
	{
		namespace Favorites
		{
			inline constexpr const wchar_t *FileName = L"favorites.txt";

			// Far above any plausible list. A menu has tens of items; a person
			// with two hundred favourites has not got favourites.
			inline constexpr size_t MaxEntries = 256;

			// U+FEFF spelled rather than written, so it survives a diff and an
			// editor. Same reason as ProviderQuarantine.h.
			inline constexpr wchar_t ByteOrderMark = static_cast<wchar_t>(0xFEFF);

			// A count that has saturated stays saturated rather than wrapping.
			// Wrapping would take the most-used item in the menu to the bottom
			// of the order, which is the one visible thing this must never do.
			inline constexpr uint32_t MaxUses = 1000000000u;

			struct Entry
			{
				MenuIdentity::Identity identity;

				// The user said so. Ordered ahead of anything merely used, and
				// never dropped to make room.
				bool pinned{};

				uint32_t uses{};
			};

			inline std::wstring_view trim(std::wstring_view s) noexcept
			{
				while(!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r'))
					s.remove_prefix(1);
				while(!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r'))
					s.remove_suffix(1);
				return s;
			}

			/*
				One line: `<pin|use> <count> <identity>`.

				An unparseable line is skipped rather than failing the load, for
				the reason quarantine gives: a half-readable list should still
				do the half it can read. There is no user waiting on a syntax
				error - `shell.exe -favorites` writes this file, and a person
				editing it by hand finds out by looking at `-favorites list`.
			*/
			inline bool parse_line(std::wstring_view line, Entry &out)
			{
				line = trim(line);
				if(line.empty() || line.front() == L'#')
					return false;

				auto take_token = [&line]() -> std::wstring_view
				{
					auto end = line.find_first_of(L" \t");
					auto token = line.substr(0, end);
					line = end == std::wstring_view::npos ? std::wstring_view{}
														  : trim(line.substr(end));
					return token;
				};

				auto state = take_token();
				bool pinned = false;
				if(state == L"pin")
					pinned = true;
				else if(state != L"use")
					return false;

				auto count = take_token();
				if(count.empty())
					return false;

				uint64_t uses = 0;
				for(wchar_t c : count)
				{
					if(c < L'0' || c > L'9')
						return false;
					uses = uses * 10 + static_cast<uint64_t>(c - L'0');
					if(uses > MaxUses)
					{
						uses = MaxUses;
						break;
					}
				}

				auto identity = MenuIdentity::parse(line);
				if(!identity.valid())
					return false;

				out.identity = std::move(identity);
				out.pinned = pinned;
				out.uses = static_cast<uint32_t>(uses);
				return true;
			}

			inline std::vector<Entry> parse(std::wstring_view text)
			{
				std::vector<Entry> out;

				size_t at = 0;
				while(at <= text.size() && out.size() < MaxEntries)
				{
					auto end = text.find(L'\n', at);
					auto line = text.substr(at, end == std::wstring_view::npos
												? std::wstring_view::npos : end - at);
					at = end == std::wstring_view::npos ? text.size() + 1 : end + 1;

					Entry entry;
					if(!parse_line(line, entry))
						continue;

					// First wins, as the quarantine list does, so a repeated
					// identity cannot consume the cap and the count the user
					// can see at the top of the file is the one in force.
					bool seen = false;
					for(const auto &have : out)
					{
						if(have.identity.hash == entry.identity.hash)
						{
							seen = true;
							break;
						}
					}
					if(seen)
						continue;

					out.push_back(std::move(entry));
				}

				return out;
			}

			inline std::wstring serialize(const std::vector<Entry> &entries)
			{
				std::wstring out =
					L"# Nilesoft Shell - favorite menu items.\r\n"
					L"# <pin|use> <count> <identity>. '#' comments a line out.\r\n"
					L"# 'pin' is yours and Shell never changes it; 'use' is a count Shell keeps.\r\n"
					L"# settings { favorites = N } promotes up to N of these to the top of a menu.\r\n";

				for(const auto &entry : entries)
				{
					wchar_t head[32]{};
					::wsprintfW(head, L"%s %u ", entry.pinned ? L"pin" : L"use", entry.uses);
					out += head;
					out += entry.identity.text;
					out += L"\r\n";
				}
				return out;
			}

			// %LocalAppData%\Nilesoft\Shell\favorites.txt
			//
			// Per-user for the reason quarantine is (section 1b): this is a
			// preference, not an administrative act, and ProgramData would need
			// elevation to record that somebody used a menu item. Section 6
			// says ProgramData; docs/refactor/08-handoff.md section 3.9 already
			// records the correction.
			//
			// SHGetKnownFolderPath's caller "is responsible for freeing this
			// resource once it is no longer needed by calling CoTaskMemFree,
			// whether SHGetKnownFolderPath succeeds or not", and "the returned
			// path does not include a trailing backslash".
			// https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath
			inline std::wstring default_path()
			{
				PWSTR local = nullptr;
				auto hr = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local);
				std::wstring out;
				if(SUCCEEDED(hr) && local)
				{
					out = local;
					out += L"\\Nilesoft\\Shell\\";
					out += FileName;
				}
				::CoTaskMemFree(local);
				return out;
			}

			/*
				A missing file is the normal case and is not a failure: nobody
				has favourites until they use something. It is still not the
				same thing as a file that would not open, which is why the
				result below has three states rather than two.

				The open itself - share modes, the reparse-point flag, the size
				cap, the byte-order mark - is src/shared/StoreFile.h, shared
				with quarantine.txt because both files are written by both
				shell.dll and shell.exe.
			*/
			// A read that says which of the three things happened, because two of
			// them are not the same fact and the caller acts on the difference.
			// src/shared/StoreFile.h.
			struct LoadResult
			{
				StoreFile::LoadState state{ StoreFile::LoadState::Failed };
				std::vector<Entry> entries;
				uint64_t write_time{};

				// An absent file is an empty list and may be written back over.
				// A failed read may not: that is the whole distinction.
				bool usable() const { return state != StoreFile::LoadState::Failed; }
				bool loaded() const { return state == StoreFile::LoadState::Loaded; }
			};

			inline constexpr uint64_t MaxFileBytes = 512 * 1024;

			inline LoadResult read(const std::wstring &path)
			{
				auto raw = StoreFile::read(path, MaxFileBytes);

				LoadResult out;
				out.state = raw.state;
				out.write_time = raw.write_time;
				if(raw.loaded())
					out.entries = parse(raw.text);
				return out;
			}

			// For callers that genuinely cannot act on the difference - a report,
			// a listing. Anything that reads in order to write back must use
			// read() and check usable(), or it will write a fresh list over a
			// file it failed to read.
			inline std::vector<Entry> load(const std::wstring &path)
			{
				return read(path).entries;
			}

			/*
				Written through a temporary and renamed over the target, so the
				destination is never a truncated or half-written file for another
				host to read as an empty list. See src/shared/StoreFile.h for the
				interleaving that made that destructive rather than merely
				untidy.
			*/
			inline bool save(const std::wstring &path, const std::vector<Entry> &entries)
			{
				return StoreFile::replace(path, serialize(entries));
			}

			/*
				Count one use of `identity`, in place.

				Ordering is not touched here: `promote` (Include/MenuFavorites.h)
				decides what a menu shows, from whatever this list holds, so a
				store that also sorted itself would be two answers to one
				question.

				Returns false when the list is full and this identity is not
				already in it. Dropping the least-used entry to make room is
				deliberately not done: the cap is far above any real list, so
				reaching it means something is wrong, and silently evicting a
				user's data to keep going would hide it.
			*/
			inline bool record_use(std::vector<Entry> &entries,
								   const MenuIdentity::Identity &identity)
			{
				if(!identity.valid())
					return false;

				for(auto &entry : entries)
				{
					if(entry.identity.hash == identity.hash)
					{
						if(entry.uses < MaxUses)
							entry.uses++;
						return true;
					}
				}

				if(entries.size() >= MaxEntries)
					return false;

				Entry fresh;
				fresh.identity = identity;
				fresh.uses = 1;
				entries.push_back(std::move(fresh));
				return true;
			}

			// Pin or unpin. Pinning something never used records it with a zero
			// count, which is right: the user has said they want it, and they
			// have not used it yet.
			inline bool set_pinned(std::vector<Entry> &entries,
								   const MenuIdentity::Identity &identity, bool pinned)
			{
				if(!identity.valid())
					return false;

				for(auto &entry : entries)
				{
					if(entry.identity.hash == identity.hash)
					{
						entry.pinned = pinned;
						return true;
					}
				}

				if(!pinned || entries.size() >= MaxEntries)
					return false;

				Entry fresh;
				fresh.identity = identity;
				fresh.pinned = true;
				entries.push_back(std::move(fresh));
				return true;
			}
		}
	}
}
