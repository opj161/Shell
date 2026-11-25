#pragma once

/*
	Activated IExplorerCommand providers, kept alive between menus, and the
	rules about how long "between menus" is allowed to be.

	## Why there is a cache at all

	Measured on this machine: CoCreateInstance costs about 2 ms per provider
	even fully warm, 46 ms across the 23 registered here, and that was paid
	again on every single right-click. It is also the one call in the sequence
	that has nothing to do with the selection - GetState, GetTitle and GetIcon
	each take an IShellItemArray parameter, so a live IExplorerCommand is meant
	to be asked again about something else. Keeping them takes a warm menu from
	~170 ms to ~41 ms. See Include/ProviderHealth.h.

	Thread-local, and that is the whole of the apartment story. These are COM
	objects created in the menu thread's apartment, and "interface pointers must
	be marshaled when passed between apartments" - so rather than marshal, each
	thread keeps its own and never lends them out.
	https://learn.microsoft.com/en-us/windows/win32/com/single-threaded-apartments

	## Why it is a class rather than a vector and two functions

	Two reference-counting defects, and neither was reachable from a test while
	the cache was a `static thread_local std::vector` and three free functions
	in an anonymous namespace inside ExplorerCommand.cpp.

	**The caller's reference is transferred on the path that succeeds, and
	reading that as a leak is how this file first got it wrong.**
	acquire_explorer_command handed out two references and said so - "one for
	the cache, one for the caller" - and the resolution loop released the
	caller's half only when the provider declined or failed. That looks exactly
	like a leak on the succeeding path and is not one:
	fill_menuitem_from_explorer_command stores the pointer on the menu item and
	sets `explorer_command_owned`, and `menuitem_t::~menuitem_t` releases it.
	The item is the second owner.

	The first version of this class made `BorrowedProvider` release
	unconditionally, which turned that transfer into a **double-release** on
	every shown packaged verb - and the object is still asked for
	EnumSubCommands and Invoke after composition, so it was a use-after-free,
	not merely a miscount. It was caught by the takeover harness (which reaches
	Shell through IShellExtInit/IContextMenu, so it also exercises
	ThisMenuOnly) failing one to five scenarios per run, differing every run.
	The unit tests did not catch it, because the fake models the cache and the
	handle and nothing had taught it that a third owner exists - AGENTS.md, "a
	test that only ever calls a function the way the test calls it".

	So `borrow()` still returns a scope-bound handle, and `detach()` is how the
	one path that gives the reference away says so. The caller keys that on
	what the item actually took, not on FillResult::Shown: an ECF_ISSEPARATOR
	command returns Shown from a branch that stores no pointer, and that path
	really did leak a reference before this.

	**Nothing ever released the cache itself.** The original comment chose
	that deliberately - "a released process is a released process" - which holds
	for Explorer, whose menus come from one long-lived thread, and does not hold
	for a host that raises menus on transient threads. Each such thread leaves
	its providers referenced until process exit, keeping provider DLLs loaded.
	R8 explicitly covers Total Commander, Directory Opus and Everything, so the
	assumption cannot stay implicit.

	## The lifetime rule

	Shell already knows which of the two situations it is in.
	Selected.loader.contextmenuhandler is set when Shell was reached through
	IShellExtInit/IContextMenu - a third-party file manager's own thread, whose
	lifetime Shell knows nothing about - and clear when Explorer handed the menu
	over on the thread it raises every menu from.

	  Lifetime::AcrossMenus   Explorer. Keep the cache; this is the measured
	                          win, and the thread outlives the process's
	                          interest in it.
	  Lifetime::ThisMenuOnly  A third-party host. Release at the end of
	                          composition - on the same thread, inside the menu
	                          path, with the apartment provably still live.

	A naive thread-local destructor would be the obvious alternative and is
	wrong: it can run after the thread's CoUninitialize, and releasing an
	apartment-bound interface then is worse than the leak.
	docs/refactor/12-closure-plan.md Part F.

	The cost of ThisMenuOnly is one re-activation per provider per menu, ~2 ms
	each - the figure the branch has already measured and accepted for an
	uncached provider elsewhere. Stated rather than hidden: a third-party host
	pays for menus that Explorer does not, and the alternative is holding
	references on a thread that may already be gone.

	## The injected table

	Every call the cache makes on a provider goes through ProviderComApi, so a
	test can count references without COM and without a desktop. That is not a
	test-only affordance bolted on: both defects above are reference-counting
	defects, and a reference count is precisely what could not be observed.
*/

#include <windows.h>
#include <shobjidl.h>

#include <vector>

namespace Nilesoft
{
	namespace Shell
	{
		// Everything the cache reaches outside itself.
		struct ProviderComApi
		{
			IExplorerCommand *(*activate)(const GUID &clsid);
			void (*add_ref)(IExplorerCommand *cmd);
			void (*release)(IExplorerCommand *cmd);
		};

		enum class ProviderLifetime
		{
			// Explorer's long-lived menu thread. Providers survive the menu.
			AcrossMenus,

			// A third-party host's thread. Providers do not.
			ThisMenuOnly,
		};

		class ProviderCache;

		/*
			One provider, borrowed for the length of a scope.

			The reference the caller has to drop is the whole of what went
			wrong, so it is not the caller's job any more. Move-only, because
			two of these naming one reference is the same defect wearing a
			different hat.
		*/
		class BorrowedProvider
		{
		public:
			BorrowedProvider() = default;
			BorrowedProvider(const ProviderComApi *api, IExplorerCommand *cmd)
				: _api(api), _cmd(cmd) {}

			BorrowedProvider(const BorrowedProvider &) = delete;
			BorrowedProvider &operator=(const BorrowedProvider &) = delete;

			BorrowedProvider(BorrowedProvider &&other) noexcept
				: _api(other._api), _cmd(other._cmd)
			{
				other._api = nullptr;
				other._cmd = nullptr;
			}

			BorrowedProvider &operator=(BorrowedProvider &&other) noexcept
			{
				if(this != &other)
				{
					reset();
					_api = other._api;
					_cmd = other._cmd;
					other._api = nullptr;
					other._cmd = nullptr;
				}
				return *this;
			}

			~BorrowedProvider() { reset(); }

			IExplorerCommand *get() const noexcept { return _cmd; }
			IExplorerCommand *operator->() const noexcept { return _cmd; }
			explicit operator bool() const noexcept { return _cmd != nullptr; }

			void reset() noexcept
			{
				if(_api && _cmd)
					_api->release(_cmd);
				_api = nullptr;
				_cmd = nullptr;
			}

			/*
				Gives the reference away instead of dropping it.

				The menu item takes ownership of the caller's reference on the
				path that stores the pointer - menuitem_t::~menuitem_t releases
				it when `explorer_command_owned` is set - so on that path the
				handle must stop being an owner without the object losing a
				reference. Releasing there instead is a double-release, and
				what the object is being used for afterwards is
				EnumSubCommands and Invoke.

				Returns the pointer so a caller cannot detach and then have
				nothing to hand over.
			*/
			[[nodiscard]] IExplorerCommand *detach() noexcept
			{
				auto cmd = _cmd;
				_api = nullptr;
				_cmd = nullptr;
				return cmd;
			}

		private:
			const ProviderComApi *_api{};
			IExplorerCommand *_cmd{};
		};

		class ProviderCache
		{
		public:
			explicit ProviderCache(const ProviderComApi &api) : _api(&api) {}

			ProviderCache(const ProviderCache &) = delete;
			ProviderCache &operator=(const ProviderCache &) = delete;

			// Borrowed for one scope. The cache keeps its own reference; the
			// returned handle owns the caller's.
			BorrowedProvider borrow(const GUID &clsid)
			{
				for(auto &entry : _entries)
				{
					if(::IsEqualGUID(entry.clsid, clsid) && entry.cmd)
					{
						_api->add_ref(entry.cmd);
						return BorrowedProvider(_api, entry.cmd);
					}
				}

				auto cmd = _api->activate(clsid);
				if(!cmd)
					return {};

				_api->add_ref(cmd);			// one for the cache, one for the caller
				_entries.push_back({ clsid, cmd });
				return BorrowedProvider(_api, cmd);
			}

			// Drops a provider this thread has stopped trusting - it failed a
			// call it had answered before, so the object may be in a state its
			// author never expected to be reused from. The next menu activates
			// a fresh one.
			void forget(const GUID &clsid)
			{
				for(size_t i = 0; i < _entries.size(); i++)
				{
					if(::IsEqualGUID(_entries[i].clsid, clsid))
					{
						if(_entries[i].cmd)
							_api->release(_entries[i].cmd);
						_entries.erase(_entries.begin() + static_cast<ptrdiff_t>(i));
						return;
					}
				}
			}

			// Called on the owning thread, from inside the menu path, where the
			// apartment is provably still live. Never from a destructor.
			void release_all()
			{
				for(auto &entry : _entries)
				{
					if(entry.cmd)
						_api->release(entry.cmd);
				}
				_entries.clear();
			}

			// What this thread is holding between menus. The number W8 is about.
			size_t size() const noexcept { return _entries.size(); }

			// Applies the lifetime rule for the host that just built a menu.
			// Kept here rather than at the call site so the two situations and
			// their reasons are one thing to read.
			void end_of_menu(ProviderLifetime lifetime)
			{
				if(lifetime == ProviderLifetime::ThisMenuOnly)
					release_all();
			}

		private:
			struct Entry
			{
				GUID clsid{};
				IExplorerCommand *cmd{};
			};

			const ProviderComApi *_api{};
			std::vector<Entry> _entries;
		};
	}
}
