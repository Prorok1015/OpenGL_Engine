#pragma once
#include <snk/resource/res_asset_id.h>
#include <boost/json/fwd.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace desc { class desc_system; }

// The direct references of a desc file (EPIC-57 section 6.3, #774), as the asset database keeps them for "Used by" and
// asset.usages.
namespace edt
{
	// The biggest file the references are read from: a level may be this big (Save As over a level reads it up to here).
	inline constexpr std::uint64_t k_level_file_limit = std::uint64_t{ 64 } << 20;

	enum class reference_place : std::uint8_t
	{
		// Any value: what it is, the schema of its owner says (classify_reference).
		value,
		// The "__parent" of a desc, an instance of a prefab included.
		parent,
		// A component of a prefab node given as a tag: "components": {"sky": "res://sky.desc"}.
		component,
	};

	// A reference as the file holds it, before a schema says what it is: every res:// or engine:// string and every
	// {guid, path} object with a guid or such a path. Other schemes (memory://, https://) can name no file of the project. Read without the desc system, on the database's worker thread; the schema is asked when the
	// references are looked at, so the database does not depend on the game module that is loaded.
	struct raw_reference
	{
		std::string json_pointer;
		// As written; empty for a {guid} without a path.
		std::string path;
		std::optional<res::asset_id> guid;
		// The "__type" of the nearest typed object around the value and where that object is; empty: none.
		std::string owner_type;
		std::string owner_pointer;
		reference_place place = reference_place::value;

		bool operator==(const raw_reference&) const = default;
	};

	std::vector<raw_reference> collect_raw_references(const boost::json::value& document);

	// The references of one file as the database keeps them: shared, never changed, so a record is copied cheaply (a walk
	// copies every record). Null: none.
	using raw_reference_list = std::shared_ptr<const std::vector<raw_reference>>;

	inline std::span<const raw_reference> references_in(const raw_reference_list& list)
	{
		return list ? std::span<const raw_reference>(*list) : std::span<const raw_reference>{};
	}

	inline bool same_references(const raw_reference_list& a, const raw_reference_list& b)
	{
		return a == b || std::ranges::equal(references_in(a), references_in(b));
	}

	enum class usage_kind : std::uint8_t
	{
		// A res_tag or desc_ref field of the schema.
		field,
		parent,
		component,
		// Not a field of a schema (a custom field such as the samplers of a material, the record of a prefab instance, a
		// type without a schema): a string naming one of the engine's protocols, or a {guid, path}.
		loose,
		// The project manifest (start_scene, input_actions).
		manifest,
	};

	std::string_view usage_kind_name(usage_kind kind);

	// What `reference` is by the schema of its owner (null `descs`: no schema is known). Nullopt: no reference, a string
	// of another scheme (https://) where no schema field is.
	std::optional<usage_kind> classify_reference(const raw_reference& reference, const desc::desc_system* descs);
}
