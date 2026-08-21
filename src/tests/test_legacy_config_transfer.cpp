#include "test.h"

#include <windows.h>
#include <string>
#include <vector>

#include "LegacyConfigTransfer.h"

namespace
{
	class Handle
	{
	public:
		explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value(value) {}
		~Handle() { if(value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
		operator HANDLE() const { return value; }
		bool valid() const { return value != INVALID_HANDLE_VALUE; }
		void close() { if(valid()) ::CloseHandle(value); value = INVALID_HANDLE_VALUE; }
		HANDLE value;
	};

	class TempPath
	{
	public:
		TempPath()
		{
			wchar_t root[MAX_PATH]{};
			wchar_t file[MAX_PATH]{};
			if(::GetTempPathW(ARRAYSIZE(root), root)
			   && ::GetTempFileNameW(root, L"nss", 0, file))
				path = file;
		}
		~TempPath() { if(!path.empty()) ::DeleteFileW(path.c_str()); }
		std::wstring path;
	};

	bool write_all(const std::wstring &path, const void *data, DWORD size)
	{
		Handle file{ ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
			nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
		if(!file.valid())
			return false;
		DWORD written = 0;
		return !!::WriteFile(file, data, size, &written, nullptr) && written == size;
	}

	std::vector<unsigned char> read_all(const std::wstring &path)
	{
		Handle file{ ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
		if(!file.valid())
			return {};
		LARGE_INTEGER size{};
		if(!::GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024)
			return {};
		std::vector<unsigned char> result(static_cast<size_t>(size.QuadPart));
		DWORD read = 0;
		if(!result.empty() && (!::ReadFile(file, result.data(), static_cast<DWORD>(result.size()),
			&read, nullptr) || read != static_cast<DWORD>(result.size())))
			return {};
		return result;
	}
}

TEST(legacy_config_transfer, sha256_matches_the_known_abc_vector)
{
	TempPath path;
	CHECK(!path.path.empty());
	CHECK(write_all(path.path, "abc", 3));

	Handle file{ ::CreateFileW(path.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
	Nilesoft::LegacyConfigTransfer::Digest digest{};
	CHECK(file.valid());
	CHECK(Nilesoft::LegacyConfigTransfer::hash(file, digest));
	CHECK(Nilesoft::LegacyConfigTransfer::hex(digest)
		== L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(legacy_config_transfer, copy_and_hash_preserves_large_input_exactly)
{
	TempPath source_path;
	TempPath destination_path;
	std::vector<unsigned char> bytes(150000);
	for(size_t i = 0; i < bytes.size(); ++i)
		bytes[i] = static_cast<unsigned char>((i * 131u + 17u) & 0xffu);
	CHECK(write_all(source_path.path, bytes.data(), static_cast<DWORD>(bytes.size())));

	Handle source{ ::CreateFileW(source_path.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
	Handle destination{ ::CreateFileW(destination_path.path.c_str(), GENERIC_WRITE, 0,
		nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
	Nilesoft::LegacyConfigTransfer::Digest copied{};
	CHECK(source.valid());
	CHECK(destination.valid());
	CHECK(Nilesoft::LegacyConfigTransfer::hash_and_copy(source, destination, copied));
	destination.close();
	CHECK(read_all(destination_path.path) == bytes);

	Nilesoft::LegacyConfigTransfer::Digest direct{};
	CHECK(Nilesoft::LegacyConfigTransfer::hash(source, direct));
	CHECK(Nilesoft::LegacyConfigTransfer::equal(copied, direct));
}

TEST(legacy_config_transfer, same_size_content_change_changes_the_digest)
{
	TempPath path;
	CHECK(write_all(path.path, "AAAA", 4));
	Handle first{ ::CreateFileW(path.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
	Nilesoft::LegacyConfigTransfer::Digest before{};
	CHECK(Nilesoft::LegacyConfigTransfer::hash(first, before));
	first.close();

	CHECK(write_all(path.path, "BBBB", 4));
	Handle second{ ::CreateFileW(path.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
	Nilesoft::LegacyConfigTransfer::Digest after{};
	CHECK(Nilesoft::LegacyConfigTransfer::hash(second, after));
	CHECK(!Nilesoft::LegacyConfigTransfer::equal(before, after));
}

TEST(legacy_config_transfer, an_open_handle_is_not_rebound_by_path_substitution)
{
	TempPath original_path;
	TempPath moved_path;
	TempPath copied_path;
	const char original[] = "trusted snapshot";
	const char attacker[] = "attacker bytes";
	CHECK(write_all(original_path.path, original, sizeof(original) - 1));
	::DeleteFileW(moved_path.path.c_str());

	Handle original_handle{ ::CreateFileW(original_path.path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
	CHECK(original_handle.valid());
	CHECK(::MoveFileExW(original_path.path.c_str(), moved_path.path.c_str(), 0));
	CHECK(write_all(original_path.path, attacker, sizeof(attacker) - 1));

	Handle copied{ ::CreateFileW(copied_path.path.c_str(), GENERIC_WRITE, 0,
		nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
	Nilesoft::LegacyConfigTransfer::Digest digest{};
	CHECK(Nilesoft::LegacyConfigTransfer::hash_and_copy(original_handle, copied, digest));
	copied.close();

	auto result = read_all(copied_path.path);
	CHECK(result == std::vector<unsigned char>(original, original + sizeof(original) - 1));
}

TEST(legacy_config_transfer, exclusive_stage_open_blocks_write_and_delete_races)
{
	TempPath path;
	CHECK(write_all(path.path, "snapshot", 8));
	Handle stage{ ::CreateFileW(path.path.c_str(), GENERIC_READ, 0, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
	CHECK(stage.valid());
	CHECK(!::DeleteFileW(path.path.c_str()));
	CHECK(::GetLastError() == static_cast<DWORD>(ERROR_SHARING_VIOLATION));
	CHECK(!write_all(path.path, "changed!", 8));
}

TEST(legacy_config_transfer, directories_are_not_accepted_as_staged_files)
{
	TempPath path;
	CHECK(!path.path.empty());
	CHECK(::DeleteFileW(path.path.c_str()));
	CHECK(::CreateDirectoryW(path.path.c_str(), nullptr));
	Handle handle{ ::CreateFileW(path.path.c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
	CHECK(handle.valid());
	CHECK(!Nilesoft::LegacyConfigTransfer::regular_non_reparse(handle));
	handle.close();
	CHECK(::RemoveDirectoryW(path.path.c_str()));
	path.path.clear();
}

TEST(legacy_config_transfer, digest_text_round_trips_and_rejects_bad_input)
{
	Nilesoft::LegacyConfigTransfer::Digest digest{};
	for(size_t i = 0; i < digest.size(); ++i)
		digest[i] = static_cast<unsigned char>(i * 7);
	auto text = Nilesoft::LegacyConfigTransfer::hex(digest);
	Nilesoft::LegacyConfigTransfer::Digest parsed{};
	CHECK(Nilesoft::LegacyConfigTransfer::parse_hex(text, parsed));
	CHECK(Nilesoft::LegacyConfigTransfer::equal(digest, parsed));
	CHECK(!Nilesoft::LegacyConfigTransfer::parse_hex(L"xyz", parsed));
	text[3] = L'g';
	CHECK(!Nilesoft::LegacyConfigTransfer::parse_hex(text, parsed));
}
