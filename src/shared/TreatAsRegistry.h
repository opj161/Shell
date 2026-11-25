#pragma once

// The Windows 11 primary-menu redirect: a TreatAs value on the Explorer
// context-menu class {86ca1aa0-...} that names Shell's CLSID.
//
// This state machine - read it, create it if absent, remove it if ours, and
// borrow write access to the protected parent key when the plain write is
// refused - used to exist twice, in src/exe/src/Main.cpp and in the installer's
// custom action, and had already started to diverge (the custom action's create
// reported whether it made the key, the exe's did not). It lives here, beside
// BorrowedKeyAccess.h, for the same reason that file does: two callers need
// exactly this and cannot be allowed to drift apart.
//
// The callers keep their own reporting. Every function returns a Win32 status
// and the caller maps it to its own message (a log line in the exe, an MSI
// message in the custom action); the borrow reports through the callback it is
// handed.
//
//   https://learn.microsoft.com/windows/win32/com/treatas
//   https://learn.microsoft.com/windows/win32/sysinfo/merged-view-of-hkey-classes-root
//   https://learn.microsoft.com/windows/win32/api/winreg/nf-winreg-regcreatekeyexw
//   https://learn.microsoft.com/windows/win32/api/winreg/nf-winreg-regdeletekeyexw

#include <windows.h>
#include "BorrowedKeyAccess.h"

namespace Nilesoft
{
	namespace TreatAsRegistry
	{
		// HKLM, 64-bit view: the redirect only means something to a 64-bit
		// Explorer, so a 32-bit caller must not read or write the WOW64 view.
		inline constexpr wchar_t Parent[] =
			L"SOFTWARE\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}";
		inline constexpr wchar_t Key[] =
			L"SOFTWARE\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\TreatAs";

		// The one definition of the redirect target. src/exe asserts it equals
		// CLS_ContextMenu, which is what RegistryConfig registers.
		inline constexpr wchar_t TargetClsid[] = L"{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}";

		constexpr bool same_text(const wchar_t *a, const wchar_t *b)
		{
			for(; *a && *a == *b; ++a, ++b) {}
			return *a == *b;
		}

		enum class State
		{
			absent,
			ours,
			foreign,
			inaccessible
		};

		// Returned when the borrow itself could not be acquired. BorrowedKeyAccess
		// has already said which step failed through the report callback; this only
		// tells the caller it was the borrow and not the write that failed.
		inline constexpr LSTATUS BorrowFailed = ERROR_PRIVILEGE_NOT_HELD;

		namespace detail
		{
			struct Handle
			{
				HKEY key{};
				Handle() = default;
				Handle(const Handle &) = delete;
				Handle &operator=(const Handle &) = delete;
				~Handle() { if(key) ::RegCloseKey(key); }
			};
		}

		// `root`/`path` exist for the test, which cannot write HKLM.
		inline State Query(LSTATUS *error = nullptr, HKEY root = HKEY_LOCAL_MACHINE, const wchar_t *path = Key)
		{
			detail::Handle key;
			auto rc = ::RegOpenKeyExW(root, path, 0,
				KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, &key.key);
			if(rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND)
			{
				if(error) *error = ERROR_SUCCESS;
				return State::absent;
			}
			if(rc != ERROR_SUCCESS)
			{
				if(error) *error = rc;
				return State::inaccessible;
			}

			DWORD subkeys = 0;
			DWORD values = 0;
			rc = ::RegQueryInfoKeyW(key.key, nullptr, nullptr, nullptr, &subkeys, nullptr,
				                     nullptr, &values, nullptr, nullptr, nullptr, nullptr);
			if(rc != ERROR_SUCCESS)
			{
				if(error) *error = rc;
				return State::inaccessible;
			}

			wchar_t value[64]{};
			DWORD bytes = sizeof(value);
			rc = ::RegGetValueW(key.key, nullptr, nullptr,
				RRF_RT_REG_SZ | RRF_ZEROONFAILURE, nullptr, value, &bytes);
			// A default that is absent, not a REG_SZ, or longer than any CLSID was
			// read fine; it is just not ours, and a redirect somebody else wrote is
			// left alone rather than failing an uninstall. RegGetValue reports the
			// latter two as failures: "If the data type of the value does not meet
			// this criteria, the function fails", and a buffer that is "not large
			// enough to hold the data ... the function returns ERROR_MORE_DATA".
			// https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-reggetvaluew
			if(rc == ERROR_FILE_NOT_FOUND || rc == ERROR_UNSUPPORTED_TYPE || rc == ERROR_MORE_DATA)
			{
				if(error) *error = ERROR_SUCCESS;
				return State::foreign;
			}
			if(rc != ERROR_SUCCESS)
			{
				if(error) *error = rc;
				return State::inaccessible;
			}

			if(error) *error = ERROR_SUCCESS;
			if(subkeys == 0 && values == 1
			   && ::CompareStringOrdinal(value, -1, TargetClsid, -1, TRUE) == CSTR_EQUAL)
				return State::ours;

			return State::foreign;
		}

		namespace detail
		{
			// created is set only when this invocation both created the key
			// (REG_CREATED_NEW_KEY) and wrote our CLSID. Opening an already-ours key
			// is success with created=false, so a rollback must not delete it.
			//
			//   https://learn.microsoft.com/windows/win32/api/winreg/nf-winreg-regcreatekeyexw
			inline LSTATUS create_unborrowed(bool *created)
			{
				if(created)
					*created = false;

				Handle parent;
				auto rc = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, Parent, 0,
					KEY_CREATE_SUB_KEY | KEY_WOW64_64KEY, &parent.key);
				if(rc != ERROR_SUCCESS)
					return rc;

				Handle key;
				DWORD disposition = 0;
				rc = ::RegCreateKeyExW(parent.key, L"TreatAs", 0, nullptr, REG_OPTION_NON_VOLATILE,
					KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY, nullptr, &key.key, &disposition);
				if(rc != ERROR_SUCCESS)
					return rc;

				if(disposition != REG_CREATED_NEW_KEY)
					return Query() == State::ours ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS;

				rc = ::RegSetValueExW(key.key, nullptr, 0, REG_SZ,
					reinterpret_cast<const BYTE *>(TargetClsid), sizeof(TargetClsid));
				if(rc != ERROR_SUCCESS)
				{
					// We created this empty key in this call, so taking it back is not
					// a recursive/shared-key deletion.
					::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, Key, KEY_WOW64_64KEY, 0);
					return rc;
				}
				if(created)
					*created = true;
				return ERROR_SUCCESS;
			}

			inline LSTATUS remove_unborrowed()
			{
				LSTATUS error = ERROR_SUCCESS;
				auto state = Query(&error);
				if(state == State::absent)
					return ERROR_SUCCESS;
				if(state == State::foreign)
					return ERROR_ALREADY_EXISTS;
				if(state == State::inaccessible)
					return error;

				return ::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, Key, KEY_WOW64_64KEY, 0);
			}
		}

		// The borrow lives inside Create and Remove rather than at their call sites,
		// so a later caller cannot forget it. Both try the plain write first: on a
		// machine whose CLSID key was already widened by some earlier install that
		// succeeds and nothing touches the security descriptor at all.
		//
		// ERROR_SUCCESS            the redirect is ours (created is true only if this
		//                          call made it)
		// ERROR_ALREADY_EXISTS     a foreign TreatAs is there; left alone
		// BorrowFailed             write access could not be borrowed
		// ERROR_NOT_ALL_ASSIGNED   created, but the key's owner or DACL was not put
		//                          back
		// ERROR_WRITE_FAULT        the write reported success but the real key did
		//                          not change, even with borrowed access
		// anything else            the registry's own error
		/*
			A refused write is not always refused. Inside msiexec's custom-action
			server (Windows 11 26300 x64, measured with NtQueryKey on the handle
			RegCreateKeyExW returned) a write this key's ACL denies is redirected
			to HKLM\SOFTWARE\Volatile\00\MACHINE\SOFTWARE\Classes\...: the open
			for KEY_CREATE_SUB_KEY succeeds, the create succeeds - or, on a later
			run, "opens" the redirected key left from the last one - and the real
			key never changes, so Explorer never sees a redirect. The same call
			from an elevated shell or a SYSTEM scheduled task is refused with
			ERROR_ACCESS_DENIED as the ACL says. This is undocumented, so success
			is judged by the real key read back, not by the write's return value,
			and anything short of a real "ours" takes the borrowed path, whose
			writes the ACL allows and which therefore land on the real key.
		*/
		inline LSTATUS Create(Security::BorrowReport report, bool *created = nullptr)
		{
			auto rc = detail::create_unborrowed(created);
			if(rc == ERROR_SUCCESS && Query() == State::ours)
				return rc;
			if(rc != ERROR_SUCCESS && rc != ERROR_ACCESS_DENIED && rc != ERROR_ALREADY_EXISTS)
				return rc;
			if(created)
				*created = false;

			// Expected on a stock machine: {86ca1aa0-...} is owned by
			// NT SERVICE\TrustedInstaller and Administrators hold ReadKey.
			Security::BorrowedKeyAccess borrowed(report);
			if(!borrowed.acquire(HKEY_LOCAL_MACHINE, Parent, KEY_CREATE_SUB_KEY))
				return BorrowFailed;

			rc = detail::create_unborrowed(created);
			if(rc == ERROR_SUCCESS && Query() != State::ours)
				rc = ERROR_WRITE_FAULT;		// redirected even with access: nothing real happened
			const bool restored = borrowed.restore();

			// A create that leaves the key widened is the machine-wide hole
			// BorrowedKeyAccess exists to prevent, so it is a failure even though the
			// redirect now exists. The destructor retries the restore and says so if
			// it cannot.
			if(rc == ERROR_SUCCESS && !restored)
				return ERROR_NOT_ALL_ASSIGNED;
			return rc;
		}

		// absent is success; a foreign redirect is ERROR_ALREADY_EXISTS and is not
		// touched. Judged by the real key afterwards for the same reason as
		// Create: a delete the ACL refuses may be redirected rather than refused.
		inline LSTATUS Remove(Security::BorrowReport report)
		{
			auto rc = detail::remove_unborrowed();
			if(rc == ERROR_SUCCESS && Query() == State::absent)
				return rc;
			if(rc != ERROR_SUCCESS && rc != ERROR_ACCESS_DENIED)
				return rc;

			// DELETE is asked for on the TreatAs key itself, not on its parent: that
			// is the object RegDeleteKeyEx needs the right on.
			// https://learn.microsoft.com/windows/win32/api/winreg/nf-winreg-regdeletekeyexw
			Security::BorrowedKeyAccess borrowed(report);
			if(!borrowed.acquire(HKEY_LOCAL_MACHINE, Key, DELETE))
				return BorrowFailed;

			rc = detail::remove_unborrowed();
			if(rc == ERROR_SUCCESS && Query() == State::absent)
			{
				// The key is gone; there is no security descriptor left to put back.
				borrowed.release_deleted();
				return rc;
			}
			if(rc == ERROR_SUCCESS)
				rc = ERROR_WRITE_FAULT;		// redirected even with access: nothing real happened
			borrowed.restore();
			return rc;
		}
	}
}
