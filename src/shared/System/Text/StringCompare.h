#pragma once

// Ordinal case-insensitive comparison for UTF-16 buffers.
//
// This replaces ::_memicmp, which was previously used across string.h and the
// lexer to compare wchar_t buffers. _memicmp is the *narrow* CRT routine: it
// case-folds every byte of the buffer, including the high byte of each UTF-16
// code unit, causing false positives in CJK and locale sensitivity.
//
// Hybrid design:
//   * ASCII text and identifiers are handled by the SIMD fast path (SSE2 / NEON).
//   * Non-ASCII mismatches containing code points > 0x7F automatically fall back
//     to Windows non-linguistic ordinal ignore-case comparison
//     (::CompareStringOrdinal(..., TRUE)), ensuring correct matching for
//     international filenames, Cyrillic, Greek, and extended Unicode pairs.

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_M_IX86) || defined(_M_X64)
#include <emmintrin.h>
#define NSS_ORDINAL_SSE2 1
#elif defined(_M_ARM64)
#include <arm64_neon.h>
#define NSS_ORDINAL_NEON 1
#endif

namespace Nilesoft
{
	namespace Text
	{
		namespace Ordinal
		{
			// Vector paths process 8 code units at a time. Below this many units
			// the setup cost is not repaid, and the scalar loop's early exit wins.
			inline constexpr size_t simd_threshold = 16;

			inline constexpr wchar_t fold(wchar_t c) noexcept
			{
				return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 0x20) : c;
			}

			inline bool equals_scalar(const wchar_t *a, const wchar_t *b, size_t count) noexcept
			{
				for(size_t i = 0; i < count; i++)
				{
					if(fold(a[i]) != fold(b[i]))
						return false;
				}
				return true;
			}

			// Ordinal, case-insensitive, UTF-16 aware. Compares exactly `count`
			// code units; neither buffer needs to be null terminated.
			inline bool equals_fold(const wchar_t *a, const wchar_t *b, size_t count) noexcept
			{
				if(count == 0)
					return true;

				if(a == b)
					return true;

				if(!a || !b)
					return false;

				// Most comparisons in this codebase fail, and most of those fail on
				// the first unit. Rejecting here keeps the vector path off the
				// common case and costs one compare when it does match.
				if(fold(a[0]) != fold(b[0]))
					return false;

				if(count < simd_threshold)
					return equals_scalar(a + 1, b + 1, count - 1);

#if defined(NSS_ORDINAL_SSE2)
				{
					const __m128i lo = _mm_set1_epi16(L'A' - 1);
					const __m128i hi = _mm_set1_epi16(L'Z' + 1);
					const __m128i bit = _mm_set1_epi16(0x20);

					size_t i = 0;
					for(; i + 8 <= count; i += 8)
					{
						__m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i *>(a + i));
						__m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + i));

						// Signed compares are safe here: units >= 0x8000 read as
						// negative, fail the (> 'A'-1) test, and are left unfolded,
						// which is what ASCII-only folding requires.
						__m128i ma = _mm_and_si128(_mm_cmpgt_epi16(va, lo), _mm_cmplt_epi16(va, hi));
						__m128i mb = _mm_and_si128(_mm_cmpgt_epi16(vb, lo), _mm_cmplt_epi16(vb, hi));

						va = _mm_or_si128(va, _mm_and_si128(ma, bit));
						vb = _mm_or_si128(vb, _mm_and_si128(mb, bit));

						if(_mm_movemask_epi8(_mm_cmpeq_epi16(va, vb)) != 0xFFFF)
							return false;
					}
					return equals_scalar(a + i, b + i, count - i);
				}
#elif defined(NSS_ORDINAL_NEON)
				{
					const uint16x8_t lo = vdupq_n_u16(L'A');
					const uint16x8_t hi = vdupq_n_u16(L'Z');
					const uint16x8_t bit = vdupq_n_u16(0x20);

					size_t i = 0;
					for(; i + 8 <= count; i += 8)
					{
						uint16x8_t va = vld1q_u16(reinterpret_cast<const uint16_t *>(a + i));
						uint16x8_t vb = vld1q_u16(reinterpret_cast<const uint16_t *>(b + i));

						uint16x8_t ma = vandq_u16(vcgeq_u16(va, lo), vcleq_u16(va, hi));
						uint16x8_t mb = vandq_u16(vcgeq_u16(vb, lo), vcleq_u16(vb, hi));

						va = vorrq_u16(va, vandq_u16(ma, bit));
						vb = vorrq_u16(vb, vandq_u16(mb, bit));

						if(vminvq_u16(vceqq_u16(va, vb)) != 0xFFFF)
							return false;
					}
					return equals_scalar(a + i, b + i, count - i);
				}
#else
				return equals_scalar(a + 1, b + 1, count - 1);
#endif
			}

			// Exact (case-sensitive) equality over `count` code units.
			inline bool equals_exact(const wchar_t *a, const wchar_t *b, size_t count) noexcept
			{
				if(count == 0)
					return true;
				if(a == b)
					return true;
				if(!a || !b)
					return false;
				return 0 == ::memcmp(a, b, count * sizeof(wchar_t));
			}

			inline bool has_non_ascii(const wchar_t *s, size_t count) noexcept
			{
				for(size_t i = 0; i < count; i++)
				{
					if(static_cast<uint16_t>(s[i]) > 0x7F)
						return true;
				}
				return false;
			}

			inline bool equals(const wchar_t *a, const wchar_t *b, size_t count, bool ignoreCase) noexcept
			{
				if(count == 0 || a == b)
					return true;
				if(!a || !b)
					return false;
				if(!ignoreCase)
					return equals_exact(a, b, count);

				if(equals_fold(a, b, count))
					return true;

				// If the fast ASCII SIMD fold didn't match, check if non-ASCII characters
				// are present. If so, fall back to Win32 CompareStringOrdinal for Unicode casing.
				if(has_non_ascii(a, count) || has_non_ascii(b, count))
				{
					return ::CompareStringOrdinal(a, static_cast<int>(count),
												  b, static_cast<int>(count),
												  TRUE) == CSTR_EQUAL;
				}
				return false;
			}

			// Narrow equivalent. Folds ASCII only, so unlike ::_memicmp it does not
			// change behaviour with the active locale.
			inline bool equals_narrow(const char *a, const char *b, size_t count, bool ignoreCase) noexcept
			{
				if(count == 0 || a == b)
					return true;
				if(!a || !b)
					return false;
				if(!ignoreCase)
					return 0 == ::memcmp(a, b, count);

				for(size_t i = 0; i < count; i++)
				{
					char ca = a[i], cb = b[i];
					if(ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 0x20);
					if(cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 0x20);
					if(ca != cb)
						return false;
				}
				return true;
			}

			// Dispatches on character width so the wide path never reaches the
			// byte-oriented comparison that caused the UTF-16 defect.
			template<typename T>
			inline bool equals_t(const T *a, const T *b, size_t count, bool ignoreCase) noexcept
			{
				if constexpr(sizeof(T) == sizeof(wchar_t))
				{
					return equals(reinterpret_cast<const wchar_t *>(a),
								  reinterpret_cast<const wchar_t *>(b), count, ignoreCase);
				}
				else
				{
					static_assert(sizeof(T) == 1, "unsupported character width");
					return equals_narrow(reinterpret_cast<const char *>(a),
										 reinterpret_cast<const char *>(b), count, ignoreCase);
				}
			}
		}
	}
}
