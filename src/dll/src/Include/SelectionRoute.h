#pragma once

/*
  Which provider is asked for the selection, and when the other one may answer.

  Step 5 of docs/refactor/04-code-health.md section 4 splits Selections into two
  providers - QuerySelectedFromShellBrowser, which walks up from the popup's
  window looking for an IShellBrowser and reads the selection off the active
  view, and QuerySelectedFromHandler, which reads the selection a host already
  handed Shell through IShellExtInit. This is the policy above them, kept
  separate from either because it is the part that can be reasoned about and
  tested without a window, a host or COM.

  The defect it exists to fix
  ---------------------------
  `Window.has_IShellBrowser` reads like a fact and is a *hypothesis*. Nothing
  queries an IShellBrowser to set it; QueryShellWindow sets it from the popup
  window's class hash alone - SHELLDLL_DefView, SysListView32, ShellTabWindowClass,
  SysTreeView32. Those are Explorer's classes, and they are also the classes of
  every third-party file manager that embeds the real shell view rather than
  writing its own.

  For such a host the hypothesis is wrong in the worst way: the window is
  classified as Explorer's, so the browser provider is asked, so the handler is
  never asked - and the host had *already handed Shell the selection* through
  IShellExtInit::Initialize. The menu is then composed against nothing. This is
  the same defect the browser provider's own comment describes one layer up
  ("third-party file managers only ever got theming"), which was fixed for hosts
  whose window class does not look like Explorer's and left in place for hosts
  whose window class does.

  Why only one failure lets the handler answer
  --------------------------------------------
  The browser provider can fail at about seven points, and they are not
  equivalent. Failing to *find* an IShellBrowser happens before it has read
  anything, so the selection state is untouched and the handler can answer into
  a clean slate. Every later failure happens after the provider may have called
  Parse, which appends to Items and sets the FSO type counters - and the handler
  appends too, so letting it answer there would merge two selections or count an
  item twice.

  So the rule is deliberately narrow: the handler answers only when no
  IShellBrowser was found at all. That covers the case above, which is the one
  that costs a user their menu, and it declines every case where "nothing was
  selected" is a real answer rather than a failed lookup.

  What this does not change
  -------------------------
  Explorer. Its browser lookup succeeds, so it never reaches the second
  provider, and its richer handling - DropTarget, Home, Quick access, Libraries -
  is untouched. Explorer also has no capture to fall back to under the default
  registration; a capture exists only for a host that called
  IShellExtInit::Initialize, and Shell registers itself as a per-filetype
  handler only in the opt-in CONTEXTMENU mode (RegistryConfig.h). In that mode
  Explorer can have one, and the rule still holds: it is consulted only when the
  browser lookup found nothing, where an exact selection beats an empty one.
*/

namespace Nilesoft
{
	namespace Shell
	{
		namespace SelectionRoute
		{
			enum class Provider
			{
				// Ask Explorer's view through IShellBrowser.
				ShellBrowser,

				// Read the selection the host handed us through IShellExtInit.
				Handler,

				// Nothing further to ask; the answer stands.
				None,
			};

			/*
				Which provider answers first.

				From the window classification alone, and deliberately not from
				whether a capture exists: preferring the capture whenever there
				is one would take Explorer down the handler path in CONTEXTMENU
				mode and silently bypass everything the browser provider does
				beyond reading a list of items.
			*/
			constexpr Provider first(bool window_looks_like_a_shell_view)
			{
				return window_looks_like_a_shell_view ? Provider::ShellBrowser
													  : Provider::Handler;
			}

			/*
				Which provider answers after the browser provider came back
				without a selection.

				`browser_was_found` is the discriminator because it is the one
				that says whether anything was read. False means the lookup
				itself failed - the window was classified as a shell view and
				no IShellBrowser answered - so nothing has been parsed and the
				handler may answer into a clean slate.

				True means a browser answered and the provider still produced
				nothing. That is a real "nothing is selected" as often as it is
				a failure, and by then Parse may have run, so the handler is not
				asked.
			*/
			constexpr Provider next_after_browser(bool browser_was_found)
			{
				return browser_was_found ? Provider::None : Provider::Handler;
			}
		}
	}
}
