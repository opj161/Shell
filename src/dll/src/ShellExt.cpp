// ShellExt.h is self-contained, so this deliberately does not pull in <pch.h> -
// it lets the test project compile this file directly.
#include "Include\ShellExt.h"

// __except(EXCEPTION_EXECUTE_HANDLER); normally from Globals.h, spelled out here
// so this translation unit stays independent of the DLL's include order.
#ifndef except
#define except __except(EXCEPTION_EXECUTE_HANDLER)
#endif

namespace Nilesoft
{
	namespace Shell
	{
		namespace
		{
			// SEH cannot coexist with objects that need unwinding in one function,
			// so the guarded part is split out.
			bool make_item_array(IDataObject *pdtobj, IShellItemArray **out) noexcept
			{
				*out = nullptr;
				__try
				{
					return pdtobj
						&& SUCCEEDED(::SHCreateShellItemArrayFromDataObject(pdtobj, IID_PPV_ARGS(out)))
						&& *out != nullptr;
				}
				except
				{
					*out = nullptr;
					return false;
				}
			}
		}

		// The selection the host is offering. pdtobj is the selected items; it is
		// null for a background click, where pidlFolder is the folder that was
		// clicked in.
		//
		// Everything lands in this handler's own pending capture. A host may hold
		// several handlers at once and interleave their calls, so there is no
		// process-wide slot for another handler's Initialize to overwrite.
		IFACEMETHODIMP ShellExtHandler::Initialize(PCIDLIST_ABSOLUTE pidlFolder,
												   IDataObject *pdtobj, HKEY)
		{
			m_pending.reset();

			IShellItemArray *sia = nullptr;
			if(make_item_array(pdtobj, &sia))
			{
				m_pending.items.assign(sia);
				sia->Release();
			}

			m_pending.background = (pdtobj == nullptr);

			// Kept even with no selection: a background menu still wants to know
			// which folder it was raised in.
			m_pending.folder = clone_pidl(pidlFolder);

			return S_OK;
		}

		// Binds the menu the host is about to build, so the TrackPopupMenu hook can
		// recognise it later by handle. Nothing is inserted.
		IFACEMETHODIMP ShellExtHandler::QueryContextMenu(HMENU hmenu, UINT, UINT, UINT,
														 UINT uFlags)
		{
			// CMF_DEFAULTONLY means the host wants the default verb, not a menu
			// the user will see - a double-click, typically.
			if(!(uFlags & CMF_DEFAULTONLY))
				ShellExtCapture::bind(hmenu, std::move(m_pending));

			m_pending.reset();

			// Zero items added.
			return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
		}

		HRESULT CreateShellExtFactory(REFIID riid, void **ppv)
		{
			auto factory = new(std::nothrow) ShellExtFactory();
			if(!factory)
				return E_OUTOFMEMORY;

			auto hr = factory->QueryInterface(riid, ppv);
			factory->Release();
			return hr;
		}
	}
}
