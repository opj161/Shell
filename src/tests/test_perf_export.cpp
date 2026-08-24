// Reading a menu's timing out of the process that recorded it.
//
// src/shared/PerfExport.h is the transport and src/shared/PerfReport.h is the
// arithmetic. What is pinned here is everything that can be wrong without
// looking wrong:
//
//   - the ring inside the block, which a host writes one record at a time and
//     somebody else reads backwards;
//   - the sequence protocol, because a reader that does not notice a torn read
//     prints half of one menu and half of another as if it were one session;
//   - the clamping, because every number in the block was written by another
//     process and this one may be the one that matters;
//   - the percentile and the age, which produce a plausible-looking report
//     whichever way they are wrong.
//
// The section itself is exercised too - one real CreateFileMapping, written
// through the writer and read back through the reader - because "the layout is
// pointer-free and stable across architectures" is a claim about this build,
// and a static_assert only checks the sizes it was told to check.
//
// docs/refactor/06-phases-and-tests.md section 4, docs/refactor/08-handoff.md
// section 3.2.

#include "test.h"

#include "..\shared\PerfExport.h"
#include "..\shared\PerfReport.h"

#include <vector>
#include <memory>
#include <string>

using namespace Nilesoft::Shell::Diagnostics;

namespace
{
	// Any CLSID will do here: what is under test is the directory, not the
	// identifier. {A0B1C2D3-E4F5-6789-ABCD-EF0123456789}
	const GUID SampleClsid = { 0xA0B1C2D3, 0xE4F5, 0x6789,
							   { 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89 } };
}

namespace
{
	PerfExportBlock make_block(uint32_t capacity = PERF_EXPORT_RECORDS)
	{
		PerfExportBlock block{};
		block.header.magic = PERF_EXPORT_MAGIC;
		block.header.version = PERF_EXPORT_VERSION;
		block.header.record_size = sizeof(PerfExportRecord);
		block.header.capacity = capacity;
		return block;
	}

	PerfExportRecord make_record(uint32_t total_us, uint64_t tick = 0)
	{
		PerfExportRecord record{};
		record.total_microseconds = total_us;
		record.tick = tick;
		record.decision = 1;			// takeover
		return record;
	}

	std::vector<PerfExportRecord> read_all(const PerfExportBlock &block, size_t want)
	{
		std::vector<PerfExportRecord> out(want);
		bool torn = true;
		auto count = perf_export_load(block, out.data(), out.size(), torn);
		out.resize(torn ? 0 : count);
		return out;
	}

	bool same(const wchar_t *a, const wchar_t *b)
	{
		return a && b && ::lstrcmpW(a, b) == 0;
	}

	// The mark the latency budget is written against, recorded the way the DLL
	// records it. A session that never displayed a menu simply does not get one.
	void set_pre_display(PerfExportRecord &record, uint32_t microseconds)
	{
		record.decision = 1;			// takeover
		record.phase_count = 1;
		record.phases[0].microseconds = microseconds;
		record.phases[0].count = -1;
		::lstrcpynW(record.phases[0].name, PERF_REPORT_PRE_DISPLAY, PERF_EXPORT_NAME);
	}
}

TEST(perf_export, a_fresh_block_reads_back_empty)
{
	auto block = make_block();
	auto records = read_all(block, PERF_EXPORT_RECORDS);
	CHECK_EQ(records.size(), size_t(0));
}

TEST(perf_export, records_come_back_newest_first)
{
	auto block = make_block();
	for(uint32_t i = 1; i <= 3; i++)
		perf_export_store(block, make_record(i * 1000));

	auto records = read_all(block, PERF_EXPORT_RECORDS);
	CHECK_EQ(records.size(), size_t(3));
	if(records.size() != 3)
		return;

	// Newest first is the whole point: a report shows what just happened, and a
	// reader that walked forwards from slot zero would open with whatever is
	// about to fall off the end of the ring.
	CHECK_EQ(records[0].total_microseconds, 3000u);
	CHECK_EQ(records[1].total_microseconds, 2000u);
	CHECK_EQ(records[2].total_microseconds, 1000u);
}

TEST(perf_export, the_ring_wraps_and_keeps_the_newest)
{
	auto block = make_block();
	for(uint32_t i = 1; i <= PERF_EXPORT_RECORDS + 5; i++)
		perf_export_store(block, make_record(i));

	auto records = read_all(block, PERF_EXPORT_RECORDS);
	CHECK_EQ(records.size(), size_t(PERF_EXPORT_RECORDS));
	if(records.size() != PERF_EXPORT_RECORDS)
		return;

	CHECK_EQ(records[0].total_microseconds, uint32_t(PERF_EXPORT_RECORDS + 5));
	CHECK_EQ(records[PERF_EXPORT_RECORDS - 1].total_microseconds, uint32_t(6));

	// published keeps counting past the wrap, which is what lets a report say
	// "1,412 menus, 16 held" rather than implying the host has ever opened
	// sixteen.
	CHECK_EQ(block.header.published, uint64_t(PERF_EXPORT_RECORDS + 5));
	CHECK_EQ(block.header.count, PERF_EXPORT_RECORDS);
}

TEST(perf_export, asking_for_fewer_than_are_held_gets_the_newest_ones)
{
	auto block = make_block();
	for(uint32_t i = 1; i <= 10; i++)
		perf_export_store(block, make_record(i));

	auto records = read_all(block, 3);
	CHECK_EQ(records.size(), size_t(3));
	if(records.size() != 3)
		return;

	CHECK_EQ(records[0].total_microseconds, 10u);
	CHECK_EQ(records[2].total_microseconds, 8u);
}

TEST(perf_export, a_sequence_caught_mid_write_is_reported_as_torn)
{
	auto block = make_block();
	perf_export_store(block, make_record(1000));

	// Exactly what a reader sees if it arrives between the writer's two bumps.
	// The alternative - reading anyway - is how half of one menu and half of
	// another get printed as a single session.
	block.header.sequence++;

	std::vector<PerfExportRecord> out(4);
	bool torn = false;
	auto count = perf_export_load(block, out.data(), out.size(), torn);
	CHECK(torn);
	CHECK_EQ(count, size_t(0));
}

TEST(perf_export, a_write_that_completes_during_the_copy_is_reported_as_torn)
{
	// The test above stages the *first* of the two sequence checks - an odd
	// counter on entry. It says nothing about the second, which catches a
	// writer completing a whole record while the reader was copying: even
	// before, even after, but a different even. Deleting that read leaves
	// every other test in this file passing, which was checked.
	//
	// A concurrency test cannot pin it either, and that was measured rather
	// than assumed: with a writer storing in a tight loop *every* read comes
	// back torn, so the test observes no clean reads and cannot tell a working
	// check from one that refuses everything; throttle the writer and no read
	// tears at all. So the reader takes a seam that runs at exactly the moment
	// in question, and this stages it deterministically.

	auto block = std::make_unique<PerfExportBlock>();
	*block = make_block();
	perf_export_store(*block, make_record(1000));

	std::vector<PerfExportRecord> out(4);
	bool torn = false;

	auto count = perf_export_load(*block, out.data(), out.size(), torn,
								  [](void *context)
								  {
									  auto &target = *static_cast<PerfExportBlock *>(context);
									  PerfExportRecord record{};
									  record.total_microseconds = 2000;
									  perf_export_store(target, record);
								  },
								  block.get());

	CHECK(torn);
	CHECK_EQ(count, size_t(0));
}

TEST(perf_export, a_settled_sequence_is_not_torn)
{
	auto block = make_block();
	perf_export_store(block, make_record(1000));

	// The complement of the case above, and the one that would make the reader
	// useless if it were wrong the other way: a block nobody is writing to must
	// read cleanly, or every report says "busy".
	std::vector<PerfExportRecord> out(4);
	bool torn = true;
	auto count = perf_export_load(block, out.data(), out.size(), torn);
	CHECK(!torn);
	CHECK_EQ(count, size_t(1));

	// And the counter is even after a completed write, which is the invariant
	// the whole protocol rests on.
	CHECK_EQ(block.header.sequence % 2, 0u);
}

TEST(perf_export, a_capacity_larger_than_the_array_is_refused_not_clamped)
{
	auto block = make_block();
	perf_export_store(block, make_record(1000));

	// Another process claiming its block holds more records than this build's
	// array does is the one lie that would read past the mapping.
	block.header.capacity = PERF_EXPORT_RECORDS + 1;

	std::vector<PerfExportRecord> out(PERF_EXPORT_RECORDS + 1);
	bool torn = true;
	auto count = perf_export_load(block, out.data(), out.size(), torn);
	CHECK(!torn);					// a lie, not a race - retrying would not help
	CHECK_EQ(count, size_t(0));
}

TEST(perf_export, a_count_beyond_the_capacity_is_clamped)
{
	auto block = make_block();
	for(uint32_t i = 1; i <= 3; i++)
		perf_export_store(block, make_record(i));

	block.header.count = PERF_EXPORT_RECORDS * 4;

	auto records = read_all(block, PERF_EXPORT_RECORDS);
	CHECK_EQ(records.size(), size_t(PERF_EXPORT_RECORDS));
}

TEST(perf_export, a_next_beyond_the_capacity_is_clamped)
{
	auto block = make_block();
	perf_export_store(block, make_record(7));
	block.header.next = 9999;

	// The property that matters is that it does not index out of the array;
	// which record comes back first after a corrupted cursor is not something
	// a reader can be right about.
	auto records = read_all(block, PERF_EXPORT_RECORDS);
	CHECK_EQ(records.size(), size_t(1));
}

TEST(perf_export, a_zero_capacity_block_is_never_written_to)
{
	auto block = make_block(0);
	perf_export_store(block, make_record(1000));

	CHECK_EQ(block.header.published, uint64_t(0));
	CHECK_EQ(block.header.count, 0u);
}

TEST(perf_export, sanitize_terminates_a_name_that_fills_its_buffer)
{
	PerfExportRecord record{};
	record.phase_count = 1;
	for(size_t i = 0; i < PERF_EXPORT_NAME; i++)
		record.phases[0].name[i] = L'x';

	perf_export_sanitize(record);

	// Everything downstream treats this as a C string. A writer that filled the
	// buffer - or lied - must not run the reader off the end of it.
	CHECK_EQ(record.phases[0].name[PERF_EXPORT_NAME - 1], L'\0');
	CHECK_EQ(size_t(::lstrlenW(record.phases[0].name)), PERF_EXPORT_NAME - 1);
}

TEST(perf_export, sanitize_clamps_counts_that_exceed_the_arrays)
{
	PerfExportRecord record{};
	record.phase_count = 500;
	record.provider_count = 500;

	perf_export_sanitize(record);

	CHECK_EQ(record.phase_count, uint32_t(PERF_EXPORT_PHASES));
	CHECK_EQ(record.provider_count, uint32_t(PERF_EXPORT_PROVIDERS));
}

TEST(perf_export, the_object_name_is_the_prefix_and_the_process_id)
{
	wchar_t name[128]{};
	CHECK(perf_export_name(4321, name, ARRAYSIZE(name)));
	CHECK(same(name, L"Local\\Nilesoft.Shell.Perf.4321"));
}

TEST(perf_export, an_object_name_that_would_not_fit_is_refused_not_truncated)
{
	// A truncated name addresses a *different* process's block, which is the
	// one failure here that would silently report the wrong host's menus.
	wchar_t name[8]{};
	CHECK(!perf_export_name(4321, name, ARRAYSIZE(name)));
	CHECK_EQ(name[0], L'\0');
}

TEST(perf_export, a_real_section_round_trips_through_the_writer_and_the_reader)
{
	// The layout being pointer-free and architecture-stable is a claim about
	// this build; a static_assert only checks what it was told to. This puts a
	// record through an actual CreateFileMapping and reads it back exactly the
	// way shell.exe does.
	PerfExportWriter writer;
	CHECK(writer.open());

	auto record = make_record(12345, ::GetTickCount64());
	record.phase_count = 1;
	record.phases[0].microseconds = 4200;
	record.phases[0].count = 30;
	::lstrcpynW(record.phases[0].name, L"explorer.commands", PERF_EXPORT_NAME);

	writer.store(record);

	PerfExportSource source{};
	std::vector<PerfExportRecord> out(PERF_EXPORT_RECORDS);
	size_t written = 0;
	auto status = perf_export_read(::GetCurrentProcessId(), source,
								   out.data(), out.size(), written);

	CHECK(status == PerfExportStatus::Ok);
	CHECK(written >= 1);
	if(written == 0)
		return;

	CHECK_EQ(out[0].total_microseconds, 12345u);
	CHECK_EQ(out[0].phase_count, 1u);
	CHECK(same(out[0].phases[0].name, L"explorer.commands"));
	CHECK_EQ(source.process_id, uint32_t(::GetCurrentProcessId()));
	CHECK_EQ(source.architecture, perf_export_architecture());

	// tests.exe, which is the point: the report names the host it read.
	CHECK(source.host[0] != L'\0');
}

TEST(perf_export, tracking_flags_are_rendered_as_words_and_returncmd_comes_first)
{
	wchar_t names[128]{};

	// TPM_RETURNCMD is the one that decides which half of
	// complete_host_contract runs, so it leads whatever else is set.
	perf_export_flag_names(TPM_RETURNCMD | TPM_RIGHTBUTTON, names, ARRAYSIZE(names));
	CHECK(same(names, L"RETURNCMD|RIGHTBUTTON"));

	// A host that passes nothing is the commonest case and a real answer. An
	// empty string in a report reads as missing data.
	perf_export_flag_names(0, names, ARRAYSIZE(names));
	CHECK(same(names, L"(none)"));

	// Flags with no bearing on the contract are not noise in the line.
	perf_export_flag_names(TPM_LEFTALIGN | TPM_TOPALIGN, names, ARRAYSIZE(names));
	CHECK(same(names, L"(none)"));
}

TEST(perf_export, rendering_flags_into_a_buffer_that_cannot_hold_them_still_terminates)
{
	// The report builds this on the stack for every session it prints.
	wchar_t tiny[6]{};
	perf_export_flag_names(TPM_RETURNCMD | TPM_NONOTIFY | TPM_VERTICAL, tiny, ARRAYSIZE(tiny));
	CHECK_EQ(tiny[ARRAYSIZE(tiny) - 1], L'\0');
	CHECK(size_t(::lstrlenW(tiny)) < ARRAYSIZE(tiny));
}

TEST(perf_export, a_process_with_no_block_reports_not_present)
{
	PerfExportSource source{};
	PerfExportRecord out[1]{};
	size_t written = 0;

	// A process id nothing plausible is running under. The reader must answer
	// "nothing here" rather than treating an unopenable name as an error worth
	// reporting - it does this for every process on the machine.
	auto status = perf_export_read(0xFFFFFFFEu, source, out, 1, written);
	CHECK(status == PerfExportStatus::NotPresent);
	CHECK_EQ(written, size_t(0));
}

TEST(perf_export, enumeration_finds_this_process)
{
	std::vector<uint32_t> pids(4096);
	auto found = perf_export_enumerate(pids.data(), pids.size());
	CHECK(found > 0);

	bool self = false;
	for(size_t i = 0; i < found; i++)
	{
		if(pids[i] == ::GetCurrentProcessId())
			self = true;
	}
	CHECK(self);
}

TEST(perf_report, a_percentile_over_one_sample_is_that_sample)
{
	uint32_t values[] = { 4200 };
	CHECK_EQ(perf_report_percentile(values, 1, 50), 4200u);

	uint32_t again[] = { 4200 };
	CHECK_EQ(perf_report_percentile(again, 1, 95), 4200u);
}

TEST(perf_report, a_percentile_is_nearest_rank_and_lands_on_a_real_sample)
{
	// Sorted: 10 20 30 40 50. p50 -> ceil(0.5*5) = 3 -> 30.
	uint32_t values[] = { 50, 10, 40, 20, 30 };
	CHECK_EQ(perf_report_percentile(values, 5, 50), 30u);

	// p95 -> ceil(4.75) = 5 -> 50. Interpolating would answer 47.5, a duration
	// no menu ever took; for a latency budget that is worse than a blunt one.
	uint32_t again[] = { 50, 10, 40, 20, 30 };
	CHECK_EQ(perf_report_percentile(again, 5, 95), 50u);
}

TEST(perf_report, a_percentile_over_nothing_is_zero_rather_than_undefined)
{
	CHECK_EQ(perf_report_percentile(nullptr, 0, 95), 0u);

	uint32_t values[1]{};
	CHECK_EQ(perf_report_percentile(values, 0, 95), 0u);
}

TEST(perf_report, an_age_never_runs_backwards)
{
	// The reader samples the tick after the writer did, and GetTickCount64
	// moves in 10-16 ms steps, so a session recorded a moment ago can read as
	// being in the future. "0.0s ago" is right; "-0.0s ago" is a bug report.
	CHECK_EQ(perf_report_age_ms(1000, 1016), uint64_t(0));
	CHECK_EQ(perf_report_age_ms(2000, 1000), uint64_t(1000));
}

TEST(perf_report, the_slowest_session_wins_ties_by_being_the_more_recent)
{
	PerfExportRecord records[3]{};
	set_pre_display(records[0], 5000);		// newest
	set_pre_display(records[1], 5000);
	set_pre_display(records[2], 1000);

	// Records arrive newest first, so a stable first-wins scan shows the menu
	// the user just opened rather than one about to fall off the ring.
	CHECK_EQ(perf_report_slowest(records, 3), size_t(0));
}

TEST(perf_report, the_slowest_session_is_the_slowest_one)
{
	PerfExportRecord records[3]{};
	set_pre_display(records[0], 1000);
	set_pre_display(records[1], 9000);
	set_pre_display(records[2], 5000);

	CHECK_EQ(perf_report_slowest(records, 3), size_t(1));
}

TEST(perf_report, the_menu_left_open_longest_is_not_the_slowest_one)
{
	// The whole reason the summary measures a phase rather than the session.
	// total_microseconds runs from the hook being entered to the hook
	// returning, so it counts the time the menu sat on screen waiting for the
	// user. Measured in a real Explorer: 1,435 ms of session for a menu that
	// took 11 ms to appear. Ranking by session would show that one every time
	// and it is the one session guaranteed to say nothing about Shell.
	PerfExportRecord records[2]{};
	records[0].total_microseconds = 1435000;	// on screen while somebody read it
	set_pre_display(records[0], 11000);
	records[1].total_microseconds = 40000;
	set_pre_display(records[1], 38000);			// genuinely slow to appear

	CHECK_EQ(perf_report_slowest(records, 2), size_t(1));

	uint32_t scratch[2]{};
	auto summary = perf_report_summarize(records, 2, scratch, 2);
	CHECK_EQ(summary.max_microseconds, 38000u);
	CHECK_EQ(summary.p95_microseconds, 38000u);
}

TEST(perf_report, a_session_that_never_displayed_a_menu_is_not_a_fast_one)
{
	// A declined popup never reaches the pre-display mark. Counting it as a
	// zero-millisecond menu would drag every percentile towards zero and report
	// a host Shell is not composing for as the fastest on the machine.
	PerfExportRecord records[3]{};
	set_pre_display(records[0], 20000);
	records[1].decision = 5;					// declined, no phases at all
	records[2].decision = 5;

	uint32_t scratch[3]{};
	auto summary = perf_report_summarize(records, 3, scratch, 3);

	CHECK_EQ(summary.sessions, size_t(3));
	CHECK_EQ(summary.measured, size_t(1));
	CHECK_EQ(summary.p50_microseconds, 20000u);
}

TEST(perf_report, a_host_that_displayed_nothing_reports_no_measurements)
{
	PerfExportRecord records[2]{};
	records[0].decision = 5;
	records[1].decision = 5;

	uint32_t scratch[2]{};
	auto summary = perf_report_summarize(records, 2, scratch, 2);

	// Not zero milliseconds - no milliseconds. The report says so in words.
	CHECK_EQ(summary.measured, size_t(0));
	CHECK_EQ(summary.decisions[5], 2u);
}

TEST(perf_report, a_phase_lookup_matches_the_whole_name)
{
	PerfExportRecord record{};
	record.phase_count = 2;
	::lstrcpynW(record.phases[0].name, L"popup.total", PERF_EXPORT_NAME);
	record.phases[0].microseconds = 111;
	::lstrcpynW(record.phases[1].name, L"popup.total_pre_display", PERF_EXPORT_NAME);
	record.phases[1].microseconds = 222;

	// A prefix match would find "popup.total" first and report a number from a
	// phase that is not the one asked for.
	CHECK_EQ(perf_report_phase(record, L"popup.total_pre_display"), 222u);
	CHECK_EQ(perf_report_phase(record, L"popup.total"), 111u);
	CHECK_EQ(perf_report_phase(record, L"popup.nothing"), 0u);
}

TEST(perf_report, a_summary_counts_the_decisions_that_happened)
{
	PerfExportRecord records[4]{};
	records[0].decision = 1;		// takeover
	records[1].decision = 5;		// declined
	records[2].decision = 5;
	records[3].decision = 1;

	uint32_t scratch[4]{};
	auto summary = perf_report_summarize(records, 4, scratch, 4);

	CHECK_EQ(summary.decisions[1], 2u);
	CHECK_EQ(summary.decisions[5], 2u);
	CHECK_EQ(summary.decisions[4], 0u);
}

TEST(perf_report, a_summary_with_no_scratch_still_reports_the_maximum)
{
	PerfExportRecord records[2]{};
	set_pre_display(records[0], 1000);
	set_pre_display(records[1], 8000);

	auto summary = perf_report_summarize(records, 2, nullptr, 0);
	CHECK_EQ(summary.max_microseconds, 8000u);
	CHECK_EQ(summary.sessions, size_t(2));
	CHECK_EQ(summary.slowest, size_t(1));
}

TEST(perf_report, a_summary_over_nothing_reports_no_slowest_session)
{
	// The caller indexes records[summary.slowest]; "there isn't one" has to be
	// out of range rather than zero.
	auto summary = perf_report_summarize(nullptr, 0, nullptr, 0);
	CHECK_EQ(summary.sessions, size_t(0));
	CHECK_EQ(summary.slowest, size_t(0));
}

TEST(perf_report, milliseconds_round_rather_than_truncate)
{
	uint32_t whole = 0, tenth = 0;

	perf_report_split_ms(1999, whole, tenth);
	CHECK_EQ(whole, 2u);
	CHECK_EQ(tenth, 0u);			// not 1.9

	perf_report_split_ms(45050, whole, tenth);
	CHECK_EQ(whole, 45u);
	CHECK_EQ(tenth, 1u);

	perf_report_split_ms(0, whole, tenth);
	CHECK_EQ(whole, 0u);
	CHECK_EQ(tenth, 0u);

	perf_report_split_ms(949, whole, tenth);
	CHECK_EQ(whole, 0u);
	CHECK_EQ(tenth, 9u);
}

/*
	The provider-name directory.

	A record carries a CLSID hash so the measured path never copies a string.
	That makes the report stable but unactionable - `provider e345019d 186 ms`
	names nothing a user can quarantine - so the names live beside the records,
	one entry per distinct provider, written the first time a host activates it.

	What can go wrong here without looking wrong is the append: the count must
	never admit an entry that is not finished, or a reader prints whatever was
	in that slot. Hence entry-then-count, and hence these.
*/
TEST(perf_export, a_fresh_block_knows_no_provider_names)
{
	auto block = make_block();
	CHECK(perf_export_find_name(block, 0xe345019du) == nullptr);
	CHECK_EQ(block.header.directory_count, 0u);
}

TEST(perf_export, a_noted_name_comes_back_for_its_hash)
{
	auto block = make_block();
	CHECK(perf_export_note_provider(block, 0xe345019du, SampleClsid, L"Rename with PowerRename"));
	CHECK_EQ(block.header.directory_count, 1u);
	CHECK(same(perf_export_find_name(block, 0xe345019du), L"Rename with PowerRename"));
}

TEST(perf_export, an_unknown_hash_still_has_no_name)
{
	auto block = make_block();
	perf_export_note_provider(block, 0xe345019du, SampleClsid, L"Rename with PowerRename");
	CHECK(perf_export_find_name(block, 0x9e0df88cu) == nullptr);
}

// The common case after the first menu: the same handler, every time. If this
// appended it would exhaust 32 slots within a few right-clicks.
TEST(perf_export, noting_the_same_provider_twice_adds_nothing)
{
	auto block = make_block();
	CHECK(perf_export_note_provider(block, 0xe345019du, SampleClsid, L"Rename with PowerRename"));
	CHECK(!perf_export_note_provider(block, 0xe345019du, SampleClsid, L"Rename with PowerRename"));
	CHECK(!perf_export_note_provider(block, 0xe345019du, SampleClsid, L"Something else entirely"));
	CHECK_EQ(block.header.directory_count, 1u);

	// The first title wins: an entry is never rewritten, which is what lets a
	// reader copy the directory outside the sequence protocol.
	CHECK(same(perf_export_find_name(block, 0xe345019du), L"Rename with PowerRename"));
}

/*
	A provider with no title is still identified.

	This is the defect docs/refactor/05-capabilities.md section 1c is about,
	as a test: identity used to be a side effect of learning the *label*, so a
	handler that cost 70 ms and returned no title reported a bare hash - and
	`shell.exe -quarantine:add` takes a CLSID. The provider a user most wants
	to act on was the one the report could not name to the command.
*/
TEST(perf_export, a_provider_with_no_title_is_still_identified)
{
	auto block = make_block();
	CHECK(perf_export_note_provider(block, 0xe345019du, SampleClsid, nullptr));
	CHECK_EQ(block.header.directory_count, 1u);

	// The CLSID is there, which is the actionable half. The second check
	// short-circuits deliberately: with the defect this test exists to catch,
	// the lookup returns null, and a bare dereference would abort the whole
	// suite instead of reporting this one test - which is how a defect gets
	// mistaken for a broken harness.
	auto clsid = perf_export_find_clsid(block, 0xe345019du);
	CHECK(clsid != nullptr);
	CHECK(clsid && ::IsEqualGUID(*clsid, SampleClsid) != FALSE);

	// The name is not, and says so rather than returning an empty string that
	// a report would print as a blank column.
	CHECK(perf_export_find_name(block, 0xe345019du) == nullptr);
}

TEST(perf_export, an_empty_title_is_the_same_as_none)
{
	auto block = make_block();
	CHECK(perf_export_note_provider(block, 0xe345019du, SampleClsid, L""));
	CHECK_EQ(block.header.directory_count, 1u);
	CHECK(perf_export_find_name(block, 0xe345019du) == nullptr);
	CHECK(perf_export_find_clsid(block, 0xe345019du) != nullptr);
}

/*
	The sequence every menu actually produces: identity for every candidate at
	the top of the loop, then a name from whichever activation first returned a
	title. The entry must be the same one - appending a second would burn a
	directory slot per provider and make the same handler appear twice.
*/
TEST(perf_export, a_name_learned_later_fills_in_the_entry_that_is_already_there)
{
	auto block = make_block();
	CHECK(perf_export_note_provider(block, 0xe345019du, SampleClsid));
	CHECK(!perf_export_note_provider(block, 0xe345019du, SampleClsid, L"NanaZip"));

	CHECK_EQ(block.header.directory_count, 1u);
	CHECK(same(perf_export_find_name(block, 0xe345019du), L"NanaZip"));
	CHECK(perf_export_find_clsid(block, 0xe345019du) != nullptr);
}

// Filling in a missing name is the only rewrite allowed, and it happens once.
// A handler may title itself differently for a different selection; letting
// the later title win would make the label move under a reader.
TEST(perf_export, a_name_already_recorded_is_never_replaced)
{
	auto block = make_block();
	perf_export_note_provider(block, 0xe345019du, SampleClsid);
	perf_export_note_provider(block, 0xe345019du, SampleClsid, L"NanaZip");
	perf_export_note_provider(block, 0xe345019du, SampleClsid, L"Something else entirely");

	CHECK_EQ(block.header.directory_count, 1u);
	CHECK(same(perf_export_find_name(block, 0xe345019du), L"NanaZip"));
}

TEST(perf_export, a_name_that_fills_the_buffer_is_truncated_and_terminated)
{
	auto block = make_block();
	std::wstring huge(PERF_EXPORT_PROVIDER_NAME + 40, L'x');
	CHECK(perf_export_note_provider(block, 1, SampleClsid, huge.c_str()));

	auto got = perf_export_find_name(block, 1);
	CHECK(got != nullptr);
	if(!got)
		return;
	CHECK_EQ(::lstrlenW(got), int(PERF_EXPORT_PROVIDER_NAME - 1));
	CHECK_EQ(block.directory[0].name[PERF_EXPORT_PROVIDER_NAME - 1], L'\0');
}

TEST(perf_export, the_directory_fills_up_and_says_so_rather_than_overwriting)
{
	auto block = make_block();
	for(uint32_t i = 0; i < PERF_EXPORT_DIRECTORY; i++)
	{
		wchar_t name[32];
		::wsprintfW(name, L"provider %u", i);
		CHECK(perf_export_note_provider(block, 1000 + i, SampleClsid, name));
	}
	CHECK_EQ(block.header.directory_count, PERF_EXPORT_DIRECTORY);
	CHECK_EQ(block.directory_dropped, 0u);

	// One too many: refused, counted, and nothing already there is disturbed.
	CHECK(!perf_export_note_provider(block, 9999, SampleClsid, L"one too many"));
	CHECK_EQ(block.header.directory_count, PERF_EXPORT_DIRECTORY);
	CHECK_EQ(block.directory_dropped, 1u);
	CHECK(perf_export_find_name(block, 9999) == nullptr);
	CHECK(same(perf_export_find_name(block, 1000), L"provider 0"));
	CHECK(same(perf_export_find_name(block, 1000 + PERF_EXPORT_DIRECTORY - 1),
			   L"provider 31"));
}

// A count larger than the array is another process's corruption, and writing at
// a clamped index would overwrite an entry somebody is about to read.
TEST(perf_export, a_directory_count_beyond_the_array_is_refused_not_clamped)
{
	auto block = make_block();
	perf_export_note_provider(block, 1, SampleClsid, L"first");
	block.header.directory_count = PERF_EXPORT_DIRECTORY + 9;

	CHECK(!perf_export_note_provider(block, 2, SampleClsid, L"second"));
	CHECK(same(block.directory[0].name, L"first"));

	// A lookup clamps instead, because reading a slot inside the array is
	// harmless and refusing would lose every name the block does hold.
	CHECK(same(perf_export_find_name(block, 1), L"first"));
}

/*
	The append order, observed at the one moment it is visible.

	A reader in another process copies the directory outside the sequence
	protocol, so `directory_count` is the only thing telling it how far the
	table is valid. If the count were raised before the entry was written, that
	reader could copy a name still being filled in.

	A test that inspects the block after the call returns cannot tell the two
	orderings apart - both leave identical state - so this uses the same
	interpose seam perf_export_load has, which runs between the entry and the
	count. Written the other way round first, and it passed; that is why the
	seam is here.
*/
TEST(perf_export, the_count_does_not_admit_an_entry_until_it_is_written)
{
	struct Seen
	{
		uint32_t count_at_that_moment;
		PerfExportBlock *block;
		wchar_t name_at_that_moment[PERF_EXPORT_PROVIDER_NAME];
		uint32_t hash_at_that_moment;
	} seen{};

	auto block = std::make_unique<PerfExportBlock>();
	*block = make_block();
	seen.block = block.get();

	perf_export_note_provider(*block, 0x1234u, SampleClsid, L"Rename with PowerRename",
		[](void *ctx)
		{
			auto *s = static_cast<Seen *>(ctx);
			s->count_at_that_moment = s->block->header.directory_count;
			s->hash_at_that_moment = s->block->directory[0].clsid_hash;
			::lstrcpynW(s->name_at_that_moment, s->block->directory[0].name,
						PERF_EXPORT_PROVIDER_NAME);
		}, &seen);

	// Mid-append: the entry is already complete and the count has not yet
	// admitted it, so a reader arriving here sees an empty directory rather
	// than a half-written name.
	CHECK_EQ(seen.count_at_that_moment, 0u);
	CHECK_EQ(seen.hash_at_that_moment, 0x1234u);
	CHECK(same(seen.name_at_that_moment, L"Rename with PowerRename"));

	// And afterwards it is visible.
	CHECK_EQ(block->header.directory_count, 1u);
	CHECK(same(perf_export_find_name(*block, 0x1234u), L"Rename with PowerRename"));
}

/*
	The identifier the report prints has to be the one -quarantine:add accepts,
	or the diagnosis and the treatment name the same extension two ways with
	nothing connecting them. That is why the directory carries the CLSID and not
	just the hash it is keyed by.
*/
TEST(perf_export, the_directory_remembers_the_clsid_the_hash_came_from)
{
	auto block = make_block();
	CHECK(perf_export_note_provider(block, 0xe345019du, SampleClsid, L"NanaZip"));

	auto *entry = block.directory[0].clsid_hash == 0xe345019du ? &block.directory[0] : nullptr;
	CHECK(entry != nullptr);
	if(!entry)
		return;
	CHECK(0 == ::memcmp(&entry->clsid, &SampleClsid, sizeof(GUID)));
}

TEST(perf_export, the_reader_copies_the_directory_out_of_the_block)
{
	// The view is unmapped before perf_export_read returns, so a source that
	// pointed into the block instead of copying would dangle at exactly the
	// moment the report prints it.
	PerfExportWriter writer;
	CHECK(writer.open());

	writer.note_provider(0xabcd1234u, SampleClsid, L"Search Everything");
	writer.store(make_record(999, ::GetTickCount64()));

	PerfExportSource source{};
	std::vector<PerfExportRecord> out(PERF_EXPORT_RECORDS);
	size_t written = 0;
	auto status = perf_export_read(::GetCurrentProcessId(), source,
								   out.data(), out.size(), written);

	CHECK(status == PerfExportStatus::Ok);
	CHECK(source.directory_count >= 1u);
	CHECK(same(source.name_for(0xabcd1234u), L"Search Everything"));
	CHECK(source.name_for(0x11112222u) == nullptr);

	// The CLSID travels with it: this is what the report prints.
	auto *clsid = source.clsid_for(0xabcd1234u);
	CHECK(clsid != nullptr);
	if(clsid)
		CHECK(0 == ::memcmp(clsid, &SampleClsid, sizeof(GUID)));
	CHECK(source.clsid_for(0x11112222u) == nullptr);
}
