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
		// The selection the host is offering. pdtobj is the selected items; it is
		// null for a background click, where pidlFolder is the folder that was
		// clicked in.
		IFACEMETHODIMP ShellExtHandler::Initialize(PCIDLIST_ABSOLUTE pidlFolder,
												   IDataObject *pdtobj, HKEY)
		{
			__try
			{
				IShellItemArray *sia = nullptr;

				if(pdtobj)
					::SHCreateShellItemArrayFromDataObject(pdtobj, IID_PPV_ARGS(&sia));

				ShellExtCapture::capture(sia, pdtobj == nullptr);

				if(sia)
					sia->Release();

				// Kept even with no selection: a background menu still wants to know
				// which folder it was raised in.
				ShellExtCapture::capture_folder(pidlFolder);
			}
			except
			{
				ShellExtCapture::clear();
			}

			return S_OK;
		}

		// Binds the menu the host is about to build, so the TrackPopupMenu hook can
		// recognise it later by handle. Nothing is inserted.
		IFACEMETHODIMP ShellExtHandler::QueryContextMenu(HMENU hmenu, UINT, UINT, UINT,
														 UINT uFlags)
		{
			__try
			{
				// CMF_DEFAULTONLY means the host wants the default verb, not a menu
				// the user will see - a double-click, typically.
				if(!(uFlags & CMF_DEFAULTONLY))
					ShellExtCapture::bind(hmenu);
			}
			except
			{
			}

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
