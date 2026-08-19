#include "test.h"

// Usage: tests.exe [suite-substring]
int main(int argc, char **argv)
{
	return ::nss_test::run(argc > 1 ? argv[1] : nullptr);
}