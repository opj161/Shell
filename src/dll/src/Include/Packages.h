#pragma once

/*
	MSIX/AppX package identity, lookup and (lazily) localized display names.

	The documented shape of the NSS surface this backs:

		appx.id("WindowsTerminal")      Microsoft.WindowsTerminal_1.11.3471.0_x64__8wekyb3d8bbwe
		appx.family("WindowsTerminal")  Microsoft.WindowsTerminal_8wekyb3d8bbwe
		appx.version("WindowsTerminal") 1.11.3471.0
		appx.path("WindowsTerminal")    C:\Program Files\WindowsApps\Microsoft.WindowsTerminal_...
		appx.name("WindowsTerminal")    Windows Terminal

	`packageName` may be a full name or any part of one.

	Two things drove this being split out of Cache.h.

	Cost: answering package.exists() used to enumerate every package in the
	repository, open every subkey, read every DisplayName, and for each
	indirect name fall back to walking the whole MrtCache tree - all on the
	menu-building thread with the cache mutex held. Only package.name() needs
	any of that, so display names are now resolved on demand for one package.

	Correctness: the identity was parsed by splitting the full name on '_' with
	a splitter that collapses the empty ResourceId field, so the parse produced
	three fields where the code required four and left family, version and id
	empty. find() therefore never matched anything and the stock Windows
	Terminal item never appeared - after paying for the full scan to say no.
	The full name is now parsed against its documented layout:

		Name_Version_Architecture_ResourceId_PublisherId

	Install paths come from the documented AppModel API rather than from the
	repository key, which holds the full name and not a path:
	https://learn.microsoft.com/en-us/windows/win32/api/appmodel/nf-appmodel-getpackagepathbyfullname
*/

#include <windows.h>
#include <string>
#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		struct PackageIdentity
		{
			std::wstring full_name;		// appx.id
			std::wstring name;			// Microsoft.WindowsTerminal
			std::wstring version;		// appx.version
			std::wstring publisher;		// 8wekyb3d8bbwe
			std::wstring family;		// appx.family

			bool empty() const noexcept { return full_name.empty(); }
		};

		/*
			Does this full name answer to `query`?

			Case-insensitive substring, which is the documented NSS behaviour:
			"`packageName` may be a full name or any part of one". Declared here
			rather than written inline in the snapshot reader because the rule and
			the case-insensitive search it is built from belong together in
			Packages.cpp; PackagesCache::find_entry is what calls it.
		*/
		bool package_full_name_matches(const std::wstring &full_name, const wchar_t *query);

		// Parses Name_Version_Architecture_ResourceId_PublisherId. ResourceId is
		// empty for almost every package, which is exactly the field the previous
		// splitter dropped. Returns false for anything that is not a full name.
		bool parse_package_full_name(const std::wstring &full_name, PackageIdentity &out);

		// Where package data comes from: package repository registry for
		// identities, AppModel for paths, DisplayName/SHLoadIndirectString/
		// MrtCache for display names.
		class RegistryPackageSource
		{
		public:
			// Cheap: subkey names only, no per-package key is opened.
			bool enumerate_full_names(std::vector<std::wstring> &out);

			// Only ever called for a package that already matched. A path comes
			// from GetInstalledPackagePath below; this class forwarded to it
			// until PackageIndex, the only caller of that forwarder, was deleted.
			std::wstring resolve_display_name(const std::wstring &full_name);
		};

		// Documented two-call buffer sizing. Empty if the package is not installed
		// for this user or the API is unavailable.
		std::wstring GetInstalledPackagePath(const std::wstring &full_name);

		// Full names of every installed package in one family, using the
		// documented count/array/shared-buffer contract:
		// https://learn.microsoft.com/en-us/windows/win32/api/appmodel/nf-appmodel-getpackagesbypackagefamily
		std::vector<std::wstring> GetPackageFullNamesByFamily(const std::wstring &family);
	}
}
