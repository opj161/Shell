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