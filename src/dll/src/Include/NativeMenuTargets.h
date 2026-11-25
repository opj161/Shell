#pragma once

/*
	Which native submenus a parent-moving rule actually needs.

	docs/refactor/04-code-health.md section 6. Today a `modify` rule that both
	moves an item to another parent and names a `location` forces
	`NativeTreePolicy::LegacyEager`: every submenu of the host's menu is sent
	`WM_INITMENUPOPUP` and enumerated before the first pixel, because a rule
	that talks about descendants cannot be evaluated until the descendants
	exist.

	What that costs was measured before any of this was written, because the
	plan asserted it mattered without a number. Same machine, same
	configuration, forced through `modify.native_eager` (Windows 11 26200.8875
	x64, 2026-08-24, six menus each):

		Lazy               ~13 ms pre-display warm, ~60 ms on the first menu
		LegacyEager        95.1 ms average, 31.7 - 359.6 ms

	One submenu alone accounted for 22.2 ms of a 33.7 ms menu - a third-party
	handler populating six items. So the eager policy is worth avoiding, and
	the plan's estimate of the prize was right.

	**The classification happens at menu time, not at config publish.** The plan
	proposed classifying each rule's `location` when the configuration is
	parsed, splitting it into "deterministic" and "dynamic/wildcard". That needs
	a way to ask an Expression whether it is constant, and it throws away the
	selection - which the rule's own `where` and `fso` already depend on. By the
	time the policy is chosen, `ContextMenu::Initialize` has the context, the
	selection and the rule list in front of it, so the locations can simply be
	*evaluated*. A rule that does not survive that evaluation, or that names a
	wildcard, falls the whole menu back to LegacyEager - which is the plan's
	"conservative default; costs latency, not correctness".

	**Both ends of a move are targets.** A `location` says which submenu's
	children the rule matches against; a `moveto` says which submenu an item is
	moved *into*, and that destination is resolved through
	`__map_system_menu[path.hash()]`, which is only populated for levels that
	were enumerated. Collecting only the sources would move items into a
	destination that had not been materialised yet, and they would land in
	`__movable_system_items` instead - a silent behaviour change, which is the
	failure QA-11 is about.

	**The wildcard set is exactly one string.** Read `is_location` in
	ContextMenu.cpp closely rather than assuming: it strips one leading
	asterisk from `**...`, returns true for a location of exactly `*`, and
	otherwise compares the path for equality. So `*` matches every level and
	nothing else does - `*foo` is compared literally, and would only match a
	submenu genuinely called `*foo`. Mirroring that exactly is the point; a
	classifier that treated any leading asterisk as a wildcard would be
	*correct* (it would fall back to eager) but would give up the optimisation
	on locations that are in fact literal.
*/

#include <string>
#include <vector>
#include <algorithm>

namespace Nilesoft
{
	namespace Shell
	{
		/*
			The set of submenu paths this menu has to materialise.

			Paths are the same shape `menuitem_t::path` uses - lowercase
			segments joined by '/', no leading or trailing separator - because
			that is what the rules are compared against.
		*/
		class NativeTargets
		{
		public:
			// Exactly what is_location() treats as matching every level.
			static bool is_wildcard(const std::wstring &location)
			{
				return trim(location) == L"*";
			}

			// Normalises the way is_location does before comparing: outer
			// whitespace, then '/' at either end, then one leading asterisk of a
			// '**' pair.
			static std::wstring normalize(const std::wstring &location)
			{
				auto value = trim(location);

				size_t begin = 0;
				size_t end = value.size();
				while(begin < end && value[begin] == L'/')
					begin++;
				while(end > begin && value[end - 1] == L'/')
					end--;
				value = value.substr(begin, end - begin);

				if(value.size() >= 2 && value[0] == L'*' && value[1] == L'*')
					value.erase(0, 1);

				return lower(value);
			}

			// A location that is empty means "the root", which needs no
			// descendant at all - the caller has already excluded those, but
			// saying so here keeps the set free of a path that matches
			// everything by being a prefix of everything.
			void add(const std::wstring &location)
			{
				auto path = normalize(location);
				if(path.empty())
					return;
				if(std::find(_paths.begin(), _paths.end(), path) == _paths.end())
					_paths.push_back(path);
			}

			bool empty() const { return _paths.empty(); }
			size_t size() const { return _paths.size(); }

			/*
				Should the submenu at `path` be initialised and enumerated?

				Yes when it is a target, and yes when it is an *ancestor* of one -
				there is no way to reach `a/b/c` without opening `a` and `a/b`
				first.

				The prefix test is on whole segments. "open" must not be treated
				as an ancestor of "open with": they are siblings whose names
				share a prefix, and descending into every such sibling would give
				back most of what this exists to save.
			*/
			bool needs(const std::wstring &path) const
			{
				auto candidate = lower(trim(path));
				if(candidate.empty())
					return false;

				for(const auto &target : _paths)
				{
					if(target == candidate)
						return true;

					// candidate is an ancestor of target: target starts with
					// candidate followed by a separator.
					if(target.size() > candidate.size()
					   && target.compare(0, candidate.size(), candidate) == 0
					   && target[candidate.size()] == L'/')
						return true;
				}
				return false;
			}

			const std::vector<std::wstring> &paths() const { return _paths; }

		private:
			static std::wstring trim(const std::wstring &value)
			{
				size_t begin = 0;
				size_t end = value.size();
				auto space = [](wchar_t c)
				{
					return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
				};
				while(begin < end && space(value[begin]))
					begin++;
				while(end > begin && space(value[end - 1]))
					end--;
				return value.substr(begin, end - begin);
			}

			static std::wstring lower(const std::wstring &value)
			{
				std::wstring out = value;
				for(auto &c : out)
				{
					if(c >= L'A' && c <= L'Z')
						c = static_cast<wchar_t>(c - L'A' + L'a');
				}
				return out;
			}

			std::vector<std::wstring> _paths;
		};
	}
}
