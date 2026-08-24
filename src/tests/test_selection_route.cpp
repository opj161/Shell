// Which provider is asked for the selection, and when the other may answer.
//
// src/dll/src/Include/SelectionRoute.h holds the reasoning. What is worth
// pinning here is that the two rules stay *asymmetric*, because the obvious
// symmetric versions of either are both wrong and both look tidier:
//
//   - "prefer the capture whenever there is one" would take Explorer down the
//     handler path in CONTEXTMENU mode and silently bypass everything the
//     browser provider does beyond listing items;
//   - "fall back to the handler whenever the browser produced no selection"
//     would let the handler append into a selection Parse had already started
//     filling, merging two selections or counting an item twice.
//
// Neither mistake fails anything else in the tree: the first shows up as
// Explorer quietly losing its Home/Quick-access handling, the second as a menu
// that occasionally reports the wrong number of items.

#include "test.h"

#include "Include/SelectionRoute.h"

using Nilesoft::Shell::SelectionRoute::Provider;
using Nilesoft::Shell::SelectionRoute::first;
using Nilesoft::Shell::SelectionRoute::next_after_browser;

/*
	A window whose class is one of Explorer's goes to the browser provider,
	whatever else is true. This is the rule that keeps Explorer's richer
	handling reachable, and it is why the first gate reads the classification
	rather than the capture.
*/
TEST(selection_route, a_shell_view_asks_the_browser_first)
{
	CHECK(first(true) == Provider::ShellBrowser);
}

/*
	The case that was already fixed one layer up, kept here so the two halves
	of the rule sit together: a host with its own view answers nothing to
	WM_GETISHELLBROWSER, and has usually already handed us the selection.
*/
TEST(selection_route, a_window_that_is_not_a_shell_view_asks_the_handler)
{
	CHECK(first(false) == Provider::Handler);
}

/*
	The defect this seam exists to fix.

	`has_IShellBrowser` is set from the popup window's class hash alone, so a
	file manager that embeds the real shell view is classified as Explorer and
	sent to the browser provider - which finds no IShellBrowser, because the
	host is not Explorer. Before this rule that ended the query, and the menu
	was composed against nothing despite the host having handed Shell an exact
	selection through IShellExtInit.
*/
TEST(selection_route, no_browser_found_lets_the_handler_answer)
{
	CHECK(next_after_browser(false) == Provider::Handler);
}

/*
	And the case that must NOT fall through, which is the whole reason the
	discriminator is "was a browser found" rather than "was anything selected".

	A browser that answered and yielded nothing is usually telling the truth -
	nothing is selected - and by then the provider may have called Parse, which
	appends to Items and sets the FSO type counters. Asking the handler on top
	of that merges two selections.
*/
TEST(selection_route, a_browser_that_answered_ends_the_query)
{
	CHECK(next_after_browser(true) == Provider::None);
}

// Stated as its own test because it is the asymmetry itself, and a
// "simplification" that made the two rules agree would pass every test above
// while reintroducing one of the two defects in the file header.
TEST(selection_route, the_two_rules_are_not_the_same_rule)
{
	// Same input, opposite meaning: for `first` a shell view means "ask the
	// browser", for `next_after_browser` a browser that was found means "stop".
	CHECK(first(true) == Provider::ShellBrowser);
	CHECK(next_after_browser(true) == Provider::None);

	// And the handler is reachable from both, by opposite conditions.
	CHECK(first(false) == Provider::Handler);
	CHECK(next_after_browser(false) == Provider::Handler);
}
