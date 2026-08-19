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
#include <mutex>
#include <unordered_map>
#include <new>

namespace Nilesoft
{
	namespace Shell
	{
		// Outstanding COM objects, so DllCanUnloadNow can answer honestly.
		inline std::atomic<long> com_object_count{ 0 };
		inline std::atomic<bool> hooks_installed{ false };

		// The selection captured by IShellExtInit::Initialize and
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

			struct Entry
			{
				IShellItemArray *items{};
				PIDLIST_ABSOLUTE folder{};
				bool background{};
				uint32_t tick{};

				Entry() = default;
				Entry(IShellItemArray *sia, PCIDLIST_ABSOLUTE pidl, bool is_bg)
					: background(is_bg), tick(::GetTickCount())
				{
					if(sia)
					{
						sia->AddRef();
						items = sia;
					}
					if(pidl)
					{
						folder = ::ILCloneFull(pidl);
					}
				}

				~Entry() { release(); }

				Entry(const Entry &other)
					: background(other.background), tick(other.tick)
				{
					if(other.items)
					{
						other.items->AddRef();
						items = other.items;
					}
					if(other.folder)
					{
						folder = ::ILCloneFull(other.folder);
					}
				}

				Entry &operator=(const Entry &other)
				{
					if(this != &other)
					{
						release();
						background = other.background;
						tick = other.tick;
						if(other.items)
						{
							other.items->AddRef();
							items = other.items;
						}
						if(other.folder)
						{
							folder = ::ILCloneFull(other.folder);
						}
					}
					return *this;
				}

				Entry(Entry &&other) noexcept
					: items(other.items), folder(other.folder), background(other.background), tick(other.tick)
				{
					other.items = nullptr;
					other.folder = nullptr;
				}

				Entry &operator=(Entry &&other) noexcept
				{
					if(this != &other)
					{
						release();
						items = other.items;
						folder = other.folder;
						background = other.background;
						tick = other.tick;
						other.items = nullptr;
						other.folder = nullptr;
					}
					return *this;
				}

				void release()
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
					background = false;
					tick = 0;
				}

				bool is_valid() const
				{
					return (items != nullptr || folder != nullptr) && ((::GetTickCount() - tick) <= 30000);
				}
			};

		private:
			inline static std::mutex _mutex;
			inline static Entry _pending;
			inline static std::unordered_map<HMENU, Entry> _bound;

			static void prune_unlocked()
			{
				auto now = ::GetTickCount();
				for(auto it = _bound.begin(); it != _bound.end(); )
				{
					if((now - it->second.tick) > 30000)
						it = _bound.erase(it);
					else
						++it;
				}
				if((now - _pending.tick) > 30000)
					_pending.release();
			}

		public:
			static void clear()
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_pending.release();
				_bound.clear();
			}

			static void clear(HMENU h)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				if(h) _bound.erase(h);
			}

			static void capture(IShellItemArray *sia, bool is_background)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_pending.release();
				if(sia)
				{
					sia->AddRef();
					_pending.items = sia;
				}
				_pending.background = is_background;
				_pending.tick = ::GetTickCount();
				prune_unlocked();
			}

			static void capture_folder(PCIDLIST_ABSOLUTE pidl)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				if(_pending.folder)
				{
					::CoTaskMemFree(_pending.folder);
					_pending.folder = nullptr;
				}
				if(pidl)
				{
					_pending.folder = ::ILCloneFull(pidl);
				}
			}

			static void bind(HMENU h)
			{
				if(!h) return;
				std::lock_guard<std::mutex> lock(_mutex);
				if(_pending.items || _pending.folder)
				{
					_bound[h] = std::move(_pending);
				}
				prune_unlocked();
			}

			static view match(HMENU h)
			{
				if(!h) return {};
				std::lock_guard<std::mutex> lock(_mutex);
				prune_unlocked();
				auto it = _bound.find(h);
				if(it != _bound.end() && it->second.is_valid())
				{
					return { it->second.items, it->second.folder, it->second.background };
				}
				return {};
			}

			static bool has_active_captures()
			{
				std::lock_guard<std::mutex> lock(_mutex);
				prune_unlocked();
				return !_bound.empty() || _pending.is_valid();
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
