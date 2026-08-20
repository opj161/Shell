#pragma once

namespace Nilesoft 
{
	namespace Security
	{
		class Permission
		{
		public:
			// SetRegistry(HKEY) and SetRegistry(HKEY, subkey) used to live here.
			// They granted BUILTIN\Users KEY_ALL_ACCESS with CONTAINER_INHERIT_ACE
			// | OBJECT_INHERIT_ACE on whatever key they were handed, and nothing in
			// the tree called either of them. A machine-wide ACL widener with no
			// callers is not worth keeping for the next person who needs a write to
			// succeed.

			static bool SetFile(const wchar_t* path)
			{
				auto_handle hFile = ::CreateFileW(path, READ_CONTROL | WRITE_DAC,
												  0, nullptr, OPEN_EXISTING,
												  FILE_ATTRIBUTE_NORMAL, nullptr);
				if(hFile)
				{
					return SetFile(hFile);
				}
				return false;
			}

			static bool SetFile(HANDLE hFile)
			{

				if(hFile == INVALID_HANDLE_VALUE)
					return false;

				struct dest
				{
					PSID sid = nullptr;
					PACL dacl = nullptr;
					PSECURITY_DESCRIPTOR sd = nullptr;

					~dest()
					{
						if(sid) ::FreeSid(sid);
						if(dacl) ::LocalFree(dacl);
						if(sd) ::LocalFree(sd);
					}
				}d;

				DWORD dwRes;
				PACL pOldDACL = nullptr;
				EXPLICIT_ACCESSW ea = { 0 };
				SID_IDENTIFIER_AUTHORITY SIDAuthNT = SECURITY_NT_AUTHORITY;

				dwRes = ::GetSecurityInfo(hFile, SE_FILE_OBJECT,
										  DACL_SECURITY_INFORMATION, nullptr, nullptr, &pOldDACL, nullptr, &d.sd);
				if(dwRes != ERROR_SUCCESS)
					return false;

				// BUILTIN\Users.
				if(!::AllocateAndInitializeSid(&SIDAuthNT, 2,
											   SECURITY_BUILTIN_DOMAIN_RID,
											   DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0, &d.sid))
				{
					return false;
				}

				// Enough to append to the log from a non-elevated Explorer, and
				// nothing else. This used to be GENERIC_ALL, which includes
				// WRITE_DAC and WRITE_OWNER - so any user could take the file over
				// outright - and it was inherited by anything created underneath.
				// The only caller is Log.cpp, on one file it has just created.
				ea.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
				ea.grfAccessMode = GRANT_ACCESS;
				ea.grfInheritance = NO_INHERITANCE;
				ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
				ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
				ea.Trustee.ptstrName = (wchar_t*)d.sid;

				// Create a new ACL that contains the new ACEs.
				if(ERROR_SUCCESS != ::SetEntriesInAclW(1, &ea, pOldDACL, &d.dacl))
					return false;

				if(d.dacl)
				{
					return ERROR_SUCCESS == ::SetSecurityInfo(
						hFile, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, d.dacl, nullptr);
				}

				return false;
			}
		};
	}
}