#pragma once
#include "edt_document_creator.h"
#include <snk/resource/res_tag.h>
#include <string>
#include <string_view>
#include <utility>

namespace edt
{
	// A prefab variant (#151, EPIC-25 US-25-6): a new `<name>.prefab.desc` next to its base that holds nothing but `__parent`, the base's
	// tag. What the variant changes is written into it later, as records of its own layer (reduce_to_parent); a variant of a variant is
	// a chain of such files. The file is new (never over a file of any layer) and lands in the project's own top layer, like a
	// document made from the asset browser.
	class prefab_variant_creator
	{
	public:
		explicit prefab_variant_creator(document_creator::ports wired) : m_ports(std::move(wired)), m_names(m_ports) {}

		// "res://a/b/hero.prefab.desc" -> "hero_variant"; a taken name gets a number ("hero_variant2", ...). Empty when `base` is no prefab file.
		std::string suggested_name(const res::tag& base) const;

		// Why `stem` cannot be the name of a variant of `base` (the file is `<stem>.prefab.desc` in the base's folder): empty when it can.
		std::string check(const res::tag& base, std::string_view stem) const;

		// Writes `{"__type": "prefab_desc", "__version", "__parent": <base>}` and nothing else. `stem` empty: the suggested name.
		document_creation create(const res::tag& base, std::string_view stem) const;

		static constexpr std::string_view file_suffix = ".prefab.desc";

	private:
		// The folder of `base` below res:// ('/' separated, empty = the root) and its name without the suffix; false when `base` is not a res:// prefab file.
		static bool split(const res::tag& base, std::string& folder, std::string& stem);

		document_creator::ports m_ports;
		document_creator m_names;
	};
}
