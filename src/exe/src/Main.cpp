
#include <System.h>
#include <Globals.h>
#include <Resource.h>
#include <dwmapi.h>

#include <vector>
#include <commctrl.h>
#include "Control.h"
#include <shlwapi.h>
#include <shlobj.h>
#include <Library/PlutoVGWrap.h>
#include <RegistryConfig.h>
#include <ConfigCheck.h>
#include <PerfReport.h>
#include <ProviderQuarantine.h>

//#pragma comment(lib, "mincore.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Comctl32")
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "shlwapi.lib")

#if defined(_M_ARM64)
	#pragma comment(lib, "plutosvg-arm64.lib")
//#elif defined(_M_ARM)
//	#pragma comment(lib, "plutosvg-arm.lib")
#elif defined(_M_X64)
	#pragma comment(lib, "plutosvg-x64.lib")
#else
	#pragma comment(lib, "plutosvg-x86.lib")
#endif

using namespace Nilesoft;
using namespace Nilesoft::Windows::Forms;
using namespace Nilesoft::Shell;

constexpr const wchar_t dll_name[] = L"shell.dll";

BOOL CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

#define ID_MAINWINDOW 100

// define id for close, minimize & demo buttons
#define ID_CLOSE 0x001
#define ID_MINIMIZE 0x002

#define ID_REG 0x003
#define ID_UNREG 0x004
#define ID_RESTART 0x005
#define ID_WEB 0x006
#define ID_DONATE 0x007
#define ID_EMAIL 0x008
#define ID_GITHUB 0x009
#define ID_DOCS 0x00d

/////////
#define SetWindowStyle(hwnd, style)	 ::SetWindowLongW((hwnd), GWL_STYLE, (style))
#define SetWindowExStyle(hwnd, style)	::SetWindowLongW((hwnd), GWL_EXSTYLE, (style))

#define GET_X_LPARAM(lp)    ((int)(short)LOWORD(lp))  // windowsx.h
#define GET_Y_LPARAM(lp)    ((int)(short)HIWORD(lp))  // windowsx.h

int __stdcall btn_on_paint(UI::Control *s, int, WPARAM, LPARAM);

LRESULT __stdcall WindowProc(HWND, UINT, WPARAM, LPARAM);

bool is_elevated = false;

// Global Variables:
HINSTANCE _hInstance;    // current instance
Logger *_log;
HANDLE _fonthandle = nullptr;
HFONT _hfont_icon = nullptr;
HFONT _hfont_icon2 = nullptr;
UINT _dpi = 96;
RECT rc_window{};
RECT rc_reg{};
RECT rc_unreg{};
POINT p{};
UI::Window *main_window = nullptr;
HBITMAP hbitmap_logo = nullptr;
//0xAA0000FF
COLORREF color_background = 0xffffff;

template<typename T = long>
T dpi(auto value) { return static_cast<T>((value * _dpi) / 96); }

string loadstring(UINT id, HMODULE hmodule = nullptr)
{
	return string::LoadStringW_full(hmodule, id).move();
}


BOOL EnablePrivilege(const wchar_t *name)
{
	BOOL result = FALSE;
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

/*
	Borrows write access to one protected key, and gives it back.

	{86ca1aa0-...} is a Windows key. On a stock machine its owner is
	NT SERVICE\TrustedInstaller and Administrators hold ReadKey, so writing
	TreatAs under it really does fail with ERROR_ACCESS_DENIED for an elevated
	administrator - the fallback is not dead code.

	What the fallback used to do was take ownership and then grant GENERIC_ALL,
	CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE, to Administrators, SYSTEM *and
	BUILTIN\Users*, and leave it that way permanently. That is a machine-wide
	hole, not a registration step: the key it opens up is the one that decides
	which handler owns the Windows 11 Explorer context menu, so any user on the
	machine could afterwards point InprocServer32 or TreatAs at a DLL of their
	choosing and have Explorer load it - for every other user, administrators
	included. It was still in place on the machine this was written on, months
	after whatever install created it: BUILTIN\Users, FullControl, inherited by
	everything underneath.

	So: never Users. Only Administrators, only the two rights the one write
	needs, no inheritance, and the previous owner and DACL go back afterwards
	whether the write succeeded or not.

	  https://learn.microsoft.com/windows/win32/api/aclapi/nf-aclapi-setsecurityinfo
	  https://learn.microsoft.com/windows/win32/sysinfo/registry-key-security-and-access-rights
	  https://learn.microsoft.com/windows/win32/api/securitybaseapi/nf-securitybaseapi-adjusttokenprivileges
	  https://learn.microsoft.com/windows/win32/secauthz/privilege-constants
*/
class BorrowedKeyAccess
{
public:
	BorrowedKeyAccess() = default;
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
		// Without the second one this could take ownership and never return it,
		// which is the trap the old code fell into.
		if(!EnablePrivilege(SE_TAKE_OWNERSHIP_NAME) || !EnablePrivilege(SE_RESTORE_NAME))
			return false;

		if(ERROR_SUCCESS != ::RegOpenKeyExW(root, subkey, 0,
											READ_CONTROL | WRITE_OWNER | reg_view, &_restore_key))
			return false;

		// Snapshot first. Everything below is undone from this.
		if(ERROR_SUCCESS != ::GetSecurityInfo(_restore_key, SE_REGISTRY_KEY,
											  OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
											  &_saved_owner, nullptr, &_saved_dacl, nullptr, &_saved))
			return false;

		PSID admins {};
		SID_IDENTIFIER_AUTHORITY sia { SECURITY_NT_AUTHORITY };
		if(!::AllocateAndInitializeSid(&sia, 2, SECURITY_BUILTIN_DOMAIN_RID,
									   DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins))
			return false;

		if(ERROR_SUCCESS != ::SetSecurityInfo(_restore_key, SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION,
											  admins, nullptr, nullptr, nullptr))
		{
			::FreeSid(admins);
			return false;
		}
		_owner_taken = true;

		// Keep the original WRITE_OWNER handle alive. If the work-handle reopen
		// fails, it is still the path that can restore the original owner.
		if(ERROR_SUCCESS != ::RegOpenKeyExW(root, subkey, 0,
											READ_CONTROL | WRITE_DAC | temporary_rights | reg_view,
											&_work_key))
		{
			::FreeSid(admins);
			restore();
			return false;
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
		if(ERROR_SUCCESS == ::SetEntriesInAclW(1, &ea, _saved_dacl, &widened))
		{
			ok = ERROR_SUCCESS == ::SetSecurityInfo(_work_key, SE_REGISTRY_KEY,
													DACL_SECURITY_INFORMATION,
													nullptr, nullptr, widened, nullptr);
			_dacl_changed = ok;
			::LocalFree(widened);
		}

		::FreeSid(admins);
		if(!ok)
			restore();
		return ok;
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
	void log_restore_phase(const wchar_t *phase, DWORD security_error, DWORD last_error) const
	{
		wchar_t detail[192]{};
		::swprintf_s(detail,
			L"BorrowedKeyAccess restore %s failed: SetSecurityInfo=%lu GetLastError=%lu",
			phase,
			static_cast<unsigned long>(security_error),
			static_cast<unsigned long>(last_error));
		if(_log)
			_log->error(detail);
	}

	void log_restore_held() const
	{
		wchar_t detail[192]{};
		::swprintf_s(detail,
			L"BorrowedKeyAccess restore did not complete (DACL changed=%d owner taken=%d); snapshot and handles kept until process exit",
			_dacl_changed ? 1 : 0, _owner_taken ? 1 : 0);
		if(_log)
			_log->error(detail);
	}

	HKEY _restore_key = nullptr;
	HKEY _work_key = nullptr;
	PSECURITY_DESCRIPTOR _saved = nullptr;   // one allocation backing both below
	PSID _saved_owner = nullptr;
	PACL _saved_dacl = nullptr;
	bool _owner_taken = false;
	bool _dacl_changed = false;
};
enum class TreatAsState
{
	absent,
	ours,
	foreign,
	inaccessible
};

static constexpr wchar_t TreatAsParent[] =
	L"SOFTWARE\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}";
static constexpr wchar_t TreatAsKey[] =
	L"SOFTWARE\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\TreatAs";

static TreatAsState QueryTreatAs(LSTATUS *error = nullptr)
{
	auto_regkey key{};
	auto rc = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, TreatAsKey, 0,
		KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, key);
	if(rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND)
	{
		if(error) *error = ERROR_SUCCESS;
		return TreatAsState::absent;
	}
	if(rc != ERROR_SUCCESS)
	{
		if(error) *error = rc;
		return TreatAsState::inaccessible;
	}

	DWORD subkeys = 0;
	DWORD values = 0;
	rc = ::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr,
		                     nullptr, &values, nullptr, nullptr, nullptr, nullptr);
	if(rc != ERROR_SUCCESS)
	{
		if(error) *error = rc;
		return TreatAsState::inaccessible;
	}

	wchar_t value[64]{};
	DWORD bytes = sizeof(value);
	rc = ::RegGetValueW(key, nullptr, nullptr,
		RRF_RT_REG_SZ | RRF_ZEROONFAILURE, nullptr, value, &bytes);
	if(rc != ERROR_SUCCESS)
	{
		if(error) *error = rc == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : rc;
		return rc == ERROR_FILE_NOT_FOUND ? TreatAsState::foreign : TreatAsState::inaccessible;
	}

	if(error) *error = ERROR_SUCCESS;
	if(subkeys == 0 && values == 1
	   && ::CompareStringOrdinal(value, -1, CLS_ContextMenu, -1, TRUE) == CSTR_EQUAL)
		return TreatAsState::ours;
	return TreatAsState::foreign;
}

static LSTATUS CreateTreatAsIfAbsent()
{
	auto_regkey parent{};
	auto rc = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, TreatAsParent, 0,
		KEY_CREATE_SUB_KEY | KEY_WOW64_64KEY, parent);
	if(rc != ERROR_SUCCESS)
		return rc;

	HKEY raw = nullptr;
	DWORD disposition = 0;
	rc = ::RegCreateKeyExW(parent, L"TreatAs", 0, nullptr, REG_OPTION_NON_VOLATILE,
		KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY, nullptr, &raw, &disposition);
	if(rc != ERROR_SUCCESS)
		return rc;
	auto_regkey key{ raw };

	if(disposition != REG_CREATED_NEW_KEY)
		return QueryTreatAs() == TreatAsState::ours ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS;

	rc = ::RegSetValueExW(key, nullptr, 0, REG_SZ,
		reinterpret_cast<const BYTE *>(CLS_ContextMenu), sizeof(CLS_ContextMenu));
	if(rc != ERROR_SUCCESS)
		::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, TreatAsKey, KEY_WOW64_64KEY, 0);
	return rc;
}

static LSTATUS RemoveTreatAsIfOurs()
{
	LSTATUS error = ERROR_SUCCESS;
	auto state = QueryTreatAs(&error);
	if(state == TreatAsState::absent)
		return ERROR_SUCCESS;
	if(state == TreatAsState::foreign)
		return ERROR_ALREADY_EXISTS;
	if(state == TreatAsState::inaccessible)
		return error;
	return ::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, TreatAsKey, KEY_WOW64_64KEY, 0);
}

bool disable_modern(bool register_redirect)
{
	auto state = QueryTreatAs();
	if(register_redirect)
	{
		if(state == TreatAsState::ours)
			return true;
		if(state != TreatAsState::absent)
			return false;

		auto rc = CreateTreatAsIfAbsent();
		if(rc != ERROR_ACCESS_DENIED)
			return rc == ERROR_SUCCESS;

		BorrowedKeyAccess borrowed;
		if(!borrowed.acquire(HKEY_LOCAL_MACHINE, TreatAsParent, KEY_CREATE_SUB_KEY))
			return false;

		rc = CreateTreatAsIfAbsent();
		const bool restored = borrowed.restore();
		return restored && rc == ERROR_SUCCESS;
	}

	if(state == TreatAsState::absent || state == TreatAsState::foreign)
		return true;
	if(state != TreatAsState::ours)
		return false;

	auto rc = RemoveTreatAsIfOurs();
	if(rc != ERROR_ACCESS_DENIED)
		return rc == ERROR_SUCCESS;

	BorrowedKeyAccess borrowed;
	if(!borrowed.acquire(HKEY_LOCAL_MACHINE, TreatAsKey, DELETE))
		return false;

	rc = RemoveTreatAsIfOurs();
	if(rc == ERROR_SUCCESS)
	{
		borrowed.release_deleted();
		return true;
	}
	borrowed.restore();
	return false;
}

//printf("Please wait shell we'll process your command \n");
// r		register
// u		unregister
// s		silent
// restart	restart explorer
// shx	cmh integrated Shell Extensions ContextMenuHandlers
// Register the COM server and the context menu handler.
bool Registration(REGOP reg)
{
	auto ver = &Windows::Version::Instance();

	try
	{
		string dir = IO::Path::Parent(IO::Path::Module(_hInstance));
		string dll_path = IO::Path::Combine(dir, dll_name).move();

		string path = IO::Path::Module(_hInstance).move();
		//string dir = IO::Path::Parent(path).move();

		if(reg.REGISTER)
		{
			// Registration used to widen the ACL on the whole install directory
			// here, granting BUILTIN\Users GENERIC_ALL with inheritance. It has
			// never actually done anything - Permission::SetFile opens with
			// CreateFileW and no FILE_FLAG_BACKUP_SEMANTICS, which cannot open a
			// directory at all - and "fixing" it would hand every user on the
			// machine write access to the binaries inside Program Files. The
			// call is gone rather than repaired.
			_log->close();
			//logger->reset();
			//IO::Path::Delete(logger->path());
		}

		if(!ver->IsWindows7OrGreater())
		{
			//windows compatibility
			_log->error(string::Extract(IDS_WINDOWS_COMPATIBILITY));
			return false;
		}

		string msg;

		if(reg.REGISTER || reg.UNREGISTER)
		{
			if(!is_elevated)
			{
				// Missing administrative privileges!
				msg = string::Extract(IDS_ADMIN_PRIVILEGES).move();
				_log->warning(msg);
				if(!reg.SILENT)
				{
					//You will need to provide administrator permission to run this Shell
					MessageBox::Show(msg, APP_FULLNAME, MessageBoxIcon::Warning);
				}
				return false;
			}

			if(reg.REGISTER)
			{
				//logger->create();

				REGOP regop{};
				regop.CONTEXTMENU = regop.ICONOVERLAY = true;

				if(!RegistryConfig::Register(dll_path, regop))
				{
					msg = string::Extract(IDS_REGISTER_NOT_SUCCESS).move();
					_log->error(msg);
					if(!reg.SILENT)
					{
						MessageBox::Show(msg, APP_FULLNAME,
										 MessageBoxIcon::Error, MessageBoxButtons::OK);
					}
					return false;
				}
				
// HKEY_CLASSES_ROOT\LibraryFolder\ShellEx\ContextMenuHandlers
// HKEY_CLASSES_ROOT\LibraryFolder\background\shellex\ContextMenuHandlers
				// is windows 11 or later
				if(ver->IsWindows11OrGreater() && reg.TREAT)
				{
					//SOFTWARE\\Classes\\CLSID"
					if(!disable_modern(true))
						_log->warning(L"Windows 11 primary-menu redirect was not applied.");
				}

				msg = string::Extract(IDS_REGISTER_SUCCESS).move();
			}
			else
			{
				// Registry deletion APIs report success only when they delete an
				// existing resource. At the command level, however, an already absent
				// registration is the requested end state and is therefore success.
				// https://learn.microsoft.com/windows/win32/api/winreg/nf-winreg-regdeletetreew
				if(!unregister_if_present(RegistryConfig::IsRegistered(), []
					{
						return RegistryConfig::Unregister();
					}))
				{
					msg = string::Extract(IDS_UNREGISTER_NOT_SUCCESS).move();
					_log->error(msg);
					if(!reg.SILENT)
					{
						MessageBox::Show(msg, APP_FULLNAME,
										 MessageBoxIcon::Error, MessageBoxButtons::OK);
					}
					return false;
				}

				// is windows 11 or later
				if(ver->IsWindows11OrGreater() && reg.TREAT)
					if(!disable_modern(false))
						_log->warning(L"Windows 11 primary-menu redirect could not be removed safely.");

				msg = string::Extract(IDS_UNREGISTER_SUCCESS).move();
			}

			if(ver->IsWindows8OrGreater())
				_log->info(msg);

			if(reg.RESTART)
			{
				if(!reg.SILENT)
				{
					msg += L"\n\n";
					msg += string::Extract(IDS_RESTART_EXPLORERQ).move();
					reg.RESTART = MessageBox::Show(msg, APP_FULLNAME,
											   MessageBoxIcon::Information,
											   MessageBoxButtons::OKCancel) == DialogResult::OK;
				}
			}

			if(reg.RESTART)
			{
				if(ver->IsWindows8OrGreater())
					_log->info(string::Extract(IDS_RESTART_EXPLORER).move());

				if(Windows::Explorer::Restart())
					::Sleep(1000);
			}
			else
			{
				// https://docs.microsoft.com/en-us/windows/desktop/shell/reg-shell-exts#predefined-shell-objects
				// If you do not call SHChangeNotify, the change might not be recognized until the system is rebooted.
				//::SHChangeNotify(SHCNE_ASSOCCHANGED, 0, 0, 0);
				//Windows::Explorer::Refresh();
			}
			return true;
		}
		else if(reg.RESTART)
		{
			if(ver->IsWindows8OrGreater())
				_log->info(string::Extract(IDS_RESTART_EXPLORER).move());
			return Windows::Explorer::Restart();
		}
	}
	catch(...)
	{
#ifdef _DEBUG
		logger->exception(__func__);
#endif
	}
	return false;
}

/*
	Write one line where the person who typed the command will see it.

	shell.exe is /SUBSYSTEM:WINDOWS, so it starts with no console and, per
	AttachConsole's own page, "the standard handles retrieved with GetStdHandle
	will likely be invalid on startup until AttachConsole is called. The
	exception to this is if the application is launched with handle inheritance
	by its parent process."

		https://learn.microsoft.com/en-us/windows/console/attachconsole

	That exception is the redirection case - `shell.exe -check > log.txt` - and
	it has to be tried first, because attaching to the parent's console would
	then send the report somewhere the user did not ask for. Console Handles
	gives the discriminator and the rule for each kind:

		"If a standard handle has been redirected to refer to a file or a pipe,
		 however, the handle can only be used by the ReadFile and WriteFile
		 functions. GetFileType can assist in determining what device type the
		 handle refers to. A console handle presents as FILE_TYPE_CHAR."

		"CreateFile enables a process to get a handle to its console's input
		 buffer and active screen buffer, even if STDIN and STDOUT have been
		 redirected... Specify the CONOUT$ value."

		https://learn.microsoft.com/en-us/windows/console/console-handles

	So: an inherited handle is used as it is; otherwise attach to the parent and
	open CONOUT$ explicitly rather than trusting the standard handles to have
	been fixed up; otherwise there is no console anywhere and a message box is
	the only place left to say it.

	One wart that cannot be fixed from here: cmd.exe does not wait for a
	Windows-subsystem process, so the report lands after the prompt has already
	been printed. Curing that needs a second, console-subsystem binary.
*/
static void write_console_line(const wchar_t *text)
{
	if(!text || !*text)
		return;

	auto length = static_cast<DWORD>(::lstrlenW(text));

	auto write = [&](HANDLE handle) -> bool
	{
		if(!handle || handle == INVALID_HANDLE_VALUE)
			return false;

		if(::GetFileType(handle) == FILE_TYPE_CHAR)
		{
			DWORD written = 0;
			if(!::WriteConsoleW(handle, text, length, &written, nullptr))
				return false;
			::WriteConsoleW(handle, L"\r\n", 2, &written, nullptr);
			return true;
		}

		// A file or a pipe. Nothing on the other end knows this process's
		// encoding, so the bytes are UTF-8 - which is what a redirected
		// console produces and what every tool that would read this expects.
		auto bytes = ::WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length),
										   nullptr, 0, nullptr, nullptr);
		if(bytes <= 0)
			return false;

		std::vector<char> utf8(static_cast<size_t>(bytes) + 2);
		if(::WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length),
								 utf8.data(), bytes, nullptr, nullptr) != bytes)
			return false;

		utf8[static_cast<size_t>(bytes)] = '\r';
		utf8[static_cast<size_t>(bytes) + 1] = '\n';

		DWORD written = 0;
		return !!::WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
	};

	if(write(::GetStdHandle(STD_OUTPUT_HANDLE)))
		return;

	if(::AttachConsole(ATTACH_PARENT_PROCESS))
	{
		auto console = ::CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
									 nullptr, OPEN_EXISTING, 0, nullptr);
		if(console != INVALID_HANDLE_VALUE)
		{
			auto ok = write(console);
			::CloseHandle(console);
			if(ok)
				return;
		}
	}

	::MessageBoxW(nullptr, text, APP_FULLNAME, MB_OK | MB_ICONINFORMATION);
}

/*
	`shell.exe -check [path]` - parse a configuration and say what is wrong
	with it, without publishing anything or touching the running shell.

	The parser lives in shell.dll, so this loads the DLL sitting beside this
	executable and calls one export. Beside it, specifically, and not whatever
	copy is registered on the machine: AGENTS.md records the installer's custom
	action being caught servicing "whatever Shell is registered" rather than the
	one it was working on, and a validator that checked a different build's idea
	of the configuration would be the same mistake.
*/
static int CheckConfig(const wchar_t *config_path)
{
	string path = IO::Path::Combine(IO::Path::Parent(IO::Path::Module(nullptr)), dll_name).move();

	// Not LOAD_LIBRARY_AS_DATAFILE - this one is called, not read.
	DLL dll(path);
	if(!dll)
	{
		write_console_line(L"shell.exe -check: shell.dll was not found next to shell.exe.");
		return CONFIG_CHECK_UNUSABLE;
	}

	auto entry = dll.Get<ConfigCheckFn>(CONFIG_CHECK_EXPORT);
	if(!entry)
	{
		write_console_line(L"shell.exe -check: this shell.dll is too old to answer -check.");
		return CONFIG_CHECK_UNUSABLE;
	}

	ConfigCheckResult result{};
	result.cbSize = sizeof(result);

	auto code = entry(config_path, &result);
	if(code == CONFIG_CHECK_UNUSABLE)
	{
		write_console_line(L"shell.exe -check: shell.dll refused the request.");
		return code;
	}

	wchar_t line[CONFIG_CHECK_PATH + CONFIG_CHECK_MESSAGE + 64]{};
	format_config_check(result, code, line, ARRAYSIZE(line));
	write_console_line(line);

	/*
		A configuration that parses can still not do what it says, and there is
		exactly one case of that worth a line here.

		`settings { priority = 0 }` asks for the Windows 11 modern menu. On a
		machine registered with `-register -treat` it does nothing at all: COM
		substitutes Shell for the modern menu class, Shell's object does not
		implement what the modern menu asks for, and Explorer falls back to the
		classic menu regardless of the setting. Measured four ways on
		2026-08-24; the table is in CoCreateInstanceHook.

		Nothing said so. Somebody who set priority = 0, restarted Explorer and
		got the same menu had no way to find out why - which is the same
		category of problem `-check` was built for.

		A warning, not an error: the configuration is valid and the exit code
		stays 0. Telling somebody their file is broken when it is not is how a
		validator gets ignored.
	*/
	if(code == CONFIG_CHECK_OK
	   && result.priority == CONFIG_CHECK_PRIORITY_OFF
	   && QueryTreatAs() == TreatAsState::ours)
	{
		write_console_line(
			L"  note: priority = 0 asks for the Windows 11 menu, but this machine is\r\n"
			L"        registered with -treat, which redirects that menu to Shell and\r\n"
			L"        overrules the setting. Run `shell.exe -unregister -treat` (as\r\n"
			L"        administrator) to get the Windows 11 menu back.");
	}

	return code;
}

/*
	`shell.exe -report perf` - what the menus in every host on this desktop
	actually cost.

	docs/refactor/06-phases-and-tests.md section 4 names this command and
	docs/refactor/05-capabilities.md section 1 needs it; section 3.2 of the
	handoff explains why the alternative does not work. The phase timings are
	always recorded (Include/Diagnostics/DiagnosticsRing.h) but the ring is
	process-local, and the process that matters is somebody else's explorer.exe.
	The documented substitute - the `perf` registry value writing breaching
	phases to shell.log - was measured producing nothing at all from
	explorer.exe while the same DLL logged freely from another host in the same
	session, so until now the numbers existed and could not be looked at.

	This reads them out of the shared block each host publishes into. It loads
	no DLL, injects nothing and takes no lock any host could be waiting on:
	src/shared/PerfExport.h has the reasoning for that shape.

	Nothing here is a diagnostic of this process. `-report perf` run on a
	machine where no menu has been opened correctly reports nothing at all, and
	says so rather than printing an empty table.
*/
static int ReportPerf(bool detailed)
{
	using namespace Nilesoft::Shell::Diagnostics;

	// The process list on a busy desktop; more than enough, and a machine with
	// more processes than this loses the tail rather than the report.
	std::vector<uint32_t> pids(2048);
	auto found = perf_export_enumerate(pids.data(), pids.size());

	string report;
	size_t hosts = 0;
	size_t busy = 0;
	size_t unsupported = 0;
	auto now = ::GetTickCount64();

	std::vector<PerfExportRecord> records(PERF_EXPORT_RECORDS);
	uint32_t scratch[PERF_EXPORT_RECORDS]{};

	for(size_t i = 0; i < found; i++)
	{
		PerfExportSource source{};
		size_t written = 0;
		auto status = perf_export_read(pids[i], source, records.data(), records.size(), written);

		if(status == PerfExportStatus::NotPresent)
			continue;
		if(status == PerfExportStatus::Busy)
		{
			busy++;
			continue;
		}
		if(status == PerfExportStatus::Unsupported)
		{
			unsupported++;
			continue;
		}

		hosts++;

		auto summary = perf_report_summarize(records.data(), written, scratch, ARRAYSIZE(scratch));
		summary.published = source.published;

		uint32_t p50_ms = 0, p50_tenth = 0, p95_ms = 0, p95_tenth = 0;
		perf_report_split_ms(summary.p50_microseconds, p50_ms, p50_tenth);
		perf_report_split_ms(summary.p95_microseconds, p95_ms, p95_tenth);

		if(hosts > 1)
			report.append(L"\r\n");

		report.append_format(L"%s  pid %u  %s  -  %llu menu%s, %u held\r\n",
							 source.host[0] ? source.host : L"(unknown)",
							 source.process_id,
							 perf_export_architecture_name(source.architecture),
							 static_cast<unsigned long long>(source.published),
							 source.published == 1 ? L"" : L"s",
							 static_cast<unsigned>(written));

		if(written == 0)
		{
			// A host that mapped its block and has not published yet. Worth a
			// line: it says Shell is loaded there, which is half of what
			// somebody running this wants to know.
			report.append(L"    no sessions recorded yet\r\n");
			continue;
		}

		// Pre-display, not the session. The session clock runs from the hook
		// being entered to the hook returning, so it includes however long the
		// menu sat on screen - measured at 1,435 ms for a menu whose
		// pre-display cost was 11 ms. Only one of those is a latency number.
		if(summary.measured > 0)
		{
			report.append_format(L"    pre-display  p50 %u.%u ms   p95 %u.%u ms   n=%u\r\n",
								 p50_ms, p50_tenth, p95_ms, p95_tenth,
								 static_cast<unsigned>(summary.measured));
		}
		else
		{
			// Every session here was declined or failed before the menu was
			// composed. A row of zeroes would read as a fast menu.
			report.append(L"    pre-display  no menu of Shell's own was displayed\r\n");
		}

		// Decisions, but only the ones that happened - a row of zeroes for the
		// four that did not is noise in a report meant to be skimmed.
		string decisions;
		for(uint32_t d = 0; d < 8; d++)
		{
			if(summary.decisions[d] == 0)
				continue;
			if(!decisions.empty())
				decisions.append(L", ");
			decisions.append_format(L"%u %s", summary.decisions[d],
									perf_export_decision_name(d));
		}
		report.append_format(L"    decisions  %s\r\n", decisions.c_str());

		// Which tracking flags this host actually passes, which is the question
		// docs/refactor/01 section 3 could not answer for any host that did not
		// come from this tree: TPM_RETURNCMD decides whether Shell hands back an
		// identifier or notifies separately, and only one of those two paths had
		// ever run outside a test.
		string flag_sets;
		for(size_t s = 0; s < written; s++)
		{
			wchar_t names[128]{};
			perf_export_flag_names(records[s].host_flags, names, ARRAYSIZE(names));
			// find() returns a pointer into the string, not an index.
			if(flag_sets.find(names) == nullptr)
			{
				if(!flag_sets.empty())
					flag_sets.append(L", ");
				flag_sets.append(names);
			}
		}
		report.append_format(L"    host flags %s\r\n", flag_sets.c_str());

		auto emit_session = [&](size_t index, const wchar_t *label)
		{
			auto &record = records[index];
			uint32_t pre_ms = 0, pre_tenth = 0;
			perf_report_split_ms(perf_report_phase(record, PERF_REPORT_PRE_DISPLAY),
								 pre_ms, pre_tenth);

			auto age = perf_report_age_ms(now, record.tick);

			report.append_format(L"    %s  %u.%u ms to display  %s  %llu.%llus ago\r\n",
								 label, pre_ms, pre_tenth,
								 perf_export_decision_name(record.decision),
								 static_cast<unsigned long long>(age / 1000),
								 static_cast<unsigned long long>((age % 1000) / 100));

			// `phase`, not `p`: this file has a global `p`, and a loop variable
			// that shadows it compiles with a warning nobody reads.
			for(uint32_t phase = 0; phase < record.phase_count; phase++)
			{
				uint32_t ms = 0, tenth = 0;
				perf_report_split_ms(record.phases[phase].microseconds, ms, tenth);

				if(record.phases[phase].count >= 0)
				{
					report.append_format(L"                 %-36s %u.%u ms  n=%d\r\n",
										 record.phases[phase].name, ms, tenth,
										 record.phases[phase].count);
				}
				else
				{
					report.append_format(L"                 %-36s %u.%u ms\r\n",
										 record.phases[phase].name, ms, tenth);
				}
			}

			if(record.dropped_phases)
			{
				report.append_format(L"                 (%u phase%s did not fit)\r\n",
									 record.dropped_phases,
									 record.dropped_phases == 1 ? L"" : L"s");
			}

			for(uint32_t v = 0; v < record.provider_count; v++)
			{
				uint32_t ms = 0, tenth = 0;
				perf_report_split_ms(record.providers[v].microseconds, ms, tenth);

				/*
					Name the extension when the host knows it.

					A hash is what the record carries and what makes "these
					forty menus were all the same handler" answerable, but
					`provider e345019d  186.0 ms` names nothing anybody can act
					on - and acting on it is the entire point of
					docs/refactor/05-capabilities.md section 1. The name comes
					from the host's own directory, filled from
					IExplorerCommand::GetTitle the first time it activated the
					provider.

					The identifier printed is the CLSID, not the title: a handler
					may title itself differently for a different selection, so
					the title is a label and the CLSID is the identity. It is
					also what `-quarantine:add` takes.
				*/
				auto known = source.name_for(record.providers[v].clsid_hash);
				auto clsid = source.clsid_for(record.providers[v].clsid_hash);

				// The CLSID rather than the hash whenever this host knows it,
				// because that is exactly what `shell.exe -quarantine:add`
				// accepts. A report whose identifier the treatment command
				// cannot take would leave the two halves of this feature
				// speaking different languages. The hash is the fallback for a
				// provider this host has never successfully activated - which
				// is also one there is nothing useful to say about yet.
				string identity;
				if(clsid)
					identity = Nilesoft::Shell::Quarantine::format_guid(*clsid).c_str();
				else
					identity.append_format(L"%08x", record.providers[v].clsid_hash);

				report.append_format(L"                 provider %-38s %u.%u ms  %s",
									 identity.c_str(), ms, tenth,
									 perf_export_result_name(record.providers[v].result));
				if(known)
					report.append_format(L"  %s", known);
				report.append(L"\r\n");
			}
		};

		if(detailed)
		{
			for(size_t s = 0; s < written; s++)
				emit_session(s, s == 0 ? L"newest " : L"       ");
		}
		else if(summary.slowest < written)
		{
			emit_session(summary.slowest, L"slowest");
		}
	}

	if(hosts == 0)
	{
		string line = L"shell.exe -report perf: no host on this desktop has an open Shell menu ring.";
		if(unsupported)
			line.append_format(L"\r\n  %u process%s had a ring this build cannot read - restart them to pick up this Shell.",
							   static_cast<unsigned>(unsupported), unsupported == 1 ? L"" : L"es");
		line.append(L"\r\n  Raise a context menu in Explorer and run this again.");
		write_console_line(line.c_str());
		return 1;
	}

	string header;
	header.append_format(L"Nilesoft Shell - menu timing, %u host%s",
						 static_cast<unsigned>(hosts), hosts == 1 ? L"" : L"s");
	if(busy)
		header.append_format(L", %u busy", static_cast<unsigned>(busy));
	if(unsupported)
		header.append_format(L", %u unreadable", static_cast<unsigned>(unsupported));
	header.append(L"\r\n\r\n");
	header.append(report);

	write_console_line(header.c_str());
	return 0;
}

/*
	`shell.exe -quarantine[:list|:add|:remove] [clsid]`

	The treatment for what `-report perf` diagnoses. A quarantined handler is one
	Shell stops asking when it builds a menu; it keeps working everywhere else.
	src/shared/ProviderQuarantine.h has the format and the reasoning.

	Deliberately not elevated and deliberately not restarting anything. The file
	is per-user under %LocalAppData% and every host re-reads it within a couple
	of seconds, so the answer to "when does this take effect" is "the next menu".

	Exit codes reuse ConfigCheckCode so a script sees the same vocabulary as
	-check: 0 for a request that was carried out, UNUSABLE for one that could
	not be understood or written.
*/
static int QuarantineCommand(const string &action, const string &argument)
{
	auto path = Nilesoft::Shell::Quarantine::default_path();
	if(path.empty())
	{
		write_console_line(L"shell.exe -quarantine: could not locate %LocalAppData%.");
		return CONFIG_CHECK_UNUSABLE;
	}

	auto entries = Nilesoft::Shell::Quarantine::load(path);

	auto show = [&]()
	{
		string line;
		if(entries.empty())
		{
			line.append(L"Nilesoft Shell - no extensions are quarantined.\r\n");
			line.append_format(L"  list: %s\r\n", path.c_str());
			line.append(L"  Run `shell.exe -report perf` to see what a menu costs, then\r\n"
						L"  `shell.exe -quarantine:add {clsid}` to stop Shell asking one.");
		}
		else
		{
			line.append_format(L"Nilesoft Shell - %u extension%s quarantined\r\n\r\n",
							   static_cast<unsigned>(entries.size()),
							   entries.size() == 1 ? L"" : L"s");
			for(const auto &entry : entries)
			{
				// The hash is what a perf report prints, so it is shown
				// alongside the CLSID the file holds - otherwise the two
				// commands name the same extension two ways with nothing
				// connecting them.
				line.append_format(L"  %08x  %s", entry.hash,
								   Nilesoft::Shell::Quarantine::format_guid(entry.clsid).c_str());
				if(!entry.note.empty())
					line.append_format(L"  %s", entry.note.c_str());
				line.append(L"\r\n");
			}
			line.append_format(L"\r\n  list: %s\r\n", path.c_str());
			line.append(L"  Takes effect on the next menu each host builds.");
		}
		write_console_line(line.c_str());
	};

	if(action.empty() || action.equals(L"list", true))
	{
		show();
		return CONFIG_CHECK_OK;
	}

	auto adding = action.equals(L"add", true);
	if(!adding && !action.equals(L"remove", true))
	{
		write_console_line(L"shell.exe -quarantine: the actions are `list`, `add` and `remove`.");
		return CONFIG_CHECK_UNUSABLE;
	}

	GUID clsid{};
	if(!Nilesoft::Shell::Quarantine::parse_guid(argument.c_str(), clsid))
	{
		write_console_line(
			L"shell.exe -quarantine: expected a CLSID, as {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}.\r\n"
			L"  A perf report shows an extension's hash; its CLSID is in the package that\r\n"
			L"  registered it.");
		return CONFIG_CHECK_UNUSABLE;
	}

	auto hash = Nilesoft::Shell::Quarantine::hash_clsid(clsid);
	auto formatted = Nilesoft::Shell::Quarantine::format_guid(clsid);

	size_t at = entries.size();
	for(size_t i = 0; i < entries.size(); i++)
	{
		if(entries[i].hash == hash)
		{
			at = i;
			break;
		}
	}

	string line;
	if(adding)
	{
		if(at < entries.size())
		{
			// Already listed is a success, not an error: the state the caller
			// asked for is the state that holds.
			line.append_format(L"Already quarantined: %s", formatted.c_str());
			write_console_line(line.c_str());
			return CONFIG_CHECK_OK;
		}
		if(entries.size() >= Nilesoft::Shell::Quarantine::MaxEntries)
		{
			write_console_line(L"shell.exe -quarantine: the list is full.");
			return CONFIG_CHECK_UNUSABLE;
		}

		Nilesoft::Shell::Quarantine::Entry entry;
		entry.clsid = clsid;
		entry.hash = hash;
		entries.push_back(std::move(entry));
	}
	else
	{
		if(at >= entries.size())
		{
			line.append_format(L"Not quarantined: %s", formatted.c_str());
			write_console_line(line.c_str());
			return CONFIG_CHECK_OK;
		}
		entries.erase(entries.begin() + static_cast<ptrdiff_t>(at));
	}

	if(!Nilesoft::Shell::Quarantine::save(path, entries))
	{
		string failed;
		failed.append_format(L"shell.exe -quarantine: could not write %s", path.c_str());
		write_console_line(failed.c_str());
		return CONFIG_CHECK_UNUSABLE;
	}

	line.append_format(adding ? L"Quarantined %s\r\n" : L"Released %s\r\n", formatted.c_str());
	line.append(L"  Takes effect on the next menu each host builds.");
	write_console_line(line.c_str());
	return CONFIG_CHECK_OK;
}

bool Register(REGOP reg, HWND hwnd = nullptr)
{
	string path = IO::Path::Combine(IO::Path::Parent(IO::Path::Module(nullptr)), dll_name).move();
	DLL dll(path, true);

	if(reg.REGISTER || reg.UNREGISTER)
	{
		if(!is_elevated)
		{
			// Missing administrative privileges!
			string msg = loadstring(IDS_ADMIN_PRIVILEGES, dll).move();
			_log->error(msg);
			if(!reg.SILENT)
			{
				//You will need to provide administrator permission to run this Shell
				::MessageBoxW(hwnd, msg, APP_FULLNAME, MB_ICONWARNING);
			}
			return false;
		}
	}
	
	if(!dll)
	{
		auto ernf = L"shell.dll not found.";
		_log->error(ernf);
		if(!reg.SILENT)
			MessageBox::Show(ernf, APP_FULLNAME, MessageBoxIcon::Warning);
		return false;
	}

	auto ret = Registration(reg);
	_log->close();

	return ret;
}

/*
class Console
{
	FILE *_handle = nullptr;
	HWND _hwnd = nullptr;

	bool init()
	{
		if(!_handle)
		{
			if(::AttachConsole(ATTACH_PARENT_PROCESS))
			{
				_hwnd = ::GetConsoleWindow();
				if(_hwnd) if(0 == ::freopen_s(&_handle, "CONOUT$", "w", stdout))
					::setvbuf(stdout, nullptr, _IONBF, 0);
			}
		}
		return _handle;
	}

public:
	Console()
	{
		init();
	}

	~Console()
	{
		if(_handle)
		{
			if(_hwnd)
				::PostMessageW(_hwnd, WM_KEYUP, VK_RETURN, 0);
			::fflush(_handle);
			::fclose(_handle);
			::FreeConsole();
		}
	}

	Console &write(const wchar_t *buff = nullptr)
	{
		wprintf(buff);
		return *this;
	}

	Console &writel(const wchar_t *buff = nullptr)
	{
		wprintf(L"%s%c", buff, L'\n');
		return *this;
	}
};

void check()
{
	Console c;
	c.writel(L"\n\nNilesoft Shell\n");

	_log->info(L"BEGIN CHECK");

	auto ver = &Windows::Version::Instance();

	auto clsid =[](const string &s)->string
	{
		return (L"CLSID\\" + s).move();
	};

	auto clsid_shex = [=](const string &s)->string
	{
		return (s + L"\\shellex\\ContextMenuHandlers\\nilesoft.shell").move();
	};

	auto exists_key = [=](HKEY k, const string &name)
	{
		if(!Registry::Exists(k, name))
		{
			wprintf(L"[warning] %s\n", name.c_str());
			_log->warning(name);
		}
	};

	auto exists_value_name = [=](HKEY k, const string &subkey, const string &name)
	{
		if(!Registry::ExistsValue(k, subkey, name))
		{
			wprintf(L"[warning] %s\\%s\n", name.c_str(), name.c_str());
			_log->warning(subkey + L"\\" + name);
		}
	};

	exists_key(HKCR, clsid(CLS_ContextMenu));
	exists_key(HKCR, clsid(CLS_FolderExtensions));
	exists_key(HKCR, clsid(CLS_IconOverlay));

	if(ver->IsWindows11OrGreater())
	{
		exists_key(HKCR, string().format(L"CLSID\\%s\\TreatAs", string::ToString(IID_FileExplorerContextMenu, 2).c_str()));
	}

	exists_key(HKCR, clsid_shex(L"*"));
	exists_key(HKCR, clsid_shex(L"Directory"));
	exists_key(HKCR, clsid_shex(L"Directory\\Background"));
	exists_key(HKCR, clsid_shex(L"Drive"));
	exists_key(HKCR, clsid_shex(L"DesktopBackground"));

	exists_value_name(HKLM, HKLM_APPROVED, CLS_ContextMenu);
	
	_log->info(L"END CHECK");
}
*/

struct Theme
{
	int mode = 1;

	struct 
	{
		uint32_t nor = 0xF1F1F1;
		uint32_t sel = 0xF1F1F1;
		uint32_t dis = 0x808080;
	}text;

	struct
	{
		uint32_t nor = 0x000000;
		uint32_t sel = 0x181818;
		uint32_t dis = 0x000000;
	}back;

	struct{
		uint32_t color = 0x000000;
		uint32_t border = 0x404040;
		uint32_t size = 1;
	}frame;
};

Theme m_theme;

HTHEME _hTheme = nullptr;

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
	//auto argc = __argc;
	//auto argv = __wargv;

	is_elevated = Security::Elevation::IsElevated();

	COM_INITIALIZER com_initializer(true);

    CommandLine cmdline;
	REGOP reg{};

	_hInstance = hInstance;
	_log = &Logger::Instance();

	if(cmdline.ShowHelp)
	{
		//::DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_CMDLINE), nullptr, (DLGPROC)WndProc, 0);
		auto hWnd = ::CreateDialogParamW(_hInstance, MAKEINTRESOURCEW(IDD_CMDLINE), NULL, (DLGPROC)WndProc, 0);
		if(hWnd)
		{
			// CenterWindow(hwnd);
			::ShowWindow(hWnd, SW_SHOW);

			MSG msg;
			BOOL bRet;
			while((bRet = ::GetMessageW(&msg, nullptr, 0, 0)) != 0)
			{
				if(bRet == -1)
				{
					// Handle the error and possibly exit
					break;
				}
				else if(!IsWindow(hWnd) || !::IsDialogMessageW(hWnd, &msg))
				{
					::TranslateMessage(&msg);
					::DispatchMessage(&msg);
				}
			}
			::DestroyWindow(hWnd);
		}
		_log->close();
		return 0;
	}
	else if(cmdline.empty())
    {
		auto shell_window = ::FindWindowExW(nullptr, nullptr, UI::WC_Window, APP_FULLNAME);
		if (shell_window)
		{
			::SetForegroundWindow(shell_window);
			::SetActiveWindow(shell_window);
			//::SwitchToThisWindow(shell_window, TRUE);
			return 0;
		}

		auto win = Windows::Version::Instance();

		if(win.IsWindowsVersionOrGreater(10, 0, 14393))
			_dpi = DLL::User32<uint32_t>("GetSystemDpiForProcess", ::GetCurrentProcess());
		else
			_dpi = 96;// DC(GetDesktopWindow()).GetDeviceCapsY();

		_hTheme = ::OpenThemeData(nullptr, L"MENU");


        UI::App app(_hInstance, WindowProc, m_theme.frame.color,
                    LoadIcon(_hInstance, MAKEINTRESOURCE(IDI_RPMICON)), CS_HREDRAW | CS_VREDRAW);

        app.Init();

        RECT rc_screen{};
        ::GetClientRect(::GetDesktopWindow(), &rc_screen);

        rc_window.right = dpi(430);
        rc_window.bottom = dpi(220);

        rc_window.left = (rc_screen.right - rc_window.right) / 2;
        rc_window.top = (rc_screen.bottom - rc_window.bottom) / 2;

        main_window = new UI::Window(APP_FULLNAME, rc_window, 0, nullptr, 0);

        if(!main_window || !main_window->Handle)
        {
            return FALSE;
        }

		string path = IO::Path::Module(hInstance);
		path = path.substr(0, path.length() - 3).append(L"dll");

		DLL d(path, true);
		if(d)
		{
			auto hRes = ::FindResourceW(d, L"FONTICON", RT_RCDATA);
			if(hRes)
			{
				auto cjSize = ::SizeofResource(d, hRes);
				auto hResData = ::LoadResource(d, hRes);
				if(hResData)
				{
					auto lpFileView = ::LockResource(hResData);
					if(lpFileView)
					{
						DWORD numFonts = 0;
						_fonthandle = ::AddFontMemResourceEx(lpFileView, cjSize, nullptr, &numFonts);
						if(_fonthandle)
						{
							_hfont_icon = ::CreateFontW(-dpi(20), 0, 0, 0, FW_NORMAL, 0, 0, 0,
														SYMBOL_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
														ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Nilesoft.Shell");
						}
						UnlockResource(lpFileView);
					}
					::FreeResource(hResData);
				}
			}
		}

        main_window->Color.Background = color_background;

        int btn_w = dpi(250), btn_h = dpi(30), left = dpi(150);
		int offset_2 = dpi(2);

        rc_reg.top = dpi(40);
        rc_unreg.top = rc_reg.top + offset_2 + btn_h;

        rc_reg.bottom = btn_h;
        rc_unreg.bottom = btn_h;

        rc_reg.right = btn_w;
        rc_unreg.right = btn_w;

        rc_reg.left = left;
        rc_unreg.left = rc_reg.left;
		
		std::string dd = "<svg viewBox='0 0 512 512' width='100%' height='100%'><path fill='#14DCFE' d='M200 100L300 0l106 106-100 100z'/><path fill='#14A0FF' d='M100 200l100-100 212 212-100 100z'/><path fill='#1465FF' d='M106 406l100-100 106 106-100 100z'/></svg>";
		PlutoVG pluto(dd.data(), static_cast<int>(dd.length()), dpi(96), dpi(96), 96);
		hbitmap_logo = pluto.tobitmap();

		auto btn_close = new UI::Button(L"\uE256", { rc_window.right - dpi(27), dpi(1), dpi(26), dpi(26) }, ID_CLOSE, main_window, BS_OWNERDRAW, _hfont_icon, L"Close");
        
		auto btn_reg = new UI::Button(L"Register\tCtrl+R", rc_reg, ID_REG, main_window, BS_OWNERDRAW);
        auto btn_unreg = new UI::Button(L"Unregister\tCtrl+U", rc_unreg, ID_UNREG, main_window, BS_OWNERDRAW);
        auto btn_restart = new UI::Button(L"Restart Explorer\tCtrl+X", { rc_reg.left, rc_unreg.top + offset_2 + btn_h, btn_w, btn_h }, ID_RESTART, main_window, BS_OWNERDRAW);

        auto tt = btn_restart->Rect.top + dpi(30) + btn_h + offset_2;
        auto tl = btn_restart->Rect.left;

        auto btn_web = new UI::Button(L"\uE11F", { tl, tt, btn_h, btn_h }, ID_WEB, main_window, BS_OWNERDRAW, _hfont_icon,  L"Website Ctrl+W");
       
        tl += btn_h + offset_2;
        auto btn_email = new UI::Button(L"\uE115", { tl, tt, btn_h, btn_h }, ID_EMAIL, main_window, BS_OWNERDRAW, _hfont_icon, L"Email Ctrl+E");
       
        tl += btn_h + offset_2;
        auto btn_bug = new UI::Button(L"\uE22B", { tl, tt, btn_h, btn_h }, ID_GITHUB, main_window, BS_OWNERDRAW, _hfont_icon, L"Github Ctrl+G");

		//tl += (dpi(50) - btn_h) + btn_h + dpi(12);
		tl += btn_h + offset_2;
        auto btn_donate = new UI::Button(L"\uE1A8", { tl, tt, btn_h, btn_h }, ID_DONATE, main_window, BS_OWNERDRAW, _hfont_icon, L"Donate Ctrl+D");


        main_window->SetColor({ btn_reg, btn_unreg,btn_restart,btn_donate,btn_web,btn_email,btn_bug }, 
							  m_theme.text.nor, m_theme.back.nor, m_theme.text.sel, m_theme.back.sel);//0xeeeee0
        main_window->SetColor({ btn_close }, 0xFFFFFF, m_theme.back.nor, m_theme.text.nor, 0x2311E8);//E81123

		btn_reg->OnDraw = btn_unreg->OnDraw = btn_restart->OnDraw = btn_on_paint;

        auto ret = app.Run(main_window);

		delete main_window; 
		_log->close();

		if(_hfont_icon)
			::DeleteObject(_hfont_icon);

		if(_fonthandle)
			::RemoveFontMemResourceEx(_fonthandle);
		
		if(_hTheme)
		{
			::CloseThemeData(_hTheme);
			_hTheme = nullptr;
		}

		return ret;
    }
    else
    {
		// -check accepts the file as a value (-check:"C:\path\shell.nss") or as
		// the next bare argument; empty means "whatever this machine would load".
		bool _check = false;
		string check_path;

		// -report perf, and -report perf:all for every session rather than the
		// slowest one. Same shape as -check: reports and exits, publishes
		// nothing, touches no running shell.
		bool _report = false;
		string report_what;

		// -quarantine[:list|:add|:remove] [clsid]. The treatment for what
		// -report perf diagnoses; writes a per-user file and exits.
		// docs/refactor/05-capabilities.md section 1b.
		bool _quarantine = false;
		string quarantine_action;
		string quarantine_argument;
		//bool runglyphs = false;
        string cmd;
        for(auto op : cmdline)
        {
            if(op->Kind == '-' || op->Kind == '/')
            {
				if(op->has_name({ L"g", L"glyphs" }))
				{
					//runglyphs = true;
					//break;
				}
				else  if(op->has_name({ L"r", L"register", L"e", L"enable" }))
				{
					reg.REGISTER = true;;
                }
                else if(op->has_name({ L"u", L"unregister", L"d", L"disable" }))
                {
					reg.UNREGISTER = true;
                }
                else if(op->has_name({ L"t", L"treat" }))
                {
					reg.TREAT = true;
                }
				else if(op->has_name({ L"f", L"force" }))
				{
					reg.FOLDEREXTENSIONS=true;
				}
                else if(op->has_name({ L"s", L"silent" }))
                {
					reg.SILENT=true;
                }
                else if(op->has_name(L"restart"))
                {
					reg.RESTART = true;
                }
                else if(op->has_name(L"cmd"))
                {
                    cmd = op->Value;
                }
				else if(op->has_name({ L"c", L"check" }))
				{
					_check = true;
					if(!op->Value.empty())
						check_path = op->Value;
				}
				else if(op->has_name(L"report"))
				{
					_report = true;
					if(!op->Value.empty())
						report_what = op->Value;
				}
				else if(op->has_name(L"quarantine"))
				{
					_quarantine = true;
					if(!op->Value.empty())
						quarantine_action = op->Value;
				}
            }
			else if(op->has_name(L"check"))
			{
				_check = true;
			}
			else if(_quarantine && quarantine_argument.empty())
			{
				// The CLSID. Argument rather than Name for the same reason
				// -check's path is: CommandLine splits at the first colon, and
				// a GUID has none but a future subject might.
				quarantine_argument = op->Argument;
			}
			else if(_report && report_what.empty())
			{
				// A bare word after -report is what to report on, the same way
				// a bare path after -check is the file. Argument rather than
				// Name, for the reason spelled out below.
				report_what = op->Argument;
			}
			else if(_check && check_path.empty())
			{
				// A bare argument after -check is the file to check; nothing
				// else on this command line takes a positional argument.
				//
				// Argument, not Name: CommandLine splits every argument at its
				// first colon into Name and Value, so a path lands here as
				// Name="C" and Value="\dir\shell.nss". Argument is the text as
				// it was typed, which is the only form that survives a drive
				// letter.
				check_path = op->Argument;
			}
        }

		if(_report)
		{
			// `perf` is the only subject there is, and it is the default: the
			// command is `-report perf` in the plan, but somebody who types
			// `-report` has said enough. An unknown subject is refused rather
			// than silently treated as perf - a typo that prints a report of
			// something else is worse than one that says what it wanted.
			auto what = report_what.empty() ? string(L"perf") : report_what;

			bool detailed = what.equals(L"perf:all", true) || what.equals(L"all", true);
			if(!detailed && !what.equals(L"perf", true))
			{
				write_console_line(L"shell.exe -report: the only report is `perf` (or `perf:all`).");
				_log->close();
				return CONFIG_CHECK_UNUSABLE;
			}

			auto code = ReportPerf(detailed);
			_log->close();
			return code;
		}

		if(_quarantine)
		{
			auto code = QuarantineCommand(quarantine_action, quarantine_argument);
			_log->close();
			return code;
		}

		if(_check)
		{
			// -check reports on a file and exits. It publishes nothing, starts
			// no UI and does not touch the running shell, which is what makes
			// it safe to run while Explorer is up.
			// docs/refactor/03-config-safety.md section 1b step 4
			auto code = CheckConfig(check_path.empty() ? nullptr : check_path.c_str());
			_log->close();
			return code;
		}

		if(reg.REGISTER || reg.UNREGISTER || reg.RESTART || reg.FOLDEREXTENSIONS)
		{
			// Register returns a bool, and this returned it straight out of
			// wWinMain - so a successful registration exited with 1 and a failed
			// one with 0, which is backwards from what every caller of a process
			// assumes. Nothing checked it, so nothing noticed; the installer
			// checks it now.
			return Register(reg) ? 0 : 1;
		}
        else
        {
            if(!cmd.empty())
            {
                int run = 1;
                if(auto o = cmdline[L"runas"])
                    run = (int)string::ToInt(o);

				int sw = SW_SHOWNORMAL;
				if(auto o = cmdline[L"show"])
					sw = (int)string::ToInt(o);

                string verb = L"open";
                if(run > 1)
                    verb = L"runas";

                ::ShellExecuteW(nullptr,
                                verb,
                                cmd,
                                cmdline[L"args"],
                                cmdline[L"wd"],
								sw);
            }
        }
    }

	_log->close();
    return 1;
}

HFONT FontSize(HFONT hFont, int size, int weight = 0, byte quality= ANTIALIASED_QUALITY)
{
    LOGFONT lf{};
	if(::GetObject(hFont, sizeof(LOGFONT), &lf))
	{
		lf.lfHeight = -size;
		lf.lfWidth = 0;
		if(weight > 0)
			lf.lfWeight = weight;
		lf.lfQuality = quality;
		return ::CreateFontIndirect(&lf);
	}
    return nullptr;
}

bool hover = false;
using namespace Nilesoft::Drawing;

void DrawPng(HDC hdc, HBITMAP hbitmap, int x, int y, int size = 16, int sx = 0, int sy = 0, int alpha = 255)
{
	if(hdc && hbitmap)
	{
		auto memDC = ::CreateCompatibleDC(hdc);
		if(memDC)
		{
			auto prev_bitmap = ::SelectObject(memDC, hbitmap);
			BLENDFUNCTION bf{};
			bf.BlendOp = AC_SRC_OVER;
			bf.BlendFlags = 0;
			bf.SourceConstantAlpha = (byte)alpha;
			bf.AlphaFormat = AC_SRC_ALPHA;
			::GdiAlphaBlend(hdc, x, y, size, size, memDC, sx, sy, size, size, bf);
			// Clean up
			::SelectObject(memDC, prev_bitmap);
			::DeleteDC(memDC);
		}
	}
}

void DrawString(HDC hdc, HFONT hFont, RECT *rc, COLORREF color, const wchar_t *text, int length, DWORD format, uint8_t opacity)
{
	if(!_hTheme || !hdc || !hFont) return;

	BLENDFUNCTION bf = { AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA };
	BP_PAINTPARAMS params = { sizeof(BP_PAINTPARAMS) };
	params.dwFlags = BPPF_ERASE | BPPF_NOCLIP;
	params.pBlendFunction = &bf;
	HDC hdcPaint = nullptr;
	auto hBufferedPaint = ::BeginBufferedPaint(hdc, rc, BPBF_TOPDOWNDIB, &params, &hdcPaint);
	if(hBufferedPaint)
	{
		if(hdcPaint)
		{
			::SetTextColor(hdcPaint, color);
			::SetBkMode(hdcPaint, TRANSPARENT);

			auto hFontOld = ::SelectObject(hdcPaint, hFont);
			DTTOPTS dttOpts = { sizeof(DTTOPTS),  DTT_COMPOSITED | DTT_TEXTCOLOR, color };
			::BufferedPaintClear(hBufferedPaint, rc);
			::DrawThemeTextEx(_hTheme, hdcPaint, 0, 0, text, length, format, rc, &dttOpts);
			::SelectObject(hdcPaint, hFontOld);
		}
		::EndBufferedPaint(hBufferedPaint, TRUE);
	}
}

int __stdcall btn_on_paint(UI::Control* s, int msg, [[maybe_unused]] WPARAM wp, [[maybe_unused]] LPARAM lp)
{
	if(msg == WM_MOUSEMOVE)
		return 1;

	RECT rect{};
	::GetClientRect(s->Handle, &rect);

	DC dc(s->Handle);

	auto color_txt = msg == WM_MOUSEHOVER ? s->Color.Select.Text : s->Color.Text;
	auto color_bg = msg == WM_MOUSEHOVER ? s->Color.Select.Background : s->Color.Background;

	if(msg == WM_MOUSEHOVER || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)
	{
		//if(wp & MK_LBUTTON)
		{
			color_bg = s->Color.Select.Background;
		}
	}

	// if(color_bg == -1)
   //      color_bg = c->Parent->Color.Select.Background;

	if(color_bg != -1)
	{
		auto br = ::CreateSolidBrush(color_bg);
		::FillRect(dc, &rect, br);
		::DeleteObject(br);
	}

	if(!s->Text.empty())
	{
		auto ofont = s->Font.Handle;
		LOGFONT lf{};
		GetObject(ofont, sizeof(LOGFONT), &lf);

		lf.lfQuality = CLEARTYPE_QUALITY;
		//lf.lfWeight = FW_SEMIBOLD;
		lf.lfHeight = -dpi(14);
		auto cfont = ::CreateFontIndirect(&lf);
		auto tf = DT_VCENTER | DT_SINGLELINE | DT_NOCLIP;

		auto pos = s->Text.index_of('\t');
		if(pos != string::npos)
		{
			string left = s->Text.substr(0, pos);
			string right = s->Text.substr(pos++);

			auto rc = rect;
			rc.left = dpi(20);
			DrawString(dc, cfont, &rc, color_txt, left, left.length<int>(), DT_LEFT | tf, 255);

			rc.right -= dpi(20);
			DrawString(dc, cfont, &rc, m_theme.text.dis, right, right.length<int>(), DT_RIGHT | tf, 128);
		}
		else
		{
			DrawString(dc, cfont, &rect, color_txt, s->Text, -1, s->Font.Format.Align | tf, 255);
		}
		::DeleteObject(cfont);
	}
    return 0;
};

void Open(HWND hWnd, const wchar_t* cmd)
{
	::ShellExecuteW(hWnd, L"open", cmd, nullptr, nullptr, SW_NORMAL);
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message)
    {
		case WM_CREATE:
		{
			::SendMessageW(hWnd, WM_SETFONT, (WPARAM)::GetStockObject(DEFAULT_GUI_FONT), TRUE);
			break;
		}
        case WM_ACTIVATE:// Extend the frame into the client area.
        {
           // MARGINS margins{ 0,0,0,1 };
            //::DwmExtendFrameIntoClientArea(hWnd, &margins);
            break;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            DC dc = BeginPaint(hWnd, &ps);
			{
				RECT r;
				::GetWindowRect(hWnd, &r);
				::OffsetRect(&r, -r.left,- r.top);
				dc.draw_fillrect(r, m_theme.frame.color, m_theme.frame.border);

				BITMAP bmp{};
				GetObject(hbitmap_logo, sizeof(BITMAP), &bmp);
				//draw logo
				DrawPng(dc, hbitmap_logo, dpi(35), dpi(35), bmp.bmHeight, 0, 0, is_elevated ? 240 : 128);

				dc.set_back_mode();
				string text = APP_NAME L" ";
#ifdef APP_CANARY
				text += APP_CANARY L" ";
#endif
				text += APP_VERSION;

				dc.set_text(0x808080);

				auto ofont = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
				auto cfont = FontSize(ofont, dpi(14), CLEARTYPE_NATURAL_QUALITY);
				ofont = dc.set_font(cfont);

				RECT rect = { 0, dpi(150 + 10), dpi(150), dpi(25 + 128 + 32) };

				DrawString(dc, cfont, &rect, 0x808080, text, text.length<int>(), DT_CENTER, 255);

				text.format(L"\xA9 %d %s", VERSION_YEAR, APP_COMPANY);
				rect = { 0, dpi(150 + 30), dpi(150), dpi(25 + 128 + 32 + 15) };
				DrawString(dc, cfont, &rect, 0x808080, text, text.length<int>(), DT_CENTER, 255);
				::DeleteObject(::SelectObject(dc, ofont));
			}
            ::EndPaint(hWnd, &ps);
            return TRUE;
        }
        //case WM_ERASEBKGND:
        //    return 0;
        case WM_NCCALCSIZE:
            return FALSE;
        case WM_NCHITTEST:
            return HTCAPTION;
		case WM_ERASEBKGND:
			return TRUE;
        case WM_COMMAND:
        {
            switch(wParam)
            {
                case ID_CLOSE:
                    ::DestroyWindow(hWnd);
                    break;
                case ID_MINIMIZE:
                    SendMessage(hWnd, WM_SYSCOMMAND, SC_MINIMIZE, lParam);
                    break;
                case ID_WEB:
					Open(hWnd, APP_WEBSITE);
                    break;
                case ID_DONATE:
					Open(hWnd, L"https://nilesoft.org/donate");
                    break;
                case ID_EMAIL:
					Open(hWnd, L"mailto:support@nilesoft.org");
                    break;
                case ID_GITHUB:
					Open(hWnd, L"https://github.com/moudey/shell");
                    break;
				case ID_RESTART:
					Windows::Explorer::Restart();
					break;
                case ID_REG:
				case ID_UNREG:
				{
					REGOP reg{};
					reg.RESTART = true;
					reg.TREAT = true;
					if(wParam == ID_REG)
						reg.REGISTER = true;
					else if(wParam == ID_UNREG)
						reg.UNREGISTER = true;
					return Register(reg, hWnd);
				}
            }
            break;
        }
        case WM_SYSKEYUP:
        case WM_KEYUP:
      //      return ::DefWindowProc(hWnd, message, wParam, lParam);
     //   case WM_SYSKEYDOWN:
     //   case WM_KEYDOWN:
        {
            if(::GetAsyncKeyState(VK_CONTROL) < 0)
            {
                //  int count = 0;
                //  for(int i = 0; i < 256; i++)
                //      count += IsAsyncKeyDown(i);

                //  if(count == 2)
                {
                    auto vkCode = LOWORD(wParam); // virtual-key code
                    switch(vkCode)
                    {
                        case 'R':
                            main_window->SendCommand(ID_REG);
                            break;
                        case 'U':
                            main_window->SendCommand(ID_UNREG);
                            break;
                        case 'X':
                            main_window->SendCommand(ID_RESTART);
                            break;
                        case 'W':
                            main_window->SendCommand(ID_WEB);
                            break;
                        case 'E':
                            main_window->SendCommand(ID_EMAIL);
                            break;
                        case 'G':
                            main_window->SendCommand(ID_GITHUB);
                            break;
                        case 'D':
                            main_window->SendCommand(ID_DONATE);
                            break;
                    }
                }
            }
            break;
        }
        case WM_CLOSE:
            ::DestroyWindow(hWnd);
            break;
        case WM_DESTROY:
        case WM_ENDSESSION:
            ::PostQuitMessage(0);
            break;
        default:
            return ::DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

BOOL CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, [[maybe_unused]] LPARAM lParam)
{
    switch(message)
    {
		case WM_INITDIALOG:
        {
			//WCHAR szTitle[100];
			// LoadString(g_hInstance, IDS_APP_TITLE, szTitle, ARRAYSIZE(szTitle));
            auto usage = L"Command-Line Help\r\n\r\n"
                //L"shell.exe [-[r][u]] [-i] [-s] [-re]\n\n"
                L"-register\t\tRegistering.\r\n"
                L"-unregister\tUnregistering.\r\n"
                L"-treat\t\tDisable Windows 11 context menu.\r\n"
                L"-silent\t\tPrevents displaying messages.\r\n"
                L"-restart\t\tRestart Windows Explorer.\r\n"
                L"-check[:file]\tParse the configuration and report; change nothing.\r\n"
                L"\t\tExits 0 when it parses and 1 when it does not.\r\n"
                L"-report perf\tWhat the menus in every host on this desktop cost.\r\n"
                L"\t\tAdd `perf:all` for every recorded session, not just the slowest.\r\n"
                L"-quarantine\tExtensions Shell stops asking when it builds a menu.\r\n"
                L"\t\t-quarantine:add {clsid} / :remove {clsid} / :list (the default).\r\n"
                L"\t\tThey keep working everywhere else. Next menu, no restart.\r\n\r\n"
                //L"-runas:N\t\tLaunch with elevated privileges.\r\n"
                //L"\t\tN=[admin | system | trustedinsaller]\r\n\r\n"
                L"-?\t\tDispay this help message.\r\n\r\n"
                L"Examples:\r\nshell.exe -register -treat\r\n"
                L"shell.exe -check\r\n"
                L"shell.exe -report perf\r\n"
              //  L"shell.exe -runas:admin -cmd:'cmd.exe' -args:\"/K echo Hello world!\"\r\n"
                ;
            ::SetDlgItemTextW(hwnd, IDC_CMDLINE_TEXT, usage);
           // SendDlgItemMessage(hwnd, IDC_CMDLINE_TEXT, EM_SETSEL, -1, -1);
            break;
        }
        case WM_CLOSE:
        {
            ::PostQuitMessage(0);
            break;
        }
        case WM_COMMAND:
        {   
            if(LOWORD(wParam) == IDOK)
                ::PostQuitMessage(0);
            break;
        }
       
    }
    return FALSE;
}
