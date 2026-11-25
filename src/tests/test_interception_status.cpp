// Which mechanism is carrying a host's popup menus, as the report sees it.
//
// src/dll/src/Include/InterceptionStatus.h holds the reasoning - why the
// PopupInterceptionBackend interface docs/refactor/01-takeover-contract.md
// section 9 asked for was declined, and why the fact worth keeping is *which*
// of the two mechanisms is live rather than an abstraction over both.
//
// What is worth pinning here is narrow but load-bearing. The value crosses a
// process boundary as one word with a backend in its low bits and a health
// flag high up, and every way of getting that packing wrong produces a report
// that lies confidently: a healthy host described as displaced, or a host
// running on the weaker fallback described as running on the primary. None of
// those would fail anything else.

#include "test.h"

#include <Windows.h>
#include "Include/InterceptionStatus.h"

using namespace Nilesoft::Shell;
using Nilesoft::Shell::Diagnostics::PERF_EXPORT_INTERCEPTION_HEALTHY;
using Nilesoft::Shell::Diagnostics::PERF_EXPORT_INTERCEPTION_BACKEND_MASK;
using Nilesoft::Shell::Diagnostics::perf_export_interception_name;

namespace
{
	bool same(const wchar_t *a, const wchar_t *b)
	{
		if(!a || !b)
			return a == b;
		return ::wcscmp(a, b) == 0;
	}
}

/*
	The two fields must not overlap.

	This is the defect the rest of the file cannot catch on its own: if the
	health bit fell inside the backend mask, `backend_of` would return a
	different mechanism depending on health, and the report would name the
	wrong one exactly when something was wrong. Asserted against the constants
	rather than against a packed value, so it fails at the point somebody
	changes them.
*/
static_assert((PERF_EXPORT_INTERCEPTION_HEALTHY & PERF_EXPORT_INTERCEPTION_BACKEND_MASK) == 0,
			  "the health flag overlaps the backend field - backend_of() would return a "
			  "different mechanism depending on health, and the report would name the wrong "
			  "one exactly when something was wrong");

static_assert(Interception::None == 0,
			  "a zeroed block must read as 'nothing hooked', not as a backend");

static_assert(Interception::Win32uImport != Interception::PerModuleImports
				  && Interception::Win32uImport != Interception::None
				  && Interception::PerModuleImports != Interception::None,
			  "the two mechanisms must be distinguishable - which one is live is the whole "
			  "point of reporting this, see docs/refactor/01-takeover-contract.md section 9c");

static_assert((Interception::Win32uImport & PERF_EXPORT_INTERCEPTION_BACKEND_MASK)
					  == Interception::Win32uImport
				  && (Interception::PerModuleImports & PERF_EXPORT_INTERCEPTION_BACKEND_MASK)
						  == Interception::PerModuleImports,
			  "a backend must survive the mask, or publishing one would read back as another");

TEST(interception_status, a_published_backend_reads_back_with_its_health)
{
	Interception::publish(Interception::Win32uImport, true);
	CHECK_EQ(Interception::backend_of(Interception::current()), Interception::Win32uImport);
	CHECK(Interception::healthy_of(Interception::current()));

	Interception::publish(Interception::PerModuleImports, true);
	CHECK_EQ(Interception::backend_of(Interception::current()), Interception::PerModuleImports);
	CHECK(Interception::healthy_of(Interception::current()));
}

/*
	The case the `intercept` line exists to report, and the one where getting
	the packing wrong is worst: a host still running on a mechanism that is no
	longer Shell's. Health must be readable without disturbing the backend,
	because the report prints both on the same line.
*/
TEST(interception_status, an_unhealthy_backend_keeps_its_identity)
{
	Interception::publish(Interception::PerModuleImports, false);

	CHECK_EQ(Interception::backend_of(Interception::current()), Interception::PerModuleImports);
	CHECK(!Interception::healthy_of(Interception::current()));
}

TEST(interception_status, nothing_hooked_is_its_own_answer)
{
	Interception::publish(Interception::None, false);

	CHECK_EQ(Interception::backend_of(Interception::current()), Interception::None);
	CHECK(!Interception::healthy_of(Interception::current()));
}

// Publishing is a plain store, so the last one wins. Stated as a test because
// the hook calls this on every menu and a "first writer wins" implementation
// would freeze the report at whatever bootstrap saw.
TEST(interception_status, the_most_recent_publish_is_the_one_reported)
{
	Interception::publish(Interception::Win32uImport, true);
	Interception::publish(Interception::PerModuleImports, false);

	CHECK_EQ(Interception::backend_of(Interception::current()), Interception::PerModuleImports);
	CHECK(!Interception::healthy_of(Interception::current()));

	// Leave the slot as a passing run found it; this is process-global.
	Interception::publish(Interception::None, false);
}

TEST(interception_status, every_backend_has_a_name_a_person_can_read)
{
	CHECK(same(perf_export_interception_name(Interception::Win32uImport), L"win32u import"));
	CHECK(same(perf_export_interception_name(Interception::PerModuleImports), L"per-module imports"));
	CHECK(same(perf_export_interception_name(Interception::None), L"none"));
}

// The health bit must not change the name. It is printed beside it, not
// instead of it, and a displaced host still has to say which mechanism it was.
TEST(interception_status, the_name_ignores_the_health_bit)
{
	CHECK(same(perf_export_interception_name(Interception::Win32uImport | PERF_EXPORT_INTERCEPTION_HEALTHY),
			   L"win32u import"));
	CHECK(same(perf_export_interception_name(Interception::PerModuleImports | PERF_EXPORT_INTERCEPTION_HEALTHY),
			   L"per-module imports"));
}

// A block written by a newer Shell than this reader, or a corrupted one. It
// gets a name rather than an out-of-range read, because this string goes
// straight into a report.
TEST(interception_status, an_unknown_backend_reads_as_none_rather_than_rubbish)
{
	CHECK(same(perf_export_interception_name(0x7Fu), L"none"));
	CHECK(same(perf_export_interception_name(0xFFFFFFFFu), L"none"));
}
