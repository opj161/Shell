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

using namespace Nilesoft::Shell::Diagnostics;

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
