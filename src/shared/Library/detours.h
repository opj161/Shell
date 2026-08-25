/////////////////////////////////////////////////////////////////////////////
//
//  Core Detours Functionality (detours.h of detours.lib)
//
//  Microsoft Research Detours Package, Version 4.0.1
//
//  Copyright (c) Microsoft Corporation.  All rights reserved.
//

#pragma once
#ifndef _DETOURS_H_
#define _DETOURS_H_

#define DETOURS_VERSION     0x4c0c1   // 0xMAJORcMINORcPATCH

typedef struct _DETOUR_TRAMPOLINE DETOUR_TRAMPOLINE, *PDETOUR_TRAMPOLINE;

extern "C" 
{
LONG WINAPI DetourTransactionBegin();
LONG WINAPI DetourTransactionAbort();
LONG WINAPI DetourTransactionCommit();
LONG WINAPI DetourTransactionCommitEx(PVOID **pppFailedPointer);
LONG WINAPI DetourUpdateThread(HANDLE hThread);
LONG WINAPI DetourAttach(PVOID *ppPointer, PVOID pDetour);
LONG WINAPI DetourAttachEx(PVOID *ppPointer,
                           PVOID pDetour,
                           PDETOUR_TRAMPOLINE *ppRealTrampoline,
                           PVOID *ppRealTarget,
                           PVOID *ppRealDetour);
LONG WINAPI DetourDetach(PVOID *ppPointer, PVOID pDetour);
BOOL WINAPI DetourSetIgnoreTooSmall(BOOL fIgnore);
BOOL WINAPI DetourSetRetainRegions(BOOL fRetain);
PVOID WINAPI DetourSetSystemRegionLowerBound(PVOID pSystemRegionLowerBound);
PVOID WINAPI DetourSetSystemRegionUpperBound(PVOID pSystemRegionUpperBound);
PVOID WINAPI DetourFindFunction(LPCWSTR pszModule, LPCSTR pszFunction);
PVOID WINAPI DetourCodeFromPointer(PVOID pPointer, PVOID *ppGlobals);
BOOL WINAPI DetourIsFunctionImported(PBYTE pbCode, PBYTE pbAddress);
}


#include <type_traits>
#include <vector>
#include <tlhelp32.h>

#include "../DetourEnlistment.h"

/*
	One detour transaction, with every other thread in the process enlisted in it.

	The mechanism, the contract it answers to and every failure path now live in
	shared/DetourEnlistment.h, which names no Detours symbol and can therefore be
	driven by the unit suite. This is the binding of that machinery to the real
	functions, and the only place the two meet.

	Two things it is worth keeping in view here:

	The current thread stays unenlisted on purpose, and by thread id rather than by
	handle comparison. DetourUpdateThread documents its pseudo handle as a no-op -
	"If hThread is equal to the current threads pseudo handle ... no action is
	performed and NO_ERROR is returned", which is why the version before this one
	enlisted nothing at all - and a real handle to the current thread "is currently
	unsupported and will result in application hangs".

	  https://github.com/microsoft/Detours/wiki/DetourUpdateThread

	And begin() answers false when the process could not be brought into a known
	state: a snapshot that failed, a live thread that could not be opened, a thread
	Detours would not record. Committing anyway is what the page above describes as
	leaving threads to "execute an illegal combination of old and new code", and
	that is worse than not installing the hook.
*/
inline const Nilesoft::Shell::InlineDetourApi &default_inline_detour_api()
{
	static const Nilesoft::Shell::InlineDetourApi api
	{
		&::CreateToolhelp32Snapshot,
		&::Thread32First,
		&::Thread32Next,
		&::CloseHandle,
		&::OpenThread,
		&DetourUpdateThread,
		&DetourTransactionBegin,
		&DetourTransactionAbort,
		&DetourTransactionCommitEx,
		&DetourSetIgnoreTooSmall,
	};
	return api;
}

class DetourTransaction : public Nilesoft::Shell::InlineDetourTransaction
{
public:
	DetourTransaction()
		: Nilesoft::Shell::InlineDetourTransaction(default_inline_detour_api())
	{
	}
};

template<typename T>
struct DetoursIsFunctionPointer : std::false_type {};

template<typename T>
struct DetoursIsFunctionPointer<T*> : std::is_function<typename std::remove_pointer<T>::type> {};

template<typename T, typename std::enable_if<DetoursIsFunctionPointer<T>::value, int>::type = 0>
LONG DetourAttach(T *ppPointer, T pDetour) noexcept
{
    return DetourAttach(reinterpret_cast<void**>(ppPointer), reinterpret_cast<void*>(pDetour));
}

template<typename T, typename std::enable_if<DetoursIsFunctionPointer<T>::value, int>::type = 0>
LONG DetourAttachEx(T *ppPointer,
                    T pDetour,
                    PDETOUR_TRAMPOLINE *ppRealTrampoline,
                    T *ppRealTarget,
                    T *ppRealDetour) noexcept
{
    return DetourAttachEx(
        reinterpret_cast<void**>(ppPointer),
        reinterpret_cast<void*>(pDetour),
        ppRealTrampoline,
        reinterpret_cast<void**>(ppRealTarget),
        reinterpret_cast<void**>(ppRealDetour));
}

template<typename T, typename std::enable_if<DetoursIsFunctionPointer<T>::value, int>::type = 0>
LONG DetourDetach(T *ppPointer,  T pDetour) noexcept
{
    return DetourDetach(reinterpret_cast<void**>(ppPointer), reinterpret_cast<void*>(pDetour));
}

template<typename T>
class Detours
{
public:

	void *_detour{};
	void *_original{};
	bool _installed = false;

	Detours()
	{
	}

	Detours(HMODULE hModule, const char *pszFunction, void *detour = {})
	{
		init(hModule, pszFunction, detour);
	}

	Detours(void *original, void *detour = {})
	{
		_original = original;
		_detour = detour;
	}

	Detours &init(HMODULE hModule, const char *pszFunction, void *detour = {})
	{
		_original = ::GetProcAddress(hModule, pszFunction);
		_detour = detour;
		return *this;
	}

	Detours &init(void *original, void *detour)
	{
		_original = original;
		_detour = detour;
		return *this;
	}

	explicit operator bool() const { return _installed; }
	//explicit operator T*() const { return reinterpret_cast<T*>(_original); }

	/*
	Detours& SetHookState(BOOL bHookState = -1)
	{
		if(bHookState == -1)
		{
			bHookState = !_installed;
		}
		if(bHookState == TRUE and !_installed)
		{
			DetourAttach(&(PVOID &)pvOldAddr, pvNewAddr);
			_installed = true;
		}
		else if(bHookState == FALSE and _installed)
		{
			DetourDetach(&(PVOID &)pvOldAddr, pvNewAddr);
			_installed = false;
		}
		return *this;
	}
	*/
	// The attach is only pending until the transaction commits, but a failure
	// here means it will never take - so it must not be recorded as installed,
	// or unhook() later detaches something that was never attached.
	Detours &hook()
	{
		if(!_installed && _original && _detour)
			_installed = (DetourAttach(&_original, _detour) == NO_ERROR);
		return *this;
	}

	Detours &unhook()
	{
		if(_installed)
		{
			DetourDetach(&_original, _detour);
			_installed = false;
		}
		return *this;
	}

	// The transaction failed to commit, so nothing was actually patched.
	void forget() { _installed = false; }

	bool is_hooked() const
	{
		return _installed;
	}

	template <typename... Args>
	auto invoke(Args&&... args)
	{
		return reinterpret_cast<T *>(_original)(args...);
	}

	/*
	template <typename T, typename... Args>
	auto invoke(Args&&... args)
	{
		return reinterpret_cast<T *>(_original)(args...);
	}*/

	/*
	template <typename... Args>
	static void Batch(BOOL bBatchState, Args&&... args)
	{
		::bBatchState = bBatchState;
		Batch(std::forward<Args>(args)...);
	}
	template <typename T, typename... Args>
	static void Batch(T &t, Args&&... args)
	{
		t.SetHookState(bBatchState);
		Batch(args...);
	}

	static void Batch() {}
	*/

};

#endif // _DETOURS_H_
