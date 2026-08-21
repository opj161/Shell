#include "test.h"

#include <windows.h>
#include "..\dll\src\Include\ExplorerCommandCatalog.h"

using namespace Nilesoft::Shell;

TEST(explorer_command, star_matches_files_only)
{
	CHECK(explorer_command_type_matches(L"*", ExplorerCommandKind::File));
	CHECK(!explorer_command_type_matches(L"*", ExplorerCommandKind::Directory));
	CHECK(!explorer_command_type_matches(L"*", ExplorerCommandKind::DirectoryBackground));
	CHECK(!explorer_command_type_matches(L"*", ExplorerCommandKind::Drive));
}

TEST(explorer_command, directory_and_drive_types)
{
	CHECK(explorer_command_type_matches(L"Directory", ExplorerCommandKind::Directory));
	CHECK(explorer_command_type_matches(L"Directory\\Background", ExplorerCommandKind::DirectoryBackground));
	CHECK(explorer_command_type_matches(L"Directory/Background", ExplorerCommandKind::DirectoryBackground));
	CHECK(explorer_command_type_matches(L"Drive", ExplorerCommandKind::Drive));
	CHECK(!explorer_command_type_matches(L"Directory", ExplorerCommandKind::File));
}

TEST(explorer_command, leading_dot_type_matches_files)
{
	// desktop4:ItemType/@Type "must begin with a period or be the wildcard *"
	// https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-desktop4-itemtype
	CHECK(explorer_command_type_matches(L".zip", ExplorerCommandKind::File));
	CHECK(explorer_command_type_matches(L".7z", ExplorerCommandKind::File));
	CHECK(!explorer_command_type_matches(L".zip", ExplorerCommandKind::Directory));
}

TEST(explorer_command, parse_guid_accepts_braced_and_plain)
{
	GUID a{}, b{};
	CHECK(parse_guid(L"{CAE3F1D4-7765-4D98-A060-52CD14D56EAB}", a));
	CHECK(parse_guid(L"CAE3F1D4-7765-4D98-A060-52CD14D56EAB", b));
	CHECK(InlineIsEqualGUID(a, b));
}

TEST(explorer_command, parse_nanazip_style_manifest)
{
	const wchar_t *xml =
		L"<desktop4:Extension Category=\"windows.fileExplorerContextMenus\">"
		L"<desktop4:FileExplorerContextMenus>"
		L"<desktop4:ItemType Type=\"*\">"
		L"<desktop4:Verb Id=\"0000NanaZipShellExtension\" Clsid=\"CAE3F1D4-7765-4D98-A060-52CD14D56EAB\" />"
		L"</desktop4:ItemType>"
		L"<desktop5:ItemType Type=\"Directory\">"
		L"<desktop5:Verb Id=\"0000NanaZipShellExtension\" Clsid=\"CAE3F1D4-7765-4D98-A060-52CD14D56EAB\" />"
		L"</desktop5:ItemType>"
		L"<desktop10:ItemType Type=\"Drive\">"
		L"<desktop10:Verb Id=\"0000NanaZipShellExtension\" Clsid=\"CAE3F1D4-7765-4D98-A060-52CD14D56EAB\" />"
		L"</desktop10:ItemType>"
		L"</desktop4:FileExplorerContextMenus>"
		L"</desktop4:Extension>";

	std::vector<ExplorerCommandRegistration> regs;
	auto parsed = parse_file_explorer_context_menus(xml, regs);
	CHECK(parsed);
	if(!parsed || regs.empty())
		return;
	CHECK_EQ(regs.size(), size_t(1));
	CHECK_EQ(regs[0].types.size(), size_t(3));
	std::vector<ExplorerCommandKind> files{ ExplorerCommandKind::File };
	std::vector<ExplorerCommandKind> dirs{ ExplorerCommandKind::Directory };
	std::vector<ExplorerCommandKind> drives{ ExplorerCommandKind::Drive };
	std::vector<ExplorerCommandKind> bg{ ExplorerCommandKind::DirectoryBackground };
	CHECK(explorer_command_matches_any(regs[0], files));
	CHECK(explorer_command_matches_any(regs[0], dirs));
	CHECK(explorer_command_matches_any(regs[0], drives));
	CHECK(!explorer_command_matches_any(regs[0], bg));
}

TEST(explorer_command, type_attribute_is_not_itemtype_suffix)
{
	const wchar_t *xml =
		L"<FileExplorerContextMenus>"
		L"<ItemType Type=\"Directory\">"
		L"<Verb Clsid=\"{CAE3F1D4-7765-4D98-A060-52CD14D56EAB}\" />"
		L"</ItemType>"
		L"</FileExplorerContextMenus>";

	std::vector<ExplorerCommandRegistration> regs;
	auto parsed = parse_file_explorer_context_menus(xml, regs);
	CHECK(parsed);
	if(!parsed || regs.empty())
		return;
	CHECK_EQ(regs[0].types.size(), size_t(1));
	std::vector<ExplorerCommandKind> dirs{ ExplorerCommandKind::Directory };
	std::vector<ExplorerCommandKind> files{ ExplorerCommandKind::File };
	CHECK(explorer_command_matches_any(regs[0], dirs));
	CHECK(!explorer_command_matches_any(regs[0], files));
}

TEST(explorer_command, pretty_printed_type_with_spaces)
{
	const wchar_t *xml =
		L"<desktop4:FileExplorerContextMenus>\n"
		L"  <desktop4:ItemType Type = \"*\">\n"
		L"    <desktop4:Verb Clsid=\"CAE3F1D4-7765-4D98-A060-52CD14D56EAB\" />\n"
		L"  </desktop4:ItemType>\n"
		L"</desktop4:FileExplorerContextMenus>";

	std::vector<ExplorerCommandRegistration> regs;
	auto parsed = parse_file_explorer_context_menus(xml, regs);
	CHECK(parsed);
	if(!parsed || regs.empty())
		return;
	std::vector<ExplorerCommandKind> files{ ExplorerCommandKind::File };
	CHECK(explorer_command_matches_any(regs[0], files));
}

TEST(explorer_command, category_attribute_is_not_a_registration)
{
	std::vector<ExplorerCommandRegistration> regs;
	CHECK(!parse_file_explorer_context_menus(
		L"<desktop4:Extension Category=\"windows.fileExplorerContextMenus\"></desktop4:Extension>",
		regs));
	CHECK(regs.empty());
}

TEST(explorer_command, missing_section_is_not_a_registration)
{
	std::vector<ExplorerCommandRegistration> regs;
	CHECK(!parse_file_explorer_context_menus(L"<Package></Package>", regs));
	CHECK(regs.empty());
}

TEST(explorer_command, same_hash_and_type_is_already_represented)
{
	ExplorerCommandIdentity have;
	have.hash = 0x1234;
	have.type = 1;
	std::vector<ExplorerCommandIdentity> accepted{ have };

	ExplorerCommandIdentity candidate;
	candidate.hash = 0x1234;
	candidate.type = 1;
	CHECK(explorer_command_already_represented(candidate, accepted));
}

TEST(explorer_command, same_hash_different_type_is_not_a_duplicate)
{
	ExplorerCommandIdentity have;
	have.hash = 0x1234;
	have.type = 0;
	std::vector<ExplorerCommandIdentity> accepted{ have };

	ExplorerCommandIdentity candidate;
	candidate.hash = 0x1234;
	candidate.type = 1;
	CHECK(!explorer_command_already_represented(candidate, accepted));
}

TEST(explorer_command, clsid_match_skips_before_title)
{
	GUID clsid{};
	CHECK(parse_guid(L"{CAE3F1D4-7765-4D98-A060-52CD14D56EAB}", clsid));

	ExplorerCommandIdentity have;
	have.clsid = clsid;
	std::vector<ExplorerCommandIdentity> accepted{ have };

	ExplorerCommandIdentity candidate;
	candidate.clsid = clsid;
	CHECK(explorer_command_already_represented(candidate, accepted));
}

TEST(explorer_command, guid_null_is_not_an_identity)
{
	ExplorerCommandIdentity have;
	have.clsid = GUID_NULL;
	have.canonical = GUID_NULL;
	have.hash = 0;
	std::vector<ExplorerCommandIdentity> accepted{ have };

	ExplorerCommandIdentity candidate;
	candidate.clsid = GUID_NULL;
	candidate.canonical = GUID_NULL;
	CHECK(!explorer_command_already_represented(candidate, accepted));
}

TEST(explorer_command, canonical_guid_skips_when_usable)
{
	GUID canonical{};
	CHECK(parse_guid(L"{01234567-89AB-CDEF-0123-456789ABCDEF}", canonical));

	ExplorerCommandIdentity have;
	have.canonical = canonical;
	std::vector<ExplorerCommandIdentity> accepted{ have };

	ExplorerCommandIdentity candidate;
	candidate.canonical = canonical;
	CHECK(explorer_command_already_represented(candidate, accepted));
	CHECK(explorer_command_guid_usable(canonical));
	CHECK(!explorer_command_guid_usable(GUID_NULL));
}
