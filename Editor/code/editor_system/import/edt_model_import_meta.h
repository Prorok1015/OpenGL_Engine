#pragma once
#include "edt_model_import_settings.h"
#include <snk/model_codec/res_asset_id.h>
#include <snk/model_codec/res_tag.h>
#include <filesystem>
#include <optional>
#include <string>

// The `.resource` of the prefab a model import writes (#1195, docs/epics/editor/EPIC-62.md): importer "model" and the
// settings with the source's name and SHA-256. Written below the project's top layer through platform::beneath, before
// the prefab, so the asset database takes its guid instead of making one.
namespace edt {

	struct model_meta_plan
	{
		std::filesystem::path folder{};	// below the top layer
		std::filesystem::path leaf{};	// "<prefab file name>.resource"
		res::tag tag{};
		// What is written.
		std::string text{};
		// The bytes there when the plan was made: they are replaced. Nullopt: a new file, never over one.
		std::optional<std::string> before{};
		res::asset_id guid{};
		std::string error{};

		bool ok() const { return error.empty(); }
	};

	// Reads what is at `<prefab>.resource` below `top` and decides what to write; writes nothing. None there: a new
	// guid. One there (only with `may_replace`, a person's overwrite): its guid and flags stay, importer and settings
	// are replaced, the settings' keys this editor does not know stay; it must read and be canonical (its bytes are
	// what write_asset_meta makes of it) and its settings of version 1, else the plan is refused. An error names the
	// file as res:// and holds no absolute path.
	model_meta_plan plan_model_meta(const std::filesystem::path& top, const res::tag& prefab,
		const model_import_settings& settings, const model_source_record& source, bool may_replace);

	// Writes the plan: a new file only if nothing is there; a replacement only while the file still holds `before`
	// (temp + rename). Never through a link. Empty on success, else why not.
	std::string write_model_meta(const std::filesystem::path& top, const model_meta_plan& plan);

	// Takes a written plan back once the prefab was not written: a new file is removed, a replaced one gets `before`.
	std::string take_back_model_meta(const std::filesystem::path& top, const model_meta_plan& plan);

} // namespace edt
