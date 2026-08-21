// R7 clean-VM regression fixture. The test host supplies a native submenu named
// "Regression Parent" whose child item is created only on WM_INITMENUPOPUP.
// Lazy discovery cannot see the child during the root pass; automatic
// LegacyEager discovery must make this rule behave as it did before 1.9.20.
settings
{
	modify.parent = true
}

modify(in="Regression Parent"
	find="Regression Nested Item"
	menu="")
