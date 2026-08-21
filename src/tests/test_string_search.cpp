#include "test.h"

#include "System.h"

using Nilesoft::Text::string;

TEST(string_search, find_last_returns_the_final_overlapping_match)
{
	constexpr auto value = L"ababa";
	auto found = string::FindLast(value, 5, L"aba", 3, false);
	CHECK(found == value + 2);
}

TEST(string_search, find_last_checks_index_zero)
{
	constexpr auto value = L"needle";
	auto found = string::FindLast(value, 6, L"needle", 6, false);
	CHECK(found == value);
}

TEST(string_search, find_last_reports_no_match_without_underflow)
{
	constexpr auto value = L"haystack";
	CHECK(string::FindLast(value, 8, L"needle", 6, false) == nullptr);
	CHECK(string::FindLast(value, 8, L"too-long-pattern", 16, false) == nullptr);
	CHECK(string::FindLast(value, 0, L"x", 1, false) == nullptr);
}

TEST(string_search, find_last_preserves_case_policy)
{
	constexpr auto value = L"One one ONE";
	CHECK(string::FindLast(value, 11, L"one", 3, false) == value + 4);
	CHECK(string::FindLast(value, 11, L"one", 3, true) == value + 8);
}
