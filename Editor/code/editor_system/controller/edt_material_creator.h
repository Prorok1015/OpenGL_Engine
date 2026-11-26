#pragma once
#include "edt_document_creator.h"
#include <snk/model_codec/res_tag.h>
#include <boost/json/object.hpp>
#include <optional>
#include <string>
#include <string_view>

// "New Material..." (EPIC-07 US-07-C, #1198): a new material desc in a folder of the project, a child of the engine's base
// material so that it draws from the start. Made through the asset browser's document_creator: the same name rules, the
// same top layer and blockers, never over a file. The menu of a shader file makes one with that shader ("Create Material"),
// the menu of a material one whose `__parent` is that material ("Create Material Variant").
namespace edt
{
	inline constexpr std::string_view k_material_type = "material_desc";
	inline constexpr std::string_view k_shader_type = "shader_desc";
	inline constexpr std::string_view k_base_material = "res://base_material.desc";
	inline constexpr std::string_view k_material_name = "Material";
	inline constexpr std::string_view k_material_suffix = ".mat.desc";
	inline constexpr std::string_view k_material_title = "New Material";

	struct material_options
	{
		// Written as `shader` (a .shader.desc tag). Empty: none, the parent's shader is used.
		res::tag shader{};
		// The `__parent`. Empty: the engine's base material (k_base_material).
		res::tag parent{};
	};

	// The fields of a new material after `__type` and `__version`: `__parent`, then `shader` when the options name one.
	boost::json::object material_fields(const material_options& options = {});

	// The material item the context menu of a file offers, by its asset kind (asset_kind_of: the kind ids are the desc types).
	enum class material_menu_item
	{
		none,
		// "Create Material": a material drawn with that shader.
		from_shader,
		// "Create Material Variant": a material whose parent is that one.
		variant,
	};
	material_menu_item material_menu_item_of(std::string_view kind);

	// A desc file a material is made from: its folder below res:// ('/' separated, empty = the root) and its name.
	struct material_source
	{
		std::string folder;
		// The file name without its kind's suffix (else without ".desc"): "toon.shader.desc" -> "toon".
		std::string stem;
		std::string file;
	};

	class material_creator
	{
	public:
		explicit material_creator(const document_creator& names) : m_names(names) {}

		// The suffix of a material's file: its asset kind's (".mat.desc" with the editor's built-in kinds).
		std::string suffix() const { return m_names.suffix_of(k_material_type); }
		// The file a material named `name` gets (any kind's .desc suffix it ends with is replaced by the material's).
		std::string file_name(std::string_view name) const { return m_names.file_name_of(k_material_type, name); }

		// "Material", else "Material-2", ...: the first name free in `folder` in every layer that the name rules take. Empty
		// when none is (an unreadable folder, all numbers taken). Walks the folder: asked when the window opens.
		std::string suggested_name(std::string_view folder) const;
		// The same from `base`: `base`, then `base + separator + "2"`, ...
		std::string suggested_name(std::string_view folder, std::string_view base, std::string_view separator) const;
		// `file` split as a desc of `type` (its kind's suffix comes off the name, else ".desc"); nullopt when it is no
		// res://... .desc file.
		std::optional<material_source> source_of(const res::tag& file, std::string_view type) const;
		// Why `name` cannot be a new material in `folder` (document_creator::check of the material type); empty when it can.
		// Refused while no asset kind gives the material type k_material_suffix.
		std::string check(std::string_view folder, std::string_view name) const;
		// Writes `{"__type", "__version"}` and material_fields(options) as `<folder>/<name><suffix>`. Refused as Create
		// Document refuses: while the project may not be written, for a name taken in any layer, and as check() refuses
		// without the material kind.
		document_creation create(std::string_view folder, std::string_view name, const material_options& options = {}) const;
		// Why no material can be made now: the kind is not registered, a file would be a plain .desc. Empty when it is.
		std::string kind_problem() const;

	private:
		const document_creator& m_names;
	};

	// What the "New Material" window holds between frames, without ImGui: the folder, the name (the suggested one at first),
	// the file it gives and what is wrong with it.
	class material_create_form
	{
	public:
		explicit material_create_form(const material_creator& creator) : m_creator(creator) {}

		// "New Material" in `folder`, suggested "Material", "Material-2", ...
		void open(std::string folder, material_options options = {});
		// The window as `title` says, with `suggestion` as the first name (empty: none was found).
		void open(std::string folder, material_options options, std::string suggestion, std::string title);
		// "Create Material" on a shader file: a material in the shader's folder that draws with it, a child of the base
		// material; suggested the shader's stem ("toon", "toon-2", ...). False, nothing opened, for no res:// .desc file.
		bool open_from_shader(const res::tag& shader);
		// "Create Material Variant" on a material file: a material in its folder with that material as `__parent` and
		// nothing else; suggested `<stem>_variant`, "_variant2", ... as a prefab variant. A material of a lower layer gets
		// its variant in the project layer, in the same folder (where a prefab variant lands too). False, nothing opened,
		// for no res:// .desc file.
		bool open_variant_of(const res::tag& material);

		// What the window says it creates: "New Material", "New Material from toon.shader.desc", ...
		const std::string& title() const { return m_title; }
		const material_options& options() const { return m_options; }
		const std::string& folder() const { return m_folder; }
		// The free name the window was opened with; empty when none was found.
		const std::string& suggested() const { return m_suggested; }

		const std::string& name() const { return m_name; }
		void set_name(std::string name);
		// The file the material gets; empty while no name is typed.
		const std::string& file_name() const { return m_file; }

		// What is wrong with the name as of its last change (the folders are walked then, not at every frame). With no name:
		// why no material can be made at all (kind_problem), so the window says why Create is off.
		const std::string& problem() const { return m_problem; }
		bool can_submit() const { return !m_name.empty() && m_problem.empty(); }
		// What the last submit said when it failed.
		const std::string& error() const { return m_error; }

		// Makes the material (the name is checked again, fresh): the tag on success, else error() says why. A file name that
		// is no longer the one shown (the asset kinds changed meanwhile) is shown anew, nothing is made.
		std::optional<res::tag> submit();

	private:
		void recheck();

		const material_creator& m_creator;
		std::string m_title{ k_material_title };
		std::string m_folder;
		material_options m_options;
		std::string m_suggested;
		std::string m_name;
		std::string m_file;
		std::string m_problem;
		std::string m_error;
	};
}
