settings
{
	priority=1

	// Explorer, plus any host that invoked us as a registered context-menu
	// handler. This used to be !process.is_explorer alone, which meant the menu
	// was excluded in every other process: ContextMenu::Initialize bails at
	// is_excluded() before building anything, so a third-party file manager fell
	// straight through to its own menu no matter what else worked.
	//
	// window.is_contextmenuhandler is the capability, not an application name -
	// it is true exactly when the host went through our IContextMenu, which is
	// what makes this work for Everything, XYplorer, Total Commander and anything
	// else honouring ContextMenuHandlers, with no per-application list.
	exclude.where = !(process.is_explorer or window.is_contextmenuhandler)

	// showdelay sets SPI_SETMENUSHOWDELAY, the delay before a submenu opens on
	// hover. That is a per-user system setting, and setting it here overrides
	// whatever the user chose, for the whole time a menu is open.
	// Left unset, so the user's own setting applies. Uncomment to override:
	// showdelay = 200

	// Options to allow modification of system items
	modify.remove.duplicate=1
	tip.enabled=true
}

// localization
// Absolute, not relative. An import path is rooted against the importing file's
// own directory, but that happens after the expression has been evaluated, so
// the path.exists() below is tested as-is. A relative path there is resolved
// against the host process working directory - Explorer's, not the install
// folder - so it never matched and every locale silently fell back to en.nss.
$loc_path = app.dir + '\imports\lang\'
import lang loc_path + "en.nss"

// Full tag first, then the bare language, then English. sys.lang is
// GetUserDefaultLocaleName, so it returns a full tag like ja-JP or it-IT - but
// ten of the fifteen files shipped in imports\lang are named for the language
// alone (ar, it, ja, ko, no, ro, ru, sl, tr, ua). Matching only the full tag
// meant those ten could never be selected by anyone. sys.lang.name is the
// ISO 639 code, which is what they are named after.
import lang if(path.exists(loc_path + sys.lang + ".nss"),
               loc_path + sys.lang + ".nss",
               if(path.exists(loc_path + sys.lang.name + ".nss"),
                  loc_path + sys.lang.name + ".nss",
                  loc_path + "en.nss"))

// or import lang 'imports/lang/en.nss'

import 'imports/theme.nss'
import 'imports/images.nss'
import 'imports/modify.nss'

menu(mode="multiple" title=loc.pin_unpin image=icon.pin)
{
}

menu(mode="multiple" title=title.more_options image=icon.more_options)
{
}

import 'imports/terminal.nss'
import 'imports/file-manage.nss'
import 'imports/develop.nss'
import 'imports/goto.nss'
import 'imports/taskbar.nss'
