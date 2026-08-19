#pragma once

/*
	Third-party shell extension selection retriever.

	Explorer hands us selected items through IShellBrowser and IShellView, which
	we reach over WM_GETISHELLBROWSER. Third-party file managers (Total Commander,
	Directory Opus, XYplorer, etc.) and custom open/save dialogs typically don't
	expose an IShellBrowser, so Nilesoft Shell could not see selected items in
	those hosts.

	Those hosts do, however, query our registered CLSID as a standard Windows
	shell extension and pass selections via IShellExtInit::Initialize!

	This class implements:
	- IShellExtInit: captures IDataObject (selection) and pidlFolder (background folder).
	- IContextMenu: captures and binds the HMENU being built in QueryContextMenu.
	- Inserts 0 items, leaving host menus clean.
*/

#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <new>

namespace Nilesoft
{
	namespace Shell
	{
		// Outstanding COM objects, so DllCanUnloadNow can answer honestly.
		inline std::atomic<long> com_object_count{ 0 };

		// The selection captured by the most recent IShellExtInit::Initialize and
		// the menu QueryContextMenu was handed.
		struct ShellExtCapture
		{
			struct view
			{
				IShellItemArray *items{};
				PCIDLIST_ABSOLUTE folder{};
				bool background{};

				explicit operator bool() const { return items != nullptr || folder != nullptr; }
			};

			inline static thread_local HMENU hmenu{};
			inline static thread_local IShellItemArray *items{};
			inline static thread_local PIDLIST_ABSOLUTE folder{};
			inline static thread_local bool background{};
			inline static thread_local uint32_t tick{};

			static void clear()
			{
				if(items)
				{
					items->Release();
					items = nullptr;
				}
				if(folder)
				{
					::CoTaskMemFree(folder);
					folder = nullptr;
				}
				hmenu = nullptr;
				background = false;
				tick = 0;
			}

			static void capture(IShellItemArray *sia, bool is_background)
			{
				clear();
				if(sia)
				{
					sia->AddRef();
					items = sia;
				}
				background = is_background;
				tick = ::GetTickCount();
			}

			static void capture_folder(PCIDLIST_ABSOLUTE pidl)
			{
				if(folder)
				{
					::CoTaskMemFree(folder);
					folder = nullptr;
				}
				if(pidl)
					folder = ::ILCloneFull(pidl);
			}

			static void bind(HMENU h) { hmenu = h; }

			static view match(HMENU h)
			{
				if(!h || h != hmenu)
					return {};
				if((::GetTickCount() - tick) > 30000)
					return {};
				return { items, folder, background };
			}
		};

		// IShellExtInit + IContextMenu.
		class ShellExtHandler : public IShellExtInit, public IContextMenu
		{
			LONG m_ref = 1;

		public:
			ShellExtHandler() { com_object_count.fetch_add(1, std::memory_order_relaxed); }
			virtual ~ShellExtHandler() { com_object_count.fetch_sub(1, std::memory_order_relaxed); }

			// IUnknown
			IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv) override
			{
				if(!ppv) return E_POINTER;
				*ppv = nullptr;

				if(riid == IID_IUnknown || riid == IID_IShellExtInit)
					*ppv = static_cast<IShellExtInit *>(this);
				else if(riid == IID_IContextMenu)
					*ppv = static_cast<IContextMenu *>(this);
				else
					return E_NOINTERFACE;

				AddRef();
				return S_OK;
			}

			IFACEMETHODIMP_(ULONG) AddRef() override
			{
				return static_cast<ULONG>(::InterlockedIncrement(&m_ref));
			}

			IFACEMETHODIMP_(ULONG) Release() override
			{
				auto n = ::InterlockedDecrement(&m_ref);
				if(n == 0)
					delete this;
				return static_cast<ULONG>(n);
			}

			// IShellExtInit
			IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE pidlFolder, IDataObject *pdtobj,
									  HKEY hkeyProgID) override;

			// IContextMenu
			IFACEMETHODIMP QueryContextMenu(HMENU hmenu, UINT indexMenu, UINT idCmdFirst,
											UINT idCmdLast, UINT uFlags) override;

			IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO *) override { return E_INVALIDARG; }

			IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT *, CHAR *, UINT) override
			{
				return E_INVALIDARG;
			}
		};

		HRESULT CreateShellExtFactory(REFIID riid, void **ppv);

		class ShellExtFactory : public IClassFactory
		{
			LONG m_ref = 1;

		public:
			ShellExtFactory() { com_object_count.fetch_add(1, std::memory_order_relaxed); }
			virtual ~ShellExtFactory() { com_object_count.fetch_sub(1, std::memory_order_relaxed); }

			IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv) override
			{
				if(!ppv) return E_POINTER;
				*ppv = nullptr;

				if(riid == IID_IUnknown || riid == IID_IClassFactory)
				{
					*ppv = static_cast<IClassFactory *>(this);
					AddRef();
					return S_OK;
				}
				return E_NOINTERFACE;
			}

			IFACEMETHODIMP_(ULONG) AddRef() override
			{
				return static_cast<ULONG>(::InterlockedIncrement(&m_ref));
			}

			IFACEMETHODIMP_(ULONG) Release() override
			{
				auto n = ::InterlockedDecrement(&m_ref);
				if(n == 0)
					delete this;
				return static_cast<ULONG>(n);
			}

			IFACEMETHODIMP CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppv) override
			{
				if(!ppv) return E_POINTER;
				*ppv = nullptr;
				if(pUnkOuter) return CLASS_E_NOAGGREGATION;

				auto obj = new(std::nothrow) ShellExtHandler();
				if(!obj) return E_OUTOFMEMORY;

				auto hr = obj->QueryInterface(riid, ppv);
				obj->Release();
				return hr;
			}

			IFACEMETHODIMP LockServer(BOOL lock) override
			{
				if(lock)
					com_object_count.fetch_add(1, std::memory_order_relaxed);
				else
					com_object_count.fetch_sub(1, std::memory_order_relaxed);
				return S_OK;
			}
		};
	}
}
