#pragma once

/*
	A name for a menu item that is still the same name tomorrow.

	docs/refactor/05-capabilities.md section 6 asks for exactly this and says
	why: favorites must "persist identities, never session wIDs". A wID is
	handed out by `ident.get_id()` while a menu is being composed, so it is a
	different number for the same command on the next right-click. Anything
	remembered against one is remembered against nothing.

	Three kinds, because there are three ways an item gets into Shell's menu and
	they have nothing in common:

	  verb    a packaged IExplorerCommand. Identity is its CLSID, which is
	          already the identity `shell.exe -quarantine:add` takes and the one
	          the ring's provider records carry.
	  item    an NSS rule built it.
	  native  the host's own item, mirrored by Shell.

	The kind is part of the identity rather than a field beside it. A
	configuration that adds its own "Delete" next to the shell's is not
	ambiguous - they are two items - and an identity scheme that collapsed them
	would pin one and promote the other.

	## The departure from section 6, and why

	Section 6 specifies "NSS rule id (file + rule ordinal hash)" for a custom
	item. This uses the same parent-path-plus-normalized-title signature as a
	native item instead, and the reason is what this product is:

	**People edit `shell.nss` constantly.** The config watcher
	(docs/refactor/03-config-safety.md section 3) exists because they do, and it
	reloads on every save. A rule identified by where it sits in a file changes
	identity when a line is added above it - so inserting one item at the top of
	a configuration would silently un-pin everything below it, on the save. That
	is not a rare event for this audience; it is Tuesday.

	A title signature has the opposite failure - renaming an item loses its
	pin - which is both rarer and *legible*: the user renamed the thing, and the
	favorite following the old name would be the surprising outcome.

	Provenance is not wasted by this choice, and is not an alternative to it.
	Include/RuleProvenance.h records file and line on every rule for the
	inspector (section 7), which wants to point at the rule a person should go
	and edit. That is a question about *this* menu, answered while the
	generation is still live. Identity is a question about menus that have not
	happened yet.

	## Why a string

	It is written to a file a person is expected to read and edit, so it has to
	survive a round trip through a text editor with its meaning intact. A packed
	binary key would be smaller and would make the file a blob. The menu path
	does not compare strings anyway - `hash` is what it matches on, and the text
	is carried for the file and for the report.

	## Case

	Matching is case-insensitive and the text is stored as it was first seen.
	`MenuItemInfo::path` is already lowercased where it is built, but
	`title.normalize` keeps the case the configuration or the handler used, and
	a favorite must not be lost because a handler retitled "Open With" as "Open
	with". `hash_identity` lowercases as it hashes rather than lowercasing the
	stored text, so `-favorites list` prints what the user would recognise.
*/

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace Nilesoft
{
	namespace Shell
	{
		namespace MenuIdentity
		{
			enum class Kind : uint8_t
			{
				// Not an identity. What an unparseable line, or an item whose
				// origin cannot be established, answers with.
				Unknown,

				// A packaged IExplorerCommand, signed by its CLSID.
				Verb,

				// Built by an NSS rule.
				Item,

				// The host's own item, mirrored by Shell.
				Native,
			};

			inline constexpr const wchar_t *kind_name(Kind kind) noexcept
			{
				switch(kind)
				{
					case Kind::Verb:   return L"verb";
					case Kind::Item:   return L"item";
					case Kind::Native: return L"native";
					default:           return L"?";
				}
			}

			inline Kind kind_from_name(std::wstring_view name) noexcept
			{
				if(name == L"verb")   return Kind::Verb;
				if(name == L"item")   return Kind::Item;
				if(name == L"native") return Kind::Native;
				return Kind::Unknown;
			}

			// An identity longer than this is refused rather than truncated.
			// A truncated identity is a *different* identity that looks like
			// the one it came from, which is the one failure this must not
			// have: it would silently match the wrong item. Titles run long -
			// third-party handlers cross MAX_PATH routinely, which is why
			// Include/MenuText.h exists - so the cap is generous.
			inline constexpr size_t MaxLength = 512;

			/*
				FNV-1a over the lowercased identity.

				The same function the quarantine list uses over GUID bytes, run
				over characters instead. Lowercasing happens here rather than in
				the stored text so the file keeps the spelling a person would
				recognise - see the note on case in the header.

				towlower is not used: it is locale-dependent, and an identity
				written on a machine with a Turkish locale would then hash
				differently from the same identity read on an English one. The
				ASCII fold is what `string::equals(..., true)` does elsewhere in
				this tree for the same reason.
			*/
			inline uint32_t hash_identity(std::wstring_view text) noexcept
			{
				uint32_t h = 2166136261u;
				for(wchar_t c : text)
				{
					if(c >= L'A' && c <= L'Z')
						c = static_cast<wchar_t>(c - L'A' + L'a');

					h ^= static_cast<uint32_t>(c & 0xFF);
					h *= 16777619u;
					h ^= static_cast<uint32_t>((c >> 8) & 0xFF);
					h *= 16777619u;
				}
				return h;
			}

			struct Identity
			{
				Kind kind{ Kind::Unknown };

				// "verb:{CAE3F1D4-...}", "item:tools/terminal", "native:view".
				std::wstring text;

				// hash_identity(text). Zero only for an empty identity, which
				// is what `valid()` is really asking about, but it is kept as a
				// separate test so a hash that legitimately came out zero
				// cannot read as "no identity".
				uint32_t hash{};

				bool valid() const noexcept { return kind != Kind::Unknown && !text.empty(); }
			};

			/*
				Build one from a kind and a signature.

				The signature for a verb is its CLSID in braced form; for an
				item or a native it is the parent path, a slash, and the
				normalized title - which is exactly the string
				`apply_system_modify_rules` already compares against when it
				matches a rule to an item (`ContextMenu.cpp`, the
				`x->path.equals((item->path + L"/" + item->title.normalize))`
				test). Reusing it is deliberate: an identity that normalized
				differently from the rules would disagree with the menu about
				which item it names.

				A signature with no useful content yields an invalid identity
				rather than "item:", which would be one identity shared by every
				untitled item on the machine.
			*/
			inline Identity make(Kind kind, std::wstring_view signature)
			{
				Identity out;
				if(kind == Kind::Unknown || signature.empty())
					return out;

				std::wstring text;
				text.reserve(signature.size() + 8);
				text += kind_name(kind);
				text += L':';
				text += signature;

				if(text.size() > MaxLength)
					return out;

				out.kind = kind;
				out.hash = hash_identity(text);
				out.text = std::move(text);
				return out;
			}

			/*
				A packaged verb signs as its CLSID, in the braced upper-case
				form the rest of this product prints.

				Formatted here rather than by calling Quarantine::format_guid,
				because an identity header that depended on the quarantine
				header would have the dependency the wrong way round - identity
				is what quarantine names things *with*. The duplication is a
				real cost and it is paid down by a test rather than by a
				comment: test_menu_identity asserts the two produce the same
				string for the same GUID, so they cannot drift apart silently.

				wsprintfW rather than swprintf_s: this header is included by
				shell.exe's validator path, which deliberately keeps its import
				table small, and user32 is already there.
			*/
			inline std::wstring verb_signature(const GUID &clsid)
			{
				wchar_t buffer[64]{};
				::wsprintfW(buffer, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
							clsid.Data1, clsid.Data2, clsid.Data3,
							clsid.Data4[0], clsid.Data4[1], clsid.Data4[2], clsid.Data4[3],
							clsid.Data4[4], clsid.Data4[5], clsid.Data4[6], clsid.Data4[7]);
				return buffer;
			}

			/*
				A view over a C string that may not be there.

				`Nilesoft::string::c_str()` answers **nullptr** when the string
				is empty - `return valid() ? m_data : nullptr` - and
				`std::wstring_view`'s pointer constructor calls
				`char_traits::length` on what it is given, so building one
				straight from it faults on every empty string.

				A top-level menu item has an empty `path`, so that is not an
				edge case: it is the first item of every menu. It cost a real
				Explorer wedge to find, and it is in AGENTS.md with the rest of
				that family.
			*/
			inline std::wstring_view view(const wchar_t *text) noexcept
			{
				return text ? std::wstring_view(text) : std::wstring_view();
			}

			// Join a parent path and a normalized title the way the menu does:
			// slash-separated, with no leading or trailing slash. A top-level
			// item has an empty path and signs as its title alone.
			//
			// Takes pointers rather than views because every caller has a
			// `Nilesoft::string`, and that is exactly where the null above
			// comes from - taking views here would move the trap to the call
			// site instead of removing it.
			inline std::wstring signature_of(const wchar_t *parent_path_text,
											 const wchar_t *normalized_title_text)
			{
				auto parent_path = view(parent_path_text);
				auto normalized_title = view(normalized_title_text);

				std::wstring out;
				out.reserve(parent_path.size() + normalized_title.size() + 1);

				auto trim_slashes = [](std::wstring_view s)
				{
					while(!s.empty() && (s.front() == L'/' || s.front() == L'\\'))
						s.remove_prefix(1);
					while(!s.empty() && (s.back() == L'/' || s.back() == L'\\'))
						s.remove_suffix(1);
					return s;
				};

				parent_path = trim_slashes(parent_path);
				normalized_title = trim_slashes(normalized_title);

				out += parent_path;
				if(!out.empty() && !normalized_title.empty())
					out += L'/';
				out += normalized_title;
				return out;
			}

			/*
				Read one back.

				Everything after the first colon is the signature, including any
				further colons - a Windows path in a title has one, and so does
				every drive letter. Splitting on the last colon, or on all of
				them, would quietly rewrite those identities.
			*/
			inline Identity parse(std::wstring_view text)
			{
				Identity out;

				while(!text.empty() && (text.front() == L' ' || text.front() == L'\t'))
					text.remove_prefix(1);
				while(!text.empty() && (text.back() == L' ' || text.back() == L'\t'
										|| text.back() == L'\r'))
					text.remove_suffix(1);

				if(text.empty() || text.size() > MaxLength)
					return out;

				auto colon = text.find(L':');
				if(colon == std::wstring_view::npos)
					return out;

				auto kind = kind_from_name(text.substr(0, colon));
				if(kind == Kind::Unknown)
					return out;

				return make(kind, text.substr(colon + 1));
			}
		}
	}
}
