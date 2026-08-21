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

constexpr wchar_t TREATAS_PARENT[] = L"SOFTWARE\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}";
constexpr wchar_t TREATAS_KEY[] = L"SOFTWARE\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\TreatAs";
constexpr wchar_t CONTEXT_MENU_CLSID[] = L"{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}";
constexpr auto TREATAS_WARNING = 25001;
constexpr auto TREATAS_REMOVE_ERROR = 25002;
constexpr auto ROLLBACK_REQUIRED_ERROR = 25003;

class RegKey
{
public:
	RegKey() = default;
	RegKey(const RegKey &) = delete;
	RegKey &operator=(const RegKey &) = delete;
	~RegKey() { if(_key) ::RegCloseKey(_key); }

	operator HKEY() const { return _key; }
	HKEY *put() { return &_key; }

private:
	HKEY _key{};
};

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

enum class TreatAsState
{
	absent,
	ours,
	foreign,
	inaccessible
};

static TreatAsState QueryTreatAs(LSTATUS *error = nullptr)
{
	RegKey key;
	auto rc = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, TREATAS_KEY, 0,
		KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, key.put());
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
		if(error) *error = (rc == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : rc);
		return rc == ERROR_FILE_NOT_FOUND ? TreatAsState::foreign : TreatAsState::inaccessible;
	}

	if(error) *error = ERROR_SUCCESS;
	if(subkeys == 0 && values == 1
	   && ::CompareStringOrdinal(value, -1, CONTEXT_MENU_CLSID, -1, TRUE) == CSTR_EQUAL)
		return TreatAsState::ours;

	return TreatAsState::foreign;
}

// created is set only when this invocation both created the key
// (REG_CREATED_NEW_KEY) and wrote our CLSID. Opening an already-ours key is
// success with created=false so rollback must not delete it.
//
//   https://learn.microsoft.com/windows/win32/api/winreg/nf-winreg-regcreatekeyexw
static LSTATUS CreateTreatAsIfAbsent(bool *created = nullptr)
{
	if(created)
		*created = false;

	RegKey parent;
	auto rc = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, TREATAS_PARENT, 0,
		KEY_CREATE_SUB_KEY | KEY_WOW64_64KEY, parent.put());
	if(rc != ERROR_SUCCESS)
		return rc;

	RegKey key;
	DWORD disposition = 0;
	rc = ::RegCreateKeyExW(parent, L"TreatAs", 0, nullptr, REG_OPTION_NON_VOLATILE,
		KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY, nullptr, key.put(), &disposition);
	if(rc != ERROR_SUCCESS)
		return rc;

	if(disposition != REG_CREATED_NEW_KEY)
		return QueryTreatAs() == TreatAsState::ours ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS;

	rc = ::RegSetValueExW(key, nullptr, 0, REG_SZ,
		reinterpret_cast<const BYTE *>(CONTEXT_MENU_CLSID), sizeof(CONTEXT_MENU_CLSID));
	if(rc != ERROR_SUCCESS)
	{
		// We created this empty key in this call, so taking it back is not a
		// recursive/shared-key deletion.
		::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, TREATAS_KEY, KEY_WOW64_64KEY, 0);
		return rc;
	}
	if(created)
		*created = true;
	return ERROR_SUCCESS;
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

	return ::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, TREATAS_KEY, KEY_WOW64_64KEY, 0);
}

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

	auto config = JoinPath(install_folder, FILECONFIG);
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

	MSI msi(hInstall);
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

	auto config = JoinPath(plan.install_folder, FILECONFIG);
	FileHandle stock{ ::CreateFileW(config.c_str(), GENERIC_READ, FILE_SHARE_READ,
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
	auto state = QueryTreatAs(&error);
	Nilesoft::TreatAsPlan::Op op = Nilesoft::TreatAsPlan::Op::noop;
	std::wstring marker;

	if(uninstall)
	{
		if(state == TreatAsState::ours)
			op = Nilesoft::TreatAsPlan::Op::uninstall_ours;
		else if(state == TreatAsState::inaccessible)
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
		if(state == TreatAsState::absent)
		{
			if(!BuildCreatedMarkerPath(marker))
				return ERROR_INSTALL_FAILURE;
			op = Nilesoft::TreatAsPlan::Op::install_absent;
		}
		else if(state == TreatAsState::foreign || state == TreatAsState::inaccessible)
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
		rc = CreateTreatAsIfAbsent(&created);
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
			RemoveTreatAsIfOurs();
			InstallerMessage(hInstall,
				INSTALLMESSAGE(INSTALLMESSAGE_WARNING | MB_OK | MB_ICONWARNING),
				TREATAS_WARNING);
			return ERROR_SUCCESS;
		}
	}
	else if(plan.op == Nilesoft::TreatAsPlan::Op::uninstall_ours)
	{
		rc = RemoveTreatAsIfOurs();
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

		rc = RemoveTreatAsIfOurs();
		if(rc == ERROR_SUCCESS)
			::DeleteFileW(plan.marker.c_str());
	}
	else if(plan.op == Nilesoft::TreatAsPlan::Op::uninstall_ours)
		rc = CreateTreatAsIfAbsent();
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

UINT __stdcall ValidatePath(MSIHANDLE hInstall)
{
	HRESULT hr = S_OK;

	BOOL fInstallPathIsRemote = TRUE;
	BOOL fInstallPathIsRemoveable = TRUE;

	MSI msi(hInstall);
	std::wstring pwszWixUIDir, pwszInstallPath;
	if(!msi.get(L"WIXUI_INSTALLDIR", pwszWixUIDir))
	{
		log("failed to get WixUI Installation Directory");
		return ERROR_INSTALL_FAILURE;
	}

	if(!msi.get(pwszWixUIDir.c_str(), pwszInstallPath))
	{
		log("failed to get Installation Directory");
		return ERROR_INSTALL_FAILURE;
	}

	std::wstring pStrippedTargetFolder = pwszInstallPath;

	// Terminate the path at the root
	if(!::PathStripToRootW(pStrippedTargetFolder.data()))
	{
		hr = HRESULT_FROM_WIN32(ERROR_INVALID_DRIVE);
		if(FAILED(hr))
		{
			log("failed to parse target folder");
			return hr;
		}
	}

	auto drive_type = ::GetDriveTypeW(pStrippedTargetFolder.c_str());

	fInstallPathIsRemote = (DRIVE_REMOTE == drive_type);
	fInstallPathIsRemoveable = ((DRIVE_CDROM == drive_type) || (DRIVE_REMOVABLE == drive_type) || (DRIVE_RAMDISK == drive_type) || (DRIVE_UNKNOWN == drive_type));

	// If the path does not point to a network drive, mapped drive, or removable drive,
	// then set WIXUI_INSTALLDIR_VALID to "1" otherwise set it to 0
	BOOL fInstallPathIsUnc = ::PathIsUNCW(pwszInstallPath.c_str());
	if(!fInstallPathIsUnc && !fInstallPathIsRemote && !fInstallPathIsRemoveable)
	{
		// path is valid
		if(!msi.set(L"WIXUI_INSTALLDIR_VALID", L"1"))
		{
			log("failed to set WIXUI_INSTALLDIR_VALID");
			return ERROR_INSTALL_FAILURE;
		}
	}
	else
	{
		// path is invalid; we can't log it because we're being called from a DoAction control event
		// but we can at least call WcaLog to get it to write to the debugger from a debug build
		log("Installation path %ls is invalid: it is %s UNC path, %s remote path, or %s path on a removable drive, and must be none of these.",
			pwszInstallPath.c_str(), fInstallPathIsUnc ? "a" : "not a", fInstallPathIsRemote ? "a" : "not a", fInstallPathIsRemoveable ? "a" : "not a");

		if(!msi.set(L"WIXUI_INSTALLDIR_VALID", L"0"))
		{
			log("failed to set WIXUI_INSTALLDIR_VALID");
			return ERROR_INSTALL_FAILURE;
		}
	}
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


