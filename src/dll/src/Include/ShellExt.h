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

	Ownership, in three parts:

	- A capture in progress belongs to the handler instance that is being
	  initialised, not to the process. A host is free to create two handlers and
	  interleave Initialize/QueryContextMenu on them; a single process-wide
	  pending slot let the second Initialize overwrite the first handler's
	  selection, so the first menu was built from the wrong items.

	- A completed capture belongs to the HMENU it was bound to, and only that
	  menu's teardown may release it. Clearing the whole registry when one popup
	  closed destroyed captures belonging to menus still on screen in other
	  windows.

	- What match() hands back is owned by the caller. It used to return raw
	  pointers into the registry after dropping the lock, so another thread
	  pruning or clearing an entry freed them underneath the caller.

	Interface pointers must be marshaled when passed between apartments, and a
	registry keyed by HMENU is reachable from every thread in the process:

		https://learn.microsoft.com/en-us/windows/win32/com/single-threaded-apartments

	There is one item-array consumer per menu. Microsoft recommends the stream
	technique when an interface is unmarshaled only once, rather than the Global
	Interface Table used for repeated unmarshaling:

		https://learn.microsoft.com/en-us/windows/win32/com/when-to-use-the-global-interface-table

	The marshaled stream is therefore move-only and consumed once. All unmarshal
	and discard work happens after the capture mutex is released. A discard also
	consumes and immediately releases the interface because an unconsumed normal
	marshal packet is itself an object reference that must be released:

		https://learn.microsoft.com/en-us/windows/win32/api/wtypesbase/ne-wtypesbase-mshlflags
*/

#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <utility>
#include <new>

namespace Nilesoft
{
	namespace Shell
	{
		// Outstanding COM objects, so DllCanUnloadNow can answer honestly.
		inline std::atomic<long> com_object_count{ 0 };
		inline std::atomic<bool> hooks_installed{ false };

		// Minimal owning COM pointer. ShellExt.h stays free of the DLL's include
		// order so the test project can compile ShellExt.cpp on its own.
		template<typename T>
		class com_ref
		{
		public:
			com_ref() = default;
			~com_ref() { reset(); }

			com_ref(const com_ref &other) { attach_addref(other._p); }
			com_ref(com_ref &&other) noexcept : _p(other._p) { other._p = nullptr; }

			com_ref &operator=(const com_ref &other)
			{
				if(this != &other) { reset(); attach_addref(other._p); }
				return *this;
			}

			com_ref &operator=(com_ref &&other) noexcept
			{
				if(this != &other) { reset(); _p = other._p; other._p = nullptr; }
				return *this;
			}

			// Takes ownership of a reference the caller already holds.
			void attach(T *p) { reset(); _p = p; }

			void attach_addref(T *p)
			{
				reset();
				_p = p;
				if(_p) _p->AddRef();
			}

			void reset()
			{
				if(_p) { _p->Release(); _p = nullptr; }
			}

			T *get() const { return _p; }
			T *operator->() const { return _p; }
			T **put() { reset(); return &_p; }
			explicit operator bool() const { return _p != nullptr; }

		private:
			T *_p = nullptr;
		};

		// PIDLIST_ABSOLUTE carries an __unaligned qualifier on 64-bit, so the
		// element type is taken from it rather than spelled out.
		using pidl_element = std::remove_pointer_t<PIDLIST_ABSOLUTE>;

		struct PidlDeleter
		{
			void operator()(pidl_element *p) const noexcept
			{
				if(p) ::CoTaskMemFree(p);
			}
		};

		using unique_pidl = std::unique_ptr<pidl_element, PidlDeleter>;

		inline unique_pidl clone_pidl(PCIDLIST_ABSOLUTE pidl)
		{
			return unique_pidl(pidl ? ::ILCloneFull(pidl) : nullptr);
		}

		// A one-shot marshaled IShellItemArray. The stream is the only cross-
		// apartment state; an apartment-specific interface pointer is never kept.
		class CapturedSelection
		{
		public:
			CapturedSelection() = default;
			~CapturedSelection() { reset(); }

			CapturedSelection(const CapturedSelection &) = delete;
			CapturedSelection &operator=(const CapturedSelection &) = delete;

			CapturedSelection(CapturedSelection &&other) noexcept
				: _stream(other._stream)
			{
				other._stream = nullptr;
			}

			CapturedSelection &operator=(CapturedSelection &&other) noexcept
			{
				if(this != &other)
				{
					reset();
					_stream = other._stream;
					other._stream = nullptr;
				}
				return *this;
			}

			void reset()
			{
				// CoGetInterfaceAndReleaseStream releases the stream even when
				// unmarshaling fails. On success, releasing the returned interface
				// also disposes a packet that was abandoned before normal match().
				// https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cogetinterfaceandreleasestream
				consume();
			}

			bool empty() const { return _stream == nullptr; }

			bool assign(IShellItemArray *items)
			{
				reset();
				if(!items)
					return false;

				IStream *stream = nullptr;
				// Available since Windows 2000. A failure deliberately leaves no
				// raw-pointer fallback.
				// https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-comarshalinterthreadinterfaceinstream
				auto hr = ::CoMarshalInterThreadInterfaceInStream(
					IID_IShellItemArray, items, &stream);
				if(FAILED(hr) || !stream)
				{
					if(stream)
						stream->Release();
					return false;
				}

				_stream = stream;
				return true;
			}

			// Consumes the stream and returns an owning pointer valid in the
			// calling apartment. A second call is deterministically empty.
			com_ref<IShellItemArray> consume()
			{
				com_ref<IShellItemArray> out;
				auto stream = _stream;
				_stream = nullptr;
				if(stream && FAILED(::CoGetInterfaceAndReleaseStream(
					stream, IID_IShellItemArray, reinterpret_cast<void **>(out.put()))))
					out.reset();
				return out;
			}

			// Ownership-only operation: safe while the capture mutex is held.
			void swap(CapturedSelection &other) noexcept
			{
				std::swap(_stream, other._stream);
			}

		private:
			IStream *_stream = nullptr;
		};

		// What a handler has collected so far, before it knows which menu it is for.
		struct PendingCapture
		{
			CapturedSelection items;
			unique_pidl folder;
			bool background{};

			bool empty() const { return items.empty() && !folder; }

			void reset()
			{
				items.reset();
				folder.reset();
				background = false;
			}
		};

		// What match() hands back: owning, and independent of the registry.
		struct ShellExtMatch
		{
			com_ref<IShellItemArray> items;
			unique_pidl folder;
			bool background{};

			explicit operator bool() const { return static_cast<bool>(items) || folder != nullptr; }
		};

		// The selection captured by IShellExtInit::Initialize, bound to the menu
		// QueryContextMenu was handed.
		struct ShellExtCapture
		{
			// An abandoned capture - a menu that was built and never tracked -
			// stops being offered after this long.
			static constexpr uint32_t TTL_MS = 30000;

		private:
			struct Entry
			{
				CapturedSelection items;
				unique_pidl folder;
				bool background{};
				uint32_t tick{};

				bool is_valid() const
				{
					return (!items.empty() || folder) && ((::GetTickCount() - tick) <= TTL_MS);
				}
			};

			inline static std::mutex _mutex;
			inline static std::unordered_map<HMENU, Entry> _bound;

			// Moves expired entries out of the map. The caller destroys them after
			// dropping the lock because discarding an unconsumed stream performs
			// COM unmarshaling, and no foreign COM call may run while the registry
			// is held.
			static void prune_unlocked(std::vector<Entry> &expired)
			{
				auto now = ::GetTickCount();
				for(auto it = _bound.begin(); it != _bound.end(); )
				{
					if((now - it->second.tick) > TTL_MS)
					{
						expired.push_back(std::move(it->second));
						it = _bound.erase(it);
					}
					else
						++it;
				}
			}

		public:
			// Binds one handler's pending capture to the menu it is building.
			static void bind(HMENU h, PendingCapture &&pending)
			{
				if(!h || pending.empty())
					return;

				Entry entry;
				entry.items = std::move(pending.items);
				entry.folder = std::move(pending.folder);
				entry.background = pending.background;
				entry.tick = ::GetTickCount();

				std::vector<Entry> expired;
				{
					std::lock_guard<std::mutex> lock(_mutex);
					prune_unlocked(expired);

					// Moving a replaced entry out first keeps its stream cleanup out
					// of the critical section.
					if(auto old = _bound.find(h); old != _bound.end())
					{
						expired.push_back(std::move(old->second));
						_bound.erase(old);
					}
					_bound.emplace(h, std::move(entry));
				}
				// `expired` consumes/discards streams here, outside the lock.
			}

			// The capture for one menu. The item stream is moved out while locked,
			// then consumed after unlocking; PIDL state remains independently owned.
			static ShellExtMatch match(HMENU h)
			{
				if(!h) return {};

				CapturedSelection items;
				ShellExtMatch result;
				std::vector<Entry> expired;

				{
					std::lock_guard<std::mutex> lock(_mutex);
					prune_unlocked(expired);

					auto it = _bound.find(h);
					if(it == _bound.end() || !it->second.is_valid())
						return {};

					items.swap(it->second.items);
					result.folder = clone_pidl(it->second.folder.get());
					result.background = it->second.background;
				}

				result.items = items.consume();
				return result;
			}

			// Is there a live capture for this menu? Answers from the registry
			// alone - no interface is resolved, which would mean marshaling just
			// to find out whether something is there.
			static bool has(HMENU h)
			{
				if(!h) return false;
				std::lock_guard<std::mutex> lock(_mutex);
				auto it = _bound.find(h);
				return it != _bound.end() && it->second.is_valid();
			}

			// Only the menu that is finishing. One popup closing must not destroy a
			// capture belonging to a menu still open in another window.
			static void clear(HMENU h)
			{
				Entry taken;
				{
					std::lock_guard<std::mutex> lock(_mutex);
					auto it = _bound.find(h);
					if(it == _bound.end())
						return;
					taken = std::move(it->second);
					_bound.erase(it);
				}
				// `taken` releases here, outside the lock.
			}

			// Process shutdown and tests only, where no other popup can exist.
			static void clear_all()
			{
				std::unordered_map<HMENU, Entry> taken;
				{
					std::lock_guard<std::mutex> lock(_mutex);
					taken.swap(_bound);
				}
			}

			static bool has_active_captures()
			{
				std::vector<Entry> expired;
				bool any = false;
				{
					std::lock_guard<std::mutex> lock(_mutex);
					prune_unlocked(expired);
					any = !_bound.empty();
				}
				return any;
			}

			static size_t bound_count()
			{
				std::lock_guard<std::mutex> lock(_mutex);
				return _bound.size();
			}

#ifdef SHELLEXT_CAPTURE_TESTING
			static void expire_for_test(HMENU h)
			{
				std::lock_guard<std::mutex> lock(_mutex);
				if(auto it = _bound.find(h); it != _bound.end())
					it->second.tick = ::GetTickCount() - TTL_MS - 1;
			}
#endif
		};

		// IShellExtInit + IContextMenu.
		class ShellExtHandler : public IShellExtInit, public IContextMenu
		{
			LONG m_ref = 1;

			// This handler's own capture. Never process-wide: a host may have
			// several handlers in flight at once.
			PendingCapture m_pending;

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

			// Tests reach in to check what one handler collected.
			const PendingCapture &pending() const { return m_pending; }
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
