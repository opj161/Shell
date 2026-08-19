// Characterisation tests for Encoding::GetType.
//
// Config files are read by Lexer::load_File, which picks a decoder purely from
// what this function returns. Guessing wrong does not fail loudly; it silently
// decodes the file through the wrong codepage, so every non-ASCII title in a
// user's shell.nss comes out as mojibake. These tests pin down what the
// detector actually does today, including where it is wrong.

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

	CHECK(detect(utf8bom, sizeof(utf8bom)) == EncodingType::UTF8BOM);
	CHECK(detect(utf16le, sizeof(utf16le)) == EncodingType::UTF16LEBOM);
	CHECK(detect(utf16be, sizeof(utf16be)) == EncodingType::UTF16BEBOM);
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

// Known gaps. These document real misdetections rather than asserting the
// behaviour is correct: each one decodes a file through the wrong codepage.
TEST(encoding, known_gaps_in_the_bomless_heuristic)
{
	// A high byte as the very last byte of the buffer. The continuation check
	// is guarded by (i < l), so when the lead byte is last there is nothing to
	// validate against and the file is claimed as UTF-8.
	const byte trailing[] = { 'c','a','f', 0xE9 };
	auto got = detect(trailing, sizeof(trailing));
	CHECK_MSG(got == EncodingType::UTF8,
			  "documents a gap: a trailing high byte is claimed as UTF-8, not ANSI");

	// Only the first continuation byte of a multi-byte sequence is checked, and
	// the remaining bytes are then consumed as if they were ASCII, so a
	// truncated 3-byte sequence still passes.
	const byte truncated[] = { 'x', 0xE4, 0xB8, 'A', 'y', '\r', '\n' };
	got = detect(truncated, sizeof(truncated));
	CHECK_MSG(got == EncodingType::UTF8,
			  "documents a gap: a malformed 3-byte sequence is claimed as UTF-8");

	// A lone continuation byte is not a valid lead, but the lookup table maps
	// 0x80-0xBF to 1 rather than 0, so it is treated as a single character.
	const byte lone[] = { 'a', 0x93, 'b', '\r', '\n' };
	got = detect(lone, sizeof(lone));
	CHECK_MSG(got == EncodingType::UTF8,
			  "documents a gap: a lone continuation byte is claimed as UTF-8");
}

TEST(encoding, degenerate_input)
{
	const byte one[] = { 'a' };
	CHECK(detect(nullptr, 0) == EncodingType::Unknown);
	CHECK(detect(one, 0) == EncodingType::Unknown);
	CHECK(detect(one, 1) == EncodingType::UTF8);
}