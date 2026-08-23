#include <pch.h>
#include "Include/ExplorerCommandCatalog.h"
#include "Include/Packages.h"
#include "Include/ContextMenu.h"

#include <shobjidl.h>
#include <mutex>
#include <memory>
#include <cstdint>

namespace Nilesoft
{
	namespace Shell
	{
		namespace
		{
			constexpr uint64_t CATALOG_TTL_MS = 30000;

			std::wstring read_text_file(const std::wstring &path)
			{
				HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
					FILE_SHARE_READ, nullptr, OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL, nullptr);
				if(file == INVALID_HANDLE_VALUE)
					return {};

				LARGE_INTEGER size{};
				if(!::GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024)
				{
					::CloseHandle(file);
					return {};
				}

				std::vector<char> raw(static_cast<size_t>(size.QuadPart));
				DWORD read = 0;
				auto ok = ::ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr);
				::CloseHandle(file);
				if(!ok || read == 0)
					return {};

				auto data = reinterpret_cast<const unsigned char *>(raw.data());
				if(read >= 2 && data[0] == 0xFF && data[1] == 0xFE)
				{
					auto chars = (read - 2) / sizeof(wchar_t);
					return std::wstring(reinterpret_cast<const wchar_t *>(data + 2), chars);
				}

				int skip = 0;
				if(read >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
					skip = 3;

				int n = ::MultiByteToWideChar(CP_UTF8, 0, raw.data() + skip,
					static_cast<int>(read) - skip, nullptr, 0);
				if(n <= 0)
					return {};
				std::wstring wide(static_cast<size_t>(n), L'\0');
				::MultiByteToWideChar(CP_UTF8, 0, raw.data() + skip,
					static_cast<int>(read) - skip, wide.data(), n);
				return wide;
			}

			void merge_catalog(std::vector<ExplorerCommandRegistration> &dst,
				const std::vector<ExplorerCommandRegistration> &src)
			{
				for(const auto &reg : src)
				{
					for(const auto &type : reg.types)
						explorer_command_xml::merge(dst, reg.clsid, type);
				}
			}

			std::vector<ExplorerCommandRegistration> scan_catalog()
			{
				std::vector<ExplorerCommandRegistration> out;
				RegistryPackageSource source;
				std::vector<std::wstring> names;
				if(!source.enumerate_full_names(names))
					return out;

				for(const auto &full : names)
				{
					// Documented two-call GetPackagePathByFullName, already wrapped:
					// https://learn.microsoft.com/en-us/windows/win32/api/appmodel/nf-appmodel-getpackagepathbyfullname
					auto root = GetInstalledPackagePath(full);
					if(root.empty())
						continue;
					auto manifest = root;
					if(!manifest.empty() && manifest.back() != L'\\' && manifest.back() != L'/')
						manifest += L'\\';
					manifest += L"AppxManifest.xml";
					auto xml = read_text_file(manifest);
					if(xml.empty())
						continue;
					std::vector<ExplorerCommandRegistration> local;
					parse_file_explorer_context_menus(xml, local);
					merge_catalog(out, local);
				}
				return out;
			}

			std::mutex g_catalog_lock;
			std::vector<ExplorerCommandRegistration> g_catalog;
			uint64_t g_catalog_tick = 0;
			bool g_catalog_ready = false;

			std::vector<ExplorerCommandRegistration> catalog_snapshot()
			{
				auto now = ::GetTickCount64();
				{
					std::lock_guard<std::mutex> lock(g_catalog_lock);
					if(g_catalog_ready && (now - g_catalog_tick) < CATALOG_TTL_MS)
						return g_catalog;
				}

				auto scanned = scan_catalog();
				std::vector<ExplorerCommandRegistration> copy;
				{
					std::lock_guard<std::mutex> lock(g_catalog_lock);
					g_catalog = scanned;
					g_catalog_tick = ::GetTickCount64();
					g_catalog_ready = true;
					copy = g_catalog;
				}
				return copy;
			}

			string take_cotask_string(LPWSTR p)
			{
				string s;
				if(!p)
					return s;
				s = p;
				// GetTitle/GetIcon samples allocate with SHStrDup (CoTaskMemAlloc).
				// https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/integrate-packaged-app-with-file-explorer
				// https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cotaskmemfree
				::CoTaskMemFree(p);
				return s.move();
			}

			HBITMAP icon_from_resource(const string &spec)
			{
				if(spec.empty())
					return nullptr;
				string path = spec;
				int index = 0;
				auto comma = path.last_index_of(',', false);
				if(comma != string::npos && comma > 0)
				{
					index = static_cast<int>(string::ToInt(path.c_str() + comma + 1, 0LL));
					path = path.substr(0, comma).move();
				}
				path.trim(L'"');
				HICON large = nullptr, small_icon = nullptr;
				// GetIcon returns the standard "file,-id" resource string.
				// https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-geticon
				// nIconSize is LOWORD=large, HIWORD=small. 0 means the system defaults.
				// https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shdefextracticonw
				if(FAILED(::SHDefExtractIconW(path.c_str(), index, 0, &large, &small_icon,
					static_cast<UINT>(MAKELONG(16, 16)))))
					return nullptr;
				HICON use = small_icon ? small_icon : large;
				HBITMAP bitmap = nullptr;
				if(use)
				{
					ICONINFO info{};
					if(::GetIconInfo(use, &info))
					{
						if(info.hbmMask) ::DeleteObject(info.hbmMask);
						bitmap = info.hbmColor;
					}
				}
				if(small_icon) ::DestroyIcon(small_icon);
				if(large && large != small_icon) ::DestroyIcon(large);
				return bitmap;
			}

			IExplorerCommand *activate_explorer_command(const GUID &clsid)
			{
				IExplorerCommand *cmd = nullptr;
				// Packaged commands register as com:InProcessServer or
				// com:SurrogateServer. Combine the in-proc and local-server
				// contexts; COM tries them in that order.
				// https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cocreateinstance
				// https://learn.microsoft.com/en-us/windows/win32/api/wtypesbase/ne-wtypesbase-clsctx
				// https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/integrate-packaged-app-with-file-explorer
				auto hr = ::CoCreateInstance(clsid, nullptr,
					CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
					IID_IExplorerCommand, reinterpret_cast<void **>(&cmd));
				if(FAILED(hr))
					return nullptr;
				return cmd;
			}

			bool fill_menuitem_from_explorer_command(menuitem_t *item,
				IExplorerCommand *cmd, IShellItemArray *selection)
			{
				if(!item || !cmd)
					return false;

				EXPCMDSTATE state = ECS_ENABLED;
				// fOkToBeSlow is FALSE and stays FALSE on this path. It means the
				// verb object "should not perform any memory intensive computations
				// that could cause the UI thread to stop responding. The verb object
				// should return E_PENDING in that case"; TRUE says "those
				// computations can be completed" - on the thread that is between the
				// user's right-click and the first menu pixel, with no bound on how
				// long a third-party handler takes.
				// https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getstate
				//
				// E_PENDING therefore is the answer, not a reason to ask again:
				// present the item provisionally enabled and let Invoke find out.
				// A handler that reports pending has told us only that it cannot
				// answer cheaply, not that the verb is unavailable, and hiding a
				// working command is worse than offering one that turns out to be a
				// no-op. docs/refactor/02-first-paint-latency.md section 2.
				auto hr_state = cmd->GetState(selection, FALSE, &state);
				if(hr_state == E_PENDING)
				{
					state = ECS_ENABLED;
					hr_state = S_OK;
				}
				if(SUCCEEDED(hr_state) && (state & ECS_HIDDEN))
					return false;

				LPWSTR title = nullptr;
				if(FAILED(cmd->GetTitle(selection, &title)) || !title || !*title)
				{
					if(title) ::CoTaskMemFree(title);
					return false;
				}
				item->title = take_cotask_string(title).move();
				item->hash = MenuItemInfo::normalize(item->title, &item->name, &item->tab,
					&item->length, &item->keys);

				if(state & ECS_DISABLED)
					item->disabled = true;
				if(state & ECS_CHECKED)
					item->checked = 1;
				if(state & ECS_RADIOCHECK)
					item->radio_check = true;

				EXPCMDFLAGS flags = ECF_DEFAULT;
				cmd->GetFlags(&flags);
				if(flags & ECF_ISSEPARATOR)
				{
					item->type = 2;
					return true;
				}
				// ECF_HASSUBCOMMANDS is the documented child-command flag.
				// ECF_ISDROPDOWN is a drop-down submenu of the same kind.
				// https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getflags
				if((flags & ECF_HASSUBCOMMANDS) || (flags & ECF_ISDROPDOWN))
					item->type = 1;
				else
					item->type = 0;

				LPWSTR icon = nullptr;
				if(SUCCEEDED(cmd->GetIcon(selection, &icon)) && icon && *icon)
				{
					string spec = take_cotask_string(icon).move();
					item->image = icon_from_resource(spec);
				}
				else if(icon)
					::CoTaskMemFree(icon);

				item->explorer_command = cmd;
				item->explorer_command_owned = true;
				return true;
			}

			IShellItemArray *create_shell_item_array_from_paths(const std::vector<std::wstring> &paths)
			{
				if(paths.empty())
					return nullptr;

				std::vector<PIDLIST_ABSOLUTE> pidls;
				pidls.reserve(paths.size());
				for(const auto &path : paths)
				{
					if(path.empty())
						continue;
					PIDLIST_ABSOLUTE pidl = nullptr;
					SFGAOF dummy = 0;
					// Preferred string-to-PIDL conversion. The remarks prefer a
					// background thread; IExplorerCommand methods run on the UI
					// thread, so the array has to exist before GetState/Invoke.
					// https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shparsedisplayname
					// https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexplorercommand
					if(SUCCEEDED(::SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, &dummy)) && pidl)
						pidls.push_back(pidl);
				}
				if(pidls.empty())
					return nullptr;

				std::vector<LPCITEMIDLIST> view(pidls.begin(), pidls.end());
				IShellItemArray *array = nullptr;
				auto hr = ::SHCreateShellItemArrayFromIDLists(
					static_cast<UINT>(view.size()), view.data(), &array);
				// Windows 2000+: ITEMIDLIST is allocated with the COM task
				// allocator, so CoTaskMemFree rather than ILFree.
				// https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-ilfree
				for(auto pidl : pidls)
					::CoTaskMemFree(pidl);
				return SUCCEEDED(hr) ? array : nullptr;
			}
		}

		IShellItemArray *ContextMenu::ensure_selection_array()
		{
			if(Selected.ItemArray)
				return Selected.ItemArray;

			std::vector<std::wstring> paths;
			if(Selected.Background && !Selected.Directory.empty())
				paths.emplace_back(Selected.Directory.c_str());
			else
			{
				for(auto item : Selected.Items)
				{
					if(item && !item->Path.empty())
						paths.emplace_back(item->Path.c_str());
				}
			}

			Selected.ItemArray = create_shell_item_array_from_paths(paths);
			Selected.ItemArrayOwned = Selected.ItemArray != nullptr;
			return Selected.ItemArray;
		}

		bool ContextMenu::materialize_explorer_command_children(menuitem_t *node)
		{
			if(!node || !node->explorer_command)
				return false;
			if(node->native_popup.materialized)
				return true;

			auto selection = ensure_selection_array();
			IEnumExplorerCommand *enumerator = nullptr;
			auto hr = node->explorer_command->EnumSubCommands(&enumerator);
			if(FAILED(hr) || !enumerator)
			{
				node->native_popup.materialized = true;
				apply_system_modify_rules(node, false);
				return false;
			}

			for(;;)
			{
				IExplorerCommand *child_cmd = nullptr;
				ULONG fetched = 0;
				hr = enumerator->Next(1, &child_cmd, &fetched);
				// IEnumExplorerCommand::Next documents S_OK on success. Treat a
				// fetched element as usable even if a server returns S_FALSE with
				// celt=1, and stop when nothing was retrieved.
				// https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ienumexplorercommand-next
				if(FAILED(hr) || fetched == 0 || !child_cmd)
				{
					if(child_cmd) child_cmd->Release();
					break;
				}

				std::unique_ptr<menuitem_t> child(new menuitem_t);
				child->parent = node;
				child->is_toplevel = false;
				child->wid = ident.get_id();
				if(fill_menuitem_from_explorer_command(child.get(), child_cmd, selection))
				{
					if(child->is_menu())
						child->native_popup.materialized = false;
					else
						child->native_popup.materialized = true;
					node->items.push_back(child.release());
				}
				else
				{
					child_cmd->Release();
				}
			}

			enumerator->Release();
			node->native_popup.materialized = true;
			apply_system_modify_rules(node, false);
			return true;
		}

		void ContextMenu::append_explorer_commands(menuitem_t *root)
		{
			if(!root)
				return;
			if(Selected.Window.id == WINDOW_TASKBAR || Selected.Window.id == WINDOW_SYSMENU)
				return;

			std::vector<ExplorerCommandKind> kinds;
			if(Selected.Background)
				kinds.push_back(ExplorerCommandKind::DirectoryBackground);
			if(Selected.count.FILE)
				kinds.push_back(ExplorerCommandKind::File);
			if(Selected.count.DIRECTORY)
				kinds.push_back(ExplorerCommandKind::Directory);
			if(Selected.count.DRIVE)
				kinds.push_back(ExplorerCommandKind::Drive);
			if(kinds.empty())
				return;

			auto selection = ensure_selection_array();
			auto regs = catalog_snapshot();

			// One composition: skip a packaged verb that the classic HMENU (or
			// an earlier catalog row) already contributed. GUID_NULL is not an
			// identity. Same-type title hash is the native duplicate rule.
			// https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getcanonicalname
			std::vector<ExplorerCommandIdentity> accepted;
			accepted.reserve(root->items.size() + regs.size());
			for(auto existing : root->items)
			{
				if(!existing || existing->is_separator() || existing->hash == 0)
					continue;
				ExplorerCommandIdentity id;
				id.hash = existing->hash;
				id.type = existing->type;
				accepted.push_back(id);
			}

			for(const auto &reg : regs)
			{
				if(!explorer_command_matches_any(reg, kinds))
					continue;

				ExplorerCommandIdentity by_clsid;
				by_clsid.clsid = reg.clsid;
				if(explorer_command_already_represented(by_clsid, accepted))
					continue;

				auto cmd = activate_explorer_command(reg.clsid);
				if(!cmd)
					continue;

				std::unique_ptr<menuitem_t> item(new menuitem_t);
				item->parent = root;
				item->is_toplevel = true;
				item->wid = ident.get_id();
				if(!fill_menuitem_from_explorer_command(item.get(), cmd, selection))
				{
					cmd->Release();
					continue;
				}

				GUID canonical{};
				// Out-parameter is valid only when the method succeeds.
				// https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-getcanonicalname
				if(FAILED(cmd->GetCanonicalName(&canonical)))
					canonical = GUID_NULL;

				ExplorerCommandIdentity identity;
				identity.hash = item->hash;
				identity.type = item->type;
				identity.clsid = reg.clsid;
				identity.canonical = canonical;
				if(explorer_command_already_represented(identity, accepted))
					continue;

				if(item->is_menu())
					item->native_popup.materialized = false;
				else
					item->native_popup.materialized = true;
				accepted.push_back(identity);
				root->items.push_back(item.release());
			}
		}

		bool ContextMenu::invoke_explorer_command(MenuItemInfo *item)
		{
			if(!item || !item->explorer_command)
				return false;
			auto selection = ensure_selection_array();
			// pbc "can be NULL if no bind context is needed".
			// https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iexplorercommand-invoke
			item->explorer_command->Invoke(selection, nullptr);
			return true;
		}
	}
}
