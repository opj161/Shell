#include "test.h"

#include <Windows.h>
#include "Include/PopupLifecycle.h"

using namespace Nilesoft::Shell;

namespace
{
	// Stands in for ContextMenu::WND. Only its identity matters here.
	struct Fake
	{
		int id{};
	};

	using Stack = PopupStack<Fake>;

	HWND h(uintptr_t v) { return reinterpret_cast<HWND>(v); }
}

TEST(popup_lifecycle, a_fresh_stack_is_empty)
{
	Stack s;
	CHECK(s.empty());
	CHECK_EQ(size_t(0), s.size());
	CHECK(s.front() == nullptr);
	CHECK(s.back() == nullptr);
	CHECK(s.parent_of_top() == nullptr);
}

TEST(popup_lifecycle, the_first_create_is_tracked)
{
	Stack s;
	Fake root{ 1 };
	CHECK(PopupAction::Track == s.on_create(h(0x10), &root));
	CHECK_EQ(size_t(1), s.size());
	CHECK(&root == s.back());
}

TEST(popup_lifecycle, a_null_handle_is_never_tracked)
{
	Stack s;
	Fake f{ 1 };
	CHECK(PopupAction::Ignore == s.on_create(nullptr, &f));
	CHECK(s.empty());
}

/*
	The measured shapes (see Include/PopupLifecycle.h) did not produce a
	duplicate CREATE on this machine, but SetWinEventHook documents that
	callbacks may reenter. If one ever arrives, pushing again would leave the
	stack one entry too deep - so the root would stop answering "I am the root"
	and would be positioned against itself.
*/
TEST(popup_lifecycle, a_second_create_for_the_same_window_is_ignored)
{
	Stack s;
	Fake root{ 1 };
	CHECK(PopupAction::Track == s.on_create(h(0x10), &root));
	CHECK(PopupAction::Ignore == s.on_create(h(0x10), &root));
	CHECK_EQ(size_t(1), s.size());
	CHECK(s.parent_of_top() == nullptr);
}

TEST(popup_lifecycle, show_reaches_a_tracked_window_exactly_once)
{
	Stack s;
	Fake root{ 1 };
	s.on_create(h(0x10), &root);
	CHECK(PopupAction::Show == s.on_show(h(0x10)));
	CHECK(PopupAction::Ignore == s.on_show(h(0x10)));
}

TEST(popup_lifecycle, show_for_an_untracked_window_is_ignored)
{
	Stack s;
	Fake root{ 1 };
	s.on_create(h(0x10), &root);
	CHECK(PopupAction::Ignore == s.on_show(h(0x99)));
}

TEST(popup_lifecycle, the_root_has_no_parent_and_a_submenu_has_the_root)
{
	Stack s;
	Fake root{ 1 }, sub{ 2 };
	s.on_create(h(0x10), &root);
	CHECK(s.parent_of_top() == nullptr);

	s.on_create(h(0x20), &sub);
	CHECK(&root == s.parent_of_top());
	CHECK(&sub == s.back());
	CHECK(&root == s.front());
}

TEST(popup_lifecycle, destroying_the_top_leaves_the_rest_in_order)
{
	Stack s;
	Fake root{ 1 }, sub{ 2 }, deep{ 3 };
	s.on_create(h(0x10), &root);
	s.on_create(h(0x20), &sub);
	s.on_create(h(0x30), &deep);

	CHECK(PopupAction::Release == s.on_destroy(h(0x30)));
	CHECK_EQ(size_t(2), s.size());
	CHECK(&sub == s.back());
	CHECK(&root == s.parent_of_top());
}

/*
	The defect this file exists for.

	WM_NCDESTROY erased the WND from the map by handle and then popped the
	*last* entry off the level stack - two different keys for one removal. A
	destroy that is not for the top entry would leave the stack holding a
	pointer to a WND the map had already destroyed, and the next submenu
	placement dereferences exactly that pointer.

	Removing by handle makes the two agree by construction. Restoring the
	positional pop fails this test and `an_out_of_order_destroy_never_strands_an_entry`.
*/
TEST(popup_lifecycle, an_out_of_order_destroy_removes_the_window_it_names)
{
	Stack s;
	Fake root{ 1 }, sub{ 2 }, deep{ 3 };
	s.on_create(h(0x10), &root);
	s.on_create(h(0x20), &sub);
	s.on_create(h(0x30), &deep);

	// The middle popup goes away first.
	CHECK(PopupAction::Release == s.on_destroy(h(0x20)));
	CHECK_EQ(size_t(2), s.size());
	CHECK(&root == s.front());
	CHECK(&deep == s.back());
	CHECK(&root == s.parent_of_top());
	CHECK(Stack::npos == s.index_of(h(0x20)));
}

TEST(popup_lifecycle, an_out_of_order_destroy_never_strands_an_entry)
{
	Stack s;
	Fake root{ 1 }, sub{ 2 };
	s.on_create(h(0x10), &root);
	s.on_create(h(0x20), &sub);

	s.on_destroy(h(0x10));   // the root closes first
	s.on_destroy(h(0x20));

	CHECK(s.empty());
}

TEST(popup_lifecycle, destroying_a_window_we_never_tracked_changes_nothing)
{
	Stack s;
	Fake root{ 1 };
	s.on_create(h(0x10), &root);

	CHECK(PopupAction::Ignore == s.on_destroy(h(0x99)));
	CHECK_EQ(size_t(1), s.size());
	CHECK(&root == s.back());
}

TEST(popup_lifecycle, an_event_after_the_window_is_gone_is_ignored)
{
	Stack s;
	Fake root{ 1 };
	s.on_create(h(0x10), &root);
	s.on_destroy(h(0x10));

	CHECK(PopupAction::Ignore == s.on_show(h(0x10)));
	CHECK(PopupAction::Ignore == s.on_destroy(h(0x10)));
	CHECK(s.empty());
}

/*
	The sibling walk the probe measured: create B, destroy B, create C, destroy
	C, create D, destroy D, all above a root that outlives them. Creation and
	destruction interleave, so at no point is more than one submenu open - the
	stack must return to depth one each time rather than growing.
*/
TEST(popup_lifecycle, a_sibling_walk_returns_to_the_root_each_time)
{
	Stack s;
	Fake root{ 0 }, b{ 1 }, c{ 2 }, d{ 3 };
	s.on_create(h(0x10), &root);
	s.on_show(h(0x10));

	Fake *subs[] = { &b, &c, &d };
	uintptr_t handles[] = { 0x21, 0x22, 0x23 };
	for(int i = 0; i < 3; i++)
	{
		CHECK(PopupAction::Track == s.on_create(h(handles[i]), subs[i]));
		CHECK(PopupAction::Show == s.on_show(h(handles[i])));
		CHECK_EQ(size_t(2), s.size());
		CHECK(&root == s.parent_of_top());
		CHECK(PopupAction::Release == s.on_destroy(h(handles[i])));
		CHECK_EQ(size_t(1), s.size());
	}

	CHECK(PopupAction::Release == s.on_destroy(h(0x10)));
	CHECK(s.empty());
}

TEST(popup_lifecycle, iteration_visits_payloads_root_first)
{
	Stack s;
	Fake root{ 1 }, sub{ 2 }, deep{ 3 };
	s.on_create(h(0x10), &root);
	s.on_create(h(0x20), &sub);
	s.on_create(h(0x30), &deep);

	int seen[3]{};
	int at = 0;
	s.for_each([&](Fake *f) { if(at < 3) seen[at++] = f->id; });

	CHECK_EQ(3, at);
	CHECK_EQ(1, seen[0]);
	CHECK_EQ(2, seen[1]);
	CHECK_EQ(3, seen[2]);
}

TEST(popup_lifecycle, a_null_payload_is_skipped_by_iteration_but_still_holds_its_place)
{
	Stack s;
	Fake root{ 1 }, deep{ 3 };
	s.on_create(h(0x10), &root);
	s.on_create(h(0x20), nullptr);
	s.on_create(h(0x30), &deep);

	int count = 0;
	s.for_each([&](Fake *) { count++; });
	CHECK_EQ(2, count);

	// The place is still held: the top's parent is the null entry, not the root.
	CHECK_EQ(size_t(3), s.size());
	CHECK(s.parent_of_top() == nullptr);
}

TEST(popup_lifecycle, clear_drops_everything)
{
	Stack s;
	Fake root{ 1 }, sub{ 2 };
	s.on_create(h(0x10), &root);
	s.on_create(h(0x20), &sub);
	s.clear();
	CHECK(s.empty());
	CHECK(PopupAction::Ignore == s.on_show(h(0x10)));
}

TEST(popup_lifecycle, at_returns_null_past_the_end)
{
	Stack s;
	Fake root{ 1 };
	s.on_create(h(0x10), &root);
	CHECK(&root == s.at(0));
	CHECK(s.at(1) == nullptr);
	CHECK(s.at(Stack::npos) == nullptr);
}
