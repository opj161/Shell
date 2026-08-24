// Extensions the user told Shell to stop asking.
//
// src/shared/ProviderQuarantine.h holds the format and the reasoning. What is
// pinned here is the part that can be wrong without looking wrong:
//
//   - the GUID parse, because a quarantine entry that silently fails to parse
//     reads exactly like an extension that was never quarantined;
//   - the hash, because it has to be the same number ProviderHealth computes or
//     the file and the perf report name the same extension differently and
//     nothing connects them;
//   - the round trip, because a list that cannot be re-read after being written
//     un-quarantines everything the next time a menu is built.

#include "test.h"

#include "..\shared\ProviderQuarantine.h"

#include <string>
#include <vector>

using namespace Nilesoft::Shell;

namespace
{
	bool same_guid(const GUID &a, const GUID &b)
	{
		return 0 == ::memcmp(&a, &b, sizeof(GUID));
	}

	// {A0B1C2D3-E4F5-6789-ABCD-EF0123456789}
	const GUID Sample = { 0xA0B1C2D3, 0xE4F5, 0x6789,
						  { 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89 } };
}

TEST(provider_quarantine, a_braced_clsid_parses)
{
	GUID got{};
	CHECK(Quarantine::parse_guid(L"{A0B1C2D3-E4F5-6789-ABCD-EF0123456789}", got));
	CHECK(same_guid(got, Sample));
}

TEST(provider_quarantine, the_braces_are_optional)
{
	GUID got{};
	CHECK(Quarantine::parse_guid(L"A0B1C2D3-E4F5-6789-ABCD-EF0123456789", got));
	CHECK(same_guid(got, Sample));
}

TEST(provider_quarantine, case_and_surrounding_space_do_not_matter)
{
	GUID got{};
	CHECK(Quarantine::parse_guid(L"   {a0b1c2d3-e4f5-6789-abcd-ef0123456789}  ", got));
	CHECK(same_guid(got, Sample));
}

/*
	Every one of these would otherwise land as "not quarantined" with no
	complaint anywhere, which is the failure this whole file is about.
*/
TEST(provider_quarantine, malformed_identifiers_are_refused)
{
	GUID got{};
	CHECK(!Quarantine::parse_guid(L"", got));
	CHECK(!Quarantine::parse_guid(L"{}", got));
	CHECK(!Quarantine::parse_guid(L"not a guid at all", got));
	// one digit short
	CHECK(!Quarantine::parse_guid(L"{A0B1C2D3-E4F5-6789-ABCD-EF012345678}", got));
	// one digit long
	CHECK(!Quarantine::parse_guid(L"{A0B1C2D3-E4F5-6789-ABCD-EF01234567890}", got));
	// a dash in the wrong place
	CHECK(!Quarantine::parse_guid(L"{A0B1C2D3-E4F5-6789-ABCDE-F0123456789}", got));
	// a non-hex digit
	CHECK(!Quarantine::parse_guid(L"{A0B1C2D3-E4F5-6789-ABCD-EF012345678G}", got));
	// braces on one end only
	CHECK(!Quarantine::parse_guid(L"{A0B1C2D3-E4F5-6789-ABCD-EF0123456789", got));
}

TEST(provider_quarantine, formatting_round_trips_through_parsing)
{
	auto text = Quarantine::format_guid(Sample);
	GUID got{};
	CHECK(Quarantine::parse_guid(text, got));
	CHECK(same_guid(got, Sample));
}

/*
	The hash has to match ProviderHealth::provider_hash byte for byte: the perf
	report prints that number and the quarantine file holds the CLSID, so if
	they ever disagree a user reads a hash in one command and cannot act on it
	with the other. Both are FNV-1a over the sixteen raw bytes; this recomputes
	it independently rather than calling the same function.
*/
TEST(provider_quarantine, the_hash_is_fnv1a_over_the_raw_guid_bytes)
{
	auto bytes = reinterpret_cast<const unsigned char *>(&Sample);
	uint32_t expected = 2166136261u;
	for(size_t i = 0; i < sizeof(GUID); i++)
	{
		expected ^= bytes[i];
		expected *= 16777619u;
	}
	CHECK_EQ(Quarantine::hash_clsid(Sample), expected);
}

TEST(provider_quarantine, two_spellings_of_one_clsid_hash_the_same)
{
	GUID upper{}, lower{};
	CHECK(Quarantine::parse_guid(L"{A0B1C2D3-E4F5-6789-ABCD-EF0123456789}", upper));
	CHECK(Quarantine::parse_guid(L"a0b1c2d3-e4f5-6789-abcd-ef0123456789", lower));
	CHECK_EQ(Quarantine::hash_clsid(upper), Quarantine::hash_clsid(lower));
}

TEST(provider_quarantine, an_empty_list_parses_to_nothing)
{
	CHECK_EQ(Quarantine::parse(L"").size(), size_t(0));
	CHECK_EQ(Quarantine::parse(L"\r\n\r\n   \r\n").size(), size_t(0));
}

TEST(provider_quarantine, comments_and_blank_lines_are_ignored)
{
	auto entries = Quarantine::parse(
		L"# a comment\r\n"
		L"\r\n"
		L"   # an indented comment\r\n"
		L"{A0B1C2D3-E4F5-6789-ABCD-EF0123456789}\r\n");
	CHECK_EQ(entries.size(), size_t(1));
	if(entries.empty())
		return;
	CHECK(same_guid(entries[0].clsid, Sample));
}

TEST(provider_quarantine, text_after_the_clsid_is_kept_as_a_note)
{
	auto entries = Quarantine::parse(
		L"{A0B1C2D3-E4F5-6789-ABCD-EF0123456789}   Rename with PowerRename\r\n");
	CHECK_EQ(entries.size(), size_t(1));
	if(entries.empty())
		return;
	CHECK(entries[0].note == L"Rename with PowerRename");
}

// A half-readable list should still quarantine the half it can read. Failing
// the whole load would silently re-enable every extension in it.
TEST(provider_quarantine, an_unparseable_line_is_skipped_rather_than_failing_the_file)
{
	auto entries = Quarantine::parse(
		L"this line is nonsense\r\n"
		L"{A0B1C2D3-E4F5-6789-ABCD-EF0123456789}\r\n"
		L"{not-a-guid}\r\n"
		L"{11111111-2222-3333-4444-555555555555}\r\n");
	CHECK_EQ(entries.size(), size_t(2));
}

TEST(provider_quarantine, the_same_clsid_twice_is_stored_once_and_keeps_the_first_note)
{
	auto entries = Quarantine::parse(
		L"{A0B1C2D3-E4F5-6789-ABCD-EF0123456789} first note\r\n"
		L"{a0b1c2d3-e4f5-6789-abcd-ef0123456789} second note\r\n");
	CHECK_EQ(entries.size(), size_t(1));
	if(entries.empty())
		return;
	CHECK(entries[0].note == L"first note");
}

// A corrupted or hostile file must not make menu build walk an unbounded list.
TEST(provider_quarantine, the_list_is_capped)
{
	std::wstring text;
	for(size_t i = 0; i < Quarantine::MaxEntries + 40; i++)
	{
		wchar_t line[64];
		::wsprintfW(line, L"{%08X-2222-3333-4444-555555555555}\r\n", static_cast<unsigned>(i + 1));
		text += line;
	}
	CHECK_EQ(Quarantine::parse(text).size(), Quarantine::MaxEntries);
}

TEST(provider_quarantine, lone_newlines_are_accepted_as_line_breaks)
{
	// An editor that writes LF should not silently un-quarantine everything.
	auto entries = Quarantine::parse(
		L"{A0B1C2D3-E4F5-6789-ABCD-EF0123456789}\n"
		L"{11111111-2222-3333-4444-555555555555}\n");
	CHECK_EQ(entries.size(), size_t(2));
}

TEST(provider_quarantine, a_serialized_list_parses_back_to_itself)
{
	std::vector<Quarantine::Entry> entries;
	{
		Quarantine::Entry e;
		e.clsid = Sample;
		e.hash = Quarantine::hash_clsid(Sample);
		e.note = L"NanaZip";
		entries.push_back(e);
	}
	{
		Quarantine::Entry e;
		CHECK(Quarantine::parse_guid(L"{11111111-2222-3333-4444-555555555555}", e.clsid));
		e.hash = Quarantine::hash_clsid(e.clsid);
		entries.push_back(e);
	}

	auto back = Quarantine::parse(Quarantine::serialize(entries));
	CHECK_EQ(back.size(), size_t(2));
	if(back.size() != 2)
		return;
	CHECK(same_guid(back[0].clsid, entries[0].clsid));
	CHECK_EQ(back[0].hash, entries[0].hash);
	CHECK(back[0].note == L"NanaZip");
	CHECK(same_guid(back[1].clsid, entries[1].clsid));
	CHECK(back[1].note.empty());
}

// The header the file is written with is comments, so a freshly written empty
// list must read back as empty rather than as one unparseable entry.
TEST(provider_quarantine, an_empty_serialized_list_reads_back_empty)
{
	CHECK_EQ(Quarantine::parse(Quarantine::serialize({})).size(), size_t(0));
}

TEST(provider_quarantine, a_real_file_round_trips_through_load_and_save)
{
	wchar_t directory[MAX_PATH]{};
	CHECK(::GetTempPathW(MAX_PATH, directory) != 0);

	std::wstring path = directory;
	path += L"nilesoft-quarantine-test\\";
	path += Quarantine::FileName;

	// Nothing there yet: a missing file is the normal state, not a failure.
	::DeleteFileW(path.c_str());
	CHECK_EQ(Quarantine::load(path).size(), size_t(0));

	std::vector<Quarantine::Entry> entries;
	Quarantine::Entry e;
	e.clsid = Sample;
	e.hash = Quarantine::hash_clsid(Sample);
	e.note = L"slow on this machine";
	entries.push_back(e);

	CHECK(Quarantine::save(path, entries));

	auto back = Quarantine::load(path);
	CHECK_EQ(back.size(), size_t(1));
	if(!back.empty())
	{
		CHECK(same_guid(back[0].clsid, Sample));
		CHECK_EQ(back[0].hash, Quarantine::hash_clsid(Sample));
		CHECK(back[0].note == L"slow on this machine");
	}

	// Saving an empty list is how a release is written, and it must not leave
	// the previous entry readable.
	CHECK(Quarantine::save(path, {}));
	CHECK_EQ(Quarantine::load(path).size(), size_t(0));

	::DeleteFileW(path.c_str());
	auto cut = path.find_last_of(L'\\');
	if(cut != std::wstring::npos)
		::RemoveDirectoryW(path.substr(0, cut).c_str());
}

// The file is written UTF-16 with a byte-order mark, and an editor that strips
// it must not un-quarantine everything.
TEST(provider_quarantine, a_file_without_a_byte_order_mark_is_still_read)
{
	wchar_t directory[MAX_PATH]{};
	CHECK(::GetTempPathW(MAX_PATH, directory) != 0);

	std::wstring path = directory;
	path += L"nilesoft-quarantine-nobom.txt";

	std::wstring text = L"{A0B1C2D3-E4F5-6789-ABCD-EF0123456789}\r\n";
	auto file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
							  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	CHECK(file != INVALID_HANDLE_VALUE);
	if(file != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		::WriteFile(file, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)),
					&written, nullptr);
		::CloseHandle(file);

		auto back = Quarantine::load(path);
		CHECK_EQ(back.size(), size_t(1));
	}
	::DeleteFileW(path.c_str());
}
