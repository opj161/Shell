// An identity that is still the same identity tomorrow.
//
// src/shared/MenuIdentity.h. docs/refactor/05-capabilities.md section 6 is the
// requirement - "persist identities, never session wIDs" - and the properties
// worth pinning are the ones whose opposite is silent: an identity that
// collapses two different items promotes the wrong one, and an identity that
// splits one item into two loses a pin without saying so.

#include "test.h"

#include <windows.h>
#include "..\shared\MenuIdentity.h"
#include "..\shared\ProviderQuarantine.h"

using namespace Nilesoft::Shell;
using namespace Nilesoft::Shell::MenuIdentity;

// CHECK_EQ renders both sides as long long, which is no use for text. This
// reports what came back and what was wanted, escaped, the way the framework
// already renders wide strings elsewhere.
#define CHECK_TEXT(actual_, want_)                                             \
	CHECK_MSG((actual_) == std::wstring(want_),                                \
			  ("got " + ::nss_test::escape(std::wstring(actual_).c_str())      \
			   + ", want " + ::nss_test::escape(want_)).c_str())

TEST(menu_identity, a_kind_and_a_signature_make_a_readable_identity)
{
	auto verb = make(Kind::Verb, L"{CAE3F1D4-7765-4D98-A060-52CD14D56EAB}");
	CHECK(verb.valid());
	CHECK_TEXT(verb.text, L"verb:{CAE3F1D4-7765-4D98-A060-52CD14D56EAB}");
	CHECK((int)verb.kind == (int)Kind::Verb);

	auto item = make(Kind::Item, L"tools/terminal");
	CHECK_TEXT(item.text, L"item:tools/terminal");
}

TEST(menu_identity, a_verb_signs_exactly_as_quarantine_names_it)
{
	// MenuIdentity formats a CLSID itself rather than calling
	// Quarantine::format_guid, because an identity header depending on the
	// quarantine header would have that dependency the wrong way round. The
	// duplication is paid down here instead of by a comment: if the two ever
	// disagree, a verb's favourite and its quarantine entry stop naming the
	// same extension, and nothing else in the tree would notice.
	GUID clsid{ 0xCAE3F1D4, 0x7765, 0x4D98, { 0xA0, 0x60, 0x52, 0xCD, 0x14, 0xD5, 0x6E, 0xAB } };

	CHECK_TEXT(verb_signature(clsid), Quarantine::format_guid(clsid).c_str());
	CHECK_TEXT(verb_signature(clsid), L"{CAE3F1D4-7765-4D98-A060-52CD14D56EAB}");

	// And an identity built from it round-trips as a CLSID the quarantine
	// parser accepts, which is what makes "the report names it, the command
	// takes it" true for favourites too.
	auto identity = make(Kind::Verb, verb_signature(clsid));
	CHECK(identity.valid());

	GUID back{};
	CHECK(Quarantine::parse_guid(identity.text.substr(5), back));
	CHECK(::IsEqualGUID(back, clsid) != 0);
}

TEST(menu_identity, the_kind_is_part_of_the_identity)
{
	// A configuration that adds its own "Delete" beside the shell's has two
	// items, not one. An identity scheme that ignored the kind would pin one
	// and promote the other, which is the failure nobody would be able to
	// describe.
	auto own = make(Kind::Item, L"delete");
	auto host = make(Kind::Native, L"delete");

	CHECK(own.valid() && host.valid());
	CHECK_MSG(own.hash != host.hash, "two kinds with one signature are two identities");
	CHECK(own.text != host.text);
}

TEST(menu_identity, matching_ignores_case_but_the_text_keeps_it)
{
	// A handler that retitles "Open With" as "Open with" between versions must
	// not lose its favourite. But `-favorites list` has to print something the
	// user recognises, so the fold happens in the hash and not in the text.
	auto shouted = make(Kind::Native, L"Open With");
	auto quiet = make(Kind::Native, L"open with");

	CHECK_EQ(shouted.hash, quiet.hash);
	CHECK_MSG(shouted.text != quiet.text, "the stored spelling is the one first seen");
	CHECK_TEXT(shouted.text, L"native:Open With");
}

TEST(menu_identity, the_fold_is_ascii_and_not_the_locale)
{
	// towlower under a Turkish locale maps 'I' to a dotless i, so an identity
	// written on one machine would hash differently on another. Only A-Z folds.
	auto a = make(Kind::Item, L"I");
	auto b = make(Kind::Item, L"i");
	CHECK_EQ(a.hash, b.hash);

	// A character outside ASCII is hashed as itself, both halves of it, so two
	// different ones cannot collide by being truncated to a byte.
	auto x = make(Kind::Item, L"\u00E9");	// e-acute
	auto y = make(Kind::Item, L"\u01E9");	// same low byte, different character
	CHECK_MSG(x.hash != y.hash, "both bytes of a wide character are hashed");
}

TEST(menu_identity, nothing_signs_as_nothing)
{
	CHECK(!make(Kind::Item, L"").valid());
	CHECK(!make(Kind::Unknown, L"whatever").valid());

	// The failure this guards: every untitled item on the machine sharing one
	// identity called "item:".
	auto empty = make(Kind::Item, L"");
	CHECK_TEXT(empty.text, L"");
}

TEST(menu_identity, a_signature_joins_a_path_and_a_title_the_way_the_menu_does)
{
	CHECK_TEXT(signature_of(L"view", L"Large icons"), L"view/Large icons");

	// A top-level item has no parent path and signs as its title alone -
	// not as "/Terminal", which would be a different identity for the same
	// item depending on which branch built it.
	CHECK_TEXT(signature_of(L"", L"Terminal"), L"Terminal");
	CHECK_TEXT(signature_of(L"/", L"Terminal"), L"Terminal");

	// The menu's own path strings are trimmed of slashes where they are built
	// (`.trim(L'/')`), and this has to agree with that or the identity would
	// not match the item it names.
	CHECK_TEXT(signature_of(L"/tools/", L"terminal"), L"tools/terminal");
}

TEST(menu_identity, a_signature_survives_a_null_where_a_string_should_be)
{
	/*
		The defect this exists for, found in a real Explorer rather than here.

		`Nilesoft::string::c_str()` returns nullptr when the string is empty
		(`return valid() ? m_data : nullptr`, System/Text/string.h), and
		std::wstring_view's pointer constructor calls char_traits::length on
		whatever it is handed. A top-level menu item has an empty `path`, so
		this is not an edge case - it is the first item of every menu, and with
		`settings { favorites = N }` set it faulted on all of them.

		The menu never appeared and Explorer stayed alive, which is the worst
		shape of failure: no crash to point at and nothing in any log.
	*/
	CHECK_TEXT(signature_of(nullptr, L"Refresh"), L"Refresh");
	CHECK_TEXT(signature_of(L"view", nullptr), L"view");
	CHECK_TEXT(signature_of(nullptr, nullptr), L"");

	// And the identity built from the first of those is the one a real
	// root-level native item gets.
	auto identity = make(Kind::Native, signature_of(nullptr, L"Refresh"));
	CHECK(identity.valid());
	CHECK_TEXT(identity.text, L"native:Refresh");
}

TEST(menu_identity, an_identity_survives_a_round_trip)
{
	auto original = make(Kind::Item, L"tools/terminal");
	auto back = parse(original.text);

	CHECK(back.valid());
	CHECK_EQ((int)back.kind, (int)original.kind);
	CHECK_TEXT(back.text, original.text.c_str());
	CHECK_EQ(back.hash, original.hash);
}

TEST(menu_identity, only_the_first_colon_separates_the_kind)
{
	// A title with a colon in it is ordinary - "C:\Windows" is a menu item on
	// plenty of machines. Splitting on the last colon, or on all of them,
	// would quietly rewrite those identities into something that matches
	// nothing.
	auto parsed = parse(L"item:open in C:\\Windows");
	CHECK(parsed.valid());
	CHECK_EQ((int)parsed.kind, (int)Kind::Item);
	CHECK_TEXT(parsed.text, L"item:open in C:\\Windows");
}

TEST(menu_identity, rubbish_is_refused_rather_than_guessed_at)
{
	CHECK(!parse(L"").valid());
	CHECK(!parse(L"no-colon-here").valid());
	CHECK(!parse(L"nosuchkind:whatever").valid());
	CHECK(!parse(L"item:").valid());
	CHECK(!parse(L":something").valid());
}

TEST(menu_identity, an_over_long_identity_is_refused_not_truncated)
{
	// The one failure this must not have. A truncated identity is a *different*
	// identity that looks like the one it came from, so it would silently match
	// some other item.
	std::wstring huge(MaxLength + 10, L'x');
	CHECK(!make(Kind::Item, huge).valid());
	CHECK(!parse(L"item:" + huge).valid());

	// And the boundary is not off by one in the direction that admits it.
	std::wstring just_under(MaxLength - 5, L'x');	// L"item:" is 5
	CHECK(make(Kind::Item, just_under).valid());
	CHECK(!make(Kind::Item, just_under + L"x").valid());
}

TEST(menu_identity, surrounding_whitespace_is_not_part_of_an_identity)
{
	// The favorites file puts the identity last on the line, so it arrives with
	// the line's trailing CR still attached.
	auto parsed = parse(L"  item:tools/terminal\r");
	CHECK(parsed.valid());
	CHECK_EQ(parsed.hash, make(Kind::Item, L"tools/terminal").hash);
}
