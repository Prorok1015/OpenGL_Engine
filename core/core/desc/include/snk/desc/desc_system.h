#pragma once
#include <snk/common/common.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/resource/res_tag.h>
#include <snk/resource/res_reference.h>
#include <snk/resource/res_tag_json.h>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/desc/desc_dynamic.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_pretty_json.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <snk/common/snk_api.h>

namespace desc
{
	// Where get_field_desc keeps the memory copy of an inline sub-desc: memory://overrides/, or, for a game module's
	// layer, under the layer's own write prefix (memory://modules/<name>/overrides/, MODULE_LAYER.md).
	inline std::string override_root(const res::resource_system& resources)
	{
		const std::string& prefix = resources.write_prefix();
		return (prefix.empty() ? std::string{ "memory://" } : prefix) + "overrides/";
	}

	inline bool is_override_tag(std::string_view tag)
	{
		return tag.starts_with("memory://overrides/")
			|| (tag.starts_with("memory://modules/") && tag.find("/overrides/") != std::string_view::npos);
	}

	// What a migration step reported besides upgrading the object: a loss of data or an assumption (EPIC-58).
	struct migration_warning
	{
		std::string type;
		int from_version = 0;
		std::string file;
		std::string message;
	};

	// Handed to a step registered with a summary. A step sees only the JSON of its object, never the project: it does not
	// rewrite references or create files (EPIC-58 3.2).
	class SNK_API migration_context
	{
	public:
		// `sink` null: warn() only logs.
		migration_context(std::string_view type, int from_version, std::string_view file, std::vector<migration_warning>* sink);

		std::string_view type() const { return m_type; }
		int from_version() const { return m_from_version; }
		// Empty when the object is migrated from memory.
		std::string_view file() const { return m_file; }

		// Logged as a warning, and kept for the report when the migration collects them.
		void warn(std::string message);

	private:
		std::string_view m_type;
		int m_from_version = 0;
		std::string_view m_file;
		std::vector<migration_warning>* m_sink = nullptr;
	};

	class SNK_API desc_system
	{
	public:
		using desc_name = res::tag;
		using desc_namespace = std::string;

		// std::hash is not stable across platforms/STLs; these names end up in saved data.
		static constexpr uint64_t fnv1a_64(std::string_view s) noexcept
		{
			return desc::schema_id(s);
		}

		desc_system(res::resource_system& res_system);
		// A layer over `parent` (MODULE_LAYER.md): factories, migrations, schemas and component names fall
		// through to it; a type the parent knows cannot be registered or migrated here (logged, not asserted).
		desc_system(res::resource_system& res_system, const desc_system* parent);
		~desc_system() = default;
		desc_system(const desc_system&) = delete;
		desc_system& operator=(const desc_system&) = delete;
		desc_system(desc_system&&) = delete;
		desc_system& operator=(desc_system&&) = delete;

		// A T described with BOOST_DESCRIBE_CLASS also gets its type_schema (and enum schemas) registered; that
		// needs <snk/desc/desc_describe.h> in the registering translation unit.
		template<typename T>
		void register_desc(const std::string_view type_name)
		{
			add_desc<T>(type_name, schema_kind::desc);
		}

		// register_component_desc marks the type as user-addable in the editor.
		// Use this for component descs that users can add to prefab nodes.
		// Use register_desc for nested/container descs not shown in "Add Component".
		template<typename T>
		void register_component_desc(const std::string_view type_name)
		{
			add_desc<T>(type_name, schema_kind::component);
		}

		// register_desc plus mark_document, for a type the editor opens as a data document (EPIC-56). A custom field that has a
		// json_codec counts as being on the schema. The type stays a registered desc when it cannot be a document (logged);
		// false then, and false when the registration itself is refused.
		template<typename T>
		bool register_document(const std::string_view type_name)
		{
			if (!add_desc<T>(type_name, schema_kind::desc)) {
				return false;
			}
			if constexpr (desc::is_described<T>) {
				const std::vector<std::string_view> codec_fields = desc::schema_generator<T>::codec_fields();
				return mark_document(type_name, codec_fields);
			} else {
				return mark_document(type_name);
			}
		}

		// Marks the schema of `type_name` as a data document (type_schema::is_document). Only a plain desc whose every field
		// the schema describes can be one: the file's `__parent` chain is merged as JSON by the schema alone, and that matches
		// what the runtime reads only when no field hides behind a custom value. `codec_fields` names the top-level custom
		// fields that have a json_codec (register_document<T> finds them); a custom field elsewhere, or without a codec,
		// refuses the type. False (logged, nothing changed) when refused, or when the type belongs to the parent desc system.
		bool mark_document(std::string_view type_name, std::span<const std::string_view> codec_fields = {});

		// Every document type, this layer's and the parent's, sorted by name.
		std::vector<std::string> document_types() const;

		void unregister_desc(std::string_view type_name);

		// Registers a schema (wasm/JSON, or a described C++ type through register_desc). The version must match
		// current_version(name). Migrations registered later move the stored schema's version along, so the order
		// does not matter.
		// A schema whose type has no factory yet gets one that creates dynamic_desc objects; a component schema is
		// also listed in get_component_type_names(). Structure fields need their nested schemas registered first.
		// Returns the stored schema, or nullptr when the name (or its id) is already taken.
		const type_schema* register_schema(type_schema schema);

		const type_schema* find_schema(std::string_view type_name) const;

		const schema_registry& schemas() const;
		schema_registry& schemas();

		bool is_registered(std::string_view type_name) const;

		// The parent's names first, then this layer's.
		std::vector<std::string> get_component_type_names() const;

		const desc_system* parent() const { return m_parent; }

		// Migration step: upgrades an object of `type` from `from_version` to `from_version + 1`.
		// Objects without __version are version 1. A duplicate (type, from_version) keeps the first one.
		void register_migration(std::string_view type, int from_version, std::function<void(json::object&)> fn);

		// The same step with a one-line summary (for the migration plan and report) and a context for warnings. Kept in
		// the one registry above, so current_version, the duplicate check and modules built before it see it as any
		// other step; migrate_tree without warnings runs it with warnings going to the log only.
		void register_migration(std::string_view type, int from_version, std::string_view summary,
			std::function<void(json::object&, migration_context&)> fn);

		// The summary of the step from `from_version`; empty for a step without one or none.
		std::string migration_summary(std::string_view type, int from_version) const;

		void unregister_migrations(std::string_view type);

		int current_version(std::string_view type) const;

		// Brings every typed object under `root` up to current_version. Throws std::runtime_error on
		// a malformed or newer-than-supported __version, or when a migration step is missing.
		void migrate_tree(json::object& root, std::string_view context) const;

		// migrate_tree that also collects what the steps with a summary warned about; `file` names the source in them.
		void migrate_tree(json::object& root, std::string_view file, std::vector<migration_warning>& warnings) const;

		// Writes the current __version into an object that has a string __type, and moves the header keys
		// (__type, __version, __guid) to the front so saved files read the same way everywhere.
		void stamp_header(json::object& obj) const;

		// stamp_header for every typed object under `value` (the files embed typed descs, and a memory://
		// copy of them goes through the adapter - and its migrations - again).
		void stamp_tree(json::value& value) const;

		void stamp_tree(json::object& obj) const;

		std::shared_ptr<desc::desc_base> create_instance(std::string_view type_id, const res::tag& tag) const;

		// resource_system::resolve_reference of the system this one loads through.
		res::tag resolve_reference(const res::reference& ref) const;

		// `json_path` (a JSON pointer in the owner's file, may be empty) is where a problem of an inline desc is reported:
		// its memory:// copy's diagnostics go to the owner's file at that path (#595).
		template<typename T>
		auto get_field_desc(const desc::desc_base& owner, const json::value& data, std::string_view json_path = {}) const
		{
			const json::value* inline_type = data.is_object() ? data.as_object().if_contains("__type") : nullptr;
			const bool inline_desc = inline_type && inline_type->is_string();
			if (!inline_desc) {
				const auto read = res::reference_from_json(data);
				if (!read) {
					std::string why = "a desc field is a path, a {guid, path} reference or an inline desc with \"__type\"";
					res::report_load_warning(res::diagnostic_kind::reference, json_path, why);
					return m_res_system.refused<T>(res::tag{}, std::move(why));
				}
				const res::tag reference = resolve_reference(*read);
				// The owner's protocol is unknown for the assembler's untagged copy: the stricter rule applies (#697).
				if (std::string why = res::reference_problem(reference, owner.get_tag().protocol()); !why.empty()) {
					res::report_load_warning(res::diagnostic_kind::reference, json_path, why);
					return m_res_system.refused<T>(reference, std::move(why));
				}
				auto handle = m_res_system.require<T>(reference);
				res::note_field_load(handle.get_control_block(), json_path, false, &core::res::holds<T>, core::res::handle_type_name<T>());
				return handle;
			}

			res::tag mem_tag = make_mem_tag_deterministic(owner, std::string{ inline_type->get_string() }, data);

			// Only store if not already in cache - avoids re-creating the same resource on every assembly.
			if (!m_res_system.exists(mem_tag)) {
				m_res_system.store(mem_tag, serialize_to_bytes(data));
			}
			auto handle = m_res_system.require<T>(mem_tag);
			res::note_field_load(handle.get_control_block(), json_path, true, &core::res::holds<T>, core::res::handle_type_name<T>());
			return handle;
		}

        void write_pretty_json(std::ostream& os, json::value const& jv, std::string* indent = nullptr) const
        {
            desc::write_pretty_json(os, jv, indent);
        }


		std::vector<std::byte> serialize_to_bytes(const json::value& data) const
		{
			return desc::pretty_json_bytes(data);
		}

		res::tag make_mem_tag_deterministic(const desc::desc_base& owner, const std::string& field, const json::value& data) const
		{
			std::string_view owner_name = owner.get_tag().pure_name();
			std::string serialized = json::serialize(data);
			uint64_t content_hash = fnv1a_64(serialized);

			std::string path = std::format("{}{}/{}_{:x}.desc",
				override_root(m_res_system), owner_name, field, content_hash);

			return res::tag{ path };
		}

	private:
		using factory_fn = std::function<std::shared_ptr<desc::desc_base>()>;
		using migration_step_fn = std::function<void(json::object&)>;
		using migration_steps = std::map<int, migration_step_fn>;

		const factory_fn* find_factory(std::string_view type_name) const;

		// Why `schema` cannot be a document (empty: it can); see mark_document.
		std::string document_problem(const type_schema& schema, std::span<const std::string_view> codec_fields) const;

		const migration_steps* find_migrations(std::string_view type) const;

		bool owned_by_parent(std::string_view type_name) const;

		// False (nothing registered) when the parent owns the type. Only builds what needs T; the registry is written by
		// the engine (add_desc_entry), so a module never compiles code that touches its members (#786).
		template<typename T>
		bool add_desc(const std::string_view type_name, const schema_kind kind)
		{
			factory_fn factory = []() -> std::shared_ptr<desc::desc_base> { return std::make_shared<T>(); };
			if constexpr (desc::is_described<T>) {
				static_assert(desc::has_schema_generator<T>, "T is described with BOOST_DESCRIBE_CLASS: include <snk/desc/desc_describe.h> where it is registered");
				return add_desc_entry(type_name, kind, std::move(factory), desc::schema_generator<T>::enums(),
					desc::schema_generator<T>::make(type_name, current_version(type_name), kind));
			} else {
				return add_desc_entry(type_name, kind, std::move(factory), {}, std::nullopt);
			}
		}

		// A schema_kind::component type is listed in get_component_type_names().
		bool add_desc_entry(std::string_view type_name, schema_kind kind, factory_fn factory, std::vector<enum_schema> enums,
			std::optional<type_schema> schema);

		// Migrates the object itself first, then its members, so children a migration creates are visited too.
		// `run(step, obj, type, from)` applies one step. Defined and instantiated in the engine only.
		template<class RunStep>
		void migrate_object(json::object& obj, std::string_view context, const RunStep& run) const;
		template<class RunStep>
		void migrate_value(json::value& value, std::string_view context, const RunStep& run) const;
		template<class RunStep>
		void migrate_typed(json::object& obj, const std::string& type, std::string_view context, const RunStep& run) const;

		res::resource_system& m_res_system;
		const desc_system* m_parent = nullptr;
		std::unordered_map<std::string, factory_fn> factory_map;
		std::unordered_map<std::string, migration_steps> m_migrations;
		std::vector<std::string> m_component_type_names;
		schema_registry m_schemas;
	};
}
