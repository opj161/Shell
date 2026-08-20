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

/*
	One detour transaction, with every other thread in the process enlisted in it.

	Detours rewrites the first instructions of the target function. Microsoft is
	explicit about what that costs threads which are not part of the transaction:
	"Threads not enlisted in the transaction are not updated when the transaction
	commits. As a result, they may attempt to execute an illegal combination of old
	and new code."

	  https://github.com/microsoft/Detours/wiki/DetourUpdateThread

	That is not theoretical here. The one function this codebase detours inline is
	CoCreateInstance, and it does so inside explorer.exe, which has dozens of
	threads and calls it from most of them.

	What was here before enlisted `GetCurrentThread()` and nothing else - which the
	same page documents as doing nothing at all: "If hThread is equal to the current
	threads pseudo handle ... no action is performed and NO_ERROR is returned." So
	no thread was ever enlisted.

	The current thread stays unenlisted on purpose, and by thread id rather than by
	handle comparison: passing a real handle to the current thread "is currently
	unsupported and will result in application hangs".

	Handles are held open until after the commit, because that is when Detours
	reads and rewrites the thread contexts.
*/
class DetourTransaction
{
public:
	DetourTransaction() = default;
	~DetourTransaction() { abort(); }

	DetourTransaction(const DetourTransaction &) = delete;
	DetourTransaction &operator=(const DetourTransaction &) = delete;

	bool begin()
	{
		if(_open)
			return false;

		DetourSetIgnoreTooSmall(TRUE);
		if(DetourTransactionBegin() != NO_ERROR)
			return false;

		_open = true;
		enlist();
		return true;
	}

	bool commit()
	{
		if(!_open)
			return false;

		auto rc = DetourTransactionCommit();
		_open = false;
		release();
		return rc == NO_ERROR;
	}

	void abort()
	{
		if(_open)
		{
			DetourTransactionAbort();
			_open = false;
		}
		release();
	}

private:
	void enlist()
	{
		auto snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if(snapshot == INVALID_HANDLE_VALUE)
			return;

		// The snapshot covers every process, so entries have to be filtered.
		const auto pid = ::GetCurrentProcessId();
		const auto self = ::GetCurrentThreadId();

		THREADENTRY32 entry{};
		entry.dwSize = sizeof(entry);

		if(::Thread32First(snapshot, &entry))
		{
			do
			{
				// th32OwnerProcessID is only present when the entry reaches it.
				if(entry.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID)
								  + sizeof(entry.th32OwnerProcessID))
					continue;

				if(entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
					continue;

				auto thread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
										   | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
										   FALSE, entry.th32ThreadID);
				if(!thread)
					continue;

				if(DetourUpdateThread(thread) == NO_ERROR)
					_threads.push_back(thread);
				else
					::CloseHandle(thread);
			}
			while(::Thread32Next(snapshot, &entry));
		}

		::CloseHandle(snapshot);
	}

	void release()
	{
		for(auto thread : _threads)
			::CloseHandle(thread);
		_threads.clear();
	}

	bool _open = false;
	std::vector<HANDLE> _threads;
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
