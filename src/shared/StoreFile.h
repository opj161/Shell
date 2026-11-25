#pragma once

/*
	Reading, rewriting and replacing the two small files Shell keeps beside a
	user's config: favorites.txt and quarantine.txt.

	Both were read with `load()` returning a vector, and written with
	`CreateFile(GENERIC_WRITE, share 0, CREATE_ALWAYS)`. Three defects follow
	from that pair, and they compose into the worst one.

	## An I/O failure was indistinguishable from an empty list

	`load()` mapped every failure - the file missing, a sharing violation, an
	unreadable size - to `{}`. A caller that reads, modifies and writes back
	then writes a *one-entry* file over the user's history, and nothing about
	that looks wrong from inside.

	## The writer's own share mode made that reachable

	`load` opens `GENERIC_READ, FILE_SHARE_READ`; `save` opened
	`GENERIC_WRITE, 0, CREATE_ALWAYS`. A conflicting open fails with
	ERROR_SHARING_VIOLATION
	(https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew),
	so with two hosts - and there are always several: every Explorer window, the
	taskbar, any file manager - the interleaving is

		A truncates and holds  ->  B's load fails  ->  B gets {}
		A finishes             ->  B saves one entry over everything

	FavoritesStore's own header claimed racing hosts "lose at worst one
	increment". They can lose the lot.

	CREATE_ALWAYS is what opens that window: it truncates first and writes
	after, so the file is empty on disk for as long as the write takes. Writing
	a temporary in the same directory and swapping it in removes the window
	entirely - there is no moment at which the destination is a half-written or
	zero-length file. replace() below says which swap and why.

	ProviderQuarantine::save carried a comment defending the non-atomic write:
	"a torn quarantine file loses at worst one entry". That is a sound argument
	about a *torn* write and it is not the risk here. The risk is another
	process reading the truncation.

	## Content and timestamp came from two separate calls

	Both stores read the file and then called GetFileAttributesEx to stamp it.
	A rewrite in between caches the old content under the new stamp, and
	nothing refreshes until the *next* write - so the store serves stale data
	and believes it is current. Reading the time from the handle the content
	came from closes it: GetFileInformationByHandle "retrieves file information
	for the specified file", the one the handle names.
	https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfileinformationbyhandle

	## Both files are hand-edited, so a write must keep what a person wrote

	Each file says '#' comments a line out and that a person may keep notes in
	it. Writing back `header + parsed entries` deleted every comment, blank
	line, mistyped line and duplicate on the first click. merge() rebuilds the
	file line by line instead: it changes only the lines whose entry changed.

	A file made by hand is also not necessarily UTF-16: Notepad's default is
	UTF-8 without a mark. Read as UTF-16 that parses as an empty list in state
	Loaded, and the next write replaces the whole list. decode() accepts what an
	editor produces and refuses what it cannot decode, and a Failed file is
	never written back.

	Kept in src/shared because shell.exe's -quarantine and -favorites commands
	read and write the same files as the DLL does, and a rule about who may
	write is not a rule if only one of them follows it.
*/

#include <windows.h>

#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		namespace StoreFile
		{
			// Three outcomes, because two of them are not the same fact.
			enum class LoadState
			{
				Missing,	// no such file: an empty list, and a real answer
				Loaded,		// read it
				Failed,		// could not read it, and does not know what is in it
			};

			struct Text
			{
				LoadState state{ LoadState::Failed };

				// Byte-order mark already removed. Empty for Missing, and
				// legitimately empty for a zero-length file.
				std::wstring text;

				// Taken from the same handle as the content above, so no
				// rewrite can slip between the two.
				uint64_t write_time{};

				// May this be modified and written back? Missing may: an absent
				// file is an empty list. Failed may not, ever - that is the
				// whole point of separating them.
				bool usable() const { return state != LoadState::Failed; }
				bool loaded() const { return state == LoadState::Loaded; }
			};

			inline constexpr wchar_t ByteOrderMark = 0xFEFF;

			// Trims ASCII spaces, tabs and CR from both ends. Deliberately not
			// locale-aware: these files hold GUIDs and identities.
			inline std::wstring_view trim(std::wstring_view s) noexcept
			{
				while(!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r'))
					s.remove_prefix(1);
				while(!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r'))
					s.remove_suffix(1);
				return s;
			}

			inline uint64_t file_time_of(HANDLE file)
			{
				BY_HANDLE_FILE_INFORMATION info{};
				if(!::GetFileInformationByHandle(file, &info))
					return 0;

				ULARGE_INTEGER value{};
				value.LowPart = info.ftLastWriteTime.dwLowDateTime;
				value.HighPart = info.ftLastWriteTime.dwHighDateTime;
				return value.QuadPart;
			}

			/*
				Bytes to text. Writes are always UTF-16LE with a mark; reads accept
				what a person's editor leaves behind.

				  FF FE      UTF-16LE mark: the rest is UTF-16.
				  EF BB BF   UTF-8 mark: the rest is UTF-8.
				  FE FF      UTF-16BE: not supported, so Failed rather than guessed.
				  no mark    UTF-16LE if the length is even and some odd-offset byte
				             is zero - the high byte of any ASCII-range code unit,
				             and these files are full of them (GUIDs, "pin", "use").
				             UTF-8 text never contains NUL, so no ordinary UTF-8 file
				             is taken for it. Everything else is UTF-8.

				UTF-8 is decoded with MB_ERR_INVALID_CHARS, which for CP_UTF8
				"must be set to either 0 or MB_ERR_INVALID_CHARS", and fails with
				ERROR_NO_UNICODE_TRANSLATION on an invalid sequence instead of
				substituting U+FFFD. A file with a replaced character in it is
				one nobody should write back over. "If cbMultiByte is 0, the
				function fails", so an empty body is handled before the call.
				https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar
			*/
			inline bool decode(std::string_view bytes, std::wstring &out)
			{
				auto at = [&bytes](size_t i) { return static_cast<unsigned char>(bytes[i]); };

				bool utf16 = false;
				if(bytes.size() >= 2 && at(0) == 0xFF && at(1) == 0xFE)
				{
					utf16 = true;
					bytes.remove_prefix(2);
				}
				else if(bytes.size() >= 3 && at(0) == 0xEF && at(1) == 0xBB && at(2) == 0xBF)
					bytes.remove_prefix(3);
				else if(bytes.size() >= 2 && at(0) == 0xFE && at(1) == 0xFF)
					return false;
				else if((bytes.size() % sizeof(wchar_t)) == 0)
				{
					for(size_t i = 1; i < bytes.size() && !utf16; i += 2)
						utf16 = at(i) == 0;
				}

				out.clear();
				if(utf16)
				{
					if((bytes.size() % sizeof(wchar_t)) != 0)
						return false;
					out.resize(bytes.size() / sizeof(wchar_t));
					if(!bytes.empty())
						std::memcpy(out.data(), bytes.data(), bytes.size());
					return true;
				}

				if(bytes.empty())
					return true;
				if(bytes.size() > INT_MAX)
					return false;

				auto size = static_cast<int>(bytes.size());
				auto need = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
												  bytes.data(), size, nullptr, 0);
				if(need <= 0)
					return false;

				out.resize(static_cast<size_t>(need));
				if(::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
										 bytes.data(), size, out.data(), need) != need)
					return false;

				return out.find(L'\0') == std::wstring::npos;
			}

			/*
				`max_bytes` is a cap because these are parsed on the way to
				building a menu. A file above it is Failed rather than empty: an
				oversized list is a machine in a state nobody should write back
				over.

				FILE_FLAG_OPEN_REPARSE_POINT so a link is opened as itself
				rather than followed - a file that steers what a menu does
				should not be redirectable by whatever got to the path first.

				FILE_SHARE_DELETE matters as much as the other two.
				replace() below renames over this path, and a rename cannot
				replace a file another process holds open without it - so a
				reader that omitted it would make every writer fail, which is
				the sharing violation this header exists to remove, moved rather
				than fixed.
			*/
			inline Text read(const std::wstring &path, uint64_t max_bytes)
			{
				Text out;
				if(path.empty())
					return out;			// Failed: nothing was asked of the disk

				auto file = ::CreateFileW(path.c_str(), GENERIC_READ,
										  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
										  nullptr, OPEN_EXISTING,
										  FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
				if(file == INVALID_HANDLE_VALUE)
				{
					auto error = ::GetLastError();
					if(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
						out.state = LoadState::Missing;
					return out;
				}

				LARGE_INTEGER size{};
				if(!::GetFileSizeEx(file, &size) || size.QuadPart < 0
				   || static_cast<uint64_t>(size.QuadPart) > max_bytes
				   || size.QuadPart > INT_MAX)
				{
					::CloseHandle(file);
					return out;			// Failed
				}

				if(size.QuadPart > 0)
				{
					std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
					DWORD got = 0;
					if(!::ReadFile(file, bytes.data(), static_cast<DWORD>(size.QuadPart),
								   &got, nullptr)
					   || got != size.QuadPart
					   || !decode(bytes, out.text))
					{
						::CloseHandle(file);
						out.text.clear();
						return out;		// Failed
					}
				}

				// From this handle, before it is closed, so the stamp describes
				// the bytes just read rather than whatever the file is by now.
				out.write_time = file_time_of(file);
				out.state = LoadState::Loaded;
				::CloseHandle(file);
				return out;
			}

			// Creates the directory, and its parent, if they are not there.
			// Lifted verbatim from the two save() implementations that each had
			// their own copy of it.
			inline void ensure_directory(const std::wstring &path)
			{
				auto cut = path.find_last_of(L"\\/");
				if(cut == std::wstring::npos)
					return;

				auto directory = path.substr(0, cut);
				if(::CreateDirectoryW(directory.c_str(), nullptr)
				   || ::GetLastError() != ERROR_PATH_NOT_FOUND)
					return;

				auto parent = directory.find_last_of(L"\\/");
				if(parent != std::wstring::npos)
					::CreateDirectoryW(directory.substr(0, parent).c_str(), nullptr);
				::CreateDirectoryW(directory.c_str(), nullptr);
			}

			// ReplaceFileW, behind a pointer only so a test can make it fail
			// with a chosen error: the two failure classes below cannot be
			// produced on demand by a real race.
			using ReplaceFileFn = BOOL(WINAPI *)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
			inline ReplaceFileFn replace_file = ::ReplaceFileW;

			/*
				Write `text` to `path`, byte-order mark first, without the
				destination ever being a partial file - and without ever being
				the reason the only copy of the data is gone.

				The temporary is in the same directory on purpose: MoveFileEx
				across volumes "simulates the move by using the CopyFile and
				DeleteFile functions", which is not a replace and not atomic. A
				sibling name keeps it a rename.
				https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw

				Named with the process id and a tick so two hosts staging at the
				same moment do not collide on the temporary itself - which would
				reintroduce, on a different file, exactly the sharing violation
				this removes.
			*/
			inline bool replace(const std::wstring &path, const std::wstring &text)
			{
				if(path.empty())
					return false;

				ensure_directory(path);

				wchar_t suffix[64]{};
				::swprintf_s(suffix, L".%lu-%llu.tmp",
							 ::GetCurrentProcessId(),
							 static_cast<unsigned long long>(::GetTickCount64()));
				auto staged = path + suffix;

				auto file = ::CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr,
										  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if(file == INVALID_HANDLE_VALUE)
					return false;

				std::wstring bytes(1, ByteOrderMark);
				bytes += text;

				auto count = static_cast<DWORD>(bytes.size() * sizeof(wchar_t));
				DWORD written = 0;
				auto ok = ::WriteFile(file, bytes.data(), count, &written, nullptr)
						  && written == count;
				::CloseHandle(file);

				if(!ok)
				{
					::DeleteFileW(staged.c_str());
					return false;
				}

				/*
					ReplaceFile rather than MoveFileEx, and the difference is
					not stylistic - it was measured before it was chosen
					(Windows 11 26200 x64, scratchpad probe):

					  reader sharing READ|WRITE|DELETE
					      MoveFileEx  = 0, ERROR_ACCESS_DENIED
					      ReplaceFile = 1
					  reader sharing READ only
					      MoveFileEx  = 0, ERROR_ACCESS_DENIED
					      ReplaceFile = 0, ERROR_SHARING_VIOLATION

					MoveFileEx fails whenever *any* handle is open on the
					destination, which for a file several hosts read on every
					menu is not an edge case. ReplaceFile documents why it does
					not: the replaced file "is opened with the GENERIC_READ,
					DELETE, and SYNCHRONIZE access rights. The sharing mode is
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE."
					https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew

					It also preserves the replaced file's DACLs, creation time
					and object identifier, which a rename does not - and this
					file lives in the user's own LocalAppData, where inheriting
					a fresh ACL from the directory is a change nobody asked for.

					MoveFileEx is still the fallback for the case ReplaceFile
					cannot serve: there is nothing to replace yet. That is the
					first save on a machine, and no reader can be holding a file
					that does not exist.
				*/
				if(replace_file(path.c_str(), staged.c_str(), nullptr,
								REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
					return true;

				auto error = ::GetLastError();
				if(error == ERROR_FILE_NOT_FOUND)
					return ::MoveFileExW(staged.c_str(), path.c_str(),
										 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
						   || (::DeleteFileW(staged.c_str()), false);

				/*
					With no backup file, these two are the failures that destroy
					the destination:

					  1176 "Otherwise, the replaced file no longer exists and the
					       replacement file exists under its original name."
					  1177 "The replacement file still exists under its original
					       name ... The file to be replaced still exists with a
					       different name."

					Either way the staged file now holds the only copy of the
					content we meant to write, so deleting it - which this did -
					leaves the next reader a Missing file, which reads as an empty
					list that is safe to write over. Finish the job by moving it
					to `path`; if that fails too, keep the staged file. A stray
					temporary is recoverable and a lost list is not.

					Every other error, 1175 included, "the replaced and
					replacement files retain their original file names", so
					`path` is untouched and the staged file is only litter.
				*/
				if(error == ERROR_UNABLE_TO_MOVE_REPLACEMENT
				   || error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2)
					return ::MoveFileExW(staged.c_str(), path.c_str(),
										 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;

				::DeleteFileW(staged.c_str());
				return false;
			}

			/*
				The next contents of a store file: `current`, with only the lines
				the caller's `entries` changed.

				Each line is walked once, in place:

				  - a line that parses to an entry still in `entries` stays where
				    it is - verbatim if its rendering is unchanged, so a person's
				    spelling survives, otherwise re-rendered with the new values;
				  - a later line carrying the same key is a duplicate the parser
				    already ignored, and is dropped;
				  - a line whose key is gone from `entries` is how a removal is
				    written, and is dropped;
				  - everything else - comments, blank lines, lines that do not
				    parse - is kept as written;
				  - entries not in the file yet are appended.

				`max_entries` is the parser's cap: past it the parser never looks
				at a line, so the caller never loaded it and cannot have removed
				it, and it is kept rather than mistaken for a removal.

				`header` goes only into a file that does not exist yet. Lines come
				out CRLF whatever they went in as.

				Entry needs default construction; `parse_line(view, Entry&)`
				returns false for anything that is not an entry; `key_of` and
				`render` read an Entry.
			*/
			template<class Entry, class ParseLine, class KeyOf, class Render>
			inline std::wstring merge(const Text &current, std::wstring_view header,
									  const std::vector<Entry> &entries, size_t max_entries,
									  ParseLine parse_line, KeyOf key_of, Render render)
			{
				std::wstring out;
				if(current.state == LoadState::Missing)
					out = header;

				std::unordered_map<uint32_t, const Entry *> wanted;
				for(const auto &entry : entries)
					wanted.emplace(key_of(entry), &entry);

				std::unordered_set<uint32_t> seen;
				std::wstring_view text(current.text);
				size_t at = 0;
				while(at < text.size())
				{
					auto end = text.find(L'\n', at);
					auto line = text.substr(at, end == std::wstring_view::npos
												? std::wstring_view::npos : end - at);
					at = end == std::wstring_view::npos ? text.size() : end + 1;
					if(!line.empty() && line.back() == L'\r')
						line.remove_suffix(1);

					Entry parsed;
					if(seen.size() < max_entries && parse_line(line, parsed))
					{
						auto key = key_of(parsed);
						if(!seen.insert(key).second)
							continue;

						auto found = wanted.find(key);
						if(found == wanted.end())
							continue;

						auto fresh = render(*found->second);
						if(fresh == render(parsed))
							out += line;
						else
							out += fresh;
						out += L"\r\n";
						continue;
					}

					out += line;
					out += L"\r\n";
				}

				for(const auto &entry : entries)
				{
					if(seen.insert(key_of(entry)).second)
					{
						out += render(entry);
						out += L"\r\n";
					}
				}
				return out;
			}
		}
	}
}
