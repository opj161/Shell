#pragma once

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <string>

namespace Nilesoft::LegacyConfigTransfer
{
	using Digest = std::array<unsigned char, 32>;

	class Sha256
	{
	public:
		Sha256()
		{
			if(::BCryptOpenAlgorithmProvider(&_algorithm, BCRYPT_SHA256_ALGORITHM,
											 nullptr, 0) >= 0)
			{
				if(::BCryptCreateHash(_algorithm, &_hash, nullptr, 0,
								   nullptr, 0, 0) < 0)
					_hash = nullptr;
			}
		}

		Sha256(const Sha256 &) = delete;
		Sha256 &operator=(const Sha256 &) = delete;

		~Sha256()
		{
			if(_hash) ::BCryptDestroyHash(_hash);
			if(_algorithm) ::BCryptCloseAlgorithmProvider(_algorithm, 0);
		}

		bool valid() const { return _algorithm && _hash; }

		bool update(const void *data, DWORD size)
		{
			return valid() && ::BCryptHashData(_hash,
				reinterpret_cast<PUCHAR>(const_cast<void *>(data)), size, 0) >= 0;
		}

		bool finish(Digest &digest)
		{
			return valid() && ::BCryptFinishHash(_hash, digest.data(),
				static_cast<ULONG>(digest.size()), 0) >= 0;
		}

	private:
		BCRYPT_ALG_HANDLE _algorithm{};
		BCRYPT_HASH_HANDLE _hash{};
	};

	inline bool rewind(HANDLE file)
	{
		LARGE_INTEGER zero{};
		return !!::SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
	}

	inline bool regular_non_reparse(HANDLE file)
	{
		if(file == nullptr || file == INVALID_HANDLE_VALUE || ::GetFileType(file) != FILE_TYPE_DISK)
			return false;

		FILE_ATTRIBUTE_TAG_INFO info{};
		return !!::GetFileInformationByHandleEx(file, FileAttributeTagInfo,
			&info, sizeof(info))
			&& !(info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
	}

	inline bool hash_and_copy(HANDLE source, HANDLE destination, Digest &digest)
	{
		if(!regular_non_reparse(source) || !rewind(source))
			return false;

		Sha256 hash;
		if(!hash.valid())
			return false;

		unsigned char buffer[64 * 1024];
		for(;;)
		{
			DWORD read = 0;
			if(!::ReadFile(source, buffer, sizeof(buffer), &read, nullptr))
				return false;
			if(read == 0)
				break;
			if(!hash.update(buffer, read))
				return false;

			if(destination && destination != INVALID_HANDLE_VALUE)
			{
				DWORD offset = 0;
				while(offset < read)
				{
					DWORD written = 0;
					if(!::WriteFile(destination, buffer + offset, read - offset,
										&written, nullptr) || written == 0)
						return false;
					offset += written;
				}
			}
		}
		return hash.finish(digest);
	}

	inline bool hash(HANDLE source, Digest &digest)
	{
		return hash_and_copy(source, INVALID_HANDLE_VALUE, digest);
	}

	inline bool equal(const Digest &left, const Digest &right)
	{
		unsigned char difference = 0;
		for(size_t i = 0; i < left.size(); ++i)
			difference |= left[i] ^ right[i];
		return difference == 0;
	}

	inline std::wstring hex(const Digest &digest)
	{
		static constexpr wchar_t alphabet[] = L"0123456789abcdef";
		std::wstring result(digest.size() * 2, L'0');
		for(size_t i = 0; i < digest.size(); ++i)
		{
			result[i * 2] = alphabet[digest[i] >> 4];
			result[i * 2 + 1] = alphabet[digest[i] & 0x0f];
		}
		return result;
	}

	inline bool parse_hex(const std::wstring &text, Digest &digest)
	{
		if(text.size() != digest.size() * 2)
			return false;

		auto nibble = [](wchar_t ch) -> int
		{
			if(ch >= L'0' && ch <= L'9') return ch - L'0';
			if(ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
			if(ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
			return -1;
		};

		for(size_t i = 0; i < digest.size(); ++i)
		{
			auto high = nibble(text[i * 2]);
			auto low = nibble(text[i * 2 + 1]);
			if(high < 0 || low < 0)
				return false;
			digest[i] = static_cast<unsigned char>((high << 4) | low);
		}
		return true;
	}
}
