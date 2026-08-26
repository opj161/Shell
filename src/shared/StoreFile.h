#pragma once

/*
	Reading and replacing the two small files Shell keeps beside a user's
	config: favorites.txt and quarantine.txt.

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
	a temporary in the same directory and renaming over the target removes the
	window entirely - there is no moment at which the destination is a
	half-written or zero-length file.

	    MOVEFILE_REPLACE_EXISTING: "If a file named lpNewFileName exists, the
	    function replaces its contents with the contents of the
	    lpExistingFileName file"
	    MOVEFILE_WRITE_THROUGH: "The function does not return until the file is
	    actually moved on the disk"
	    https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw

	Which is the same swap ConfigShadow.h already performs for the
	last-known-good manifest, for the same reason.

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

	Kept in src/shared because shell.exe's -quarantine and -favorites commands
	read and write the same files as the DLL does, and a rule about who may
	write is not a rule if only one of them follows it.
*/

#include <windows.h>

#include <string>

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
				`max_bytes` is a cap because these are parsed on the way to
				building a menu. A file above it is Failed rather than empty: an
				oversized list is a machine in a state nobody should write back
				over.

				FILE_FLAG_OPEN_REPARSE_POINT so a link is opened as itself
				rather than followed - a file that steers what a menu does
				should not be redirectable by whatever got to the path first.

				FILE_SHARE_DELETE matters as much as the other two and is new.
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
				   || (size.QuadPart % sizeof(wchar_t)) != 0)
				{
					::CloseHandle(file);
					return out;			// Failed
				}

				if(size.QuadPart > 0)
				{
					std::wstring text(static_cast<size_t>(size.QuadPart / sizeof(wchar_t)), L'\0');
					DWORD got = 0;
					if(!::ReadFile(file, text.data(), static_cast<DWORD>(size.QuadPart),
								   &got, nullptr)
					   || got != size.QuadPart)
					{
						::CloseHandle(file);
						return out;		// Failed
					}

					std::wstring_view view(text);
					if(!view.empty() && view.front() == ByteOrderMark)
						view.remove_prefix(1);
					out.text.assign(view);
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

			/*
				Write `text` to `path`, byte-order mark first, without the
				destination ever being a partial file.

				The temporary is in the same directory on purpose: MoveFileEx
				across volumes "simulates the move by using the CopyFile and
				DeleteFile functions", which is not a replace and not atomic. A
				sibling name keeps it a rename.

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
				if(::ReplaceFileW(path.c_str(), staged.c_str(), nullptr,
								  REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
					return true;

				if(::GetLastError() == ERROR_FILE_NOT_FOUND
				   && ::MoveFileExW(staged.c_str(), path.c_str(),
									MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
					return true;

				// On ERROR_UNABLE_TO_MOVE_REPLACEMENT and its two siblings the
				// staged file may already be gone or renamed; deleting what is
				// not there is harmless and leaving one behind is not.
				::DeleteFileW(staged.c_str());
				return false;
			}
		}
	}
}
