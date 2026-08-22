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
