#pragma once
#include "edt_desc_file_commands.h"
#include "edt_document_creator.h"
#include <snk/resource/res_tag.h>
#include <functional>
#include <optional>
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
		// `claim`: why a file of that tag may not be made (empty: it may), asked right before the write: the name of the open level, #966.
		explicit prefab_variant_creator(document_creator::ports wired, std::function<std::string(const res::tag&)> claim = {})
			: m_ports(std::move(wired)), m_names(m_ports), m_claim(std::move(claim)) {}

		// "res://a/b/hero.prefab.desc" -> "hero_variant"; a taken name gets a number ("hero_variant2", ...). Empty when `base` is no prefab file.
		std::string suggested_name(const res::tag& base) const;

		// Why `stem` cannot be the name of a variant of `base` (the file is `<stem>.prefab.desc` in the base's folder): empty when it can.
		std::string check(const res::tag& base, std::string_view stem) const;

		// Writes `{"__type": "prefab_desc", "__version", "__parent": <base>, "__layer": 2}` and nothing else (a layer that changes nothing). `stem`
		// empty: the suggested name. Refused: while the project may not be written, for a base that does not load as a prefab, for a name the open
		// level has, for a name taken in any layer.
		document_creation create(const res::tag& base, std::string_view stem) const;

		static constexpr std::string_view file_suffix = ".prefab.desc";

	private:
		// The folder of `base` below res:// ('/' separated, empty = the root) and its name without the suffix; false when `base` is not a res:// prefab file.
		static bool split(const res::tag& base, std::string& folder, std::string& stem);

		document_creator::ports m_ports;
		document_creator m_names;
		std::function<std::string(const res::tag&)> m_claim;
	};

	// The reason a new file `tag` is not the project's to make because the open level has that name (its file or one of its worlds), else empty (#966).
	std::string open_level_claim(res::resource_system& resources, const desc::desc_system& descs, const desc_file_layout& layout, const res::tag& tag);

	// What the "Create Variant" window of the asset browser holds between frames, without ImGui: the base, the name (the suggested one
	// at first) and what is wrong with it.
	class prefab_variant_form
	{
	public:
		explicit prefab_variant_form(const prefab_variant_creator& creator) : m_creator(creator) {}

		void open(res::tag base);
		const res::tag& base() const { return m_base; }

		const std::string& name() const { return m_name; }
		void set_name(std::string name);

		// What is wrong with the name as of its last change (the folders are walked then, not at every frame).
		const std::string& problem() const { return m_problem; }
		bool can_submit() const { return !m_name.empty() && m_problem.empty(); }
		// What the last submit said when it failed.
		const std::string& error() const { return m_error; }

		// Makes the variant (the name is checked again, fresh): the tag on success, else error() says why. Nothing is written over a file.
		std::optional<res::tag> submit();

	private:
		const prefab_variant_creator& m_creator;
		res::tag m_base;
		std::string m_name;
		std::string m_problem;
		std::string m_error;
	};
}
