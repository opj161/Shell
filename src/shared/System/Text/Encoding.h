#pragma once

namespace Nilesoft
{
	namespace Text
	{
		enum class EncodingType
		{
			Unknown = -1,
			ANSI,
			UTF8,
			UTF8BOM,
			UTF16LE,
			UTF16LEBOM,
			UTF16BE,
			UTF16BEBOM,
			UTF32LE,
			UTF32BE,
			UTF7,
			UTF1
		};

/*
BOM Byte order mark

EF BB BF xx - UTF-8
FF FE xx 00 - UTF-16LE
FE FF 00 xx - UTF-16BE
FF FE 00 00 - UTF-32LE
00 00 FE FF - UTF-32BE

UTF-7
2B 2F 76 38
2B 2F 76 39
2B 2F 76 2B
2B 2F 76 2F
2B 2F 76 38 2D

Note that:
	UTF-32BE doesn't start with three NULs so it won't be recognized
	UTF-32LE the first byte is not followed by 3 NULs so it won't be recognized
	UTF-16BE has only 1 NUL in the first 4 bytes so it won't be recognized
	UTF-16LE has only 1 NUL in the first 4 bytes so it won't be recognized
*/
		class Encoding
		{
		public:
			static EncodingType GetType(byte* b, size_t l)
			{
				if(!b || l <= 0)
					return EncodingType::Unknown;

				if(l >= 4)
				{
					if((b[0] == 0x00) && (b[1] == 0x00) && (b[2] == 0xFE) && (b[3] == 0xFF))
					{
						return EncodingType::UTF32BE;
					}
					else if((b[0] == 0xFF) && (b[1] == 0xFE) && (b[2] == 0x00) && (b[3] == 0x00))
					{
						return EncodingType::UTF32LE;
					}
					else if(b[0] && !b[1] && b[2] && !b[3])
					{
						return EncodingType::UTF16LE;
					}
					else if(!b[0] && b[1] && !b[2] && b[3])
					{
						return EncodingType::UTF16BE;
					}
				}

				if(l >= 3)
				{
					if((b[0] == 0xef) && (b[1] == 0xbb) && (b[2] == 0xbf))
					{
						return EncodingType::UTF8BOM;
					}
					else if((b[0] == 0xf7) && (b[1] == 0x64) && (b[2] == 0x4c))
					{
						return EncodingType::UTF1;
					}
				}

				if(l >= 2)
				{
					if((b[0] == 0xff) && (b[1] == 0xfe))
					{
						return EncodingType::UTF16LEBOM;
					}
					else if((b[0] == 0xfe) && (b[1] == 0xff))
					{
						return EncodingType::UTF16BEBOM;
					}
				}

				if(l >= 4)
				{
					if((b[0] == 0x2B) && (b[1] == 0x2F) && (b[2] == 0x76))
					{
						if(l >= 5)
						{
							if((b[3] == 0x38) && (b[4] == 0x2d))
								return EncodingType::UTF7;
						}

						if(b[3] == 0x38)
							return EncodingType::UTF7;
						else if(b[3] == 0x39)
							return EncodingType::UTF7;
						else if(b[3] == 0x2b)
							return EncodingType::UTF7;
						else if(b[3] == 0x2f)
							return EncodingType::UTF7;
					}
				}

				// Validate BOM-less UTF-8 strictly. The previous table-driven loop
				// checked only the first continuation byte, treated continuation
				// bytes (0x80-0xBF) as one-byte characters, and accepted truncated
				// sequences at EOF. Those cases silently sent ANSI files through
				// the UTF-8 decoder.
				auto continuation = [](byte c) { return (c & 0xC0) == 0x80; };
				for(size_t i = 0; i < l;)
				{
					const byte c = b[i];
					if(c <= 0x7F)
					{
						i++;
						continue;
					}

					if(c >= 0xC2 && c <= 0xDF)
					{
						if(i + 1 >= l || !continuation(b[i + 1]))
							return EncodingType::ANSI;
						i += 2;
						continue;
					}

					if(c >= 0xE0 && c <= 0xEF)
					{
						if(i + 2 >= l || !continuation(b[i + 1]) || !continuation(b[i + 2]))
							return EncodingType::ANSI;
						// Reject overlong forms and UTF-16 surrogate code points.
						if((c == 0xE0 && b[i + 1] < 0xA0) ||
						   (c == 0xED && b[i + 1] > 0x9F))
							return EncodingType::ANSI;
						i += 3;
						continue;
					}

					if(c >= 0xF0 && c <= 0xF4)
					{
						if(i + 3 >= l || !continuation(b[i + 1]) ||
						   !continuation(b[i + 2]) || !continuation(b[i + 3]))
							return EncodingType::ANSI;
						// Reject overlong forms and code points above U+10FFFF.
						if((c == 0xF0 && b[i + 1] < 0x90) ||
						   (c == 0xF4 && b[i + 1] > 0x8F))
							return EncodingType::ANSI;
						i += 4;
						continue;
					}

					// Includes lone continuation bytes, C0/C1 and F5-FF.
					return EncodingType::ANSI;
				}
				return EncodingType::UTF8;
			}
		};


		class Unicode
		{
		public:
			// Convert an ANSI string to a wide Unicode String
			static size_t From(const char *ansi, size_t ansi_length, wchar_t **unicode, uint32_t codePage)
			{
				int length = 0;
				if(ansi)
				{
					DWORD dwFlags = (codePage == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0;
					length = ::MultiByteToWideChar(codePage, dwFlags, ansi, static_cast<int>(ansi_length), nullptr, 0);
					if(length > 0 && unicode)
					{
						*unicode = new wchar_t[static_cast<size_t>(length) + 1] { };
						::MultiByteToWideChar(codePage, dwFlags, ansi, static_cast<int>(ansi_length), *unicode, length);
					}
				}
				return static_cast<size_t>(length);
			}

			static std::wstring From(const char *str, size_t str_length, uint32_t codePage)
			{
				if(str)
				{
					DWORD dwFlags = (codePage == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0;
					auto length = ::MultiByteToWideChar(codePage, dwFlags, str, static_cast<int>(str_length), nullptr, 0);
					if(length > 0)
					{
						std::wstring w(length, L'\0');
						length = ::MultiByteToWideChar(codePage, dwFlags, str, static_cast<int>(str_length), &w[0], length);
						w.resize(length);
						return std::move(w);
					}
				}
				return L"";
			}

			static std::wstring From(const std::string_view& str, uint32_t codePage = CP_ACP)
			{
				return std::move(From(str.data(), str.length(), codePage));
			}

			static std::wstring FromAnsi(const std::string_view &str)
			{
				return std::move(From(str.data(), str.length(), CP_ACP));
			}

			static size_t From(const char *mstr, wchar_t **wstr, uint32_t codePage)
			{
				return From(mstr, size_t(-1), wstr, codePage);
			}

			// Convert an ANSI string to a wide Unicode String
			static wchar_t *From(const char *str, uint32_t codePage = CP_ACP)
			{
				wchar_t *ret = nullptr;
				From(str, uint32_t(-1), &ret, codePage);
				return ret;
			}

			// Convert an UTF8 string to a wide Unicode String
			static size_t FromANSI(const char *mstr, size_t length, wchar_t **wstr)
			{
				return From(mstr, length, wstr, CP_ACP);
			}

			// Decode a byte run to UTF-16. Used by File::ReadText, i.e. by the
			// .nss file.read() function.
			//
			// This was `wchar_t *wstr = nullptr; From(mstr, length, &wstr, cp);
			// return wstr;`, which leaked the raw new wchar_t[] on EVERY call
			// and, whenever MultiByteToWideChar failed, constructed a
			// std::wstring from nullptr - undefined behaviour, and per AGENTS.md
			// an access violation there is invisible to catch(...) under /EHsc
			// because it happens inside the hook's SEH region. That failure is
			// reachable from ReadText: "if cbMultiByte is 0, the function fails"
			// (a file that is exactly a UTF-8 BOM, or a read of zero bytes), and
			// with MB_ERR_INVALID_CHARS - which the CP_UTF8 path sets - "The
			// function fails ... if an invalid character is encountered", which
			// a BOM followed by non-UTF-8 reaches because Encoding::GetType does
			// not validate what follows a BOM.
			// https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar
			//
			// Embedded NULs: returning through std::wstring(const wchar_t *)
			// stopped at the first NUL, so file.read() of a file containing one
			// has always truncated there. That is preserved deliberately.
			// Unicode::From(const char *, size_t, uint32_t) resize()s to the
			// converted length and would keep them, which is a different answer
			// for file.read() - a product decision, not part of this fix.
			static std::wstring FromMultiByte(const char *mstr, size_t length, uint32_t codePage)
			{
				std::wstring decoded = From(mstr, length, codePage);
				if(const auto nul = decoded.find(L'\0'); nul != std::wstring::npos)
					decoded.resize(nul);
				return decoded;
			}

			static std::wstring FromANSI(const char *mstr, size_t length)
			{
				return FromMultiByte(mstr, length, CP_ACP);
			}

			static wchar_t *FromANSI(const char *mstr)
			{
				return From(mstr, CP_ACP);
			}

			// Convert an UTF8 string to a wide Unicode String
			static size_t FromUTF8(const char *utf8, size_t length, wchar_t **wstr)
			{
				return From(utf8, length, wstr, CP_UTF8);
			}

			// See FromANSI(const char *, size_t) above for the contract, the
			// reachable failure paths and the embedded-NUL decision.
			static std::wstring FromUTF8(const char *utf8, size_t length)
			{
				return FromMultiByte(utf8, length, CP_UTF8);
			}

			// Convert an UTF8 string to a wide Unicode String
			static wchar_t *FromUTF8(const char *utf8)
			{
				return From(utf8, CP_UTF8);
			}

			static size_t ToUTF8(const wchar_t*wstr, size_t wstr_length, char **utf8)
			{
				int length = 0;
				if(wstr)
				{
					// dwFlags must be 0 here. MB_ERR_INVALID_CHARS belongs to
					// MultiByteToWideChar; WideCharToMultiByte rejects it for
					// CP_UTF8 with ERROR_INVALID_FLAGS and returns 0 every time.
					// https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-widechartomultibyte
					length = ::WideCharToMultiByte(CP_UTF8, 0, wstr, (int)wstr_length, nullptr, 0, nullptr, nullptr);
					if(length > 0 && utf8)
					{
						*utf8 = new char[static_cast<unsigned __int64>(length) + 1] { };
						::WideCharToMultiByte(CP_UTF8, 0, wstr, (int)wstr_length, *utf8, length, nullptr, nullptr);
					}
				}
				return static_cast<size_t>(length);
			}
		};

		class UTF8
		{
		public:

			// Convert a wide Unicode string to UTF-8.
			//
			// This was a hand-rolled loop over `short w = src[i]`. wchar_t is
			// unsigned 16-bit (range 0-65,535) and short is signed (-32,768-32,767)
			// - https://learn.microsoft.com/en-us/cpp/cpp/data-type-ranges - so
			// every code unit at or above U+8000 arrived negative, took the
			// `w <= 0x7f` branch and was written as one truncated byte. The
			// three- and four-byte branches under it were unreachable for a
			// short, a surrogate pair was never recombined, and a null src was
			// dereferenced. Hangul, CJK compatibility, private use and every
			// emoji went into the file as a run of 0x00.
			//
			// WideCharToMultiByte is the documented conversion and has "fully
			// conform[ed] with the Unicode 4.1 specification for UTF-8 and
			// UTF-16" since Vista. Contract notes that matter here:
			//   - dwFlags must be 0 or WC_ERR_INVALID_CHARS for CP_UTF8, else
			//     ERROR_INVALID_FLAGS. 0 is deliberate: it "replaces illegal
			//     sequences with U+FFFD ... and succeeds", so one unpaired
			//     surrogate in a selected path cannot abandon the whole write.
			//   - cbMultiByte == 0 returns "the required buffer size, in bytes".
			//   - cchWideChar == 0 fails, so the empty case is answered here
			//     rather than by the API.
			//   - An explicit positive cchWideChar leaves the output NOT
			//     null-terminated and excludes a terminator from the count, so
			//     an embedded U+0000 stays one 0x00 byte and the bytes after it
			//     are still written - which is what the old loop did.
			// https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-widechartomultibyte
			static void Utf16ToUtf8(std::string &dest, const wchar_t *src, const size_t length)
			{
				dest.clear();
				if(!src || length == 0)
					return;

				const int wlength = static_cast<int>(length);
				const int needed = ::WideCharToMultiByte(CP_UTF8, 0, src, wlength,
														 nullptr, 0, nullptr, nullptr);
				if(needed <= 0)
					return;

				dest.resize(static_cast<size_t>(needed));
				const int written = ::WideCharToMultiByte(CP_UTF8, 0, src, wlength,
														  &dest[0], needed, nullptr, nullptr);
				if(written <= 0)
				{
					dest.clear();
					return;
				}

				dest.resize(static_cast<size_t>(written));
			}

			// Convert a wide Unicode string to an UTF8 string
			static std::string Utf16ToUtf8(const wchar_t * const src, const size_t length)
			{
				std::string dest;
				Utf16ToUtf8(dest, src, length);
				return std::move(dest);
			}

			// Convert a wide Unicode string to an UTF8 string
			static size_t FromUnicode(const wchar_t *wstr, const size_t wstr_length, char **utf8)
			{
				const int wlength = static_cast<int>(wstr_length);
				int length = 0;
				if(wstr)
				{
					// dwFlags must be 0 here - see Unicode::ToUTF8 above. Passing
					// MB_ERR_INVALID_CHARS made this return 0 for every input,
					// which is what left sel.tofile() writing an empty file.
					// https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-widechartomultibyte
					length = ::WideCharToMultiByte(CP_UTF8, 0, wstr, wlength, nullptr, 0, nullptr, nullptr);

					if(length > 0 && utf8)
					{
						*utf8 = new char[static_cast<size_t>(length) + 1]{ };
						::WideCharToMultiByte(CP_UTF8, 0, wstr, wlength, *utf8, length, nullptr, nullptr);
					}
				}
				return static_cast<size_t>(length);
			}

			static size_t FromUnicode(const wchar_t*wstr, char **utf8)
			{
				return FromUnicode(wstr, size_t(-1), utf8);
			}
		};
	}
}
