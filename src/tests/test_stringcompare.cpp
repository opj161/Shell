// Regression tests for Ordinal::equals_fold, the replacement for the
// _memicmp-on-UTF-16 comparison that string.h and Lexer.h used to call.
//
// The CJK and Cyrillic cases below are the ones the old code got wrong; they
// are the reason this file exists.

#include "test.h"
#include "../shared/System/Text/StringCompare.h"

#include <cwchar>
#include <string>

using namespace Nilesoft::Text;

static bool eq(const wchar_t *a, const wchar_t *b)
{
	auto la = std::wcslen(a), lb = std::wcslen(b);
	if(la != lb) return false;
	return Ordinal::equals_fold(a, b, la);
}

TEST(ordinal_fold, ascii_case_pairs_match)
{
	CHECK(eq(L"Copy", L"copy"));
	CHECK(eq(L"COPY", L"copy"));
	CHECK(eq(L"CoPy", L"cOpY"));
	CHECK(eq(L"", L""));
	CHECK(eq(L"a", L"A"));
}

TEST(ordinal_fold, ascii_distinct_do_not_match)
{
	CHECK(!eq(L"Copy", L"Paste"));
	CHECK(!eq(L"copy", L"copz"));
	CHECK(!eq(L"copy", L"copy "));
	CHECK(!eq(L"a", L"b"));
}

TEST(ordinal_fold, ascii_adjacent_bytes_are_not_folded_together)
{
	// '@' is 0x40, one below 'A'; '[' is 0x5B, one above 'Z'. Neither may fold.
	CHECK(!eq(L"@", L"`"));
	CHECK(!eq(L"[", L"{"));
	CHECK(eq(L"@[", L"@["));
}

TEST(ordinal_fold, cjk_high_byte_collisions_rejected)
{
	// The _memicmp implementation folded the high byte of each code unit, so
	// every one of these pairs compared equal. They are distinct characters.
	CHECK_MSG(!eq(L"䄀", L"愀"), "U+4100 must not equal U+6100");
	CHECK_MSG(!eq(L"䅁", L"慡"), "U+4141 must not equal U+6161");
	CHECK_MSG(!eq(L"中", L"渭"), "U+4E2D must not equal U+6E2D");
	CHECK_MSG(!eq(L"婚", L"穚"), "U+5A5A must not equal U+7A5A");

	// Mixed: an ASCII-safe prefix must not mask the collision that follows.
	CHECK(!eq(L"menu中", L"menu渭"));
	CHECK(!eq(L"ア䄀", L"ア愀"));
}

TEST(ordinal_fold, non_ascii_is_compared_exactly)
{
	// ASCII-only folding is the documented contract: real non-ASCII case pairs
	// compare unequal, but they compare *consistently*, which _memicmp did not.
	CHECK(!eq(L"А", L"а"));          // Cyrillic A vs a
	CHECK(eq(L"А", L"А"));           // and identical input still matches
	CHECK(eq(L"éè", L"éè"));
	CHECK(!eq(L"é", L"É"));          // e-acute lower vs upper
}

TEST(ordinal_fold, surrogate_pairs_compare_exactly)
{
	CHECK(eq(L"\U0001F600", L"\U0001F600"));
	CHECK(!eq(L"\U0001F600", L"\U0001F601"));
	// A surrogate half must never be folded into another.
	CHECK(!eq(L"😀", L"😁"));
}

// The scalar path runs below Ordinal::simd_threshold and the vector path above
// it, with a scalar tail. These lengths straddle both boundaries and the
// 8-unit vector stride so neither path escapes coverage.
TEST(ordinal_fold, simd_and_scalar_paths_agree)
{
	for(size_t len = 1; len <= 200; len++)
	{
		std::wstring a(len, L'a');
		for(size_t i = 0; i < len; i += 3) a[i] = L'Q';

		std::wstring b = a;
		CHECK_MSG(Ordinal::equals_fold(a.c_str(), b.c_str(), len), "identical buffers must match");

		// Flip case everywhere: still equal under ASCII folding.
		std::wstring c = a;
		for(auto &ch : c) ch = (ch >= L'A' && ch <= L'Z') ? wchar_t(ch + 32) : wchar_t(ch - 32);
		CHECK_MSG(Ordinal::equals_fold(a.c_str(), c.c_str(), len), "case-flipped buffers must match");

		// A single differing unit must be caught at every position, including
		// the last, which is where a bad tail loop would let it through.
		for(size_t pos = 0; pos < len; pos++)
		{
			std::wstring d = a;
			d[pos] = L'#';
			CHECK_MSG(!Ordinal::equals_fold(a.c_str(), d.c_str(), len),
					  "difference must be detected at every position");
		}

		// And a CJK collision at every position, which is the vector path's
		// version of the _memicmp bug.
		for(size_t pos = 0; pos < len; pos += 7)
		{
			std::wstring e = a, f = a;
			e[pos] = L'中';
			f[pos] = L'渭';
			CHECK_MSG(!Ordinal::equals_fold(e.c_str(), f.c_str(), len),
					  "CJK collision must be rejected on the vector path too");
		}
	}
}

TEST(ordinal_fold, count_is_respected_without_null_termination)
{
	const wchar_t a[] = { L'a', L'b', L'c', L'X' };
	const wchar_t b[] = { L'A', L'B', L'C', L'Y' };
	CHECK(Ordinal::equals_fold(a, b, 3));
	CHECK(!Ordinal::equals_fold(a, b, 4));
	CHECK(Ordinal::equals_fold(a, b, 0));
}

TEST(ordinal_fold, null_and_alias_handling)
{
	const wchar_t *s = L"abc";
	CHECK(Ordinal::equals_fold(s, s, 3));
	CHECK(Ordinal::equals_fold(nullptr, nullptr, 0));
	CHECK(!Ordinal::equals_fold(nullptr, s, 3));
	CHECK(!Ordinal::equals_fold(s, nullptr, 3));
}

TEST(ordinal_exact, case_sensitive_path)
{
	CHECK(Ordinal::equals_exact(L"Copy", L"Copy", 4));
	CHECK(!Ordinal::equals_exact(L"Copy", L"copy", 4));
	CHECK(Ordinal::equals(L"Copy", L"copy", 4, true));
	CHECK(!Ordinal::equals(L"Copy", L"copy", 4, false));
}