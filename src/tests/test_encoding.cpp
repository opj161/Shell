// Characterisation tests for Encoding::GetType.
//
// Config files are read by Lexer::load_File, which picks a decoder purely from
// what this function returns. Guessing wrong does not fail loudly; it silently
// decodes the file through the wrong codepage, so every non-ASCII title in a
// user's shell.nss comes out as mojibake.

#include "test.h"

#include <windows.h>
#include <cstdint>
#include <initializer_list>
#include <string>
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

// ---------------------------------------------------------------------------
// UTF8::Utf16ToUtf8 - the sel.tofile() / sel.append() write path.
//
// The hand-rolled loop this replaced declared `short w = src[i]` for an
// unsigned 16-bit wchar_t. wchar_t's range is 0-65,535 and short's is
// -32,768-32,767 (https://learn.microsoft.com/en-us/cpp/cpp/data-type-ranges),
// so every code unit at or above U+8000 arrived negative, took the `w <= 0x7f`
// branch and was written as ONE TRUNCATED BYTE. The three- and four-byte
// branches below it were unreachable for a short, and a surrogate pair was
// never recombined - so Hangul, CJK compatibility, private use and every emoji
// went into the file as a run of 0x00.
//
// Expected byte sequences are built from explicit octets rather than string
// literals: a hex escape in C++ is greedy, so "\xEA" followed by a literal 'a'
// would be read as one escape.
// ---------------------------------------------------------------------------

static std::string bytes(std::initializer_list<unsigned char> b)
{
	return std::string(reinterpret_cast<const char *>(b.begin()), b.size());
}

// U+8000 is the first code point the old loop got wrong, so this is the exact
// boundary between the branch that worked and the branch that did not.
TEST(encoding, utf16_to_utf8_encodes_the_first_code_point_above_the_short_boundary)
{
	const wchar_t src[] = { 0x8000, 0 };
	const std::string got = UTF8::Utf16ToUtf8(src, 1);

	CHECK_MSG(got == bytes({ 0xE8, 0x80, 0x80 }), "U+8000 is three UTF-8 bytes, not one truncated one");
	CHECK_EQ((int)got.size(), 3);
}

TEST(encoding, utf16_to_utf8_encodes_hangul_and_cjk)
{
	const wchar_t hangul[] = { 0xAC00, 0 };            // U+AC00 HANGUL SYLLABLE GA
	CHECK_MSG(UTF8::Utf16ToUtf8(hangul, 1) == bytes({ 0xEA, 0xB0, 0x80 }), "U+AC00");

	const wchar_t cjk[] = { 0x4E2D, 0 };               // U+4E2D, below 0x8000
	CHECK_MSG(UTF8::Utf16ToUtf8(cjk, 1) == bytes({ 0xE4, 0xB8, 0xAD }), "U+4E2D was already correct");
}

// A surrogate pair is two code units that must be recombined into one 4-byte
// sequence. The old loop saw two negative shorts and wrote two zero bytes.
TEST(encoding, utf16_to_utf8_recombines_a_surrogate_pair)
{
	const wchar_t emoji[] = { 0xD83D, 0xDE00, 0 };     // U+1F600
	const std::string got = UTF8::Utf16ToUtf8(emoji, 2);

	CHECK_MSG(got == bytes({ 0xF0, 0x9F, 0x98, 0x80 }), "U+1F600 is one four-byte sequence");
	CHECK_EQ((int)got.size(), 4);
}

TEST(encoding, utf16_to_utf8_keeps_ascii_around_a_high_code_point)
{
	const wchar_t mixed[] = { L'A', 0x8000, L'B', 0 };
	CHECK(UTF8::Utf16ToUtf8(mixed, 3) == bytes({ 'A', 0xE8, 0x80, 0x80, 'B' }));
}

// Same rule the FromUnicode path already follows: a lone surrogate must not
// abandon the whole write. dwFlags is 0, so WideCharToMultiByte "replaces
// illegal sequences with U+FFFD ... and succeeds".
TEST(encoding, utf16_to_utf8_substitutes_an_unpaired_surrogate)
{
	const wchar_t bad[] = { L'a', 0xD800, L'b', 0 };
	CHECK(UTF8::Utf16ToUtf8(bad, 3) == bytes({ 'a', 0xEF, 0xBF, 0xBD, 'b' }));
}

// Embedded-NUL semantics on the WRITE path, pinned deliberately: the length is
// explicit, so a U+0000 in the middle of the argument is one 0x00 byte in the
// file and the bytes after it are still written. This is what the old loop did
// (w == 0 took the `w <= 0x7f` branch and pushed a 0 byte) and it is preserved.
TEST(encoding, utf16_to_utf8_preserves_an_embedded_nul)
{
	const wchar_t src[] = { L'a', 0, L'b' };
	const std::string got = UTF8::Utf16ToUtf8(src, 3);

	CHECK_EQ((int)got.size(), 3);
	CHECK_MSG(got == bytes({ 'a', 0x00, 'b' }), "an embedded NUL does not truncate the write");
}

// WideCharToMultiByte fails outright when cchWideChar is 0, so the zero case
// has to be answered before the call rather than by it.
TEST(encoding, utf16_to_utf8_of_nothing_is_nothing)
{
	CHECK(UTF8::Utf16ToUtf8(L"", 0).empty());
	CHECK(UTF8::Utf16ToUtf8(nullptr, 0).empty());
	CHECK(UTF8::Utf16ToUtf8(nullptr, 4).empty());

	std::string dest = "stale";
	UTF8::Utf16ToUtf8(dest, L"", 0);
	CHECK_MSG(dest.empty(), "the out-parameter form still clears its destination");
}

// ---------------------------------------------------------------------------
// Unicode::FromANSI / FromUTF8, the std::wstring overloads.
//
// These are the two File::ReadText uses. Both were written as
//     wchar_t *wstr = nullptr; From(mstr, length, &wstr, cp); return wstr;
// which leaks the raw new wchar_t[] on every call, and on the failure path
// returns nullptr into a std::wstring constructor - undefined behaviour, and
// per AGENTS.md an access violation there is invisible to catch(...) under
// /EHsc because it happens inside the hook's SEH region.
//
// MultiByteToWideChar reaches that failure path on inputs File::ReadText can
// actually hand it: "if cbMultiByte is 0, the function fails", and with
// MB_ERR_INVALID_CHARS - which the CP_UTF8 branch sets - "The function fails
// ... if an invalid character is encountered in the source string".
// ---------------------------------------------------------------------------

// A file that is exactly the UTF-8 BOM: GetType says UTF8BOM, ReadText strips
// three bytes and asks for a conversion of zero.
TEST(encoding, a_bom_only_file_decodes_to_nothing_rather_than_faulting)
{
	const char nothing[] = { 0 };
	CHECK(Unicode::FromUTF8(nothing, 0).empty());
}

// A BOM followed by bytes that are not UTF-8. GetType does not validate what
// follows the BOM, so this reaches the decoder and trips MB_ERR_INVALID_CHARS.
TEST(encoding, malformed_utf8_decodes_to_nothing_rather_than_faulting)
{
	const std::string bad = bytes({ 0xFF, 0xFE, 0xFF });
	CHECK(Unicode::FromUTF8(bad.data(), bad.size()).empty());

	const std::string lone_continuation = bytes({ 'a', 0x80, 'b' });
	CHECK(Unicode::FromUTF8(lone_continuation.data(), lone_continuation.size()).empty());
}

TEST(encoding, an_empty_ansi_read_decodes_to_nothing_rather_than_faulting)
{
	const char nothing[] = { 0 };
	CHECK(Unicode::FromANSI(nothing, 0).empty());
	CHECK(Unicode::FromANSI(nullptr, 4).empty());
	CHECK(Unicode::FromUTF8(nullptr, 4).empty());
}

TEST(encoding, a_valid_utf8_read_still_decodes)
{
	const std::string utf8 = bytes({ 'A', 0xE4, 0xB8, 0xAD });
	const std::wstring got = Unicode::FromUTF8(utf8.data(), utf8.size());

	CHECK_EQ((int)got.size(), 2);
	CHECK(got == std::wstring(L"A\x4e2d"));

	const char ansi[] = { 'h', 'i' };
	CHECK(Unicode::FromANSI(ansi, 2) == std::wstring(L"hi"));
}

// Embedded-NUL semantics on the READ path, pinned deliberately and NOT the same
// answer as the write path. Returning through `std::wstring(const wchar_t *)`
// stopped at the first NUL, so file.read() of a file containing one has always
// truncated there. That behaviour is preserved rather than silently widened:
// Unicode::From(const char*, size_t, uint32_t) resize()s to the converted
// length and would keep the NULs, which is a different answer for
// file.read() and is a product decision, not a bug fix.
TEST(encoding, an_embedded_nul_still_truncates_a_decoded_read)
{
	const char embedded[] = { 'a', 0, 'b' };

	const std::wstring utf8 = Unicode::FromUTF8(embedded, 3);
	CHECK_EQ((int)utf8.size(), 1);
	CHECK(utf8 == std::wstring(L"a"));

	const std::wstring ansi = Unicode::FromANSI(embedded, 3);
	CHECK_EQ((int)ansi.size(), 1);
	CHECK(ansi == std::wstring(L"a"));
}
