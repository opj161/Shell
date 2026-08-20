// Characterisation tests for Encoding::GetType.
//
// Config files are read by Lexer::load_File, which picks a decoder purely from
// what this function returns. Guessing wrong does not fail loudly; it silently
// decodes the file through the wrong codepage, so every non-ASCII title in a
// user's shell.nss comes out as mojibake.

#include "test.h"

#include <windows.h>
#include <cstdint>
using byte = uint8_t;

#include "../shared/System/Text/Encoding.h"

using namespace Nilesoft::Text;

static EncodingType detect(const byte *data, size_t len)
{
	return Encoding::GetType(const_cast<byte *>(data), len);
}

TEST(encoding, bom_forms_are_recognised)
{
	const byte utf8bom[]  = { 0xEF, 0xBB, 0xBF, 'a', 'b' };
	const byte utf16le[]  = { 0xFF, 0xFE, 'a', 0x00 };
	const byte utf16be[]  = { 0xFE, 0xFF, 0x00, 'a' };
	const byte utf32le[]  = { 0xFF, 0xFE, 0x00, 0x00, 'a', 0x00, 0x00, 0x00 };
	const byte utf32be[]  = { 0x00, 0x00, 0xFE, 0xFF, 0x00, 0x00, 0x00, 'a' };

	CHECK(detect(utf8bom, sizeof(utf8bom)) == EncodingType::UTF8BOM);
	CHECK(detect(utf16le, sizeof(utf16le)) == EncodingType::UTF16LEBOM);
	CHECK(detect(utf16be, sizeof(utf16be)) == EncodingType::UTF16BEBOM);
	CHECK(detect(utf32le, sizeof(utf32le)) == EncodingType::UTF32LE);
	CHECK(detect(utf32be, sizeof(utf32be)) == EncodingType::UTF32BE);
}

// The important case: modern editors, git and Notepad all default to UTF-8
// without a BOM, so this is what a user's config most often looks like.
TEST(encoding, bomless_utf8_is_detected)
{
	// item(title='A_<U+4E2D>')
	const byte cjk[] = { 'i','t','e','m','(', '\'', 'A', 0xE4, 0xB8, 0xAD, '\'', ')', '\r', '\n' };
	CHECK_MSG(detect(cjk, sizeof(cjk)) == EncodingType::UTF8, "BOM-less UTF-8 with CJK");

	// Cyrillic "Papka" in UTF-8
	const byte cyr[] = { 0xD0,0x9F, 0xD0,0xB0, 0xD0,0xBF, 0xD0,0xBA, 0xD0,0xB0, '\r','\n' };
	CHECK_MSG(detect(cyr, sizeof(cyr)) == EncodingType::UTF8, "BOM-less UTF-8, 2-byte sequences");

	// 4-byte sequence (emoji U+1F600)
	const byte emoji[] = { 'x', 0xF0, 0x9F, 0x98, 0x80, 'y', '\r', '\n' };
	CHECK_MSG(detect(emoji, sizeof(emoji)) == EncodingType::UTF8, "BOM-less UTF-8, 4-byte sequence");

	const byte ascii[] = { 'i','t','e','m','\r','\n' };
	CHECK_MSG(detect(ascii, sizeof(ascii)) == EncodingType::UTF8, "pure ASCII");
}

TEST(encoding, legacy_codepage_bytes_are_detected_as_ansi)
{
	// Windows-1252 "cafe" with 0xE9, with text following. 0xE9 is a 3-byte
	// lead in UTF-8, and the byte after it is not a continuation byte, so the
	// detector correctly rejects UTF-8 here.
	const byte cp1252[] = { 'c','a','f', 0xE9, ' ', 'x', '\r', '\n' };
	CHECK(detect(cp1252, sizeof(cp1252)) == EncodingType::ANSI);
}

TEST(encoding, malformed_utf8_is_rejected)
{
	const byte trailing[] = { 'c','a','f', 0xE9 };
	CHECK_MSG(detect(trailing, sizeof(trailing)) == EncodingType::ANSI,
			  "truncated multibyte lead must be rejected");

	const byte truncated[] = { 'x', 0xE4, 0xB8, 'A', 'y', '\r', '\n' };
	CHECK_MSG(detect(truncated, sizeof(truncated)) == EncodingType::ANSI,
			  "malformed 3-byte sequence must be rejected");

	const byte lone[] = { 'a', 0x93, 'b', '\r', '\n' };
	CHECK_MSG(detect(lone, sizeof(lone)) == EncodingType::ANSI,
			  "lone continuation byte must be rejected");

	const byte overlong[] = { 0xE0, 0x80, 0x80 };
	CHECK_MSG(detect(overlong, sizeof(overlong)) == EncodingType::ANSI,
			  "overlong UTF-8 must be rejected");

	const byte surrogate[] = { 0xED, 0xA0, 0x80 };
	CHECK_MSG(detect(surrogate, sizeof(surrogate)) == EncodingType::ANSI,
			  "UTF-8 surrogate encoding must be rejected");

	const byte too_high[] = { 0xF4, 0x90, 0x80, 0x80 };
	CHECK_MSG(detect(too_high, sizeof(too_high)) == EncodingType::ANSI,
			  "code point above U+10FFFF must be rejected");
}

TEST(encoding, degenerate_input)
{
	const byte one[] = { 'a' };
	CHECK(detect(nullptr, 0) == EncodingType::Unknown);
	CHECK(detect(one, 0) == EncodingType::Unknown);
	CHECK(detect(one, 1) == EncodingType::UTF8);
}

// -----------------------------------------------------------------------------
// UTF-8 conversion.
//
// These two converters spent their whole life returning 0. WideCharToMultiByte
// was being handed MB_ERR_INVALID_CHARS (0x0008), which is a MultiByteToWideChar
// flag; for CP_UTF8 the only legal flag is WC_ERR_INVALID_CHARS (0x0080), and
// anything else fails the call with ERROR_INVALID_FLAGS.
//
//   https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-widechartomultibyte
//
// What it cost in practice: sel.tofile() writes the selected paths to a temp
// file through UTF8::FromUnicode, so the file came out empty.

static std::string as_bytes(const char *p, size_t n)
{
	return std::string(p, n);
}

TEST(encoding, wide_to_utf8_encodes_rather_than_failing)
{
	const wchar_t *text = L"Hello \x00e9\x4e2d";   // ASCII + 2-byte + 3-byte

	char *out = nullptr;
	auto len = UTF8::FromUnicode(text, ::wcslen(text), &out);

	CHECK_MSG(len == 11, "6 ASCII + 2 for U+00E9 + 3 for U+4E2D, and no terminator");
	CHECK(out != nullptr);
	if(out)
	{
		CHECK(as_bytes(out, len) == "Hello \xC3\xA9\xE4\xB8\xAD");
		delete[] out;
	}
}

TEST(encoding, unicode_to_utf8_encodes_rather_than_failing)
{
	const wchar_t *text = L"\x4e2d\x6587";

	char *out = nullptr;
	auto len = Unicode::ToUTF8(text, (uint32_t)::wcslen(text), &out);

	CHECK_EQ((int)len, 6);
	CHECK(out != nullptr);
	if(out)
	{
		CHECK(as_bytes(out, len) == "\xE4\xB8\xAD\xE6\x96\x87");
		delete[] out;
	}
}

// A path that survived a round trip through the shell can still contain an
// unpaired surrogate. Substituting is the right answer here - failing the whole
// write because one name is malformed would lose every other selected path.
TEST(encoding, an_unpaired_surrogate_is_substituted_not_fatal)
{
	const wchar_t bad[] = { L'a', 0xD800, L'b', 0 };

	char *out = nullptr;
	auto len = UTF8::FromUnicode(bad, 3, &out);

	CHECK_MSG(len > 0, "a malformed unit must not abandon the whole conversion");
	if(out)
	{
		CHECK_MSG(as_bytes(out, len) == "a\xEF\xBF\xBD" "b", "U+FFFD in place of the lone surrogate");
		delete[] out;
	}
}

TEST(encoding, utf8_conversion_of_nothing_is_nothing)
{
	char *out = nullptr;
	CHECK_EQ((int)UTF8::FromUnicode(L"", 0, &out), 0);
	CHECK(out == nullptr);
	CHECK_EQ((int)UTF8::FromUnicode(nullptr, 4, &out), 0);
}
