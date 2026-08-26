#include <pch.h>
#include "Include/ExplorerCommandCatalog.h"
#include "Include/PackageCatalogService.h"
#include "Include/ProviderHealth.h"
#include "Include/ProviderSchedule.h"
#include "Include/ProviderCache.h"
#include "Include/ProviderCall.h"
#include "Include/ExplorerCommandState.h"
#include "Include/ProviderQuarantineStore.h"
#include "Include/IconResource.h"
#include "Include/Diagnostics/DiagnosticsRing.h"
#include "Include/Diagnostics/MenuPerf.h"
#include "Include/ContextMenu.h"

#include <shobjidl.h>
#include <memory>
#include <cstdint>

namespace Nilesoft
{
	namespace Shell
	{
		namespace
		{
			// The scan, its TTL and its cache moved to PackageCatalogService.
			// It used to live here behind a 30 s TTL that whichever caller found
			// expired paid for itself, on the menu thread: 244 manifests read
			// and parsed, measured at 111 ms cold on this machine. See
			// Include/PackageCatalogService.h.

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

			// Extraction moved to Include/IconResource.h, where the ownership it
			// creates is expressed by a type rather than left to a raw HBITMAP
			// that nobody deleted. See the note there for the leak this was.

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

			/*
				The provider cache moved to Include/ProviderCache.h, with the
				reference-counting rules it enforces and the lifetime rule for a
				host whose thread Shell does not own. Two defects came out of it
				being a static thread_local vector and three free functions in
				this anonymous namespace: the caller's reference was leaked on
				the path that succeeded, and nothing ever released the cache.
				Neither was reachable from a test, because a reference count was
				exactly what could not be observed.
			*/
			void provider_add_ref(IExplorerCommand *cmd) { cmd->AddRef(); }
			void provider_release(IExplorerCommand *cmd) { cmd->Release(); }

			const ProviderComApi &default_provider_api()
			{
				static const ProviderComApi api
				{
					&activate_explorer_command,
					&provider_add_ref,
					&provider_release,
				};
				return api;
			}

			ProviderCache &provider_cache()
			{
				static thread_local ProviderCache cache(default_provider_api());
				return cache;
			}

			// Why an item did or did not end up in the menu.
			//
			// "It declined to appear" and "it errored" used to be the same
			// answer, which was harmless while every menu activated a fresh
			// object and stopped being harmless the moment they are reused: half
			// the providers on this machine legitimately return no title for a
			// given selection, and dropping a cached object for that would
			// re-activate them on every single right-click - exactly the cost
			// the cache exists to remove.
			enum class FillResult
			{
				Shown,
				Hidden,		// a live provider that has nothing to offer here
				Failed,		// the call itself did not succeed
			};

			// `state_pending`, when given, is set for a handler that answered
			// E_PENDING: the item is still shown, provisionally enabled, but the
			// report says so. ProviderResult::Pending existed and was emitted
			// nowhere. docs/refactor/09-remediation-plan.md finding P.
			FillResult fill_menuitem_from_explorer_command(menuitem_t *item,
				IExplorerCommand *cmd, IShellItemArray *selection,
				bool *state_pending = nullptr)
			{
				if(state_pending)
					*state_pending = false;

				if(!item || !cmd)
					return FillResult::Failed;

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
				//
				// Every other failure omits the item, which the same section has
				// always said and this function did not do: it fell through to
				// GetTitle carrying the ECS_ENABLED initialiser above, so a
				// handler whose state call failed outright still got an enabled
				// item and was recorded as having succeeded. The rule is in
				// Include/ExplorerCommandState.h so it can be tested without a
				// handler. docs/refactor/09-remediation-plan.md finding P.
				auto classified = classify_command_state(cmd->GetState(selection, FALSE, &state),
														 state);
				state = classified.state;

				switch(classified.verdict)
				{
					case CommandStateVerdict::Hidden:
						return FillResult::Hidden;
					case CommandStateVerdict::Failed:
						return FillResult::Failed;
					case CommandStateVerdict::Pending:
						if(state_pending)
							*state_pending = true;
						break;
					case CommandStateVerdict::Show:
						break;
				}

				LPWSTR title = nullptr;
				auto hr_title = cmd->GetTitle(selection, &title);
				if(FAILED(hr_title) || !title || !*title)
				{
					if(title) ::CoTaskMemFree(title);
					// A provider that answers with no title is choosing not to
					// appear for this selection; one whose call failed is a
					// different thing, and only the second is a reason to stop
					// reusing the object.
					return FAILED(hr_title) ? FillResult::Failed : FillResult::Hidden;
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
					return FillResult::Shown;
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
					// The item owns this one, unlike a native item's bitmap
					// which is borrowed from the host's MENUITEMINFO.
					item->image = icon_bitmap_from_resource(spec.c_str()).release();
					item->image_owned = item->image != nullptr;
				}
				else if(icon)
					::CoTaskMemFree(icon);

				item->explorer_command = cmd;
				item->explorer_command_owned = true;
				return FillResult::Shown;
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

			/*
				The rebuild, and it gets a phase of its own.

				docs/refactor/02-first-paint-latency.md section 3: "add an
				assertion/log when the SHParseDisplayName fallback path runs -
				it should be rare after capture-first selection. Any occurrence
				on the menu thread is a diagnostics event, not silence."

				It was silence, and it was not rare. Both selection providers
				are handed an IShellItemArray by the host, and only one of them
				kept it, so every Explorer menu rebuilt one path at a time -
				measured at ~420 ms of a ~450 ms menu over 200 files, hidden
				inside `explorer.commands` because nothing named it.

				Now it is named. A report showing `selection.rebuild_array` is a
				host whose view gave Shell no array, which is a real case
				(a background click has no selection to reuse) but should never
				again be the common one without somebody noticing.
			*/
			Diagnostics::MenuPerfScope perf(L"selection.rebuild_array");

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

			perf.annotate(static_cast<long>(paths.size()));

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
				// Sub-commands come from the parent's enumerator rather than from
				// the catalog, so they are not cached and not budgeted - this
				// runs when the user opens the submenu, which is after first
				// paint and after they have asked for it.
				if(fill_menuitem_from_explorer_command(child.get(), child_cmd, selection)
					== FillResult::Shown)
				{
					if(child->is_menu())
						child->native_popup.materialized = false;
					else
						child->native_popup.materialized = true;

					// The same ownership rule as the catalog path above, and
					// the same reason it has to be written out: the item takes
					// this reference only on the branch that stores the
					// pointer. An ECF_ISSEPARATOR child returns Shown from an
					// earlier branch that stores nothing, and the reference
					// IEnumExplorerCommand::Next handed over is then still
					// ours to drop.
					if(!child->explorer_command_owned
					   || child->explorer_command != child_cmd)
						child_cmd->Release();

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

			// O(1). Queues a refresh if the catalog is past its TTL and serves
			// the previous one meanwhile; only the very first menu in a process
			// can wait here, and only for a bounded budget.
			auto catalog = PackageCatalogService::instance().snapshot_for_menu();
			if(!catalog)
				return;
			const auto &regs = catalog->commands;

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

			// One allowance for the whole menu, spent between providers. Nothing
			// interrupts a call that is already running - see the note in
			// Include/ProviderHealth.h about why these stay on this thread - but
			// a menu with twenty slow handlers now costs one overrun instead of
			// twenty.
			auto budget = ProviderBudget::begin();
			auto &health = ProviderHealth::instance();

			// How much selection these handlers are about to be asked about.
			// A provider's remembered cost is keyed by this as well as by its
			// CLSID, because a time measured against one file predicts nothing
			// about two hundred - measured at 209 ms for one handler and 634 ms
			// for the menu on 2026-08-25. Include/ProviderHealth.h.
			auto shape = selection_shape(Selected.Count());

			/*
				Three passes, and the separation is the point.

				One pass in registration order is what produced the failure this
				replaces: the budget is spent as the walk goes, so four providers
				costing 0.3-1.6 ms lost their place in a menu because they sat
				*after* one costing 13.9 ms. Measured on 2026-08-25 against the
				deployed build - see the record in Include/ProviderSchedule.h.

				  1. Plan. Read what is remembered; activate nothing; mutate no
				     re-probe counter. A provider that never gets called must not
				     have been moved one menu closer to a re-probe by the act of
				     considering it.
				  2. Resolve, cheapest known first, with one exploration
				     guaranteed and slow re-probes last. The live budget is still
				     checked before every call: a prediction is a prediction.
				  3. Publish in registration order. Resolution order is not
				     presentation order, and duplicate resolution runs in
				     registration order too, so a cheaper later provider cannot
				     take the first-registration-wins identity from an earlier
				     one. The menu the user sees is unchanged.
			*/
			struct PlannedProvider
			{
				const ExplorerCommandRegistration *reg{};
				std::unique_ptr<menuitem_t> item;
				GUID canonical{};
			};

			std::vector<ProviderCandidate> candidates;
			std::vector<PlannedProvider> planned;
			candidates.reserve(regs.size());
			planned.reserve(regs.size());

			for(const auto &reg : regs)
			{
				if(!explorer_command_matches_any(reg, kinds))
					continue;

				ExplorerCommandIdentity by_clsid;
				by_clsid.clsid = reg.clsid;
				if(explorer_command_already_represented(by_clsid, accepted))
					continue;

				// The same CLSID registered twice is one handler, and asking it
				// twice costs the menu twice for one item. Previously this fell
				// out of `accepted` growing inside the single loop; with the
				// walk split in two it has to be said.
				bool already_planned = false;
				for(const auto &p : planned)
				{
					if(p.reg && ::IsEqualGUID(p.reg->clsid, reg.clsid))
					{
						already_planned = true;
						break;
					}
				}
				if(already_planned)
					continue;

				auto hash = provider_hash(reg.clsid);

				// Identity first, before any decision that might skip this
				// provider. The report prints the CLSID and `-quarantine:add`
				// takes it, so recording it only where a *title* was obtained
				// left the two cases a user most wants to act on - a handler
				// that cost 70 ms and returned no title, and every provider
				// Shell deferred without asking - printing a bare hash the
				// treatment command does not accept.
				// docs/refactor/05-capabilities.md section 1c.
				Diagnostics::provider_identity(hash, reg.clsid);

				// Quarantine is checked before health, and before anything is
				// activated: the whole point is that this provider costs the
				// menu nothing at all. It is the user's declaration rather than
				// Shell's measurement, so it is reported with its own word and
				// never re-probed the way a deferral is.
				// docs/refactor/05-capabilities.md section 1b.
				if(ProviderQuarantineStore::instance().contains(hash))
				{
					Diagnostics::session_provider(hash, 0, Diagnostics::ProviderResult::Quarantined);
					continue;
				}

				ProviderCandidate candidate;
				candidate.ordinal = static_cast<uint32_t>(planned.size());
				candidate.hash = hash;

				ProviderTiming timing;
				if(health.classify(hash, shape, &timing))
				{
					candidate.has_timing = true;
					candidate.samples = timing.samples;
					candidate.best_us = timing.best_us;
					candidate.last_us = timing.last_us;

					// Judged on its *best* ever time and never before it has
					// answered twice, both because the first menu in a process
					// is cold and makes every handler look pathological. See
					// MIN_SAMPLES_TO_JUDGE in Include/ProviderHealth.h.
					if(timing.samples >= MIN_SAMPLES_TO_JUDGE
					   && timing.best_us > SLOW_PROVIDER_US)
					{
						candidate.slow = true;
						candidate.reprobe_due =
							static_cast<uint32_t>(timing.since_probe) + 1 >= REPROBE_AFTER;
					}

					// The other way a provider stops being called, and the one
					// with no exit until this existed: refused for predicted
					// cost, menu after menu, while the prediction that refuses
					// it can only be corrected by calling it. See the note in
					// plan_providers.
					candidate.budget_reprobe_due =
						static_cast<uint32_t>(timing.budget_deferrals) + 1
							>= BUDGET_REPROBE_AFTER;
				}

				candidates.push_back(candidate);

				PlannedProvider entry;
				entry.reg = &reg;
				planned.push_back(std::move(entry));
			}

			auto plan = plan_providers(candidates, health.next_exploration_cursor());

			uint32_t refused_for_budget = 0;

			for(const auto &step : plan.order)
			{
				// A prediction is checked against what is actually left, not
				// against what was left when the plan was made. One call can
				// still overrun - nothing interrupts a call already running, see
				// the note in Include/ProviderHealth.h - so this is what keeps
				// an overrun from costing the providers behind it.
				if(!provider_step_fits(step, budget.remaining_us()))
				{
					health.note_budget_deferral(step.hash, shape);
					Diagnostics::session_provider(step.hash, 0,
												  Diagnostics::ProviderResult::DeferredBudget);
					refused_for_budget++;
					continue;
				}

				// Granted before the call rather than after it, so a probe that
				// is started and then fails does not leave the provider due
				// forever.
				if(step.call == ProviderCall::Reprobe)
					health.note_reprobe_started(step.hash, shape);

				auto &slot = planned[step.ordinal];
				const auto &reg = *slot.reg;

				auto spent_before = budget.spent_us();

				// Borrowed for this scope: the reference the caller has to drop
				// is released when `cmd` goes out of scope, on every path.
				// Releasing it by hand is what the Shown path below used to
				// forget, leaking one reference per provider per menu.
				auto cmd = provider_cache().borrow(reg.clsid);
				if(!cmd)
				{
					health.record(step.hash, shape, budget.spent_us() - spent_before, false);
					Diagnostics::session_provider(step.hash, budget.spent_us() - spent_before,
												  Diagnostics::ProviderResult::Failed);
					continue;
				}

				std::unique_ptr<menuitem_t> item(new menuitem_t);
				item->parent = root;
				item->is_toplevel = true;
				item->wid = ident.get_id();
				// Recorded from the registration, not from the activation, for
				// the same reason Diagnostics::provider_identity is above: it is
				// knowable without asking the handler anything, and it is what
				// this item is called next time. src/shared/MenuIdentity.h.
				item->explorer_clsid = reg.clsid;

				/*
					The fill and the canonical-name read are one measured span,
					and Include/ProviderCall.h is where the rule that they are
					lives. GetCanonicalName used to sit *after* health.record and
					session_provider, so every remembered and reported provider
					cost excluded it while the whole-menu budget charged it - the
					gap section 09 2.1 read as Shell's own pre-paint work.

					`spent_before` was taken above, before the cache was asked,
					so activation stays inside the cost.
				*/
				bool state_pending = false;
				auto filled = FillResult::Failed;

				auto measured = measure_provider_call(
					spent_before,
					[&budget] { return budget.spent_us(); },
					cmd.get(),
					[&]
					{
						filled = fill_menuitem_from_explorer_command(
							item.get(), cmd.get(), selection, &state_pending);
						return filled == FillResult::Shown;
					},
					&slot.canonical);

				/*
					Ownership, and the reason this is not a Release.

					On the path that shows a command, the *item* takes the
					caller's reference: fill_menuitem_from_explorer_command
					stores the pointer and sets explorer_command_owned, and
					menuitem_t::~menuitem_t releases it. That is why the
					pre-W8 loop released only on Hidden and Failed - it was a
					transfer, not a leak, and reading it as one is what put a
					double-release into a menu whose items are still asked for
					EnumSubCommands and Invoke afterwards.

					Keyed on what the item actually took rather than on
					FillResult::Shown, because the two are not the same: an
					ECF_ISSEPARATOR command returns Shown from an earlier
					branch that stores no pointer. That path used to leak the
					reference outright, and now releases it with the handle.
				*/
				if(item->explorer_command_owned && item->explorer_command == cmd.get())
					(void)cmd.detach();

				auto cost = measured.cost_us;
				health.record(step.hash, shape, cost, filled != FillResult::Failed);
				Diagnostics::session_provider(step.hash, cost,
					filled == FillResult::Failed ? Diagnostics::ProviderResult::Failed
					: state_pending				 ? Diagnostics::ProviderResult::Pending
												 : Diagnostics::ProviderResult::Ok);

				// The handler's own title, for the report's name directory.
				// This is the one place it exists: GetTitle takes the selection
				// and is answered by the handler, so nothing outside a host
				// that has built a menu can resolve a CLSID to it. Written once
				// per distinct provider per process.
				// docs/refactor/05-capabilities.md section 1.
				if(!item->title.empty())
					Diagnostics::provider_name(step.hash, reg.clsid, item->title.c_str());

				if(filled != FillResult::Shown)
				{
					// Hidden is a live provider declining this selection, and
					// happens constantly - it stays cached. Failed means a call
					// that used to work did not, so this thread stops reusing
					// that object; a fresh activation next time costs 2 ms and
					// beats asking a wedged provider forever.
					if(filled == FillResult::Failed)
					{
						// Two references exist here and both have to go: this
						// scope's, which `cmd` owns, and the cache's, which
						// forget() drops. Either order is correct - borrow()
						// took a reference for the handle, so the object cannot
						// be released out from under it - and this one is
						// written first because it is the order the two counts
						// read in: 2 -> 1 -> 0.
						cmd.reset();
						provider_cache().forget(reg.clsid);
					}
					continue;
				}

				// slot.canonical was read above, inside the measured cost.
				slot.item = std::move(item);
			}

			/*
				And only now the ones that were passed over before the menu
				started.

				This ran before the resolution loop, which decided which
				providers survive the telemetry caps. MAX_PROVIDERS and
				PERF_EXPORT_PROVIDERS are both 32; this machine has 55 distinct
				packaged context-menu CLSIDs, measured by walking every installed
				package's AppxManifest.xml, and section 09 R7.5 puts a file menu
				at 37. The cap bites here routinely.

				Writing the whole deferral batch first guaranteed a slot to every
				stable-verdict deferral - the records that say the least, because
				a provider deferred for being slow is deferred for the same
				reason on every menu - and evicted the tail of the resolution
				order, which is the most expensive known providers, the unsampled
				ones and the re-probes. Exactly what a person reads the report to
				find.

				Outcomes claim their slots first now. The deferral records are
				unchanged in content and in count; only the order in which they
				are offered to a fixed-size array has moved.
			*/
			// Passed over before the menu even started. Charged here, once, for
			// the providers that really were passed over - and not for the ones
			// the planner merely looked at.
			//
			// Two reasons reach this list now, and they are charged
			// differently. Slow moves the provider one menu closer to a
			// REPROBE_AFTER re-probe. Budget means the planner refused to
			// schedule a Known step whose prediction no menu could satisfy; that
			// is the same refusal the resolution loop below would have made, so
			// it is charged the same way and counts the same way, and the
			// provider accrues toward a forced turn instead of toward nothing.
			for(const auto &skipped : plan.deferred)
			{
				if(skipped.why == ProviderDeferral::Budget)
				{
					health.note_budget_deferral(skipped.hash, shape);
					Diagnostics::session_provider(skipped.hash, 0,
												  Diagnostics::ProviderResult::DeferredBudget);
					refused_for_budget++;
					continue;
				}

				health.note_slow_deferral(skipped.hash, shape);
				Diagnostics::session_provider(skipped.hash, 0,
											  Diagnostics::ProviderResult::DeferredSlow);
			}

			/*
				A menu that did not fit, said out loud, once.

				Recorded only when at least one provider was actually refused, so
				it is a signal rather than a line on every report. The
				per-provider `deferred(budget)` records remain the detail; this
				is the summary, and it follows the rule
				docs/refactor/08-handoff.md section 1 already states: when adding
				a cap, print its overflow in the same commit.

				Not to be confused with `dropped_providers`, which means the
				fixed-size telemetry array overflowed - a fact about the report,
				not about the menu.
			*/
			if(refused_for_budget)
			{
				Diagnostics::MenuPerfScope over(L"explorer.commands.over_budget");
				over.annotate(static_cast<long>(refused_for_budget));
			}

			/*
				What this thread may keep, and for how long.

				Selected.loader.contextmenuhandler is set when Shell was
				reached through IShellExtInit/IContextMenu - a third-party file
				manager's own thread, whose lifetime Shell knows nothing
				about. Holding COM references on such a thread until process
				exit is the leak D9 names, and a thread-local destructor is
				not the fix: it can run after that thread's CoUninitialize.

				Here is the point where the apartment is provably still live -
				same thread, inside the menu path, before anything unwinds.
				Include/ProviderCache.h has the rule and what it costs.
			*/
			provider_cache().end_of_menu(
				Selected.loader.contextmenuhandler ? ProviderLifetime::ThisMenuOnly
												   : ProviderLifetime::AcrossMenus);

			// Registration order, for both what is shown and which of two
			// providers claiming the same identity wins it.
			for(auto &slot : planned)
			{
				if(!slot.item)
					continue;

				auto item = std::move(slot.item);

				ExplorerCommandIdentity identity;
				identity.hash = item->hash;
				identity.type = item->type;
				identity.clsid = slot.reg->clsid;
				identity.canonical = slot.canonical;
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
