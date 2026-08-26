#pragma once

/*
	Extensions the user has told Shell to stop asking.

	docs/refactor/05-capabilities.md section 1 makes this the actionable half of
	the Reliability Center: the report can now name the handler that cost 186 ms
	(section 1a), and this is what a user does about it. Without it the feature
	is a diagnosis with no treatment.

	Three decisions, each with a plausible opposite.

	**Skipped when Shell builds its menu - not refused at CoCreateInstance.**
	Section 1 proposed "activation refused via the existing E_NOINTERFACE path",
	reusing the CLSID blocklist that `CoCreateInstanceHook` already compiles.
	That is the wrong instrument here, for two reasons that only became clear
	once the cost was measured (docs/refactor/02-first-paint-latency.md section
	2a-i):

	  1. It is far too blunt. The detour sees *every* activation in the host, so
	     refusing a CLSID there disables that extension for everything the
	     process does, not just for Shell's context menu. A user quarantining a
	     slow shell extension has not asked for its shell-integration DLL to
	     start failing in unrelated code paths.
	  2. It does not save the thing worth saving. The measured cost is the
	     activation *plus* GetState/GetTitle/GetIcon - about 46 ms of
	     CoCreateInstance and the rest in metadata calls. Skipping the provider
	     in `append_explorer_commands` pays none of it; refusing the activation
	     still walks the catalog and still enters the hook.

	So a quarantined provider is one `append_explorer_commands` never asks. The
	extension keeps working everywhere else, and the menu it was slowing down
	simply stops containing it.

	**Keyed by CLSID, reported by hash.** The ring carries a 32-bit FNV-1a of
	the CLSID because that is what keeps strings off the measured path; a
	quarantine file has no such constraint and a truncated hash is a poor thing
	to ask a user to edit. The file holds real GUIDs, and the hash is computed
	when the list is loaded - so the report and the file agree without either
	storing the other's key.

	**%LocalAppData%, not ProgramData.** Section 1 said ProgramData, which needs
	elevation to write and would make quarantining an administrative act. The
	integrity rule this tree states for the catalog cache
	(docs/refactor/02-first-paint-latency.md section 1) is that a file a
	medium-integrity process can write must never make an activation *possible*
	that a live query would not have authorised. This file only ever removes
	providers, so the direction is fail-safe: the worst a tampered file can do
	is hide a menu item, which the user can see and undo, and it can never cause
	something to be activated. Per-user is also the right scope - the complaint
	is "my menu is slow", and menus are per-user.

	Parsing is deliberately forgiving about whitespace and casing and strict
	about everything else: an unparseable line is skipped rather than failing
	the load, because a half-readable quarantine list should still quarantine
	the entries it can read.
*/

#include <windows.h>
#include <shlobj.h>
#include <cstdint>
#include <string>
#include <vector>
#include <StoreFile.h>

namespace Nilesoft
{
	namespace Shell
	{
		namespace Quarantine
		{
			inline constexpr const wchar_t *FileName = L"quarantine.txt";

			// Bounded so a corrupted or malicious file cannot make menu build
			// walk an unbounded list. Far above any plausible real one: this
			// machine registers 23 packaged handlers in total.
			inline constexpr size_t MaxEntries = 128;

			// U+FEFF, spelled rather than written: a literal byte-order mark in
			// source is invisible in a diff and does not survive every editor.
			inline constexpr wchar_t ByteOrderMark = static_cast<wchar_t>(0xFEFF);

			// The same FNV-1a over the sixteen GUID bytes that
			// ProviderHealth::provider_hash computes, duplicated here rather
			// than shared because that header is DLL-side and this one is read
			// by shell.exe too. Pinned by a test that hashes a known GUID, so
			// the two cannot drift apart silently.
			inline uint32_t hash_clsid(const GUID &clsid) noexcept
			{
				auto bytes = reinterpret_cast<const unsigned char *>(&clsid);
				uint32_t h = 2166136261u;
				for(size_t i = 0; i < sizeof(GUID); i++)
				{
					h ^= bytes[i];
					h *= 16777619u;
				}
				return h;
			}

			struct Entry
			{
				GUID clsid{};
				uint32_t hash{};
				// What the report called it when it was quarantined. Only a
				// label - nothing matches on it - so a handler that retitles
				// itself stays quarantined.
				std::wstring note;
			};

			// Trims ASCII spaces and tabs from both ends. Deliberately not
			// locale-aware: this file holds GUIDs.
			inline std::wstring_view trim(std::wstring_view s) noexcept
			{
				while(!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r'))
					s.remove_prefix(1);
				while(!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r'))
					s.remove_suffix(1);
				return s;
			}

			/*
				`{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}`, with or without the
				braces and in either case.

				CLSIDFromString is not used: it is documented to accept only the
				braced form, and it would pull ole32 into a header that
				shell.exe's validator path includes. Sixteen bytes of hex is not
				worth a dependency.
				https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-clsidfromstring
			*/
			inline bool parse_guid(std::wstring_view text, GUID &out) noexcept
			{
				text = trim(text);
				if(text.size() >= 2 && text.front() == L'{' && text.back() == L'}')
				{
					text.remove_prefix(1);
					text.remove_suffix(1);
				}
				if(text.size() != 36)
					return false;

				auto nibble = [](wchar_t c, int &value) noexcept
				{
					if(c >= L'0' && c <= L'9') { value = c - L'0'; return true; }
					if(c >= L'a' && c <= L'f') { value = c - L'a' + 10; return true; }
					if(c >= L'A' && c <= L'F') { value = c - L'A' + 10; return true; }
					return false;
				};

				// Byte positions of the 32 hex digits, skipping the four dashes.
				unsigned char bytes[16]{};
				size_t at = 0;
				for(size_t i = 0; i < 36; i++)
				{
					if(i == 8 || i == 13 || i == 18 || i == 23)
					{
						if(text[i] != L'-')
							return false;
						continue;
					}
					int hi = 0, lo = 0;
					if(!nibble(text[i], hi))
						return false;
					i++;
					if(i >= 36 || !nibble(text[i], lo))
						return false;
					if(at >= 16)
						return false;
					bytes[at++] = static_cast<unsigned char>((hi << 4) | lo);
				}
				if(at != 16)
					return false;

				// The registry string form is big-endian for the first three
				// fields and byte-order-neutral for the rest, which is why this
				// is assembled rather than memcpy'd.
				out.Data1 = (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16)
						  | (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
				out.Data2 = static_cast<uint16_t>((bytes[4] << 8) | bytes[5]);
				out.Data3 = static_cast<uint16_t>((bytes[6] << 8) | bytes[7]);
				for(size_t i = 0; i < 8; i++)
					out.Data4[i] = bytes[8 + i];
				return true;
			}

			inline std::wstring format_guid(const GUID &clsid)
			{
				wchar_t buffer[64]{};
				::wsprintfW(buffer, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
							clsid.Data1, clsid.Data2, clsid.Data3,
							clsid.Data4[0], clsid.Data4[1], clsid.Data4[2], clsid.Data4[3],
							clsid.Data4[4], clsid.Data4[5], clsid.Data4[6], clsid.Data4[7]);
				return buffer;
			}

			/*
				One line is one CLSID, optionally followed by whitespace and a
				free-text note. `#` starts a comment and blank lines are ignored,
				so the file stays something a person can keep notes in.

				An unparseable line is skipped, not fatal: a list that is half
				readable should still quarantine the half it can read. Nothing
				here reports a syntax error, because there is no user waiting on
				one - `shell.exe -quarantine` writes this file, and a person
				editing it by hand finds out by looking at `-quarantine list`.
			*/
			inline std::vector<Entry> parse(std::wstring_view text)
			{
				std::vector<Entry> out;

				size_t at = 0;
				while(at <= text.size() && out.size() < MaxEntries)
				{
					auto end = text.find(L'\n', at);
					auto line = text.substr(at, end == std::wstring_view::npos ? std::wstring_view::npos
																			   : end - at);
					if(end == std::wstring_view::npos)
						at = text.size() + 1;
					else
						at = end + 1;

					line = trim(line);
					if(line.empty() || line.front() == L'#')
						continue;

					// The GUID is the first token; anything after the first run
					// of whitespace is a note.
					auto split = line.find_first_of(L" \t");
					auto id = split == std::wstring_view::npos ? line : line.substr(0, split);
					auto note = split == std::wstring_view::npos ? std::wstring_view{}
																 : trim(line.substr(split));

					Entry entry;
					if(!parse_guid(id, entry.clsid))
						continue;

					entry.hash = hash_clsid(entry.clsid);

					// Already listed. Keeping the first keeps the first note,
					// and stops a file that repeats one CLSID from consuming
					// the whole cap.
					bool seen = false;
					for(const auto &have : out)
					{
						if(have.hash == entry.hash)
						{
							seen = true;
							break;
						}
					}
					if(seen)
						continue;

					entry.note.assign(note);
					out.push_back(std::move(entry));
				}

				return out;
			}

			inline std::wstring serialize(const std::vector<Entry> &entries)
			{
				std::wstring out =
					L"# Nilesoft Shell - quarantined context-menu extensions.\r\n"
					L"# One CLSID per line; text after it is a note. '#' comments a line out.\r\n"
					L"# Shell stops asking these handlers when it builds a menu. They keep\r\n"
					L"# working everywhere else. Delete a line to un-quarantine it.\r\n";

				for(const auto &entry : entries)
				{
					out += format_guid(entry.clsid);
					if(!entry.note.empty())
					{
						out += L' ';
						out += entry.note;
					}
					out += L"\r\n";
				}
				return out;
			}

			// %LocalAppData%\Nilesoft\Shell
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
				Read the list.

				A missing file is the normal case and is not a failure - nothing
				is quarantined until somebody quarantines something. It is not
				the same thing as a file that would not open, though, and this
				used to answer with an empty list either way: "the caller does
				not have to tell 'no file' from 'no entries'". A caller that
				writes back does have to, and both of them do.

				The file is UTF-16LE with a byte-order mark, which is what
				Notepad writes back if a person edits it. A file without the
				mark is still read, because a text editor that strips it should
				not silently un-quarantine everything.

				FILE_FLAG_OPEN_REPARSE_POINT so a link is opened as itself
				rather than followed: this path is under the user's own
				LocalAppData, but a file that steers what Shell does should not
				be redirectable by something that got there first. That, the
				share modes and the size cap now live in
				src/shared/StoreFile.h, shared with favorites.txt.
			*/
			// A read that says which of the three things happened. Anything that
			// reads in order to write back must use this and check usable(), or a
			// failed read becomes a fresh list written over the user's.
			// src/shared/StoreFile.h.
			struct LoadResult
			{
				StoreFile::LoadState state{ StoreFile::LoadState::Failed };
				std::vector<Entry> entries;
				uint64_t write_time{};

				bool usable() const { return state != StoreFile::LoadState::Failed; }
				bool loaded() const { return state == StoreFile::LoadState::Loaded; }
			};

			// A cap, because this is parsed before a menu is built. 256 KB is a
			// hundred times the largest plausible list.
			inline constexpr uint64_t MaxFileBytes = 256 * 1024;

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

			// For a caller that cannot act on the difference - a listing, a
			// report. Never for one that writes back.
			inline std::vector<Entry> load(const std::wstring &path)
			{
				return read(path).entries;
			}

			/*
				Written through a temporary and renamed over the target.

				This used to say the non-atomic write was deliberate: "a torn
				quarantine file loses at worst one entry ... the last-known-good
				shadow gets a MoveFileEx swap because a torn *config* is the
				failure it exists to survive; this is not that."

				That reasoning is sound about a torn write and it addresses the
				wrong risk. CREATE_ALWAYS truncates first and writes after, so
				the destination is a zero-length file for the duration - and
				load() mapped every failed read to an empty list, so another
				host reading during that window did not observe a torn file, it
				observed an empty one, and then wrote its own single entry over
				the top. The swap is not about tearing. It is about the
				destination never being observable in a state it was never in.
				src/shared/StoreFile.h.
			*/
			inline bool save(const std::wstring &path, const std::vector<Entry> &entries)
			{
				return StoreFile::replace(path, serialize(entries));
			}

		}
	}
}
