#pragma once

/*
	A copy of the last configuration that parsed cleanly.

	The problem it solves. Initializer::init() parses into a fresh CACHE and
	publishes only on success, so a process that already has a generation
	loaded keeps serving it when a later parse fails - that is the in-memory
	half of last-known-good, and it is enough while Explorer stays up. It is no
	help at all to a process that starts *after* the file was broken: a fresh
	Initializer has nothing in memory, init() fails, and there is no context
	menu until somebody finds and fixes the file.

	So the resolved input set of every successful parse is mirrored to disk.
	A mirror, not a content-addressed blob store, because relative imports are
	rooted against the importing file's own directory:

		if(!rooted) path = Path::Combine(l->location, path);

	Preserving the layout means the shadow parses exactly the way the original
	did, with no redirection table and no parser changes beyond knowing which
	files it read.

	Two properties matter more than the layout, because up to ~two dozen hosts
	reload and save at once after every edit:

	Only what was parsed is saved. The parser stamps each file before reading
	it (ConfigStamp.h) and save() takes those stamps. A file whose stamp has
	moved on by the time save() hashes it, or again after it has been copied,
	is not what was parsed - a newer, possibly half-typed save landed - and
	the previous shadow is kept rather than replaced by something that was
	never known to be good.

	Generations are immutable. Files live under <directory>\<generation>\...,
	where the generation is a digest of the file set, so processes that parsed
	the same configuration agree on the name and a generation is never
	modified after a manifest names it. A set is copied into a private staging
	directory, verified, and renamed into place; the manifest is then the only
	mutable pointer and is swapped atomically. A copy that dies part-way, or
	two hosts saving at once, can therefore not leave a manifest naming
	changed bytes. Superseded generations are deleted later, once old enough
	that no concurrent saver can still be about to name them (prune()).

	Integrity, not authenticity. Every file is checksummed on the way in and
	checked on the way out. A shadow that does not verify is refused outright
	and the process falls back to the ordinary "never loaded" refusal.

	Manifest v1 kept the files directly under <directory> and rewrote them in
	place. resolve() still accepts it, so an upgraded machine keeps its
	fallback until the first v2 save, which then deletes the v1 files.

	The checksum is FNV-1a and deliberately not a cryptographic hash. Against
	tampering it would buy nothing: the manifest sits in the same directory as
	the files it names, so anyone able to rewrite a copy can rewrite its digest
	too. What is actually being caught is truncation, corruption and a
	half-replaced set, and a 64-bit checksum over content plus an exact size
	does that. The alternative would mean linking bcrypt into the DLL, which
	adds a system DLL to the import table of every process that raises a
	context menu - the cost docs/refactor/04-code-health.md section 2 removed
	for d2d1 and dwrite, bought here for no security.

	The content itself is .nss source that gets parsed by the same parser that
	reads the user's own files, not code that gets executed, so the exposure is
	the parser's robustness rather than a new class of trust.

	Reparse points are rejected on both sides (regular_non_reparse), so neither
	saving nor loading can be redirected out of the directory it was told to
	work in.

	docs/refactor/03-config-safety.md section 1b
*/

#include <windows.h>
#include <shlobj.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ConfigStamp.h"
#include "LegacyConfigTransfer.h"

namespace Nilesoft
{
	namespace ConfigShadow
	{
		// FNV-1a, 64-bit. See the note above on why this is not SHA-256.
		using Digest = unsigned long long;

		inline constexpr Digest FnvOffsetBasis = 14695981039346656037ULL;
		inline constexpr Digest FnvPrime = 1099511628211ULL;

		inline constexpr wchar_t Separator = 0x5C;			// backslash
		inline constexpr wchar_t Field = L'\t';				// illegal in a Windows filename
		inline constexpr const wchar_t *ManifestName = L"manifest.lkg";
		inline constexpr const wchar_t *ManifestHeader = L"nilesoft-shell-lkg\t2";
		inline constexpr const wchar_t *LegacyManifestHeader = L"nilesoft-shell-lkg\t1";

		// How old a superseded generation or a leftover staging entry must be
		// before prune() may delete it: long enough for a concurrent saver to
		// have named it, or finished with it. 100 ns units.
		inline constexpr unsigned long long PruneAge = 10ULL * 60ULL * 10000000ULL;

		// A file that was loaded during the parse and lives under the
		// configuration's own directory, so it can be mirrored.
		struct Entry
		{
			std::wstring relative;
			unsigned long long size{};
			Digest digest{};
		};

		struct Manifest
		{
			std::wstring root;			// relative path of the root file, e.g. "shell.nss"
			std::vector<Entry> files;

			// Subdirectory of the store holding the files; empty for a v1
			// manifest, whose files sit directly in the store.
			std::wstring generation;

			// At least one loaded file lived outside the configuration directory
			// and could not be mirrored. The shadow is still worth keeping - an
			// absolute import usually resolves the same way from anywhere - but
			// it is not a complete picture, and if that outside file is the
			// broken one, parsing the shadow will fail too and the caller falls
			// back to refusing.
			bool partial{};

			bool empty() const noexcept { return root.empty(); }
		};

		// ---- small helpers -------------------------------------------------

		inline std::wstring to_hex(Digest d)
		{
			static const wchar_t *digits = L"0123456789abcdef";
			std::wstring out(16, L'0');
			for(int i = 15; i >= 0; i--)
			{
				out[static_cast<size_t>(i)] = digits[d & 0x0F];
				d >>= 4;
			}
			return out;
		}

		inline bool from_hex(const std::wstring &text, Digest &out)
		{
			if(text.size() != 16)
				return false;
			Digest value = 0;
			for(auto c : text)
			{
				Digest nibble = 0;
				if(c >= L'0' && c <= L'9') nibble = static_cast<Digest>(c - L'0');
				else if(c >= L'a' && c <= L'f') nibble = static_cast<Digest>(c - L'a' + 10);
				else return false;
				value = (value << 4) | nibble;
			}
			out = value;
			return true;
		}

		inline bool equal_ignoring_case(const std::wstring &a, const std::wstring &b)
		{
			return a.size() == b.size()
				&& ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
										  b.c_str(), static_cast<int>(b.size()),
										  TRUE) == CSTR_EQUAL;
		}

		inline std::wstring join(const std::wstring &directory, const std::wstring &relative)
		{
			if(directory.empty())
				return relative;
			auto out = directory;
			if(out.back() != Separator && out.back() != L'/')
				out += Separator;
			out += relative;
			return out;
		}

		/*
			The long form of `path`, or `path` unchanged when there is not one.

			relative_to() below decides whether one path is under another by
			comparing their leading characters, and two spellings of the same
			directory do not compare equal. The one that bites is 8.3: a caller
			that passes `C:\Users\RUNNER~1\...` while the parser recorded
			`C:\Users\runneradmin\...` gets no matches at all, every file is
			skipped, `pending` ends up empty and save() answers false. The
			shadow is then silently never written - a last-known-good mechanism
			that has quietly stopped existing, which is the worst way for this
			particular feature to fail.

			Two calls, because the return value means different things:
			"If the lpszLongPath buffer is too small to contain the path, the
			return value is the size, in TCHARs, of the buffer that is required
			to hold the path and the terminating null character", while on
			success it is "the length ... not including the terminating null
			character".
			https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getlongpathnamew

			No tilde shortcut, because the same page says not to take one: "not
			all file systems follow this convention. Therefore, do not assume
			that you can skip calling GetLongPathName if the path does not
			contain a tilde (~) character."

			Zero means the path could not be queried - it does not exist, or a
			parent directory is not listable - and the caller's string is then
			the best answer available, which is also what the function itself
			does when the file exists but has no long form.
		*/
		inline std::wstring long_form(const std::wstring &path)
		{
			if(path.empty())
				return path;

			auto needed = ::GetLongPathNameW(path.c_str(), nullptr, 0);
			if(needed == 0)
				return path;

			std::wstring out(needed, L'\0');
			auto length = ::GetLongPathNameW(path.c_str(), out.data(), needed);
			if(length == 0 || length >= needed)
				return path;

			out.resize(length);
			return out;
		}

		// The part of `full` below `directory`, or empty when it is not below it.
		// Comparison is ordinal and case-insensitive, which is how the file
		// system treats these paths.
		//
		// Both sides must already be in the same spelling; save() puts them
		// there with long_form() above.
		inline std::wstring relative_to(const std::wstring &directory, const std::wstring &full)
		{
			if(directory.empty() || full.size() <= directory.size())
				return {};

			auto head = full.substr(0, directory.size());
			if(!equal_ignoring_case(head, directory))
				return {};

			auto next = full[directory.size()];
			if(next != Separator && next != L'/')
				return {};

			return full.substr(directory.size() + 1);
		}

		// Creates `path` and every missing parent. Existing directories are not
		// an error; anything else is.
		inline bool ensure_directory(const std::wstring &path)
		{
			if(path.empty())
				return false;
			if(::CreateDirectoryW(path.c_str(), nullptr))
				return true;

			auto err = ::GetLastError();
			if(err == ERROR_ALREADY_EXISTS)
				return true;
			if(err != ERROR_PATH_NOT_FOUND)
				return false;

			auto cut = path.find_last_of(L"\\/");
			if(cut == std::wstring::npos || cut == 0)
				return false;
			if(!ensure_directory(path.substr(0, cut)))
				return false;

			return ::CreateDirectoryW(path.c_str(), nullptr)
				|| ::GetLastError() == ERROR_ALREADY_EXISTS;
		}

		inline HANDLE open_for_read(const std::wstring &path)
		{
			// FILE_FLAG_OPEN_REPARSE_POINT so a link is opened as itself and
			// rejected below rather than silently followed somewhere else.
			// Every share mode, because save() hashes the user's own files and must
			// not make an editor's save fail; a write that lands anyway is what the
			// stamp and verify() checks around the hash catch.
			return ::CreateFileW(path.c_str(), GENERIC_READ,
								 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
								 FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		}

		inline bool file_size_of(HANDLE file, unsigned long long &out)
		{
			LARGE_INTEGER size{};
			if(!::GetFileSizeEx(file, &size) || size.QuadPart < 0)
				return false;
			out = static_cast<unsigned long long>(size.QuadPart);
			return true;
		}

		// Reads the whole file and checksums it. Rejects anything that is not
		// an ordinary on-disk file, so neither saving nor loading can be
		// redirected through a link.
		inline bool digest_of(HANDLE file, Digest &out, unsigned long long &size)
		{
			if(!LegacyConfigTransfer::regular_non_reparse(file)
			   || !file_size_of(file, size)
			   || !LegacyConfigTransfer::rewind(file))
				return false;

			Digest value = FnvOffsetBasis;

			// Heap for the same reason as LegacyConfigTransfer::hash_and_copy:
			// a 65,576-byte frame (MSVC /analyze C6262, default threshold
			// 16 KiB) on a thread whose stack belongs to an arbitrary host
			// process. https://learn.microsoft.com/en-us/cpp/code-quality/c6262?view=msvc-170
			constexpr DWORD ChunkBytes = 64 * 1024;
			auto storage = std::make_unique<unsigned char[]>(ChunkBytes);
			auto buffer = storage.get();

			for(;;)
			{
				DWORD read = 0;
				if(!::ReadFile(file, buffer, ChunkBytes, &read, nullptr))
					return false;
				if(read == 0)
					break;
				for(DWORD i = 0; i < read; i++)
				{
					value ^= buffer[i];
					value *= FnvPrime;
				}
			}

			out = value;
			return true;
		}

		// ---- manifest ------------------------------------------------------

		// The lines that identify a file set: root and files, not the header
		// or the partial flag. The generation name is a digest of exactly
		// these, so every process that parsed the same configuration derives
		// the same name.
		inline std::wstring render_body(const Manifest &manifest)
		{
			std::wstring out = L"root";
			out += Field;
			out += manifest.root;
			out += L'\n';
			for(const auto &e : manifest.files)
			{
				out += L"file";
				out += Field;
				out += e.relative;
				out += Field;
				out += std::to_wstring(e.size);
				out += Field;
				out += to_hex(e.digest);
				out += L'\n';
			}
			return out;
		}

		inline Digest fnv_text(const std::wstring &text)
		{
			Digest value = FnvOffsetBasis;
			for(auto c : text)
			{
				value ^= static_cast<Digest>(c & 0xFF);
				value *= FnvPrime;
				value ^= static_cast<Digest>((c >> 8) & 0xFF);
				value *= FnvPrime;
			}
			return value;
		}

		inline std::wstring render(const Manifest &manifest)
		{
			std::wstring out = ManifestHeader;
			out += L'\n';
			out += L"generation";
			out += Field;
			out += manifest.generation;
			out += L'\n';
			if(manifest.partial)
				out += L"partial\n";
			out += render_body(manifest);
			return out;
		}

		inline bool write_all(const std::wstring &path, const std::wstring &text)
		{
			auto file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
									  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if(file == INVALID_HANDLE_VALUE)
				return false;

			auto bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
			DWORD written = 0;
			auto ok = bytes == 0
				|| (::WriteFile(file, text.data(), bytes, &written, nullptr) && written == bytes);
			::CloseHandle(file);
			return ok;
		}

		inline bool read_all(const std::wstring &path, std::wstring &text)
		{
			// FILE_SHARE_DELETE as well, because this reads the manifest and a
			// saver replaces it by rename: MoveFileEx with REPLACE_EXISTING
			// needs delete access to the target, which an open handle without
			// FILE_SHARE_DELETE denies, so a reader mid-read would fail
			// somebody's commit.
			// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew
			auto file = ::CreateFileW(path.c_str(), GENERIC_READ,
									  FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
									  FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if(file == INVALID_HANDLE_VALUE)
				return false;

			unsigned long long size = 0;
			auto ok = LegacyConfigTransfer::regular_non_reparse(file)
				   && file_size_of(file, size)
				   && size % sizeof(wchar_t) == 0
				   && size < 4u * 1024u * 1024u;

			if(ok)
			{
				text.assign(static_cast<size_t>(size / sizeof(wchar_t)), L'\0');
				DWORD read = 0;
				ok = size == 0
					|| (::ReadFile(file, text.data(), static_cast<DWORD>(size), &read, nullptr)
						&& read == size);
			}

			::CloseHandle(file);
			return ok;
		}

		inline std::vector<std::wstring> split(const std::wstring &line, wchar_t sep)
		{
			std::vector<std::wstring> out;
			size_t start = 0;
			for(;;)
			{
				auto cut = line.find(sep, start);
				if(cut == std::wstring::npos)
				{
					out.push_back(line.substr(start));
					return out;
				}
				out.push_back(line.substr(start, cut - start));
				start = cut + 1;
			}
		}

		// A manifest names files by path relative to a directory this code
		// then reads - and, for v1 cleanup, deletes - so it must not be able to
		// name anything outside it: no drive, no rooted path, no "." or ".."
		// segment, no empty one.
		inline bool safe_relative(const std::wstring &relative)
		{
			if(relative.empty() || relative.find(L':') != std::wstring::npos)
				return false;

			size_t start = 0;
			for(;;)
			{
				auto cut = relative.find_first_of(L"\\/", start);
				auto part = relative.substr(start, cut == std::wstring::npos ? cut : cut - start);
				if(part.empty() || part == L"." || part == L"..")
					return false;
				if(cut == std::wstring::npos)
					return true;
				start = cut + 1;
			}
		}

		inline bool parse_manifest(const std::wstring &text, Manifest &out)
		{
			out = {};
			auto lines = split(text, L'\n');
			if(lines.empty())
				return false;

			bool v2 = lines.front() == ManifestHeader;
			if(!v2 && lines.front() != LegacyManifestHeader)
				return false;

			for(size_t i = 1; i < lines.size(); i++)
			{
				const auto &line = lines[i];
				if(line.empty())
					continue;

				if(line == L"partial")
				{
					out.partial = true;
					continue;
				}

				auto parts = split(line, Field);
				if(v2 && parts[0] == L"generation" && parts.size() == 2)
				{
					Digest ignored{};
					if(!from_hex(parts[1], ignored))
						return false;
					out.generation = parts[1];
					continue;
				}
				if(parts[0] == L"root" && parts.size() == 2)
				{
					if(!safe_relative(parts[1]))
						return false;
					out.root = parts[1];
					continue;
				}
				if(parts[0] == L"file" && parts.size() == 4)
				{
					Entry e;
					e.relative = parts[1];
					if(!safe_relative(e.relative))
						return false;
					try
					{
						e.size = std::stoull(parts[2]);
					}
					catch(...)
					{
						return false;
					}
					if(!from_hex(parts[3], e.digest))
						return false;
					out.files.push_back(std::move(e));
					continue;
				}

				// An unknown line means a manifest this build does not
				// understand. Refuse it rather than guess.
				return false;
			}

			if(v2 && out.generation.empty())
				return false;

			return !out.root.empty() && !out.files.empty();
		}

		// Every file the manifest names, under `base`, has its recorded size
		// and digest, and the root is among them.
		inline bool verify(const std::wstring &base, const Manifest &manifest)
		{
			bool root_present = false;
			for(const auto &e : manifest.files)
			{
				auto file = open_for_read(join(base, e.relative));
				if(file == INVALID_HANDLE_VALUE)
					return false;

				Digest digest{};
				unsigned long long size = 0;
				auto ok = digest_of(file, digest, size)
					   && size == e.size
					   && digest == e.digest;
				::CloseHandle(file);
				if(!ok)
					return false;

				root_present = root_present || equal_ignoring_case(e.relative, manifest.root);
			}

			return root_present;
		}

		// ---- deleting ------------------------------------------------------

		inline unsigned long long file_time_value(const FILETIME &t)
		{
			return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
		}

		// A link is removed as the entry it is and never entered:
		// RemoveDirectory "can be used to remove a directory junction ... will
		// remove the specified link regardless of whether the target directory
		// is empty or not", and the target is not affected.
		// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-removedirectorya
		// FindFirstFile reports a link's own attributes, so `attributes` here
		// is never about a target.
		inline void remove_entry(const std::wstring &path, DWORD attributes)
		{
			if(!(attributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				::DeleteFileW(path.c_str());
				return;
			}

			if(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
			{
				WIN32_FIND_DATAW find{};
				auto handle = ::FindFirstFileW(join(path, L"*").c_str(), &find);
				if(handle != INVALID_HANDLE_VALUE)
				{
					do
					{
						std::wstring name = find.cFileName;
						if(name != L"." && name != L"..")
							remove_entry(join(path, name), find.dwFileAttributes);
					}
					while(::FindNextFileW(handle, &find));
					::FindClose(handle);
				}
			}

			::RemoveDirectoryW(path.c_str());
		}

		// Removes `path` if it exists, whatever it is. GetFileAttributes
		// "returns attributes for the symbolic link" when given one.
		// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfileattributesexw
		inline void remove_tree(const std::wstring &path)
		{
			auto attributes = ::GetFileAttributesW(path.c_str());
			if(attributes != INVALID_FILE_ATTRIBUTES)
				remove_entry(path, attributes);
		}

		inline bool is_generation_name(const std::wstring &name)
		{
			Digest ignored{};
			return from_hex(name, ignored);
		}

		inline bool ends_with(const std::wstring &text, const wchar_t *suffix)
		{
			std::wstring tail = suffix;
			return text.size() >= tail.size()
				&& text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
		}

		// A name no other process can have picked: process id plus tick.
		inline std::wstring unique_suffix()
		{
			return std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
		}

		/*
			Best-effort cleanup of what a committed save leaves behind:
			generations nothing names any more, and staging entries whose owner
			died. Never fails and never throws; whatever it cannot delete the
			next save tries again.

			Only direct children of `directory`, and only names this code
			creates, so a user's file that happens to live there is never
			touched. `keep` is the generation the caller just committed. The
			generation the manifest names *now* is also kept, because another
			process may have committed a newer one in between.

			`min_age` guards the race this cannot see: a process that has
			chosen an existing generation but not yet named it in the manifest.
			Nothing younger than that is deleted, and save() refreshes a reused
			generation's time for the same reason.
		*/
		inline void prune(const std::wstring &directory, const std::wstring &keep,
						  unsigned long long min_age = PruneAge)
		{
			// Without a readable manifest we do not know what is named, and
			// the only safe answer is to delete nothing.
			std::wstring text;
			Manifest named;
			if(!read_all(join(directory, ManifestName), text) || !parse_manifest(text, named))
				return;

			FILETIME now{};
			::GetSystemTimeAsFileTime(&now);
			auto now_value = file_time_value(now);

			WIN32_FIND_DATAW find{};
			auto handle = ::FindFirstFileW(join(directory, L"*").c_str(), &find);
			if(handle == INVALID_HANDLE_VALUE)
				return;

			do
			{
				std::wstring name = find.cFileName;
				auto written = file_time_value(find.ftLastWriteTime);
				if((now_value > written ? now_value - written : 0) < min_age)
					continue;

				bool is_directory = (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
				bool superseded = is_directory && is_generation_name(name)
							   && name != keep && name != named.generation;
				bool stale_staging_directory = is_directory && name.size() > 17
							   && is_generation_name(name.substr(0, 16)) && name[16] == L'.'
							   && ends_with(name, L".tmp");
				bool stale_staging_manifest = !is_directory && name.compare(0, 9, L"manifest.") == 0
							   && ends_with(name, L".staging");

				if(superseded || stale_staging_directory || stale_staging_manifest)
					remove_entry(join(directory, name), find.dwFileAttributes);
			}
			while(::FindNextFileW(handle, &find));
			::FindClose(handle);
		}

		// v1 kept its files directly under `directory`; once a v2 manifest has
		// replaced the v1 one nothing names them. Only the files that manifest
		// listed are deleted, then the directories that emptied, never above
		// `directory` and never through a link.
		inline void remove_legacy(const std::wstring &directory, const Manifest &legacy)
		{
			for(const auto &e : legacy.files)
			{
				auto path = join(directory, e.relative);

				bool linked = false;
				auto walk = directory;
				size_t start = 0;
				for(;;)
				{
					auto cut = e.relative.find_first_of(L"\\/", start);
					walk = join(walk, e.relative.substr(start, cut == std::wstring::npos ? cut : cut - start));
					auto attributes = ::GetFileAttributesW(walk.c_str());
					linked = linked || (attributes != INVALID_FILE_ATTRIBUTES
										&& (attributes & FILE_ATTRIBUTE_REPARSE_POINT));
					if(cut == std::wstring::npos)
						break;
					start = cut + 1;
				}
				if(linked)
					continue;

				::DeleteFileW(path.c_str());
				for(auto parent = path.substr(0, path.find_last_of(L"\\/"));
					!relative_to(directory, parent).empty();
					parent = parent.substr(0, parent.find_last_of(L"\\/")))
				{
					if(!::RemoveDirectoryW(parent.c_str()))
						break;
				}
			}
		}

		// ---- save ----------------------------------------------------------

		// Sets a directory's last-write time to now, so a generation that was
		// just chosen for reuse is not "old" to a concurrent prune().
		// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfiletime
		inline void touch_directory(const std::wstring &path)
		{
			auto handle = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
										FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
										nullptr, OPEN_EXISTING,
										FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if(handle == INVALID_HANDLE_VALUE)
				return;

			FILETIME now{};
			::GetSystemTimeAsFileTime(&now);
			::SetFileTime(handle, nullptr, nullptr, &now);
			::CloseHandle(handle);
		}

		/*
			Mirrors the files a successful parse read into `directory`.

			`root` is the configuration's own path and `loaded` is every file
			the parse opened, root included. `stamps[i]` is
			Shell::config_file_stamp(loaded[i]) taken by the parser *before* it
			read that file. Files outside the root's directory are recorded as
			making the shadow partial but are not copied - mirroring them would
			need a second root, and an absolute import usually resolves the same
			way wherever it is read from.

			Returns false and leaves the previous shadow serving when the
			stamps and `loaded` disagree in size, when any file no longer has
			the stamp it was parsed with (before it is hashed, or after it has
			been copied: a write landed, so what is on disk is not what was
			parsed), or on any I/O failure. Returns true without writing when
			the identical set is already the shadow and still verifies, which is
			what stops every process on the machine rewriting the same files.

			MoveFileEx moves a directory "including its children", and "when
			moving a directory, the destination must be on the same drive" -
			the staging directory is a sibling of the generation, so it is.
			Without MOVEFILE_REPLACE_EXISTING it does not replace an existing
			destination, which is what makes the first committer win.
			https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw
		*/
		inline bool save(const std::wstring &directory,
						 const std::wstring &root,
						 const std::vector<std::wstring> &loaded,
						 const std::vector<uint64_t> &stamps)
		{
			if(directory.empty() || root.empty() || loaded.empty() || loaded.size() != stamps.size())
				return false;

			// One spelling for everything that is about to be compared against
			// everything else. The caller's `root` and the parser's `loaded`
			// come from different places and need not agree on short versus
			// long form; see long_form() for what that used to cost.
			auto normalized_root = long_form(root);

			auto cut = normalized_root.find_last_of(L"\\/");
			if(cut == std::wstring::npos)
				return false;
			auto config_directory = normalized_root.substr(0, cut);

			Manifest manifest;
			manifest.root = normalized_root.substr(cut + 1);

			struct Pending { std::wstring source, relative; uint64_t stamp; };
			std::vector<Pending> pending;

			for(size_t i = 0; i < loaded.size(); i++)
			{
				const auto &full = loaded[i];

				// `source` stays as the caller gave it - both spellings open
				// the same file - while the comparison and the shadow's own
				// layout use the long one.
				auto relative = relative_to(config_directory, long_form(full));
				if(relative.empty())
				{
					manifest.partial = true;
					continue;
				}

				bool seen = false;
				for(const auto &p : pending)
				{
					if(!equal_ignoring_case(p.relative, relative))
						continue;
					// The same file read twice under two stamps changed in
					// between; neither read is the file as it is now.
					if(p.stamp != stamps[i])
						return false;
					seen = true;
				}
				if(!seen)
					pending.push_back({ full, relative, stamps[i] });
			}

			if(pending.empty())
				return false;

			auto unchanged = [&pending]()
			{
				for(const auto &p : pending)
					if(Nilesoft::Shell::config_file_stamp(p.source) != p.stamp)
						return false;
				return true;
			};

			if(!unchanged())
				return false;

			// Hash everything first. Nothing is written until the whole set
			// has been read successfully.
			for(const auto &p : pending)
			{
				auto file = open_for_read(p.source);
				if(file == INVALID_HANDLE_VALUE)
					return false;

				Digest digest{};
				unsigned long long size = 0;
				auto ok = digest_of(file, digest, size);
				::CloseHandle(file);
				if(!ok)
					return false;

				Entry e;
				e.relative = p.relative;
				e.size = size;
				e.digest = digest;
				manifest.files.push_back(std::move(e));
			}

			manifest.generation = to_hex(fnv_text(render_body(manifest)));
			auto generation_path = join(directory, manifest.generation);

			// Unchanged since last time: every process that parses the same
			// configuration would otherwise rewrite the same bytes.
			//
			// The manifest matching is not enough on its own. It says the
			// *input* has not changed, not that the shadow is still intact - so
			// a store whose copies were deleted or damaged would be skipped
			// over for as long as the user left their configuration alone,
			// which is exactly as long as there would be nothing to recover
			// from. So the generation is re-read as well.
			auto rendered = render(manifest);
			auto manifest_path = join(directory, ManifestName);
			std::wstring existing;
			read_all(manifest_path, existing);
			if(existing == rendered && verify(generation_path, manifest))
				return true;

			if(!ensure_directory(directory))
				return false;

			// The generation does not exist, or does not verify. Build it
			// privately and rename it into place, so no other process ever
			// sees a half-copied one under its final name.
			if(!verify(generation_path, manifest))
			{
				auto staging = join(directory, manifest.generation + L"." + unique_suffix() + L".tmp");

				bool ok = ensure_directory(staging);
				for(size_t i = 0; ok && i < pending.size(); i++)
				{
					auto target = join(staging, pending[i].relative);
					ok = ensure_directory(target.substr(0, target.find_last_of(L"\\/")))
					  && ::CopyFileW(pending[i].source.c_str(), target.c_str(), FALSE);
				}

				// The copy is what gets named, so it is the copy that is
				// checked against what was hashed: a write between the hash
				// and the copy shows up here.
				ok = ok && verify(staging, manifest);

				if(ok && !::MoveFileExW(staging.c_str(), generation_path.c_str(), 0))
				{
					// Someone else committed this generation first - fine if
					// theirs is intact. If it is damaged, replace it, but only
					// when it is a real directory: whatever else sits at that
					// name is not ours to delete.
					auto attributes = ::GetFileAttributesW(generation_path.c_str());
					if(verify(generation_path, manifest))
						ok = true;
					else if(attributes != INVALID_FILE_ATTRIBUTES
							&& (attributes & FILE_ATTRIBUTE_DIRECTORY)
							&& !(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
					{
						remove_entry(generation_path, attributes);
						ok = ::MoveFileExW(staging.c_str(), generation_path.c_str(), 0) != 0;
					}
					else
						ok = false;
				}

				remove_tree(staging);		// nothing left to remove after a successful move
				if(!ok)
					return false;
			}

			touch_directory(generation_path);

			// A write landed while we were saving: what is on disk is not what
			// was parsed. The generation just built is valid for the bytes it
			// holds and prune() will collect it; it is simply not named.
			if(!unchanged())
				return false;

			// The manifest is the commit point, and the only thing that has to
			// land atomically. MOVEFILE_REPLACE_EXISTING "replaces its contents
			// with the contents of the lpExistingFileName file". The staging
			// name is per process so two savers never write the same file.
			auto staged = join(directory, L"manifest." + unique_suffix() + L".staging");
			if(!write_all(staged, rendered))
			{
				::DeleteFileW(staged.c_str());
				return false;
			}

			if(!::MoveFileExW(staged.c_str(), manifest_path.c_str(),
							  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				::DeleteFileW(staged.c_str());
				return false;
			}

			Manifest previous;
			if(parse_manifest(existing, previous) && previous.generation.empty())
				remove_legacy(directory, previous);

			prune(directory, manifest.generation);
			return true;
		}

		// ---- load ----------------------------------------------------------

		/*
			The shadow's root file, or empty when there is no shadow that
			verifies. Every named file is checked for size and content before
			the root is offered, so a shadow that was damaged, truncated or
			partly replaced is refused rather than parsed.
		*/
		inline std::wstring resolve(const std::wstring &directory)
		{
			if(directory.empty())
				return {};

			std::wstring text;
			if(!read_all(join(directory, ManifestName), text))
				return {};

			Manifest manifest;
			if(!parse_manifest(text, manifest))
				return {};

			auto base = manifest.generation.empty() ? directory : join(directory, manifest.generation);
			if(!verify(base, manifest))
				return {};

			return join(base, manifest.root);
		}

		// %LocalAppData%\Nilesoft\Shell\lkg
		//
		// SHGetKnownFolderPath's caller "is responsible for freeing this
		// resource once it is no longer needed by calling CoTaskMemFree,
		// whether SHGetKnownFolderPath succeeds or not", and "the returned path
		// does not include a trailing backslash".
		// https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath
		inline std::wstring default_directory()
		{
			PWSTR local = nullptr;
			auto hr = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local);
			std::wstring out;
			if(SUCCEEDED(hr) && local)
			{
				out = local;
				out += Separator;
				out += L"Nilesoft";
				out += Separator;
				out += L"Shell";
				out += Separator;
				out += L"lkg";
			}
			::CoTaskMemFree(local);
			return out;
		}
	}
}
