// Expression-engine semantics, against the real evaluator.
//
// tests.vcxproj links the shipping Expression/*.cpp translation units, so these
// drive the same code the DLL runs rather than a reimplementation of it. That
// matters here: every defect below reads as ordinary code and was invisible to
// both the compiler and /analyze.
//
// Two shapes recur:
//   - Copy() protocols that inspect the *freshly constructed* object instead of
//     the one being copied, so they silently drop what they meant to clone;
//   - constants and operators that are simply the wrong value, which nothing
//     downstream can detect because the wrong value is still a valid one.

#include "test.h"

#include <pch.h>
#include "Expression\Constants.h"

#include <typeinfo>
#include <tuple>

using namespace Nilesoft;
using namespace Nilesoft::Shell;

// ---------------------------------------------------------------------------
// Test-local stubs.
//
// ImmersiveColor is defined in the DLL's entry translation unit (Main.cpp),
// which cannot be linked into a console test binary: it carries DllMain, the
// IAT hooks and the process-wide runtime state. Neither function has anything
// to do with expression semantics — theme colour lookup is not under test here.
// ---------------------------------------------------------------------------
namespace Nilesoft
{
	namespace Shell
	{
		uint32_t ImmersiveColor::GetColorByColorType(uint32_t) { return 0; }
		bool ImmersiveColor::IsSupported() { return false; }
	}
}

namespace
{
	// The evaluator needs a context; none of these tests touch selections,
	// windows or the registry, so a default one is enough.
	struct Eval
	{
		Context context;

		Object operator()(Expression *e) { return context.Eval(e).move(); }
	};

	// Builds `left < right` exactly as the parser would: a FuncExpression whose
	// Ident is IDENT_LESS with two argument expressions.
	FuncExpression *less(Expression *left, Expression *right)
	{
		Ident id(IDENT_LESS);
		auto f = new FuncExpression(id, nullptr);
		f->Arguments.push_back(left);
		f->Arguments.push_back(right);
		return f;
	}
}

// ---------------------------------------------------------------------------
// 1. Numeric `<` was computing `>`.
//
// FuncExpression.cpp case IDENT_LESS evaluated `arg0 > arg1` on the numeric
// branch while the string branch (comparing lengths) correctly used `<`. No
// operand swap anywhere compensated for it, so every numeric less-than in every
// .nss config produced the opposite answer.
// ---------------------------------------------------------------------------

TEST(expression, less_numeric_is_less_not_greater)
{
	Eval eval;

	CHECK_MSG(eval(less(new NumberExpression(1.0), new NumberExpression(2.0))).to_bool(),
			  "1 < 2 must be true");
	CHECK_MSG(!eval(less(new NumberExpression(2.0), new NumberExpression(1.0))).to_bool(),
			  "2 < 1 must be false");
	CHECK_MSG(!eval(less(new NumberExpression(2.0), new NumberExpression(2.0))).to_bool(),
			  "2 < 2 must be false");
}

TEST(expression, less_numeric_handles_negatives_and_fractions)
{
	Eval eval;

	CHECK(eval(less(new NumberExpression(-5.0), new NumberExpression(-1.0))).to_bool());
	CHECK(!eval(less(new NumberExpression(-1.0), new NumberExpression(-5.0))).to_bool());
	CHECK(eval(less(new NumberExpression(0.5), new NumberExpression(0.75))).to_bool());
}

TEST(expression, less_on_strings_still_compares_length)
{
	// The string branch is unchanged behaviour and is pinned so the numeric fix
	// cannot be "tidied" into changing it.
	Eval eval;

	CHECK(eval(less(new StringExpression(L"ab"), new StringExpression(L"abcd"))).to_bool());
	CHECK(!eval(less(new StringExpression(L"abcd"), new StringExpression(L"ab"))).to_bool());
}

// ---------------------------------------------------------------------------
// 2. TernaryExpression::Copy tested the fresh object's branches.
//
//     auto ternary = new TernaryExpression(Condition->Copy());
//     if(ternary->True)  ternary->True  = True->Copy();   // always null
//     if(ternary->False) ternary->False = False->Copy();  // always null
//
// The one-argument constructor leaves True/False null, so both guards were
// false and a cloned ternary lost both arms. It then evaluated whichever arm
// the condition chose as a null expression.
// ---------------------------------------------------------------------------

TEST(expression, ternary_copy_keeps_both_branches)
{
	auto original = new TernaryExpression(new NumberExpression(1.0),
										  new StringExpression(L"yes"),
										  new StringExpression(L"no"));

	auto clone = static_cast<TernaryExpression *>(original->Copy());

	CHECK_MSG(clone->Condition != nullptr, "condition must be cloned");
	CHECK_MSG(clone->True != nullptr, "true branch must be cloned");
	CHECK_MSG(clone->False != nullptr, "false branch must be cloned");

	// Deep copy, not aliasing: deleting the original must leave the clone intact.
	CHECK(clone->True != original->True);
	CHECK(clone->False != original->False);

	delete original;

	Eval eval;
	CHECK_MSG(eval(clone).to_string().equals(L"yes"), "cloned ternary must evaluate its true arm");

	delete clone;
}

TEST(expression, ternary_copy_evaluates_the_false_arm_too)
{
	auto original = new TernaryExpression(new NumberExpression(0.0),
										  new StringExpression(L"yes"),
										  new StringExpression(L"no"));
	auto clone = static_cast<TernaryExpression *>(original->Copy());
	delete original;

	Eval eval;
	CHECK_MSG(eval(clone).to_string().equals(L"no"), "cloned ternary must evaluate its false arm");

	delete clone;
}

TEST(expression, ternary_copy_tolerates_missing_branches)
{
	// The one-argument form is what the parser builds before it fills the arms;
	// copying it must not dereference null.
	auto partial = new TernaryExpression(new NumberExpression(1.0));
	auto clone = static_cast<TernaryExpression *>(partial->Copy());

	CHECK(clone->Condition != nullptr);
	CHECK(clone->True == nullptr);
	CHECK(clone->False == nullptr);

	delete partial;
	delete clone;
}

// ---------------------------------------------------------------------------
// 3. FuncExpression::Copy dropped Array unless there was a Child, and
//    dereferenced it unguarded inside that branch.
//
// Array is the subscript expression on the node itself (`sel.path[0]`), so it
// travels with the node, exactly as VariableExpression::Copy already did. The
// old code copied it only inside `if(Child)`, and did so as `Array->Copy()`
// with no null test — dropping the subscript for childless nodes and crashing
// for a child node that had none.
// ---------------------------------------------------------------------------

TEST(expression, func_copy_keeps_array_without_a_child)
{
	Ident id(IDENT_SEL);

	auto original = new FuncExpression(id, nullptr);
	original->Array = new NumberExpression(2.0);

	auto clone = static_cast<FuncExpression *>(original->Copy());

	CHECK_MSG(clone->Array != nullptr, "subscript must survive a copy with no child");
	CHECK_MSG(clone->Array != original->Array, "subscript must be deep-copied");
	CHECK(clone->Child == nullptr);

	delete original;
	delete clone;
}

TEST(expression, func_copy_without_array_does_not_dereference_null)
{
	// The crash case: a node with a Child but no Array reached `Array->Copy()`.
	Ident id(IDENT_SEL);

	auto original = new FuncExpression(id, nullptr);
	Ident child_id(IDENT_PATH);
	original->Child = new FuncExpression(child_id, original);
	original->Array = nullptr;

	auto clone = static_cast<FuncExpression *>(original->Copy());

	CHECK(clone->Child != nullptr);
	CHECK(clone->Array == nullptr);

	delete original;
	delete clone;
}

TEST(expression, func_copy_keeps_arguments_and_reparents_the_child)
{
	Ident id(IDENT_SEL);

	auto original = new FuncExpression(id, nullptr);
	original->Arguments.push_back(new NumberExpression(7.0));
	Ident child_id(IDENT_PATH);
	original->Child = new FuncExpression(child_id, original);
	original->Array = new NumberExpression(1.0);

	auto clone = static_cast<FuncExpression *>(original->Copy());

	CHECK_EQ(clone->Arguments.size(), original->Arguments.size());
	CHECK(clone->Arguments[0] != original->Arguments[0]);
	CHECK(clone->Array != nullptr);
	CHECK(clone->Child != nullptr);
	CHECK_MSG(clone->Child->Parent == clone, "the cloned child must point at the clone");
	CHECK(clone->Child->ischild);

	delete original;
	delete clone;
}

// ---------------------------------------------------------------------------
// 4. Array2Expression::Copy returned a NumberExpression.
//
// Both report ExpressionType::Literal, so nothing downstream could notice the
// node type had changed on copy. Only the dynamic type distinguishes them.
// ---------------------------------------------------------------------------

TEST(expression, array_literal_copy_preserves_the_node_type)
{
	auto original = new Array2Expression(Object(3.0));
	auto clone = original->Copy();

	CHECK_MSG(dynamic_cast<Array2Expression *>(clone) != nullptr,
			  "copying an array literal must yield an array literal");
	CHECK_MSG(dynamic_cast<NumberExpression *>(clone) == nullptr,
			  "copying an array literal must not yield a number literal");

	delete original;
	delete clone;
}

TEST(expression, number_literal_copy_still_yields_a_number)
{
	auto original = new NumberExpression(3.0);
	auto clone = original->Copy();

	CHECK(dynamic_cast<NumberExpression *>(clone) != nullptr);
	CHECK(dynamic_cast<Array2Expression *>(clone) == nullptr);

	delete original;
	delete clone;
}

// ---------------------------------------------------------------------------
// 5. msg.right mapped to IDNO.
//
// The repo's own reference defines msg.right as "The text is right-justified"
// (docs/functions/msg.html), i.e. MB_RIGHT. MSG_FLAGS values are passed straight
// into MessageBoxW's uType, so the old mapping turned `msg(msg.yesno|msg.right)`
// into MB_YESNO|7 — garbage flags rather than right-justified text.
//
// IDNO is 7 and MB_RIGHT is 0x00080000, so this is not a cosmetic difference.
// ---------------------------------------------------------------------------

TEST(expression, msg_right_maps_to_the_alignment_flag)
{
	auto found = false;
	uint32_t value = 0;

	for(const auto &c : MSG_FLAGS)
	{
		if(std::get<0>(c) == IDENT_MSG_RIGHT)
		{
			found = true;
			value = static_cast<uint32_t>(std::get<1>(c));
			break;
		}
	}

	CHECK_MSG(found, "msg.right must still be a known constant");
	CHECK_EQ(value, static_cast<uint32_t>(MB_RIGHT));
	CHECK_MSG(value != static_cast<uint32_t>(IDNO), "msg.right must not be the No button's result code");
}

TEST(expression, msg_button_results_are_unchanged)
{
	// Guard against "fixing" the neighbours: these really are result codes.
	auto value_of = [](uint32_t ident, uint32_t *out)
	{
		for(const auto &c : MSG_FLAGS)
			if(std::get<0>(c) == ident) { *out = static_cast<uint32_t>(std::get<1>(c)); return true; }
		return false;
	};

	uint32_t v = 0;
	CHECK(value_of(IDENT_MSG_IDOK, &v));  CHECK_EQ(v, static_cast<uint32_t>(IDOK));
	CHECK(value_of(IDENT_MSG_IDYES, &v)); CHECK_EQ(v, static_cast<uint32_t>(IDYES));
	CHECK(value_of(IDENT_MSG_IDNO, &v));  CHECK_EQ(v, static_cast<uint32_t>(IDNO));
	CHECK(value_of(IDENT_MSG_RTLREADING, &v)); CHECK_EQ(v, static_cast<uint32_t>(MB_RTLREADING));
}

// ---------------------------------------------------------------------------
// Object's two ways of being asked "is this true?", which disagree.
// ---------------------------------------------------------------------------

TEST(expression, an_objects_truth_is_to_bool_not_a_cast)
{
	// Object declares `explicit operator bool` meaning *not null*, and separately
	// a template conversion for numeric types that means *not zero*. Being
	// non-template, the explicit operator wins `static_cast<bool>` - so a
	// numeric zero casts to true.
	//
	// This is live: `settings { priority = 0 }` is read through exactly this
	// object, and reading it with a cast reports it as switched on. The hook in
	// Main.cpp is correct by accident, because it assigns to a bool - where the
	// explicit operator is not a candidate and the numeric template is chosen.
	// Initializer::check had to say which one it wanted.
	Object zero = 0;
	Object one = 1;

	CHECK(!zero.to_bool());
	CHECK(one.to_bool());

	// The trap itself, asserted rather than described, so anyone "simplifying"
	// to_bool() into a cast sees this fail rather than shipping it.
	CHECK_MSG(static_cast<bool>(zero),
			  "explicit operator bool means not-null; if this ever means "
			  "not-zero, to_bool() calls guarding numeric settings can be "
			  "simplified - until then they cannot");

	// A bool destination picks the numeric conversion, which is why the two
	// call sites that do this are right.
	bool assigned = zero;
	CHECK(!assigned);
}
