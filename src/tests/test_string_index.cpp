// Indexing contract for Text::string.
//
// The class has three ways to read a character — at(), const operator[] and
// non-const operator[] — and they used to disagree. at() has always returned
// L'\0' for an out-of-range index; the non-const subscript was an unchecked
// m_data[index], which happened to read the terminator at index == m_length
// because terminate() always writes one, and ran off the buffer past that.
//
// The trap when "bounds-checking" this is to clamp to the last *character*
// (m_length - 1). That silently turns the ubiquitous
//
//     for(size_t i = 0; s[i]; ++i)
//
// into an endless loop, because the loop never sees a zero. These tests pin the
// property that actually matters: every read at or beyond m_length is L'\0', by
// all three routes, so scanning terminates and nothing reads out of bounds.

#include "test.h"

// string.h is not standalone (it reaches for the module handle); the umbrella
// header is how every other consumer picks it up.
#include <System.h>

using Nilesoft::Text::string;

TEST(string_index, in_range_characters_read_back)
{
	string s(L"abc");

	CHECK_EQ((int)s[(size_t)0], (int)L'a');
	CHECK_EQ((int)s[(size_t)1], (int)L'b');
	CHECK_EQ((int)s[(size_t)2], (int)L'c');
}

TEST(string_index, reading_at_length_gives_the_terminator)
{
	string s(L"abc");

	CHECK_MSG(s[(size_t)3] == L'\0', "index == length must read the terminator");

	const string &cs = s;
	CHECK_MSG(cs[(size_t)3] == L'\0', "const subscript must agree");
	CHECK_MSG(cs.at(3) == L'\0', "at() must agree");
}

TEST(string_index, reading_past_the_end_gives_the_terminator_not_the_last_character)
{
	string s(L"abc");

	// The regression this exists to prevent: clamping to m_length - 1 would
	// return L'c' here and make every scan loop run forever.
	CHECK_MSG(s[(size_t)4] != L'c', "out-of-range read must not clamp to the last character");
	CHECK_MSG(s[(size_t)4] == L'\0', "out-of-range read must be the terminator");
	CHECK_MSG(s[(size_t)9999] == L'\0', "far out-of-range read must be the terminator");

	const string &cs = s;
	CHECK_MSG(cs[(size_t)4] == L'\0', "const subscript must agree");
	CHECK_MSG(cs[(size_t)9999] == L'\0', "const subscript must agree far out of range");
}

TEST(string_index, scanning_to_the_terminator_stops)
{
	string s(L"abcd");

	size_t n = 0;
	for(; s[n] && n < 64; ++n) { }

	CHECK_EQ(n, (size_t)4);
}

TEST(string_index, an_empty_string_reads_as_the_terminator)
{
	string empty;

	CHECK_MSG(empty[(size_t)0] == L'\0', "index 0 of an empty string is the terminator");
	CHECK_MSG(empty[(size_t)5] == L'\0', "any index of an empty string is the terminator");

	const string &ce = empty;
	CHECK_MSG(ce[(size_t)0] == L'\0', "const subscript must agree");

	size_t n = 0;
	for(; empty[n] && n < 64; ++n) { }
	CHECK_EQ(n, (size_t)0);
}

TEST(string_index, a_cleared_string_reads_as_the_terminator)
{
	string s(L"abc");
	s.clear();

	CHECK_MSG(s[(size_t)0] == L'\0', "a cleared string has no characters to hand out");
	CHECK_MSG(s[(size_t)2] == L'\0', "not even the ones it used to hold");
}

TEST(string_index, writing_through_the_subscript_still_works)
{
	string s(L"abc");

	s[(size_t)0] = L'X';

	CHECK_EQ((int)s[(size_t)0], (int)L'X');
	CHECK_MSG(s.equals(L"Xbc"), "an in-range write must be visible through the whole string");
}

// ---------------------------------------------------------------------------
// Copy assignment used to alias.
//
//     clear(); m_capacity = other.m_capacity; m_length = other.m_length;
//     m_data = other.m_data;
//
// Two owners, one buffer, and a double free when both destructors ran.
// ---------------------------------------------------------------------------

TEST(string_index, copy_assignment_is_a_deep_copy)
{
	string source(L"hello");
	string target;

	target = source;

	CHECK_MSG(target.equals(L"hello"), "the copy must carry the same text");
	CHECK_MSG(target.c_str() != source.c_str(), "the copy must own a separate buffer");

	// Mutating one must not disturb the other, and both must destruct cleanly
	// at the end of this scope.
	target[(size_t)0] = L'J';
	CHECK_MSG(source.equals(L"hello"), "mutating the copy must not touch the source");
	CHECK_MSG(target.equals(L"Jello"), "the copy must be independently mutable");
}

TEST(string_index, self_assignment_leaves_the_string_intact)
{
	string s(L"stable");
	const string &alias = s;

	s = alias;

	CHECK_MSG(s.equals(L"stable"), "self-assignment must not clear or free the buffer");
}
