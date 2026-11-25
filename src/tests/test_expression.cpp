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

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <typeinfo>
#include <tuple>
#include <vector>

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

// ---------------------------------------------------------------------------
// Configuration text through the real parser, then the real evaluator.
//
// Everything below is a defect that needs an actual .nss to reach: a lexer loop,
// a number literal, a variable definition that refers to itself, a Shlwapi
// call in a path function. The text is written to a temporary file and parsed
// exactly as the DLL parses shell.nss; top-level `$name = ...` definitions are
// then evaluated by name.
// ---------------------------------------------------------------------------

namespace
{
	// Parser reads its application context off Initializer::instance. Leaked on
	// purpose, as in test_parser_imports.cpp: ~Initializer drags in the whole
	// menu engine and a process-lifetime singleton has nothing to tear down.
	void ensure_initializer()
	{
		static Initializer *one = new Initializer();
		(void)one;
	}

	// A directory holding one config file, removed with it.
	struct ScratchDir
	{
		std::wstring dir;

		ScratchDir()
		{
			wchar_t base[MAX_PATH]{};
			::GetTempPathW(MAX_PATH, base);

			// Long form: a runner's %TEMP% carries an 8.3 component, and the
			// paths below are compared with what Windows hands back.
			// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getlongpathnamew
			wchar_t longer[MAX_PATH]{};
			if(auto n = ::GetLongPathNameW(base, longer, MAX_PATH); n > 0 && n < MAX_PATH)
				::wcscpy_s(base, longer);

			static std::atomic<unsigned> serial{ 0 };
			wchar_t unique[MAX_PATH]{};
			::swprintf_s(unique, L"%snss_expr_%lu_%llu_%u\\", base, ::GetCurrentProcessId(),
						 (unsigned long long)::GetTickCount64(), serial++);
			dir = unique;
			::CreateDirectoryW(dir.c_str(), nullptr);
		}

		~ScratchDir()
		{
			// Whatever a test left behind: the config, and files the scripts wrote.
			WIN32_FIND_DATAW fd{};
			if(HANDLE h = ::FindFirstFileW((dir + L"*").c_str(), &fd); h != INVALID_HANDLE_VALUE)
			{
				do { ::DeleteFileW((dir + fd.cFileName).c_str()); } while(::FindNextFileW(h, &fd));
				::FindClose(h);
			}
			::RemoveDirectoryW(dir.c_str());
		}

		std::wstring write(const wchar_t *name, const std::string &utf8)
		{
			auto full = dir + name;
			HANDLE h = ::CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
									 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if(h != INVALID_HANDLE_VALUE)
			{
				DWORD wrote = 0;
				::WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wrote, nullptr);
				::CloseHandle(h);
			}
			return full;
		}

		// The directory as UTF-8, for pasting into a script.
		std::string utf8() const
		{
			std::string out;
			int n = ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), (int)dir.size(), nullptr, 0, nullptr, nullptr);
			if(n > 0)
			{
				out.resize(n);
				::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), (int)dir.size(), out.data(), n, nullptr, nullptr);
			}
			return out;
		}
	};

	struct Config
	{
		ScratchDir dir;
		CACHE cache;
		bool ok{};
		int error{};

		explicit Config(const std::string &text)
		{
			ensure_initializer();
			auto root = dir.write(L"c.nss", text);

			Parser parser{ string(root.c_str()) };
			parser.context.Cache = &cache;
			parser.context.variables.global = &cache.variables.global;
			parser.context.variables.runtime = &cache.variables.runtime;

			ok = parser.Load();
			error = (int)parser.Error();
		}

		// Evaluates the global variable `name` as a script would read `$name`.
		Object value(const wchar_t *name)
		{
			Context context;
			context.variables.global = &cache.variables.global;
			context.variables.runtime = &cache.variables.runtime;

			VariableExpression variable(Ident(Hash(name).value()), nullptr);
			return context.Eval(&variable).move();
		}

		std::wstring text(const wchar_t *name)
		{
			auto s = value(name).to_string();
			return s.empty() ? std::wstring() : std::wstring(s.c_str(), s.length());
		}
	};

	struct Outcome
	{
		std::atomic<bool> done{ false };
		bool ok{};
		int error{};
	};

	// A parse that never returns must fail the test, not hang the suite - which
	// is exactly what the defect did, on a loop with nothing to bound it. The
	// worker is detached; if it is stuck it is left spinning until the process
	// exits, which is the end of the run anyway.
	std::shared_ptr<Outcome> parse_with_timeout(const std::string &text, std::chrono::milliseconds limit)
	{
		auto outcome = std::make_shared<Outcome>();
		ensure_initializer();

		std::thread([outcome, text]
		{
			Config config(text);
			outcome->ok = config.ok;
			outcome->error = config.error;
			outcome->done = true;
		}).detach();

		auto deadline = std::chrono::steady_clock::now() + limit;
		while(!outcome->done && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return outcome;
	}

	double number(Object o) { return o.to_number<double>(); }
}

// ---------------------------------------------------------------------------
// 6. An unterminated string at end of file spun forever.
//
// parse_interpolated_string loops `while(tok != close)`. At end of input tok is
// 0, the inner loop leaves on skip() returning false, and the outer loop had no
// way out - so a config whose last line is `item(title='New` never finished
// parsing: on Explorer's UI thread for a first load, or on the watcher thread
// holding the reload lock for every other host.
// ---------------------------------------------------------------------------

TEST(expression, an_unclosed_single_quote_at_end_of_file_is_an_error)
{
	auto r = parse_with_timeout("item(title='New", std::chrono::milliseconds(5000));

	CHECK_MSG(r->done, "the parse must terminate");
	if(r->done)
	{
		CHECK(!r->ok);
		CHECK_EQ(r->error, (int)TokenError::CloseSingleQuoteExpected);
	}
}

TEST(expression, an_unclosed_interpolated_string_at_end_of_file_is_an_error)
{
	auto r = parse_with_timeout("item(title=`New", std::chrono::milliseconds(5000));

	CHECK_MSG(r->done, "the parse must terminate");
	if(r->done)
	{
		CHECK(!r->ok);
		CHECK_EQ(r->error, (int)TokenError::CloseSingleQuoteExpected);
	}
}

TEST(expression, a_closed_string_still_parses)
{
	// The exit added for end of input must not fire on a string that ends.
	Config c("$s = 'New item'\r\n$t = `x`\r\n");
	CHECK(c.ok);
	CHECK(c.text(L"s") == L"New item");
	CHECK(c.text(L"t") == L"x");
}

// ---------------------------------------------------------------------------
// 7. Number literals with more than nine fractional digits.
//
// The digit count was an int that was multiplied by ten per digit, so the tenth
// fractional digit overflowed it and the literal came out as a different number
// (0.1234567890 read as about 0.8755).
// ---------------------------------------------------------------------------

TEST(expression, a_long_fractional_literal_keeps_its_value)
{
	Config c("$a = 0.1234567890\r\n"
			 "$b = 0.12345678901234567890123\r\n"
			 "$c = 12345678901.5\r\n"
			 "$d = 1.5\r\n"
			 "$e = 0.000000000001\r\n");
	CHECK(c.ok);

	CHECK(std::fabs(number(c.value(L"a")) - 0.123456789) < 1e-12);
	CHECK(std::fabs(number(c.value(L"b")) - 0.12345678901234568) < 1e-15);
	CHECK(std::fabs(number(c.value(L"c")) - 12345678901.5) < 1e-6);
	CHECK(std::fabs(number(c.value(L"d")) - 1.5) < 1e-15);
	CHECK(std::fabs(number(c.value(L"e")) - 1e-12) < 1e-24);
}

// ---------------------------------------------------------------------------
// 8. A variable that refers to itself overflowed the stack.
//
// Variables are stored as unevaluated expressions, and a second definition
// replaces the first, so `$a = $a + 1` is a definition that evaluates itself.
// Nothing makes a stack overflow recoverable inside the host, so the evaluator
// has to notice rather than recurse.
// ---------------------------------------------------------------------------

TEST(expression, a_self_referencing_variable_evaluates_without_overflowing)
{
	Config c("$a = $a + 1\r\n");
	CHECK(c.ok);

	// Reaching the end of this call is the assertion; the number is incidental.
	(void)c.value(L"a");
	CHECK(true);
}

TEST(expression, mutually_recursive_variables_evaluate_to_null)
{
	Config c("$x = $y\r\n$y = $x\r\n");
	CHECK(c.ok);

	CHECK_MSG(c.value(L"x").is_null(), "$x = $y, $y = $x has no value");
	CHECK_MSG(c.value(L"y").is_null(), "and neither has the other end of it");
}

TEST(expression, a_chain_of_variables_still_evaluates)
{
	// The record of definitions in progress must be popped on the way out: a
	// variable used twice in one expression is two sequential reads, not a cycle.
	Config c("$a = 1\r\n$b = $a + 1\r\n$c = $b + $b\r\n$d = $c + $a\r\n");
	CHECK(c.ok);

	CHECK_EQ((int)number(c.value(L"a")), 1);
	CHECK_EQ((int)number(c.value(L"b")), 2);
	CHECK_EQ((int)number(c.value(L"c")), 4);
	CHECK_EQ((int)number(c.value(L"d")), 5);

	// And again, after the guard has been through every one of them.
	CHECK_EQ((int)number(c.value(L"d")), 5);
}

// ---------------------------------------------------------------------------
// 9. Path functions ran Shlwapi's in-place calls on the string's own buffer.
//
// PathAddExtensionW "You must set the size of this buffer to MAX_PATH"; the
// others document the same buffer. A string allocated for `a` has 16 characters,
// so a long extension wrote past it. A path that already fills MAX_PATH is
// returned unchanged now, which also makes the old behaviour - appending to it
// anyway - observable without a debug heap.
// ---------------------------------------------------------------------------

TEST(expression, path_add_extension_fits_a_long_extension)
{
	Config c("$a = path.add_extension('a', '.txt')\r\n"
			 "$b = path.add_extension('a', '.abcdefghijklmnopqrstuvwxyz')\r\n"
			 "$c = path.add_extension('a.doc', '.txt')\r\n");
	CHECK(c.ok);

	CHECK(c.text(L"a") == L"a.txt");
	CHECK(c.text(L"b") == L"a.abcdefghijklmnopqrstuvwxyz");
	CHECK_MSG(c.text(L"c") == L"a.doc", "an existing extension is kept");
}

TEST(expression, path_rename_extension_fits_a_long_extension)
{
	Config c("$a = path.rename_extension('a.txt', '.md')\r\n"
			 "$b = path.rename_extension('a.t', '.abcdefghijklmnopqrstuvwxyz')\r\n");
	CHECK(c.ok);

	CHECK(c.text(L"a") == L"a.md");
	CHECK(c.text(L"b") == L"a.abcdefghijklmnopqrstuvwxyz");
}

TEST(expression, path_quote_spaces_quotes_only_when_there_is_a_space)
{
	Config c("$a = path.quote_spaces('a b')\r\n"
			 "$b = path.quote_spaces('ab')\r\n"
			 "$c = path.quote_spaces('')\r\n");
	CHECK(c.ok);

	CHECK(c.text(L"a") == L"\"a b\"");
	CHECK(c.text(L"b") == L"ab");
	CHECK(c.text(L"c").empty());
}

TEST(expression, path_functions_leave_a_path_that_fills_max_path_unchanged)
{
	std::string long_path(MAX_PATH + 40, 'a');
	std::string spaced = std::string(MAX_PATH - 1, 'a') + " b";

	Config c("$a = path.add_extension('" + long_path + "', '.txt')\r\n"
			 "$b = path.rename_extension('" + long_path + ".t', '.txt')\r\n"
			 "$c = path.quote_spaces('" + spaced + "')\r\n");
	CHECK(c.ok);

	CHECK_MSG(c.text(L"a") == std::wstring(long_path.begin(), long_path.end()),
			  "nothing may be appended past a MAX_PATH buffer");
	CHECK(c.text(L"b") == std::wstring(long_path.begin(), long_path.end()) + L".t");
	CHECK(c.text(L"c") == std::wstring(spaced.begin(), spaced.end()));
}

// ---------------------------------------------------------------------------
// 10. reg.set('HKCU\...') with one argument did nothing.
//
// docs/functions/reg.html documents it as "Create Subkey". The code handed
// Registry::SetKeyValue no subkey and no value, which it rejects, so the call
// returned false and created nothing. A key that already holds a default value
// must keep it.
// ---------------------------------------------------------------------------

TEST(expression, reg_set_with_one_argument_creates_the_key)
{
	char leaf8[64]{};
	::sprintf_s(leaf8, "k%lu_%llu", ::GetCurrentProcessId(), (unsigned long long)::GetTickCount64());

	std::wstring parent = L"Software\\NilesoftShellTests";
	std::string key8 = std::string("Software\\NilesoftShellTests\\") + leaf8;
	std::wstring key(key8.begin(), key8.end());

	Config c("$r = reg.set('HKCU\\" + key8 + "')\r\n");
	CHECK(c.ok);

	CHECK_MSG(c.value(L"r").to_bool(), "reg.set must report that the key now exists");

	HKEY opened = nullptr;
	auto rc = ::RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ | KEY_WRITE, &opened);
	CHECK_EQ(rc, (LONG)ERROR_SUCCESS);

	if(opened)
	{
		// Calling it again must not disturb what the key holds.
		const wchar_t keep[] = L"keep";
		::RegSetValueExW(opened, nullptr, 0, REG_SZ, (const BYTE *)keep, sizeof(keep));
		::RegCloseKey(opened);

		CHECK(c.value(L"r").to_bool());

		wchar_t got[16]{};
		DWORD cb = sizeof(got) - sizeof(wchar_t);
		rc = ::RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, got, &cb);
		CHECK_EQ(rc, (LONG)ERROR_SUCCESS);
		CHECK_MSG(std::wstring(got) == L"keep", "an existing default value must survive");
	}

	::RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
	::RegDeleteKeyW(HKEY_CURRENT_USER, parent.c_str());
}

// ---------------------------------------------------------------------------
// 11. io.file.write emptied the file before looking at what to write.
//
// The file was opened with CREATE_ALWAYS, which truncates at once, and only then
// was the content expression evaluated. So content that turns out to be unset
// still destroyed what was there.
// ---------------------------------------------------------------------------

namespace
{
	std::string read_file(const std::wstring &path)
	{
		std::string out;
		HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
								 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if(h != INVALID_HANDLE_VALUE)
		{
			char buf[256];
			DWORD got = 0;
			while(::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got)
				out.append(buf, got);
			::CloseHandle(h);
		}
		return out;
	}
}

TEST(expression, io_file_write_with_unset_content_leaves_the_file_alone)
{
	ScratchDir scratch;
	auto target = scratch.write(L"keep.txt", "precious");
	std::string target8 = scratch.utf8() + "keep.txt";

	Config c("$w = io.file.write('" + target8 + "', $nothing)\r\n");
	CHECK(c.ok);

	(void)c.value(L"w");
	CHECK_MSG(read_file(target) == "precious", "an unset content argument must not truncate the file");
}

TEST(expression, io_file_write_still_writes_and_still_creates)
{
	ScratchDir scratch;
	std::string base = scratch.utf8();
	auto written = scratch.dir + L"written.txt";
	auto created = scratch.dir + L"created.txt";
	scratch.write(L"written.txt", "old contents that are longer");

	Config c("$w = io.file.write('" + base + "written.txt', 'new')\r\n"
			 "$c = io.file.create('" + base + "created.txt')\r\n");
	CHECK(c.ok);

	(void)c.value(L"w");
	(void)c.value(L"c");

	CHECK_MSG(read_file(written) == "new", "write replaces the contents");
	CHECK_MSG(::GetFileAttributesW(created.c_str()) != INVALID_FILE_ATTRIBUTES,
			  "create with no content still makes the file");
	CHECK(read_file(created).empty());
}
