#include "test.h"

#include <Windows.h>
#include "Include/MenuModel.h"

using namespace Nilesoft::Shell;

namespace
{
	// Stands in for MenuItemInfo, which derives from MENUITEMINFOW and can hold
	// a live IExplorerCommand. None of that is what the table does, so the
	// suite drives the real code with the two fields it actually reads - the
	// same trick test_popup_lifecycle.cpp plays on PopupStack.
	struct Fake
	{
		int id{};
	};

	using Model = MenuModel<Fake>;
}

TEST(menu_model, a_fresh_model_owns_nothing)
{
	Model m;
	Fake f{ 1 };
	CHECK_EQ(size_t(0), m.size());
	CHECK(!m.owns(&f));
	CHECK(m.command(1) == nullptr);
}

TEST(menu_model, an_added_item_is_owned)
{
	Model m;
	Fake f{ 1 };
	m.add(&f, ItemOrigin::Custom, 4001, false);
	CHECK(m.owns(&f));
	CHECK_EQ(size_t(1), m.size());
}

TEST(menu_model, an_item_that_was_never_added_is_not_owned)
{
	Model m;
	Fake mine{ 1 }, theirs{ 2 };
	m.add(&mine, ItemOrigin::Custom, 4001, false);
	CHECK(!m.owns(&theirs));
}

// owns() is asked about a pointer read out of a borrowed popup's dwItemData,
// where the host may have put anything at all - including nothing.
//
// The add(nullptr) is what makes this test able to fail at all. Without it
// the property holds however many guards are deleted: nothing ever puts a
// null into the table, so looking one up finds nothing whatever owns()
// checks. Written the obvious way, this passed with *both* null guards
// removed - which is the shape docs/refactor/08-handoff.md section 1 rule 2
// exists to catch.
TEST(menu_model, a_null_pointer_is_never_owned)
{
	Model m;
	Fake f{ 1 };
	m.add(&f, ItemOrigin::Custom, 4001, false);
	m.add(nullptr, ItemOrigin::Custom, 4002, false);
	CHECK(!m.owns(nullptr));
}

TEST(menu_model, adding_nothing_adds_nothing)
{
	Model m;
	m.add(nullptr, ItemOrigin::Custom, 4001, false);
	CHECK_EQ(size_t(0), m.size());
	CHECK(m.command(4001) == nullptr);
}

TEST(menu_model, a_command_is_found_by_its_identifier)
{
	Model m;
	Fake a{ 1 }, b{ 2 };
	m.add(&a, ItemOrigin::Custom, 4001, false);
	m.add(&b, ItemOrigin::ExplorerCommand, 4002, false);
	CHECK(&a == m.command(4001));
	CHECK(&b == m.command(4002));
	CHECK(m.command(4003) == nullptr);
}

// Rule 1 in MenuModel.h. _items_command never held a dynamic popup, so an
// identifier lookup could not land on one; the popup flag is what carries that
// now, and dropping it would let a submenu be invoked as though it were a
// command.
TEST(menu_model, a_popup_is_never_a_command)
{
	Model m;
	Fake submenu{ 1 };
	m.add(&submenu, ItemOrigin::Custom, 4001, true);

	CHECK(m.owns(&submenu));			// it is still ours
	CHECK(m.command(4001) == nullptr);	// but it is not a command
}

TEST(menu_model, a_popup_does_not_shadow_a_real_command_sharing_its_identifier)
{
	Model m;
	Fake submenu{ 1 }, command{ 2 };
	m.add(&submenu, ItemOrigin::Custom, 4001, true);
	m.add(&command, ItemOrigin::Custom, 4001, false);
	CHECK(&command == m.command(4001));
}

// Rule 2. The forward scan this replaced stopped at the first hit, so a
// duplicated identifier resolved to the earlier item. An index built with
// operator[] would silently resolve to the later one instead.
TEST(menu_model, the_first_item_with_an_identifier_wins)
{
	Model m;
	Fake first{ 1 }, second{ 2 };
	m.add(&first, ItemOrigin::Custom, 4001, false);
	m.add(&second, ItemOrigin::Custom, 4001, false);
	CHECK(&first == m.command(4001));
}

TEST(menu_model, entries_are_kept_in_composition_order)
{
	Model m;
	Fake a{ 1 }, b{ 2 }, c{ 3 };
	m.add(&a, ItemOrigin::Custom, 4001, false);
	m.add(&b, ItemOrigin::Custom, 4002, true);
	m.add(&c, ItemOrigin::ExplorerCommand, 4003, false);

	CHECK_EQ(size_t(3), m.entries().size());
	CHECK(&a == m.entries()[0].item);
	CHECK(&b == m.entries()[1].item);
	CHECK(&c == m.entries()[2].item);
}

TEST(menu_model, an_entry_remembers_where_it_came_from)
{
	Model m;
	Fake rule{ 1 }, packaged{ 2 };
	m.add(&rule, ItemOrigin::Custom, 4001, false);
	m.add(&packaged, ItemOrigin::ExplorerCommand, 4002, false);

	CHECK(ItemOrigin::Custom == m.entries()[0].origin);
	CHECK(ItemOrigin::ExplorerCommand == m.entries()[1].origin);
}

TEST(menu_model, commandable_is_the_opposite_of_popup)
{
	Model m;
	Fake command{ 1 }, submenu{ 2 };
	m.add(&command, ItemOrigin::Custom, 4001, false);
	m.add(&submenu, ItemOrigin::Custom, 4002, true);

	CHECK(m.entries()[0].commandable());
	CHECK(!m.entries()[1].commandable());
}

TEST(menu_model, clearing_forgets_everything)
{
	Model m;
	Fake f{ 1 };
	m.add(&f, ItemOrigin::Custom, 4001, false);
	m.clear();

	CHECK_EQ(size_t(0), m.size());
	CHECK(!m.owns(&f));
	CHECK(m.command(4001) == nullptr);
}

// The composition site calls reserve once per menu and add many times, and a
// submenu's items arrive on a later call than the root's - so reserving again
// part-way through must not disturb what is already there. `_entries` growing
// is also what the identifier index stores positions into.
TEST(menu_model, reserving_again_keeps_what_is_already_there)
{
	Model m;
	Fake a{ 1 }, b{ 2 };
	m.add(&a, ItemOrigin::Custom, 4001, false);
	m.reserve(64);
	m.add(&b, ItemOrigin::Custom, 4002, false);

	CHECK(&a == m.command(4001));
	CHECK(&b == m.command(4002));
	CHECK(m.owns(&a));
	CHECK(m.owns(&b));
}
