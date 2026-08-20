#include <windows.h>
#include <shlwapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include "test.h"

namespace
{
	struct ImportGraph
	{
		static constexpr size_t MAX_IMPORT_DEPTH = 32;

		enum class ImportResult
		{
			Success,
			SelfCycle,
			MutualCycle,
			MaxDepthExceeded
		};

		static std::wstring to_lower(std::wstring s)
		{
			std::transform(s.begin(), s.end(), s.begin(), ::towlower);
			return s;
		}

		static bool is_absolute(const std::wstring &path)
		{
			if(path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
				return true;
			if(path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/'))
				return true;
			return false;
		}

		static std::wstring normalize_import_path(const std::wstring &base_dir, const std::wstring &import_path)
		{
			std::wstring path = import_path;
			std::replace(path.begin(), path.end(), L'/', L'\\');
			wchar_t combined[MAX_PATH]{};
			if(is_absolute(path))
			{
				::PathCanonicalizeW(combined, path.c_str());
			}
			else
			{
				wchar_t joined[MAX_PATH]{};
				::PathCombineW(joined, base_dir.c_str(), path.c_str());
				::PathCanonicalizeW(combined, joined);
			}
			return to_lower(combined);
		}

		ImportResult can_import(const std::vector<std::wstring> &chain, const std::wstring &next_file)
		{
			if(chain.size() >= MAX_IMPORT_DEPTH)
				return ImportResult::MaxDepthExceeded;

			auto target = to_lower(next_file);

			for(size_t i = 0; i < chain.size(); i++)
			{
				if(to_lower(chain[i]) == target)
				{
					if(i == chain.size() - 1)
						return ImportResult::SelfCycle;
					else
						return ImportResult::MutualCycle;
				}
			}

			return ImportResult::Success;
		}
	};
}

TEST(parser, import_path_normalization)
{
	// Absolute paths with forward and backward slashes
	CHECK(ImportGraph::is_absolute(L"C:/folder/file.nss"));
	CHECK(ImportGraph::is_absolute(L"C:\\folder\\file.nss"));
	CHECK(ImportGraph::is_absolute(L"\\\\server\\share\\file.nss"));

	// Relative paths
	CHECK(!ImportGraph::is_absolute(L"imports/theme.nss"));
	CHECK(!ImportGraph::is_absolute(L"./imports/theme.nss"));
	CHECK(!ImportGraph::is_absolute(L"../lang/en.nss"));

	// Combination and canonicalization
	auto base = L"C:\\Program Files\\Shell";
	auto combined = ImportGraph::normalize_import_path(base, L"imports/../imports/theme.nss");
	CHECK(combined.find(L"theme.nss") != std::wstring::npos);
	CHECK(combined.find(L"..") == std::wstring::npos);
}

TEST(parser, import_cycle_detection)
{
	ImportGraph graph;

	// 1. Self cycle (A -> A)
	std::vector<std::wstring> chain1 = { L"C:\\shell\\main.nss" };
	CHECK(graph.can_import(chain1, L"C:\\shell\\main.nss") == ImportGraph::ImportResult::SelfCycle);

	// Case-insensitive self-cycle
	CHECK(graph.can_import(chain1, L"c:\\SHELL\\MAIN.NSS") == ImportGraph::ImportResult::SelfCycle);

	// 2. Mutual cycle (A -> B -> A)
	std::vector<std::wstring> chain2 = { L"C:\\shell\\a.nss", L"C:\\shell\\b.nss" };
	CHECK(graph.can_import(chain2, L"C:\\shell\\a.nss") == ImportGraph::ImportResult::MutualCycle);

	// 3. Three-node cycle (A -> B -> C -> A)
	std::vector<std::wstring> chain3 = { L"C:\\shell\\a.nss", L"C:\\shell\\b.nss", L"C:\\shell\\c.nss" };
	CHECK(graph.can_import(chain3, L"C:\\shell\\a.nss") == ImportGraph::ImportResult::MutualCycle);

	// 4. Valid non-cyclic DAG (A -> B, A -> C, B -> D, C -> D is allowed in separate branches)
	std::vector<std::wstring> branch1 = { L"C:\\shell\\a.nss", L"C:\\shell\\b.nss" };
	CHECK(graph.can_import(branch1, L"C:\\shell\\d.nss") == ImportGraph::ImportResult::Success);

	std::vector<std::wstring> branch2 = { L"C:\\shell\\a.nss", L"C:\\shell\\c.nss" };
	CHECK(graph.can_import(branch2, L"C:\\shell\\d.nss") == ImportGraph::ImportResult::Success);

	// 5. Depth limits (depth <= 32 accepted, depth > 32 rejected)
	std::vector<std::wstring> deep_chain;
	for(int i = 0; i < 32; i++)
	{
		wchar_t buf[64];
		swprintf_s(buf, L"C:\\shell\\file%d.nss", i);
		deep_chain.push_back(buf);
	}
	CHECK(graph.can_import(deep_chain, L"C:\\shell\\file33.nss") == ImportGraph::ImportResult::MaxDepthExceeded);
}
