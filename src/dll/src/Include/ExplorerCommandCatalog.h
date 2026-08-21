#pragma once

/*
	Parse windows.fileExplorerContextMenus registrations out of an AppxManifest.

	The schema is the specification:

		https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-desktop4-fileexplorercontextmenus
		https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-desktop4-itemtype
		https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-desktop4-verb
		https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-desktop5-itemtype
		https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/integrate-packaged-app-with-file-explorer

	desktop4:ItemType/@Type is "*" or a leading-dot extension.
	desktop5:ItemType/@Type is "*", Directory, or Directory\Background.
	desktop10:ItemType adds Drive.

	A packaged IExplorerCommand is not an IContextMenu handler and is not
	written under HKCR\...\shellex\ContextMenuHandlers. Explorer activates the
	COM class when it builds the modern menu. Shell hosts the same contract by
	reading the same manifest and calling the same IExplorerCommand methods.
*/

#include <windows.h>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

namespace Nilesoft
{
	namespace Shell
	{
		struct ExplorerCommandRegistration
		{
			GUID clsid{};
			std::vector<std::wstring> types;
		};

		enum class ExplorerCommandKind
		{
			File,
			Directory,
			DirectoryBackground,
			Drive
		};

		inline bool explorer_command_type_matches(
			std::wstring_view type, ExplorerCommandKind kind) noexcept
		{
			auto ieq = [](std::wstring_view a, std::wstring_view b) noexcept
			{
				if(a.size() != b.size())
					return false;
				for(size_t i = 0; i < a.size(); i++)
				{
					auto ca = a[i];
					auto cb = b[i];
					if(ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
					if(cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
					if(ca != cb) return false;
				}
				return true;
			};

			// "*" is files. A leading-dot Type is a file extension.
			// https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-desktop4-itemtype
			// https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/integrate-packaged-app-with-file-explorer
			if(type == L"*")
				return kind == ExplorerCommandKind::File;
			if(!type.empty() && type.front() == L'.')
				return kind == ExplorerCommandKind::File;
			if(ieq(type, L"Directory"))
				return kind == ExplorerCommandKind::Directory;
			if(ieq(type, L"Directory\\Background") || ieq(type, L"Directory/Background"))
				return kind == ExplorerCommandKind::DirectoryBackground;
			if(ieq(type, L"Drive"))
				return kind == ExplorerCommandKind::Drive;
			return false;
		}

		inline bool explorer_command_matches_any(
			const ExplorerCommandRegistration &reg,
			const std::vector<ExplorerCommandKind> &kinds) noexcept
		{
			for(const auto &type : reg.types)
			{
				for(auto kind : kinds)
				{
					if(explorer_command_type_matches(type, kind))
						return true;
				}
			}
			return false;
		}

		inline bool parse_guid(std::wstring_view text, GUID &out) noexcept
		{
			out = {};
			while(!text.empty() && (text.front() == L' ' || text.front() == L'\t' ||
				text.front() == L'"' || text.front() == L'\''))
				text.remove_prefix(1);
			while(!text.empty() && (text.back() == L' ' || text.back() == L'\t' ||
				text.back() == L'"' || text.back() == L'\''))
				text.remove_suffix(1);
			if(text.empty())
				return false;

			wchar_t buf[64]{};
			if(text.front() != L'{')
			{
				if(text.size() + 2 >= std::size(buf))
					return false;
				buf[0] = L'{';
				text.copy(buf + 1, text.size());
				buf[text.size() + 1] = L'}';
			}
			else
			{
				if(text.size() >= std::size(buf))
					return false;
				text.copy(buf, text.size());
			}
			return SUCCEEDED(::CLSIDFromString(buf, &out));
		}

		namespace explorer_command_xml
		{
			inline size_t ifind(std::wstring_view hay, std::wstring_view needle, size_t from = 0) noexcept
			{
				if(needle.empty() || from > hay.size())
					return std::wstring_view::npos;
				auto it = std::search(hay.begin() + static_cast<std::ptrdiff_t>(from), hay.end(),
					needle.begin(), needle.end(),
					[](wchar_t a, wchar_t b)
					{
						if(a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
						if(b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
						return a == b;
					});
				if(it == hay.end())
					return std::wstring_view::npos;
				return static_cast<size_t>(it - hay.begin());
			}

			inline std::wstring_view tag_local_name(std::wstring_view tag) noexcept
			{
				if(tag.empty() || tag.front() != L'<')
					return {};
				size_t i = 1;
				if(i < tag.size() && tag[i] == L'/')
					i++;
				auto start = i;
				while(i < tag.size() && tag[i] != L' ' && tag[i] != L'\t' &&
					tag[i] != L'\r' && tag[i] != L'\n' &&
					tag[i] != L'>' && tag[i] != L'/')
					i++;
				auto name = tag.substr(start, i - start);
				auto colon = name.find(L':');
				if(colon != std::wstring_view::npos)
					name.remove_prefix(colon + 1);
				return name;
			}

			inline bool tag_is(std::wstring_view tag, std::wstring_view local) noexcept
			{
				auto name = tag_local_name(tag);
				return name.size() == local.size() && ifind(name, local) == 0;
			}

			inline bool is_attr_name_char(wchar_t c) noexcept
			{
				return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
					(c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
			}

			// Match a whole attribute name. "Type" must not hit the suffix of
			// "ItemType"; "Clsid" must not hit a longer token that contains it.
			inline std::wstring_view attr(std::wstring_view tag, std::wstring_view name) noexcept
			{
				size_t p = 0;
				while(p < tag.size())
				{
					auto found = ifind(tag, name, p);
					if(found == std::wstring_view::npos)
						return {};

					bool before_ok = (found == 0);
					if(!before_ok)
					{
						auto prev = tag[found - 1];
						before_ok = !is_attr_name_char(prev);
					}

					auto after = found + name.size();
					bool after_ok = after >= tag.size() || !is_attr_name_char(tag[after]);
					if(before_ok && after_ok)
					{
						p = after;
						while(p < tag.size() && (tag[p] == L' ' || tag[p] == L'\t' ||
							tag[p] == L'\r' || tag[p] == L'\n'))
							p++;
						if(p >= tag.size() || tag[p] != L'=')
						{
							p = found + 1;
							continue;
						}
						p++;
						while(p < tag.size() && (tag[p] == L' ' || tag[p] == L'\t' ||
							tag[p] == L'\r' || tag[p] == L'\n'))
							p++;
						if(p >= tag.size())
							return {};
						wchar_t quote = 0;
						if(tag[p] == L'"' || tag[p] == L'\'')
						{
							quote = tag[p];
							p++;
						}
						auto start = p;
						while(p < tag.size() && tag[p] != quote && tag[p] != L'>' &&
							!(quote == 0 && (tag[p] == L' ' || tag[p] == L'/')))
							p++;
						return tag.substr(start, p - start);
					}
					p = found + 1;
				}
				return {};
			}

			inline void merge(std::vector<ExplorerCommandRegistration> &out,
				const GUID &clsid, std::wstring type)
			{
				if(type.empty())
					return;
				for(auto &reg : out)
				{
					if(InlineIsEqualGUID(reg.clsid, clsid))
					{
						for(const auto &existing : reg.types)
						{
							if(existing == type)
								return;
						}
						reg.types.push_back(std::move(type));
						return;
					}
				}
				ExplorerCommandRegistration rec;
				rec.clsid = clsid;
				rec.types.push_back(std::move(type));
				out.push_back(std::move(rec));
			}
		}

		// Extracts Verb/@Clsid + enclosing ItemType/@Type pairs from one manifest.
		// desktop4:Verb/@Clsid is "a GUID in the form xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
		// (no braces):
		// https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-desktop4-verb
		inline bool parse_file_explorer_context_menus(
			std::wstring_view xml, std::vector<ExplorerCommandRegistration> &out)
		{
			using namespace explorer_command_xml;
			auto before = out.size();

			for(size_t p = 0; p < xml.size();)
			{
				auto lt = xml.find(L'<', p);
				if(lt == std::wstring_view::npos)
					break;
				auto gt = xml.find(L'>', lt + 1);
				if(gt == std::wstring_view::npos)
					break;
				auto tag = xml.substr(lt, gt - lt + 1);
				p = gt + 1;

				// Match the element, not Category="windows.fileExplorerContextMenus".
				if(tag.size() <= 1 || tag[1] == L'/' ||
				   !tag_is(tag, L"FileExplorerContextMenus"))
					continue;

				auto close = ifind(xml, L"/FileExplorerContextMenus", p);
				auto block_end = close == std::wstring_view::npos ? xml.size() : close;
				auto body = xml.substr(lt, block_end - lt);

				std::wstring current_type;
				size_t pos = 0;
				while(pos < body.size())
				{
					auto tlt = body.find(L'<', pos);
					if(tlt == std::wstring_view::npos)
						break;
					auto tgt = body.find(L'>', tlt + 1);
					if(tgt == std::wstring_view::npos)
						break;
					auto inner = body.substr(tlt, tgt - tlt + 1);
					pos = tgt + 1;

					if(inner.size() > 1 && inner[1] != L'/' && tag_is(inner, L"ItemType"))
					{
						auto type = attr(inner, L"Type");
						current_type.assign(type.begin(), type.end());
					}
					else if(inner.size() > 1 && inner[1] != L'/' && tag_is(inner, L"Verb"))
					{
						GUID clsid{};
						auto id = attr(inner, L"Clsid");
						if(!id.empty() && parse_guid(id, clsid))
							merge(out, clsid, current_type);
					}
				}

				p = block_end == xml.size() ? xml.size() : block_end + 1;
			}

			return out.size() > before;
		}
	}
}
