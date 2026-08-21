#include "test.h"

#include <windows.h>
#include <string>

#include "../setup/ca/TreatAsPlan.h"

namespace
{
	std::wstring owned_marker()
	{
		return L"C:\\ProgramData\\Nilesoft\\Shell\\Staging\\treatas."
			L"0123456789abcdef0123456789abcdef.created";
	}
}

TEST(treatas_plan, noop_and_uninstall_roundtrip)
{
	Nilesoft::TreatAsPlan::Plan plan;
	CHECK(Nilesoft::TreatAsPlan::Parse(L"noop", plan));
	CHECK(plan.op == Nilesoft::TreatAsPlan::Op::noop);
	CHECK(plan.marker.empty());
	CHECK(Nilesoft::TreatAsPlan::Serialize(plan.op) == L"noop");

	CHECK(Nilesoft::TreatAsPlan::Parse(L"uninstall-ours", plan));
	CHECK(plan.op == Nilesoft::TreatAsPlan::Op::uninstall_ours);
	CHECK(plan.marker.empty());
	CHECK(Nilesoft::TreatAsPlan::Serialize(plan.op) == L"uninstall-ours");
}

TEST(treatas_plan, install_absent_carries_owned_marker_path)
{
	auto marker = owned_marker();
	auto data = Nilesoft::TreatAsPlan::Serialize(
		Nilesoft::TreatAsPlan::Op::install_absent, marker);
	CHECK(data == L"install-absent|" + marker);

	Nilesoft::TreatAsPlan::Plan plan;
	CHECK(Nilesoft::TreatAsPlan::Parse(data, plan));
	CHECK(plan.op == Nilesoft::TreatAsPlan::Op::install_absent);
	CHECK(plan.marker == marker);
}

TEST(treatas_plan, install_absent_without_owned_marker_is_rejected)
{
	Nilesoft::TreatAsPlan::Plan plan;
	CHECK(!Nilesoft::TreatAsPlan::Parse(L"install-absent", plan));
	CHECK(!Nilesoft::TreatAsPlan::Parse(L"install-absent|", plan));
	CHECK(!Nilesoft::TreatAsPlan::Parse(
		L"install-absent|C:\\Temp\\treatas.0123456789abcdef0123456789abcdef.created",
		plan));
	CHECK(!Nilesoft::TreatAsPlan::Parse(
		L"install-absent|C:\\ProgramData\\Nilesoft\\Shell\\Staging\\..\\treatas."
		L"0123456789abcdef0123456789abcdef.created",
		plan));
	CHECK(!Nilesoft::TreatAsPlan::Parse(
		L"install-absent|C:\\ProgramData\\Nilesoft\\Shell\\Staging\\other.created",
		plan));
	CHECK(!Nilesoft::TreatAsPlan::Parse(L"install-present|x", plan));
}

TEST(treatas_plan, rollback_deletes_treatas_only_when_this_transaction_created_it)
{
	Nilesoft::TreatAsPlan::Plan created;
	CHECK(Nilesoft::TreatAsPlan::Parse(
		Nilesoft::TreatAsPlan::Serialize(Nilesoft::TreatAsPlan::Op::install_absent,
			owned_marker()),
		created));

	CHECK(Nilesoft::TreatAsPlan::RollbackRemovesTreatAs(created, true));
	CHECK(!Nilesoft::TreatAsPlan::RollbackRemovesTreatAs(created, false));

	Nilesoft::TreatAsPlan::Plan noop;
	CHECK(Nilesoft::TreatAsPlan::Parse(L"noop", noop));
	CHECK(!Nilesoft::TreatAsPlan::RollbackRemovesTreatAs(noop, true));

	Nilesoft::TreatAsPlan::Plan uninstall;
	CHECK(Nilesoft::TreatAsPlan::Parse(L"uninstall-ours", uninstall));
	CHECK(!Nilesoft::TreatAsPlan::RollbackRemovesTreatAs(uninstall, true));
}
