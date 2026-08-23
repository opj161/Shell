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

	Integrity, not authenticity. Every file is checksummed on the way in and
	checked on the way out, and the manifest that names them is written last
	and swapped atomically, so a torn write is invisible rather than dangerous
	- a half-written shadow has no manifest naming it. A shadow that does not
	verify is refused outright and the process falls back to the ordinary
	"never loaded" refusal.

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

#include <string>
#include <vector>

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
		inline constexpr const wchar_t *ManifestHeader = L"nilesoft-shell-lkg\t1";

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

		// The part of `full` below `directory`, or empty when it is not below it.
		// Comparison is ordinal and case-insensitive, which is how the file
		// system treats these paths.
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
			return ::CreateFileW(path.c_str(), GENERIC_READ,
								 FILE_SHARE_READ, nullptr, OPEN_EXISTING,
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
			unsigned char buffer[64 * 1024];
			for(;;)
			{
				DWORD read = 0;
				if(!::ReadFile(file, buffer, sizeof(buffer), &read, nullptr))
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

		inline std::wstring render(const Manifest &manifest)
		{
			std::wstring out = ManifestHeader;
			out += L'\n';
			if(manifest.partial)
				out += L"partial\n";
			out += L"root";
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
			auto file = open_for_read(path);
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

		inline bool parse_manifest(const std::wstring &text, Manifest &out)
		{
			out = {};
			auto lines = split(text, L'\n');
			if(lines.empty() || lines.front() != ManifestHeader)
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
				if(parts[0] == L"root" && parts.size() == 2)
				{
					out.root = parts[1];
					continue;
				}
				if(parts[0] == L"file" && parts.size() == 4)
				{
					Entry e;
					e.relative = parts[1];
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

			return !out.root.empty() && !out.files.empty();
		}

		// ---- save ----------------------------------------------------------

		// Defined below; save() consults it before deciding an existing shadow
		// is still worth keeping.
		inline std::wstring resolve(const std::wstring &directory);

		/*
			Mirrors the files a successful parse read into `directory`.

			`root` is the configuration's own path and `loaded` is every file
			the parse opened, root included. Files outside the root's directory
			are recorded as making the shadow partial but are not copied -
			mirroring them would need a second root, and an absolute import
			usually resolves the same way wherever it is read from.

			Returns false and leaves any previous shadow in place on failure:
			the manifest is written last, so nothing points at a half-copied
			set. Returns true without writing anything when the manifest would
			be byte-identical to the one already there, which is what stops
			every process on the machine rewriting the same files.
		*/
		inline bool save(const std::wstring &directory,
						 const std::wstring &root,
						 const std::vector<std::wstring> &loaded)
		{
			if(directory.empty() || root.empty() || loaded.empty())
				return false;

			auto cut = root.find_last_of(L"\\/");
			if(cut == std::wstring::npos)
				return false;
			auto config_directory = root.substr(0, cut);

			Manifest manifest;
			manifest.root = root.substr(cut + 1);

			struct Pending { std::wstring source, relative; };
			std::vector<Pending> pending;

			for(const auto &full : loaded)
			{
				auto relative = relative_to(config_directory, full);
				if(relative.empty())
				{
					manifest.partial = true;
					continue;
				}

				bool seen = false;
				for(const auto &p : pending)
					seen = seen || equal_ignoring_case(p.relative, relative);
				if(!seen)
					pending.push_back({ full, relative });
			}

			if(pending.empty())
				return false;

			// Hash everything first. Nothing is written into the shadow until
			// the whole set has been read successfully.
			std::vector<std::pair<Pending, Digest>> hashed;
			hashed.reserve(pending.size());
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
				hashed.push_back({ p, digest });
			}

			// Unchanged since last time: every process that parses the same
			// configuration would otherwise rewrite the same bytes.
			//
			// The manifest matching is not enough on its own. It says the
			// *input* has not changed, not that the shadow is still intact - so
			// a store whose copies were deleted or damaged would be skipped
			// over for as long as the user left their configuration alone,
			// which is exactly as long as there would be nothing to recover
			// from. resolve() re-reads the copies, so the skip is only taken
			// when there is something worth keeping.
			auto rendered = render(manifest);
			std::wstring existing;
			if(read_all(join(directory, ManifestName), existing)
			   && existing == rendered
			   && !resolve(directory).empty())
				return true;

			if(!ensure_directory(directory))
				return false;

			for(const auto &[p, digest] : hashed)
			{
				auto target = join(directory, p.relative);
				auto target_cut = target.find_last_of(L"\\/");
				if(target_cut != std::wstring::npos && !ensure_directory(target.substr(0, target_cut)))
					return false;

				// CopyFileW rather than a hand-rolled loop: the bytes were
				// already hashed above, and the copy is verified on load.
				if(!::CopyFileW(p.source.c_str(), target.c_str(), FALSE))
					return false;
			}

			// The manifest is the commit point, and the only thing that has to
			// land atomically: until it names them, the copies above are
			// unreferenced. MOVEFILE_REPLACE_EXISTING "replaces its contents
			// with the contents of the lpExistingFileName file".
			// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw
			auto staged = join(directory, L"manifest.staging");
			if(!write_all(staged, rendered))
				return false;

			if(!::MoveFileExW(staged.c_str(), join(directory, ManifestName).c_str(),
							  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				::DeleteFileW(staged.c_str());
				return false;
			}

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

			bool root_present = false;
			for(const auto &e : manifest.files)
			{
				auto path = join(directory, e.relative);
				auto file = open_for_read(path);
				if(file == INVALID_HANDLE_VALUE)
					return {};

				Digest digest{};
				unsigned long long size = 0;
				auto ok = digest_of(file, digest, size)
					   && size == e.size
					   && digest == e.digest;
				::CloseHandle(file);
				if(!ok)
					return {};

				root_present = root_present || equal_ignoring_case(e.relative, manifest.root);
			}

			if(!root_present)
				return {};

			return join(directory, manifest.root);
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
