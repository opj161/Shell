#pragma once

#include <windows.h>

#include <string>

// TreatAs CustomActionData is planned immediately and consumed later by the
// deferred apply and rollback actions. Those actions cannot set installer
// properties for each other, so "this invocation created TreatAs" is a marker
// file whose unique path is encoded here at planning time.
//
//   noop
//   uninstall-ours
//   install-absent|<absolute marker path>
//
//   https://learn.microsoft.com/windows/win32/msi/obtaining-context-information-for-deferred-execution-custom-actions
//   https://learn.microsoft.com/windows/win32/msi/deferred-execution-custom-actions
//   https://learn.microsoft.com/windows/win32/msi/rollback-custom-actions
namespace Nilesoft::TreatAsPlan
{
	enum class Op
	{
		noop,
		install_absent,
		uninstall_ours
	};

	struct Plan
	{
		Op op = Op::noop;
		std::wstring marker;
	};

	inline constexpr wchar_t kInstallAbsent[] = L"install-absent";
	inline constexpr wchar_t kUninstallOurs[] = L"uninstall-ours";
	inline constexpr wchar_t kNoop[] = L"noop";
	inline constexpr wchar_t kStagingInfix[] = L"\\Nilesoft\\Shell\\Staging\\";
	inline constexpr wchar_t kMarkerPrefix[] = L"treatas.";
	inline constexpr wchar_t kMarkerSuffix[] = L".created";
	inline constexpr size_t kMarkerTokenChars = 32;

	inline bool MarkerFileNameLooksOwned(const wchar_t *name)
	{
		if(!name)
			return false;

		const size_t prefix_len = 8; // treatas.
		const size_t suffix_len = 8; // .created
		const size_t n = ::lstrlenW(name);
		if(n != prefix_len + kMarkerTokenChars + suffix_len)
			return false;
		if(::CompareStringOrdinal(name, static_cast<int>(prefix_len),
			kMarkerPrefix, static_cast<int>(prefix_len), FALSE) != CSTR_EQUAL)
			return false;
		for(size_t i = 0; i < kMarkerTokenChars; ++i)
		{
			const wchar_t c = name[prefix_len + i];
			if(!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')))
				return false;
		}
		return ::CompareStringOrdinal(name + prefix_len + kMarkerTokenChars,
			static_cast<int>(suffix_len), kMarkerSuffix,
			static_cast<int>(suffix_len), FALSE) == CSTR_EQUAL;
	}

	inline bool MarkerPathLooksOwned(const std::wstring &path)
	{
		if(path.size() < 8
		   || path.find(L'|') != std::wstring::npos
		   || path.find(L"..") != std::wstring::npos
		   || path.find(L'/') != std::wstring::npos)
			return false;
		if(!((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))
		   || path[1] != L':' || path[2] != L'\\')
			return false;

		const size_t infix_len = sizeof(kStagingInfix) / sizeof(wchar_t) - 1;
		size_t pos = std::wstring::npos;
		if(path.size() >= infix_len)
		{
			for(size_t i = 0; i + infix_len <= path.size(); ++i)
			{
				if(::CompareStringOrdinal(path.c_str() + i, static_cast<int>(infix_len),
					kStagingInfix, static_cast<int>(infix_len), TRUE) == CSTR_EQUAL)
					pos = i;
			}
		}
		if(pos == std::wstring::npos)
			return false;

		const wchar_t *name = path.c_str() + pos + infix_len;
		if(::wcschr(name, L'\\'))
			return false;
		return MarkerFileNameLooksOwned(name);
	}

	inline bool Parse(const std::wstring &data, Plan &plan)
	{
		plan = {};
		if(data == kNoop)
		{
			plan.op = Op::noop;
			return true;
		}
		if(data == kUninstallOurs)
		{
			plan.op = Op::uninstall_ours;
			return true;
		}

		static constexpr wchar_t prefix[] = L"install-absent|";
		const size_t prefix_len = 15;
		if(data.size() <= prefix_len
		   || ::CompareStringOrdinal(data.c_str(), static_cast<int>(prefix_len),
			   prefix, static_cast<int>(prefix_len), FALSE) != CSTR_EQUAL)
			return false;

		std::wstring marker = data.substr(prefix_len);
		if(!MarkerPathLooksOwned(marker))
			return false;

		plan.op = Op::install_absent;
		plan.marker = std::move(marker);
		return true;
	}

	inline std::wstring Serialize(Op op, const std::wstring &marker = {})
	{
		if(op == Op::noop)
			return kNoop;
		if(op == Op::uninstall_ours)
			return kUninstallOurs;
		return std::wstring(kInstallAbsent) + L"|" + marker;
	}

	// Rollback of install-absent deletes TreatAs only when apply recorded that
	// this transaction created it. An already-ours key at apply time must not
	// be removed.
	inline bool RollbackRemovesTreatAs(const Plan &plan, bool marker_present)
	{
		return plan.op == Op::install_absent && marker_present;
	}
}
