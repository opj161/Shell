#include "test.h"

#include <windows.h>

#include "../dll/src/Parser/IdentHash.h"
#include <FileSystemObjects.h>

using namespace Nilesoft::Shell;

namespace
{
	void check_state(const FileSystemObjects &value)
	{
		CHECK_EQ(value.Types[FSO_FILE], int32_t(TRUE));
		CHECK_EQ(value.Types[FSO_DIRECTORY], FileSystemObjects::EXCLUDE);
		CHECK_EQ(value.count, 2);
		CHECK_EQ(value.exclude, 1);
		CHECK(value.defined);
		CHECK(value.any_types);
	}
}

TEST(fso, copy_assignment_preserves_every_selection_field)
{
	FileSystemObjects source;
	source.Types[FSO_FILE] = TRUE;
	source.Types[FSO_DIRECTORY] = FileSystemObjects::EXCLUDE;
	source.count = 2;
	source.exclude = 1;
	source.defined = true;
	source.any_types = true;

	FileSystemObjects copy;
	copy = source;
	check_state(copy);
}

TEST(fso, self_assignment_preserves_selection_matching_state)
{
	FileSystemObjects value;
	value.Types[FSO_FILE] = TRUE;
	value.Types[FSO_DIRECTORY] = FileSystemObjects::EXCLUDE;
	value.count = 2;
	value.exclude = 1;
	value.defined = true;
	value.any_types = true;

	value = value;
	check_state(value);
}

TEST(fso, set_uses_boolean_elements_not_byte_fill_patterns)
{
	FileSystemObjects value;
	value.set(TRUE);
	for(int32_t i = 0; i < FSO_SIZE; ++i)
		CHECK_EQ(value.Types[i], int32_t(TRUE));
	CHECK_EQ(value.Types[FSO_COUNT], FSO_SIZE - 1);

	value.set(FALSE);
	for(auto type : value.Types)
		CHECK_EQ(type, int32_t(FALSE));
}

TEST(fso, every_supported_background_subtype_reaches_the_parser_switch)
{
	CHECK(FileSystemObjects::is_type_back(IDENT_TYPE_DIR));
	CHECK(FileSystemObjects::is_type_back(IDENT_TYPE_DRIVE));
	CHECK(FileSystemObjects::is_type_back(IDENT_TYPE_REMOTE));
	CHECK(FileSystemObjects::is_type_back(IDENT_TYPE_NAMESPACE));
	CHECK(FileSystemObjects::is_type_back(IDENT_TYPE_COMPUTER));
	CHECK(FileSystemObjects::is_type_back(IDENT_TYPE_RECYCLEBIN));
}
