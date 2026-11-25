#pragma once

/*
	The presenter, as a named boundary.

	Seam step 7 of docs/refactor/04-code-health.md section 4, second half, and
	docs/refactor/09-remediation-plan.md R4. The first half split the paint code
	into its own translation unit and deliberately left it as `ContextMenu::`
	members, with the reason written down: naming a class means giving it an
	interface onto ContextMenu's private state, "and that interface cannot be
	designed honestly until the dependency is visible."

	Splitting the file made it visible. Measured against MenuPresenter.cpp at
	`450985f`, what the paint code reaches for is:

		_theme        157      msg              4
		symbol         10      _tip             3
		composition     8      _level           3
		font            7      _items           3
		dpi            7      _hTheme          3
		current         6      ident            2
		_hbackground    5      hwnd             1
		                       _screenshot      1
		                       _menus           1

	Fifteen members, dominated by one. That list is this struct, and the fact
	that it fits on a screen is the argument the first half was waiting for.

	Why a struct of references rather than an interface
	---------------------------------------------------

	A virtual interface would put a vtable dispatch on the item-drawing path -
	`_theme` alone is read 157 times per paint - to buy a substitutability
	nothing needs: there is one presenter and one context, and the alternative
	implementation this would allow does not exist and is not planned. The
	repository has already refused a vtable once for the same reason
	(`PopupInterceptionBackend`, docs/refactor/01-takeover-contract.md 9c).

	References rather than pointers, because every one of them is a member of a
	live ContextMenu for the whole lifetime of the presenter that holds them:
	the presenter is constructed by the ContextMenu and cannot outlive it. A
	pointer would invite a null check that can never fire.

	What this does not change
	-------------------------

	Nothing about how a menu draws. The bodies moved with the accessor
	substitution and nothing else, which is section 04.4's rule for every seam -
	"move code, don't improve it in the same commit" - and the gate is the four
	`render.*` scenarios in src/tests/hostprobe, which read the live menu back
	through MSAA and assert item order, layout containment and submenu
	placement. Those are exactly the regressions this class could introduce and
	that no other test in the tree would notice.
*/

namespace Nilesoft
{
	namespace Shell
	{
		class ContextMenu;

		/*
			Everything the paint path reaches for, and nothing else.

			The value of the list is that it is closed: a paint function that
			starts needing something new has to add it here, in front of a
			reviewer, rather than reaching through `this` for whatever happens to
			be in scope. That is the whole difference between a translation unit
			and a boundary.
		*/
		struct PresenterContext
		{
			Theme &theme;
			ContextMenu::Composition &composition;
			ContextMenu::FontSet &font;
			ContextMenu::Symbols &symbol;
			ContextMenu::Current &current;
			ContextMenu::ID &ident;
			ContextMenu::Windows &hwnd;

			DPI &dpi;
			MESSAGE &msg;
			Tip &tip;

			HTHEME &hTheme;
			HBRUSH &hbackground;

			PopupStack<WND> &level;
			std::vector<MenuItemInfo *> &items;
			std::unordered_map<HMENU, ContextMenu::menu_t> &menus;

			const string &screenshot;
		};

		/*
			Turns a composed menu item into pixels.

			One per ContextMenu, constructed with a context that points at that
			ContextMenu's members. Holds no state of its own on purpose: two
			presenters over one menu would be a bug, and giving this class
			members is how that bug becomes possible.
		*/
		class Win32MenuPresenter
		{
		public:
			explicit Win32MenuPresenter(const PresenterContext &context) noexcept
				: _ctx(context) {}

			Win32MenuPresenter(const Win32MenuPresenter &) = delete;
			Win32MenuPresenter &operator=(const Win32MenuPresenter &) = delete;

			void draw_string(HDC hdc, HFONT hFont, const Rect *rc, const Color &color,
							 const wchar_t *text, int length = -1, DWORD format = 0,
							 bool disable_BufferedPaint = false);

			static void draw_rect(DC *dc, const POINT &pt, const SIZE &size,
								  const Color &color, const Color &border = {},
								  int radius = 0);

			LRESULT OnDrawItem(DRAWITEMSTRUCT *di);
			LRESULT OnMeasureItem(MEASUREITEMSTRUCT *mi);

			void screenshot();
			bool draw_layer(WND *wnd, SIZE size, int margin);
			void UpdateLayered(WND *wnd, bool update_blurry = false);
			bool CreateLayer(WND *wnd);

		private:
			const PresenterContext &_ctx;
		};
	}
}
