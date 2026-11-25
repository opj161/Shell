#define VC_EXTRALEAN
//#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <msi.h>
#include <msiquery.h>
#include <string>
#include <sddl.h>
#include "../../shared/LegacyConfigTransfer.h"
#include "../../shared/TreatAsRegistry.h"
#include "TreatAsPlan.h"
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "msi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

template<typename ... Args>
static void log(const char *format, Args ... args)
{
	/*
	using ___snprintfFT = size_t(__stdcall *)(char *, size_t, const char *, Args... args);
	if(auto hm = ::GetModuleHandleW(L"msvcrt.dll"); hm)
	{
		auto ___snprintf = (___snprintfFT)::GetProcAddress(hm, "_snprintf");
		if(___snprintf)
		{
			auto size = ___snprintf(nullptr, 0, format, args ...);
			if(size > 0)
			{
				std::string buf(size, '\0');
				___snprintf(buf.data(), size, format, args ...);
				::OutputDebugStringA(buf.c_str());
			}
		}
	}
	*/
	auto size = ::_snprintf(nullptr, 0, format, args ...);
	if(size > 0)
	{
		std::string buf(size, '\0');
		::_snprintf(buf.data(), size, format, args ...);
		::OutputDebugStringA(buf.c_str());
	}
}

struct MSI
{
	MSIHANDLE hInstall = 0;

	MSI(MSIHANDLE hInstall = 0)
		: hInstall{ hInstall }
	{
	}

	bool get(const wchar_t *name, std::wstring &value) const
	{
		if(hInstall && name != nullptr)
		{
			wchar_t empty = L'\0';
			DWORD required = 0;
			auto result = ::MsiGetPropertyW(hInstall, name, &empty, &required);
			if(result == ERROR_MORE_DATA && required > 0)
			{
				std::wstring buffer(required + 1, L'\0');
				DWORD capacity = required + 1;
				result = ::MsiGetPropertyW(hInstall, name, buffer.data(), &capacity);
				if(result == ERROR_SUCCESS)
				{
					buffer.resize(capacity);
					value = std::move(buffer);
					return true;
				}
			}
		}
		value.clear();
		return false;
	}


	std::wstring InstallFolder()
	{
		std::wstring result;
		get(L"INSTALLFOLDER", result);
		return std::move(result);
	}

	std::wstring ProductCode()
	{
		std::wstring result;
		get(L"ProductCode", result);
		return std::move(result);
	}

	bool set(const wchar_t *name, const wchar_t *value) const
	{
		if(hInstall && name != nullptr)
		{
			return ERROR_SUCCESS == ::MsiSetPropertyW(hInstall, name, value);
		}
		return false;
	}
};

constexpr auto FILECONFIG = L"shell.nss";
constexpr auto FILECONFIGSTOCK = L"shell.nss.stock-new";

constexpr auto TREATAS_WARNING = 25001;
constexpr auto TREATAS_REMOVE_ERROR = 25002;
constexpr auto ROLLBACK_REQUIRED_ERROR = 25003;

namespace TreatAs = Nilesoft::TreatAsRegistry;

// Borrowing write access to TreatAs::Parent reports through a plain function
// pointer, which cannot carry an MSIHANDLE. The deferred custom actions here are
// single-threaded, so BorrowReportScope below parks the current one here for the
// duration of one action and the diagnostic reaches the MSI log instead of
// nowhere. MsiProcessMessage is one of the few handle-taking functions a
// deferred custom action may call.
// https://learn.microsoft.com/windows/win32/msi/obtaining-context-information-for-deferred-execution-custom-actions
static MSIHANDLE g_borrow_install = 0;

static void BorrowReportToInstaller(const wchar_t *message)
{
	::OutputDebugStringW(message);
	if(!g_borrow_install || !message)
		return;
	PMSIHANDLE record = ::MsiCreateRecord(1);
	if(!record)
		return;
	::MsiRecordSetStringW(record, 0, message);
	::MsiProcessMessage(g_borrow_install, INSTALLMESSAGE_INFO, record);
}

// The handle is the installer's, and it is only valid for the duration of the
// custom action it was handed to. Assigning it at entry and relying on every
// return path to clear it is how a stale MSIHANDLE ends up being passed to
// MsiProcessMessage later; both entry points below have several returns, and
// this DLL stays loaded between actions.
struct BorrowReportScope
{
	explicit BorrowReportScope(MSIHANDLE h) { g_borrow_install = h; }
	~BorrowReportScope() { g_borrow_install = 0; }
	BorrowReportScope(const BorrowReportScope &) = delete;
	BorrowReportScope &operator=(const BorrowReportScope &) = delete;
};

// The custom actions run deferred and no-impersonate, i.e. as LocalSystem, and
// LocalSystem holds ReadKey on TreatAs::Parent and nothing more - measured on
// Windows 11 26200 on 2026-08-27 by opening the key from a SYSTEM scheduled
// task: CreateSubKey and Delete are both refused, ReadKey succeeds. So
// TreatAs::Create and TreatAs::Remove borrow write access (src/shared/
// TreatAsRegistry.h), the same code shell.exe runs. Without it install warned
// and skipped the redirect, and uninstall returned ERROR_INSTALL_FAILURE once a
// redirect existed.
//
// SeTakeOwnershipPrivilege and SeRestorePrivilege are present but disabled in
// that token, which is what BorrowedKeyAccess::acquire enables.

class FileHandle
{
public:
	FileHandle() = default;
	explicit FileHandle(HANDLE handle) : _handle{ handle } {}
	FileHandle(const FileHandle &) = delete;
	FileHandle &operator=(const FileHandle &) = delete;
	~FileHandle() { if(valid()) ::CloseHandle(_handle); }

	bool valid() const { return _handle && _handle != INVALID_HANDLE_VALUE; }
	operator HANDLE() const { return _handle; }
	void close() { if(valid()) ::CloseHandle(_handle); _handle = INVALID_HANDLE_VALUE; }

private:
	HANDLE _handle{ INVALID_HANDLE_VALUE };
};

enum class MarkerState
{
	absent,
	present,
	invalid
};

static std::wstring JoinPath(const std::wstring &path1, const std::wstring &path2);
static std::wstring ProgramDataFolder();
static bool EnsurePlainDirectory(const std::wstring &path);
static bool RandomToken(std::wstring &token);

static bool EnsureMarkerParents(const std::wstring &marker)
{
	if(!Nilesoft::TreatAsPlan::MarkerPathLooksOwned(marker))
		return false;

	auto slash = marker.find_last_of(L'\\');
	if(slash == std::wstring::npos)
		return false;

	auto root = ProgramDataFolder();
	if(root.empty())
		return false;
	auto vendor = JoinPath(root, L"Nilesoft");
	auto product = JoinPath(vendor, L"Shell");
	auto staging = JoinPath(product, L"Staging");
	if(::CompareStringOrdinal(marker.substr(0, slash).c_str(), -1,
		staging.c_str(), -1, TRUE) != CSTR_EQUAL)
		return false;

	return EnsurePlainDirectory(vendor) && EnsurePlainDirectory(product)
		&& EnsurePlainDirectory(staging);
}

// Restrictive DACL at create time: protected, SYSTEM + Administrators FILE_ALL,
// no Users, no inherit. A world-writable marker would be a delete primitive
// for the TreatAs key.
//
//   https://learn.microsoft.com/windows/win32/secbp/creating-a-dacl
//   https://learn.microsoft.com/windows/win32/secauthz/security-descriptor-string-format
//   https://learn.microsoft.com/windows/win32/api/sddl/nf-sddl-convertstringsecuritydescriptortosecuritydescriptorw
//   https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-createfilew
static bool WriteCreatedMarker(const std::wstring &path)
{
	if(!EnsureMarkerParents(path))
		return false;

	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = FALSE;
	if(!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
		L"D:P(A;;FA;;;SY)(A;;FA;;;BA)",
		SDDL_REVISION_1,
		&sa.lpSecurityDescriptor,
		nullptr))
		return false;

	HANDLE raw = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW,
		FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_WRITE_THROUGH,
		nullptr);
	::LocalFree(sa.lpSecurityDescriptor);
	FileHandle file{ raw };
	return file.valid();
}

static MarkerState QueryCreatedMarker(const std::wstring &path)
{
	if(!Nilesoft::TreatAsPlan::MarkerPathLooksOwned(path))
		return MarkerState::invalid;

	auto attributes = ::GetFileAttributesW(path.c_str());
	if(attributes == INVALID_FILE_ATTRIBUTES)
	{
		auto error = ::GetLastError();
		if(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			return MarkerState::absent;
		return MarkerState::invalid;
	}
	if(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
		return MarkerState::invalid;

	FileHandle file{ ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
	if(!file.valid() || !Nilesoft::LegacyConfigTransfer::regular_non_reparse(file))
		return MarkerState::invalid;
	return MarkerState::present;
}

static bool BuildCreatedMarkerPath(std::wstring &path)
{
	auto root = ProgramDataFolder();
	if(root.empty())
		return false;

	std::wstring token;
	if(!RandomToken(token))
		return false;

	path = JoinPath(JoinPath(JoinPath(JoinPath(root, L"Nilesoft"), L"Shell"),
		L"Staging"), L"treatas." + token + L".created");
	return Nilesoft::TreatAsPlan::MarkerPathLooksOwned(path);
}

static void InstallerMessage(MSIHANDLE hInstall, INSTALLMESSAGE type, int message_id,
	                         const wchar_t *detail = nullptr)
{
	PMSIHANDLE record = ::MsiCreateRecord(detail ? 2 : 1);
	if(!record)
		return;

	::MsiRecordSetInteger(record, 1, message_id);
	if(detail)
		::MsiRecordSetStringW(record, 2, detail);
	::MsiProcessMessage(hInstall, type, record);
}

static std::wstring JoinPath(const std::wstring &path1, const std::wstring &path2)
{
	std::wstring path = path1;
	if(path.size() > 0 && path.back() != L'\\')
	{
		if(path2.size() > 0 && path2.front() != L'\\')
			path += L'\\';
	}
	// path, not path1: the separator was appended to the local copy and then
	// thrown away.
	return path + path2;
}

static bool InstallFolder(MSIHANDLE hInstall, std::wstring& install_folder)
{
	MSI msi(hInstall);
	install_folder = msi.InstallFolder();
	return !install_folder.empty();
}
/*
	Only a package older than 1.9.20 can remove shell.nss during its removal half;
	CONFIG has been permanent since 1.9.20. The one-time bridge snapshots that
	legacy file before RemoveExistingProducts, passes a random path plus SHA-256
	through hidden CustomActionData, and restores only bytes read from the same
	validated non-reparse handle. A checked rollback action restores the new stock
	file, and a commit action removes the staging files.

	  https://learn.microsoft.com/windows/win32/msi/installing-permanent-components-files-fonts-registry-keys
	  https://learn.microsoft.com/windows/win32/msi/rollback-custom-actions
	  https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-createfilew
	  https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex
*/
static std::wstring ProgramDataFolder()
{
	auto initialized = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if(FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
		return {};

	PWSTR raw = nullptr;
	auto result = ::SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT,
		nullptr, &raw);
	std::wstring path = SUCCEEDED(result) && raw ? raw : L"";
	::CoTaskMemFree(raw);
	if(SUCCEEDED(initialized))
		::CoUninitialize();
	return path;
}

static bool EnsurePlainDirectory(const std::wstring &path)
{
	if(!::CreateDirectoryW(path.c_str(), nullptr)
	   && ::GetLastError() != ERROR_ALREADY_EXISTS)
		return false;

	auto attributes = ::GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES
		&& (attributes & FILE_ATTRIBUTE_DIRECTORY)
		&& !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static bool RandomToken(std::wstring &token)
{
	unsigned char random[16]{};
	if(::BCryptGenRandom(nullptr, random, sizeof(random),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
		return false;

	static constexpr wchar_t alphabet[] = L"0123456789abcdef";
	token.resize(sizeof(random) * 2);
	for(size_t i = 0; i < sizeof(random); ++i)
	{
		token[i * 2] = alphabet[random[i] >> 4];
		token[i * 2 + 1] = alphabet[random[i] & 0x0f];
	}
	return true;
}

struct LegacyConfigPlan
{
	bool restore{};
	std::wstring stage;
	Nilesoft::LegacyConfigTransfer::Digest digest{};
	std::wstring install_folder;
	std::wstring rollback_file;
	std::wstring restore_file;
	bool stock_new_existed{};
};

static std::wstring Serialize(const LegacyConfigPlan &plan)
{
	if(!plan.restore)
		return L"0||||||0";

	return L"1|" + plan.stage + L"|"
		+ Nilesoft::LegacyConfigTransfer::hex(plan.digest) + L"|"
		+ plan.install_folder + L"|" + plan.rollback_file + L"|"
		+ plan.restore_file + L"|" + (plan.stock_new_existed ? L"1" : L"0");
}

static bool ParseLegacyConfigPlan(const std::wstring &data, LegacyConfigPlan &plan)
{
	std::array<std::wstring, 7> fields;
	size_t start = 0;
	for(size_t i = 0; i < fields.size(); ++i)
	{
		auto separator = data.find(L'|', start);
		if(i + 1 == fields.size())
		{
			if(separator != std::wstring::npos)
				return false;
			fields[i] = data.substr(start);
		}
		else
		{
			if(separator == std::wstring::npos)
				return false;
			fields[i] = data.substr(start, separator - start);
			start = separator + 1;
		}
	}

	if(fields[0] == L"0")
	{
		plan = {};
		return true;
	}
	if(fields[0] != L"1" || fields[1].empty() || fields[3].empty()
	   || fields[4].empty() || fields[5].empty()
	   || !Nilesoft::LegacyConfigTransfer::parse_hex(fields[2], plan.digest))
		return false;

	plan.restore = true;
	plan.stage = std::move(fields[1]);
	plan.install_folder = std::move(fields[3]);
	plan.rollback_file = std::move(fields[4]);
	plan.restore_file = std::move(fields[5]);
	plan.stock_new_existed = fields[6] == L"1";
	return fields[6] == L"0" || fields[6] == L"1";
}

static bool SetLegacyConfigData(MSIHANDLE hInstall, const std::wstring &data)
{
	MSI msi(hInstall);
	return msi.set(L"RestoreLegacyConfigRollback", data.c_str())
		&& msi.set(L"RestoreLegacyConfig", data.c_str())
		&& msi.set(L"CleanupLegacyConfig", data.c_str());
}

UINT __stdcall BackupLegacyConfig(MSIHANDLE hInstall)
{
	std::wstring install_folder;
	if(!InstallFolder(hInstall, install_folder))
		return ERROR_INSTALL_FAILURE;

	// The outgoing file is where the previous product put it, which is not
	// INSTALLFOLDER when it was installed to a custom folder: nothing seeds
	// INSTALLFOLDER from the old install, so this upgrade resolves it to the
	// default and a read there finds nothing while the old product's removal
	// deletes the real file. PREVIOUSINSTALLFOLDER is the component search for it
	// (setup.wxs). The restore below still targets the new INSTALLFOLDER, which
	// the user may have changed in the folder dialog.
	MSI msi(hInstall);
	std::wstring previous_folder;
	if(!msi.get(L"PREVIOUSINSTALLFOLDER", previous_folder) || previous_folder.empty())
		previous_folder = install_folder;

	auto config = JoinPath(previous_folder, FILECONFIG);
	FileHandle source{ ::CreateFileW(config.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
		nullptr) };
	if(!source.valid())
	{
		auto error = ::GetLastError();
		if(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			return SetLegacyConfigData(hInstall, Serialize({}))
				? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
		return ERROR_INSTALL_FAILURE;
	}
	if(!Nilesoft::LegacyConfigTransfer::regular_non_reparse(source))
		return ERROR_INSTALL_FAILURE;

	std::wstring rollback_disabled;
	if(msi.get(L"RollbackDisabled", rollback_disabled))
		return ERROR_INSTALL_FAILURE;

	auto root = ProgramDataFolder();
	if(root.empty())
		return ERROR_INSTALL_FAILURE;
	auto vendor = JoinPath(root, L"Nilesoft");
	auto product = JoinPath(vendor, L"Shell");
	auto staging = JoinPath(product, L"Staging");
	if(!EnsurePlainDirectory(vendor) || !EnsurePlainDirectory(product)
	   || !EnsurePlainDirectory(staging))
		return ERROR_INSTALL_FAILURE;

	std::wstring token;
	std::wstring stage_path;
	HANDLE stage_raw = INVALID_HANDLE_VALUE;
	for(unsigned attempt = 0; attempt < 16 && stage_raw == INVALID_HANDLE_VALUE; ++attempt)
	{
		if(!RandomToken(token))
			return ERROR_INSTALL_FAILURE;
		stage_path = JoinPath(staging, L"shell.nss." + token + L".stage");
		stage_raw = ::CreateFileW(stage_path.c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN
				| FILE_FLAG_WRITE_THROUGH, nullptr);
		if(stage_raw == INVALID_HANDLE_VALUE && ::GetLastError() != ERROR_FILE_EXISTS)
			return ERROR_INSTALL_FAILURE;
	}
	FileHandle stage{ stage_raw };
	if(!stage.valid())
		return ERROR_INSTALL_FAILURE;

	LegacyConfigPlan plan;
	plan.restore = true;
	plan.stage = stage_path;
	plan.install_folder = install_folder;
	plan.rollback_file = JoinPath(install_folder, L"shell.nss.rollback." + token);
	plan.restore_file = JoinPath(install_folder, L"shell.nss.restore." + token);
	plan.stock_new_existed = ::GetFileAttributesW(
		JoinPath(install_folder, FILECONFIGSTOCK).c_str()) != INVALID_FILE_ATTRIBUTES;

	if(!Nilesoft::LegacyConfigTransfer::hash_and_copy(source, stage, plan.digest)
	   || !::FlushFileBuffers(stage))
	{
		stage.close();
		::DeleteFileW(stage_path.c_str());
		return ERROR_INSTALL_FAILURE;
	}
	stage.close();

	if(!SetLegacyConfigData(hInstall, Serialize(plan)))
	{
		::DeleteFileW(stage_path.c_str());
		return ERROR_INSTALL_FAILURE;
	}
	return ERROR_SUCCESS;
}

UINT __stdcall RestoreLegacyConfig(MSIHANDLE hInstall)
{
	MSI msi(hInstall);
	std::wstring data;
	LegacyConfigPlan plan;
	if(!msi.get(L"CustomActionData", data) || !ParseLegacyConfigPlan(data, plan))
		return ERROR_INSTALL_FAILURE;
	if(!plan.restore)
		return ERROR_SUCCESS;

	FileHandle stage{ ::CreateFileW(plan.stage.c_str(), GENERIC_READ, 0, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT
			| FILE_FLAG_SEQUENTIAL_SCAN, nullptr) };
	if(!stage.valid() || !Nilesoft::LegacyConfigTransfer::regular_non_reparse(stage))
		return ERROR_INSTALL_FAILURE;

	Nilesoft::LegacyConfigTransfer::Digest actual{};
	if(!Nilesoft::LegacyConfigTransfer::hash(stage, actual)
	   || !Nilesoft::LegacyConfigTransfer::equal(actual, plan.digest))
		return ERROR_INSTALL_FAILURE;

	// FILE_SHARE_DELETE because this handle is still open when ReplaceFileW below
	// replaces the same file, which it opens "with the GENERIC_READ, DELETE, and
	// SYNCHRONIZE access rights". Without it that open is a sharing violation
	// (error 32, measured), so every legacy upgrade that had a shell.nss to carry
	// over failed here with 1603 and rolled back.
	// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew
	auto config = JoinPath(plan.install_folder, FILECONFIG);
	FileHandle stock{ ::CreateFileW(config.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT
			| FILE_FLAG_SEQUENTIAL_SCAN, nullptr) };
	if(!stock.valid() || !Nilesoft::LegacyConfigTransfer::regular_non_reparse(stock))
		return ERROR_INSTALL_FAILURE;

	Nilesoft::LegacyConfigTransfer::Digest stock_digest{};
	if(!Nilesoft::LegacyConfigTransfer::hash(stock, stock_digest))
		return ERROR_INSTALL_FAILURE;

	FileHandle rollback{ ::CreateFileW(plan.rollback_file.c_str(), GENERIC_WRITE, 0,
		nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN
			| FILE_FLAG_WRITE_THROUGH, nullptr) };
	Nilesoft::LegacyConfigTransfer::Digest rollback_digest{};
	if(!rollback.valid()
	   || !Nilesoft::LegacyConfigTransfer::hash_and_copy(stock, rollback, rollback_digest)
	   || !::FlushFileBuffers(rollback))
		return ERROR_INSTALL_FAILURE;
	rollback.close();

	if(!plan.stock_new_existed
	   && !Nilesoft::LegacyConfigTransfer::equal(stock_digest, plan.digest))
	{
		auto stock_new = JoinPath(plan.install_folder, FILECONFIGSTOCK);
		auto stock_temp = plan.restore_file + L".stock";
		FileHandle output{ ::CreateFileW(stock_temp.c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN
				| FILE_FLAG_WRITE_THROUGH, nullptr) };
		Nilesoft::LegacyConfigTransfer::Digest copied{};
		if(!output.valid()
		   || !Nilesoft::LegacyConfigTransfer::hash_and_copy(stock, output, copied)
		   || !::FlushFileBuffers(output))
			return ERROR_INSTALL_FAILURE;
		output.close();
		if(!::MoveFileExW(stock_temp.c_str(), stock_new.c_str(), MOVEFILE_WRITE_THROUGH))
			return ERROR_INSTALL_FAILURE;
	}

	FileHandle replacement{ ::CreateFileW(plan.restore_file.c_str(), GENERIC_WRITE, 0,
		nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN
			| FILE_FLAG_WRITE_THROUGH, nullptr) };
	Nilesoft::LegacyConfigTransfer::Digest copied{};
	if(!replacement.valid()
	   || !Nilesoft::LegacyConfigTransfer::hash_and_copy(stage, replacement, copied)
	   || !Nilesoft::LegacyConfigTransfer::equal(copied, plan.digest)
	   || !::FlushFileBuffers(replacement))
		return ERROR_INSTALL_FAILURE;
	replacement.close();

	if(!::ReplaceFileW(config.c_str(), plan.restore_file.c_str(), nullptr, 0,
					   nullptr, nullptr))
		return ERROR_INSTALL_FAILURE;

	return ERROR_SUCCESS;
}

UINT __stdcall RestoreLegacyConfigRollback(MSIHANDLE hInstall)
{
	MSI msi(hInstall);
	std::wstring data;
	LegacyConfigPlan plan;
	if(!msi.get(L"CustomActionData", data) || !ParseLegacyConfigPlan(data, plan))
		return ERROR_INSTALL_FAILURE;
	if(!plan.restore)
		return ERROR_SUCCESS;

	bool restored = true;
	if(::GetFileAttributesW(plan.rollback_file.c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		FileHandle rollback{ ::CreateFileW(plan.rollback_file.c_str(), GENERIC_READ, 0,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
			nullptr) };
		restored = rollback.valid()
			&& Nilesoft::LegacyConfigTransfer::regular_non_reparse(rollback);
		rollback.close();

		auto config = JoinPath(plan.install_folder, FILECONFIG);
		if(restored && !::ReplaceFileW(config.c_str(), plan.rollback_file.c_str(),
				nullptr, 0, nullptr, nullptr))
		{
			if(::GetLastError() == ERROR_FILE_NOT_FOUND)
				restored = !!::MoveFileExW(plan.rollback_file.c_str(), config.c_str(),
					MOVEFILE_WRITE_THROUGH);
			else
				restored = false;
		}
	}

	if(!plan.stock_new_existed)
		::DeleteFileW(JoinPath(plan.install_folder, FILECONFIGSTOCK).c_str());
	::DeleteFileW(plan.stage.c_str());
	::DeleteFileW(plan.restore_file.c_str());
	::DeleteFileW((plan.restore_file + L".stock").c_str());
	::DeleteFileW(plan.rollback_file.c_str());
	return restored ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

UINT __stdcall CleanupLegacyConfig(MSIHANDLE hInstall)
{
	MSI msi(hInstall);
	std::wstring data;
	LegacyConfigPlan plan;
	if(!msi.get(L"CustomActionData", data) || !ParseLegacyConfigPlan(data, plan))
		return ERROR_SUCCESS;
	if(plan.restore)
	{
		::DeleteFileW(plan.stage.c_str());
		::DeleteFileW(plan.rollback_file.c_str());
		::DeleteFileW(plan.restore_file.c_str());
		::DeleteFileW((plan.restore_file + L".stock").c_str());
	}
	return ERROR_SUCCESS;
}

UINT __stdcall PrepareTreatAs(MSIHANDLE hInstall)
{
	MSI msi(hInstall);
	std::wstring remove;
	const bool uninstall = msi.get(L"REMOVE", remove)
		&& ::CompareStringOrdinal(remove.c_str(), -1, L"ALL", -1, TRUE) == CSTR_EQUAL;

	LSTATUS error = ERROR_SUCCESS;
	auto state = TreatAs::Query(&error);
	Nilesoft::TreatAsPlan::Op op = Nilesoft::TreatAsPlan::Op::noop;
	std::wstring marker;

	if(uninstall)
	{
		if(state == TreatAs::State::ours)
			op = Nilesoft::TreatAsPlan::Op::uninstall_ours;
		else if(state == TreatAs::State::inaccessible)
		{
			wchar_t detail[32]{};
			::swprintf_s(detail, L"%ld", error);
			InstallerMessage(hInstall,
				INSTALLMESSAGE(INSTALLMESSAGE_ERROR | MB_OK | MB_ICONERROR),
				TREATAS_REMOVE_ERROR, detail);
			return ERROR_INSTALL_FAILURE;
		}
	}
	else
	{
		if(state == TreatAs::State::absent)
		{
			if(!BuildCreatedMarkerPath(marker))
				return ERROR_INSTALL_FAILURE;
			op = Nilesoft::TreatAsPlan::Op::install_absent;
		}
		else if(state == TreatAs::State::foreign || state == TreatAs::State::inaccessible)
		{
			wchar_t detail[32]{};
			::swprintf_s(detail, L"%ld", error);
			InstallerMessage(hInstall,
				INSTALLMESSAGE(INSTALLMESSAGE_WARNING | MB_OK | MB_ICONWARNING),
				TREATAS_WARNING, detail);
		}
	}

	// Rollback and commit custom actions do not run when rollback is disabled.
	// Refuse the mutating plan rather than apply TreatAs without an undo.
	// https://learn.microsoft.com/windows/win32/msi/rollbackdisabled
	// https://learn.microsoft.com/windows/win32/msi/rollback-custom-actions
	if(op != Nilesoft::TreatAsPlan::Op::noop)
	{
		std::wstring rollback_disabled;
		if(msi.get(L"RollbackDisabled", rollback_disabled))
		{
			InstallerMessage(hInstall,
				INSTALLMESSAGE(INSTALLMESSAGE_ERROR | MB_OK | MB_ICONERROR),
				ROLLBACK_REQUIRED_ERROR);
			return ERROR_INSTALL_FAILURE;
		}
	}

	auto data = Nilesoft::TreatAsPlan::Serialize(op, marker);
	if(!msi.set(L"TreatAsRollback", data.c_str())
	   || !msi.set(L"TreatAsApply", data.c_str())
	   || !msi.set(L"TreatAsCommit", data.c_str()))
		return ERROR_INSTALL_FAILURE;

	return ERROR_SUCCESS;
}

// Custom actions that change the system must be deferred, with rollback
// scheduled immediately before them. TreatAsApply mutates HKLM; it only sees
// CustomActionData, ProductCode, and UserSID.
// https://learn.microsoft.com/windows/win32/msi/changing-the-system-state-using-a-custom-action
// https://learn.microsoft.com/windows/win32/msi/deferred-execution-custom-actions
// https://learn.microsoft.com/windows/win32/msi/obtaining-context-information-for-deferred-execution-custom-actions
UINT __stdcall TreatAsApply(MSIHANDLE hInstall)
{
	// Route any BorrowedKeyAccess diagnostic into this install's log.
	BorrowReportScope borrow_report_scope(hInstall);

	MSI msi(hInstall);
	std::wstring data;
	if(!msi.get(L"CustomActionData", data))
		return ERROR_SUCCESS;

	Nilesoft::TreatAsPlan::Plan plan;
	if(!Nilesoft::TreatAsPlan::Parse(data, plan))
		return ERROR_INSTALL_FAILURE;
	if(plan.op == Nilesoft::TreatAsPlan::Op::noop)
		return ERROR_SUCCESS;

	LSTATUS rc = ERROR_INVALID_DATA;
	if(plan.op == Nilesoft::TreatAsPlan::Op::install_absent)
	{
		bool created = false;
		rc = TreatAs::Create(BorrowReportToInstaller, &created);
		if(rc != ERROR_SUCCESS)
		{
			wchar_t detail[32]{};
			::swprintf_s(detail, L"%ld", rc);
			InstallerMessage(hInstall,
				INSTALLMESSAGE(INSTALLMESSAGE_WARNING | MB_OK | MB_ICONWARNING),
				TREATAS_WARNING, detail);
			// Primary-menu takeover is optional; ordinary MSI registration remains
			// valid and the classic context menu is still available.
			return ERROR_SUCCESS;
		}
		// Marker after create: writing it first would let rollback delete a
		// lookalike that appeared between plan and apply. If apply is
		// interrupted after creating TreatAs but before the marker, rollback
		// noops and our redirect remains — safer than deleting foreign state.
		// https://learn.microsoft.com/windows/win32/msi/rollback-custom-actions
		if(created && !WriteCreatedMarker(plan.marker))
		{
			TreatAs::Remove(BorrowReportToInstaller);
			InstallerMessage(hInstall,
				INSTALLMESSAGE(INSTALLMESSAGE_WARNING | MB_OK | MB_ICONWARNING),
				TREATAS_WARNING);
			return ERROR_SUCCESS;
		}
	}
	else if(plan.op == Nilesoft::TreatAsPlan::Op::uninstall_ours)
	{
		rc = TreatAs::Remove(BorrowReportToInstaller);
		if(rc != ERROR_SUCCESS)
		{
			wchar_t detail[32]{};
			::swprintf_s(detail, L"%ld", rc);
			InstallerMessage(hInstall,
				INSTALLMESSAGE(INSTALLMESSAGE_ERROR | MB_OK | MB_ICONERROR),
				TREATAS_REMOVE_ERROR, detail);
			return ERROR_INSTALL_FAILURE;
		}
	}
	else
		return ERROR_INSTALL_FAILURE;

	return ERROR_SUCCESS;
}

UINT __stdcall TreatAsRollback(MSIHANDLE hInstall)
{
	// Route any BorrowedKeyAccess diagnostic into this install's log.
	BorrowReportScope borrow_report_scope(hInstall);

	MSI msi(hInstall);
	std::wstring data;
	if(!msi.get(L"CustomActionData", data))
		return ERROR_SUCCESS;

	Nilesoft::TreatAsPlan::Plan plan;
	if(!Nilesoft::TreatAsPlan::Parse(data, plan))
		return ERROR_INSTALL_FAILURE;
	if(plan.op == Nilesoft::TreatAsPlan::Op::noop)
		return ERROR_SUCCESS;

	LSTATUS rc = ERROR_SUCCESS;
	if(plan.op == Nilesoft::TreatAsPlan::Op::install_absent)
	{
		auto marker = QueryCreatedMarker(plan.marker);
		if(marker == MarkerState::invalid)
			return ERROR_INSTALL_FAILURE;
		if(!Nilesoft::TreatAsPlan::RollbackRemovesTreatAs(plan,
			marker == MarkerState::present))
			return ERROR_SUCCESS;

		rc = TreatAs::Remove(BorrowReportToInstaller);
		if(rc == ERROR_SUCCESS)
			::DeleteFileW(plan.marker.c_str());
	}
	else if(plan.op == Nilesoft::TreatAsPlan::Op::uninstall_ours)
		rc = TreatAs::Create(BorrowReportToInstaller);
	else
		return ERROR_INSTALL_FAILURE;

	if(rc != ERROR_SUCCESS)
	{
		log("TreatAs rollback failed: %ld", rc);
		return ERROR_INSTALL_FAILURE;
	}
	return ERROR_SUCCESS;
}

// Commit custom actions run after a successful installation script, not during
// rollback, so the marker is still present if TreatAsRollback needs it.
// https://learn.microsoft.com/windows/win32/msi/commit-custom-actions
UINT __stdcall TreatAsCommit(MSIHANDLE hInstall)
{
	MSI msi(hInstall);
	std::wstring data;
	if(!msi.get(L"CustomActionData", data))
		return ERROR_SUCCESS;

	Nilesoft::TreatAsPlan::Plan plan;
	if(!Nilesoft::TreatAsPlan::Parse(data, plan))
		return ERROR_SUCCESS;
	if(plan.op == Nilesoft::TreatAsPlan::Op::install_absent
	   && Nilesoft::TreatAsPlan::MarkerPathLooksOwned(plan.marker))
		::DeleteFileW(plan.marker.c_str());
	return ERROR_SUCCESS;
}

UINT __stdcall NotifyShellChanged(MSIHANDLE)
{
	// SHCNE_ASSOCCHANGED requires SHCNF_IDLIST and two null items.
	// https://learn.microsoft.com/windows/win32/api/shlobj_core/nf-shlobj_core-shchangenotify
	::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
	return ERROR_SUCCESS;
}

extern "C" BOOL WINAPI DllMain(IN HINSTANCE hInst, IN ULONG ulReason, IN LPVOID)
{
	switch(ulReason)
	{
		case DLL_PROCESS_ATTACH:
			break;

		case DLL_PROCESS_DETACH:
			break;
	}

	return TRUE;
}


