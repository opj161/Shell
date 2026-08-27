#pragma once

// Borrows write access to one protected registry key, and gives it back.
//
// {86ca1aa0-34aa-4e8b-a509-50c905bae2a2} is a Windows key. On a stock machine
// its owner is NT SERVICE\TrustedInstaller, Administrators hold ReadKey and
// SYSTEM holds ReadKey - so creating TreatAs under it fails with
// ERROR_ACCESS_DENIED for an elevated administrator AND for a deferred,
// no-impersonate MSI custom action running as LocalSystem. Measured on this
// machine 2026-08-27 by opening the key from both contexts.
//
// What the original fallback did was take ownership and then grant GENERIC_ALL,
// CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE, to Administrators, SYSTEM *and
// BUILTIN\Users*, and leave it that way permanently. That is a machine-wide
// hole, not a registration step: the key it opens up is the one that decides
// which handler owns the Windows 11 Explorer context menu, so any user on the
// machine could afterwards point InprocServer32 or TreatAs at a DLL of their
// choosing and have Explorer load it - for every other user, administrators
// included.
//
// So: never Users. Only Administrators, only the rights the one write needs, no
// inheritance, and the previous owner and DACL go back afterwards whether the
// write succeeded or not.
//
// This lives in src/shared because two callers need exactly this and had
// diverged: src/exe/src/Main.cpp had the borrow, and the installer's
// TreatAsApply custom action had none at all - so an MSI install skipped the
// redirect with a warning and an MSI uninstall failed outright once a redirect
// existed. One implementation is the only way those stay in step.
//
//   https://learn.microsoft.com/windows/win32/api/aclapi/nf-aclapi-setsecurityinfo
//   https://learn.microsoft.com/windows/win32/sysinfo/registry-key-security-and-access-rights
//   https://learn.microsoft.com/windows/win32/api/securitybaseapi/nf-securitybaseapi-adjusttokenprivileges
//   https://learn.microsoft.com/windows/win32/secauthz/privilege-constants
//   https://learn.microsoft.com/windows/win32/secauthz/owner-of-a-new-object

#include <windows.h>
#include <aclapi.h>
#include <cstdio>

namespace Nilesoft
{
	namespace Security
	{
		// Where a diagnostic goes is the caller's business: the exe has a
		// Logger, the custom action has MSI's message stream, a probe has
		// stdout. Nothing here depends on any of them.
		using BorrowReport = void (*)(const wchar_t *message);

		// SE_TAKE_OWNERSHIP_NAME and SE_RESTORE_NAME are TEXT(...) macros, so
		// they are only wide in a translation unit that defines UNICODE. This
		// header is shared, and LookupPrivilegeValueW is wide either way, so the
		// two names are spelled wide here rather than left to depend on how the
		// including project happens to be configured. Values per
		// https://learn.microsoft.com/windows/win32/secauthz/privilege-constants
		constexpr wchar_t TakeOwnershipPrivilege[] = L"SeTakeOwnershipPrivilege";
		constexpr wchar_t RestorePrivilege[] = L"SeRestorePrivilege";

		inline bool EnablePrivilege(const wchar_t *name)
		{
			bool result = false;
			HANDLE hToken {};
			if(::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
			{
				LUID luid {};
				TOKEN_PRIVILEGES tp {};
				if(::LookupPrivilegeValueW(nullptr, name, &luid))
				{
					tp.PrivilegeCount = 1;
					tp.Privileges[0].Luid = luid;
					tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
					// Success does not mean every requested privilege was assigned.
					// https://learn.microsoft.com/windows/win32/api/securitybaseapi/nf-securitybaseapi-adjusttokenprivileges
					if(::AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr))
						result = (::GetLastError() == ERROR_SUCCESS);
				}
				::CloseHandle(hToken);
			}
			return result;
		}

		class BorrowedKeyAccess
		{
		public:
			explicit BorrowedKeyAccess(BorrowReport report = nullptr) : _report{ report } {}
			BorrowedKeyAccess(const BorrowedKeyAccess &) = delete;
			BorrowedKeyAccess &operator=(const BorrowedKeyAccess &) = delete;
			~BorrowedKeyAccess()
			{
				// Explicit callers observe the first result. The destructor is only the
				// final best-effort retry required for every early-return path. If both
				// fail, keep the snapshot and key handles until process exit rather
				// than discarding the only copy of the original owner and DACL.
				if(!restore())
					restore();
				if(_dacl_changed || _owner_taken)
					log_restore_held();
			}

			bool acquire(HKEY root, const wchar_t *subkey, ACCESS_MASK temporary_rights,
						 REGSAM reg_view = KEY_WOW64_64KEY)
			{
				restore();

				// Taking ownership needs SeTakeOwnership; handing it back to whoever had
				// it - TrustedInstaller, a SID we are not a member of - needs SeRestore.
				// "A process with the SE_TAKE_OWNERSHIP privilege enabled can set itself
				// as the owner of an object. A process with the SE_RESTORE_NAME privilege
				// enabled or with WRITE_OWNER access to the object can set any valid user
				// or group SID as the owner" - and the owner set below is a group, not us.
				// https://learn.microsoft.com/windows/win32/secauthz/owner-of-a-new-object
				// Without the second one this could take ownership and never return it,
				// which is the trap the original code fell into. Both are present but
				// disabled in an elevated administrator's token and in LocalSystem's,
				// which is why they are enabled here rather than assumed.
				if(!EnablePrivilege(TakeOwnershipPrivilege) || !EnablePrivilege(RestorePrivilege))
					return fail(L"enable SeTakeOwnership/SeRestore", ::GetLastError());

				if(auto rc = ::RegOpenKeyExW(root, subkey, 0,
											 READ_CONTROL | WRITE_OWNER | reg_view, &_restore_key);
				   rc != ERROR_SUCCESS)
					return fail(L"open for WRITE_OWNER", rc);

				// Snapshot first. Everything below is undone from this.
				if(auto rc = ::GetSecurityInfo(_restore_key, SE_REGISTRY_KEY,
											   OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
											   &_saved_owner, nullptr, &_saved_dacl, nullptr, &_saved);
				   rc != ERROR_SUCCESS)
					return fail(L"snapshot owner and DACL", rc);

				PSID admins {};
				SID_IDENTIFIER_AUTHORITY sia { SECURITY_NT_AUTHORITY };
				if(!::AllocateAndInitializeSid(&sia, 2, SECURITY_BUILTIN_DOMAIN_RID,
											   DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins))
					return fail(L"build the Administrators SID", ::GetLastError());

				if(auto rc = ::SetSecurityInfo(_restore_key, SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION,
											   admins, nullptr, nullptr, nullptr);
				   rc != ERROR_SUCCESS)
				{
					::FreeSid(admins);
					return fail(L"take ownership", rc);
				}
				_owner_taken = true;

				// Keep the original WRITE_OWNER handle alive. If the work-handle reopen
				// fails, it is still the path that can restore the original owner.
				//
				// This asks for READ_CONTROL | WRITE_DAC and NOT for temporary_rights,
				// which is the whole point of the borrow: ownership buys exactly one
				// thing - "An object's owner implicitly has WRITE_DAC access to the
				// object" - and nothing object-specific. At this instant the DACL still
				// reads Administrators: ReadKey, so asking for KEY_CREATE_SUB_KEY here
				// was refused with ERROR_ACCESS_DENIED, acquire returned false,
				// restore() gave ownership straight back, and the whole mechanism
				// failed silently on exactly the machines it exists for. The caller
				// opens its own handle once the ACE below is in place.
				// https://learn.microsoft.com/windows/win32/secauthz/owner-of-a-new-object
				if(auto rc = ::RegOpenKeyExW(root, subkey, 0,
											 READ_CONTROL | WRITE_DAC | reg_view, &_work_key);
				   rc != ERROR_SUCCESS)
				{
					::FreeSid(admins);
					restore();
					return fail(L"open for WRITE_DAC", rc);
				}

				// Exactly what the one write needs, to one principal, inherited by
				// nothing.
				EXPLICIT_ACCESSW ea {};
				ea.grfAccessMode = GRANT_ACCESS;
				ea.grfAccessPermissions = temporary_rights;
				ea.grfInheritance = NO_INHERITANCE;
				ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
				ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
				ea.Trustee.ptstrName = reinterpret_cast<wchar_t *>(admins);

				PACL widened {};
				bool ok = false;
				DWORD rc = ::SetEntriesInAclW(1, &ea, _saved_dacl, &widened);
				if(rc == ERROR_SUCCESS)
				{
					rc = ::SetSecurityInfo(_work_key, SE_REGISTRY_KEY,
										   DACL_SECURITY_INFORMATION,
										   nullptr, nullptr, widened, nullptr);
					ok = rc == ERROR_SUCCESS;
					_dacl_changed = ok;
					::LocalFree(widened);
				}

				::FreeSid(admins);
				if(!ok)
				{
					restore();
					return fail(L"widen the DACL", rc);
				}
				return true;
			}

			// Puts the DACL back before the owner: restoring the owner first can cost us
			// the implicit WRITE_DAC that the DACL restore depends on.
			// SetSecurityInfo returns the Win32 error directly; GetLastError is captured
			// immediately because it is not the documented failure channel.
			// https://learn.microsoft.com/windows/win32/api/aclapi/nf-aclapi-setsecurityinfo
			bool restore()
			{
				bool ok = true;
				if(_dacl_changed)
				{
					DWORD rc = ERROR_INVALID_HANDLE;
					DWORD last = ERROR_INVALID_HANDLE;
					if(_work_key)
					{
						rc = ::SetSecurityInfo(_work_key, SE_REGISTRY_KEY,
							DACL_SECURITY_INFORMATION,
							nullptr, nullptr, _saved_dacl, nullptr);
						last = ::GetLastError();
					}
					if(rc == ERROR_SUCCESS)
						_dacl_changed = false;
					else
					{
						ok = false;
						log_restore_phase(L"DACL", rc, last);
					}
				}

				// Do not surrender ownership while the DACL is still widened: ownership
				// carries the WRITE_DAC needed for a later restoration retry.
				if(_owner_taken && !_dacl_changed)
				{
					DWORD rc = ERROR_INVALID_HANDLE;
					DWORD last = ERROR_INVALID_HANDLE;
					if(_restore_key && _saved_owner)
					{
						rc = ::SetSecurityInfo(_restore_key, SE_REGISTRY_KEY,
							OWNER_SECURITY_INFORMATION,
							_saved_owner, nullptr, nullptr, nullptr);
						last = ::GetLastError();
					}
					if(rc == ERROR_SUCCESS)
						_owner_taken = false;
					else
					{
						ok = false;
						log_restore_phase(L"owner", rc, last);
					}
				}

				if(!_owner_taken && !_dacl_changed)
				{
					if(_work_key) { ::RegCloseKey(_work_key); _work_key = nullptr; }
					if(_restore_key) { ::RegCloseKey(_restore_key); _restore_key = nullptr; }
					if(_saved)
					{
						::LocalFree(_saved);
						_saved = nullptr;
						_saved_owner = nullptr;
						_saved_dacl = nullptr;
					}
				}
				return ok && !_owner_taken && !_dacl_changed;
			}

			void release_deleted()
			{
				// The key has been successfully marked for deletion. Its security
				// descriptor is no longer persistent state; closing the retained handles
				// completes deletion.
				_owner_taken = false;
				_dacl_changed = false;
				if(_work_key) { ::RegCloseKey(_work_key); _work_key = nullptr; }
				if(_restore_key) { ::RegCloseKey(_restore_key); _restore_key = nullptr; }
				if(_saved) { ::LocalFree(_saved); _saved = nullptr; }
				_saved_owner = nullptr;
				_saved_dacl = nullptr;
			}

		private:
			// Every acquire failure says which step and which Win32 error, because the
			// alternative is what shipped: one caller-side warning with no step in it,
			// for eight distinct failures, on a path no test can reach.
			bool fail(const wchar_t *step, DWORD error) const
			{
				wchar_t detail[192]{};
				::swprintf_s(detail, L"BorrowedKeyAccess could not %s: error %lu",
							 step, static_cast<unsigned long>(error));
				say(detail);
				return false;
			}

			void log_restore_phase(const wchar_t *phase, DWORD security_error, DWORD last_error) const
			{
				wchar_t detail[192]{};
				::swprintf_s(detail,
					L"BorrowedKeyAccess restore %s failed: SetSecurityInfo=%lu GetLastError=%lu",
					phase,
					static_cast<unsigned long>(security_error),
					static_cast<unsigned long>(last_error));
				say(detail);
			}

			void log_restore_held() const
			{
				wchar_t detail[192]{};
				::swprintf_s(detail,
					L"BorrowedKeyAccess restore did not complete (DACL changed=%d owner taken=%d); snapshot and handles kept until process exit",
					_dacl_changed ? 1 : 0, _owner_taken ? 1 : 0);
				say(detail);
			}

			void say(const wchar_t *message) const
			{
				if(_report)
					_report(message);
			}

			BorrowReport _report = nullptr;
			HKEY _restore_key = nullptr;
			HKEY _work_key = nullptr;
			PSECURITY_DESCRIPTOR _saved = nullptr;   // one allocation backing both below
			PSID _saved_owner = nullptr;
			PACL _saved_dacl = nullptr;
			bool _owner_taken = false;
			bool _dacl_changed = false;
		};
	}
}
