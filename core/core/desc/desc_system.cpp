#include <snk/desc/desc_system.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/desc/desc_json_ops.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_reference.h>
#include <snk/resource/res_tag_json.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	// What register_migration with a summary stores in the registry. Its type stays in the engine: only the engine asks
	// a step for it (std::function::target), so the summary needs no table of its own and desc_system keeps its size
	// (scn::scene_layer holds one by value and modules read the members after it, #751).
	struct described_step
	{
		std::string type;
		int from_version = 0;
		std::string summary;
		std::function<void(json::object&, desc::migration_context&)> fn;

		// The path of migrate_tree without a warning list: warnings go to the log.
		void operator()(json::object& obj) const
		{
			desc::migration_context context(type, from_version, {}, nullptr);
			fn(obj, context);
		}
	};
}

desc::migration_context::migration_context(std::string_view type, int from_version, std::string_view file,
	std::vector<migration_warning>* sink)
	: m_type(type)
	, m_from_version(from_version)
	, m_file(file)
	, m_sink(sink)
{
}

void desc::migration_context::warn(std::string message)
{
	if (m_file.empty()) {
		egLOG_WARN("desc/migrate", "'{}' {} -> {}: {}", m_type, m_from_version, m_from_version + 1, message);
	} else {
		egLOG_WARN("desc/migrate", "{}: '{}' {} -> {}: {}", m_file, m_type, m_from_version, m_from_version + 1, message);
	}
	if (m_sink) {
		m_sink->push_back(migration_warning{ std::string{ m_type }, m_from_version, std::string{ m_file }, std::move(message) });
	}
}

void desc::desc_system::register_migration(std::string_view type, int from_version, std::string_view summary,
	std::function<void(json::object&, migration_context&)> fn)
{
	register_migration(type, from_version, migration_step_fn{ described_step{ std::string{ type }, from_version, std::string{ summary }, std::move(fn) } });
}

std::string desc::desc_system::migration_summary(std::string_view type, int from_version) const
{
	const migration_steps* steps = find_migrations(type);
	if (!steps) return {};
	const auto step = steps->find(from_version);
	if (step == steps->end()) return {};
	const auto* described = step->second.target<described_step>();
	return described ? described->summary : std::string{};
}

template<class RunStep>
void desc::desc_system::migrate_object(json::object& obj, std::string_view context, const RunStep& run) const
{
	auto type_it = obj.find("__type");
	if (type_it != obj.end() && type_it->value().is_string()) {
		migrate_typed(obj, std::string{ type_it->value().as_string() }, context, run);
	}
	for (auto& member : obj) {
		migrate_value(member.value(), context, run);
	}
}

template<class RunStep>
void desc::desc_system::migrate_value(json::value& value, std::string_view context, const RunStep& run) const
{
	if (value.is_object()) {
		migrate_object(value.as_object(), context, run);
	}
	else if (value.is_array()) {
		auto& arr = value.as_array();
		// Numeric payloads (geometry) can be megabytes long and never contain typed descs.
		if (!arr.empty() && arr.front().is_number()) {
			return;
		}
		for (auto& element : arr) {
			migrate_value(element, context, run);
		}
	}
}

template<class RunStep>
void desc::desc_system::migrate_typed(json::object& obj, const std::string& type, std::string_view context, const RunStep& run) const
{
	constexpr std::int64_t max_version = std::numeric_limits<int>::max();

	std::int64_t parsed = 1;
	if (auto ver_it = obj.find("__version"); ver_it != obj.end()) {
		const json::value& raw = ver_it->value();
		if (raw.is_int64()) {
			parsed = raw.as_int64();
		}
		else if (raw.is_uint64()) {
			parsed = raw.as_uint64() > static_cast<std::uint64_t>(max_version) ? max_version + 1 : static_cast<std::int64_t>(raw.as_uint64());
		}
		else {
			throw std::runtime_error(std::format("{}: '{}' has a non-integer __version", context, type));
		}
		if (parsed < 1 || parsed > max_version) {
			throw std::runtime_error(std::format("{}: '{}' has an invalid __version", context, type));
		}
	}

	const int current = current_version(type);
	const int version = static_cast<int>(parsed);
	if (version > current) {
		throw std::runtime_error(std::format("{}: '{}' version {} is newer than supported {}", context, type, version, current));
	}
	if (version < current) {
		const auto& steps = *find_migrations(type);
		for (int from = version; from < current; ++from) {
			auto step = steps.find(from);
			if (step == steps.end()) {
				throw std::runtime_error(std::format("{}: no migration for '{}' from version {}", context, type, from));
			}
			run(step->second, obj, type, from);
		}
	}
	obj["__version"] = current;
}

void desc::desc_system::unregister_desc(std::string_view type_name)
{
	factory_map.erase(std::string{ type_name });
	std::erase(m_component_type_names, std::string{ type_name });
	m_schemas.remove(type_name);
}

const desc::type_schema* desc::desc_system::register_schema(type_schema schema)
{
	if (owned_by_parent(schema.name)) {
		egLOG_ERROR("desc/error", "Schema '{}' not registered: the type belongs to the parent desc system", schema.name);
		return nullptr;
	}
	ASSERT_MSG(schema.version == current_version(schema.name), "Schema '{}' is version {}, but the registered migrations lead to version {}!",
		schema.name, schema.version, current_version(schema.name));
	ASSERT_MSG(!m_schemas.contains(schema.name), "Schema '{}' is already registered!", schema.name);
	const type_schema* stored = m_schemas.add(std::move(schema));
	if (stored && !factory_map.contains(stored->name)) {
		if (auto factory = make_dynamic_factory(*stored, m_schemas)) {
			factory_map[stored->name] = std::move(factory);
			if (stored->is_component) {
				m_component_type_names.push_back(stored->name);
			}
		}
	}
	return stored;
}

const desc::type_schema* desc::desc_system::find_schema(std::string_view type_name) const
{
	return m_schemas.find(type_name);
}

const desc::schema_registry& desc::desc_system::schemas() const
{
	return m_schemas;
}

desc::schema_registry& desc::desc_system::schemas()
{
	return m_schemas;
}

bool desc::desc_system::is_registered(std::string_view type_name) const
{
	return find_factory(type_name) != nullptr;
}

std::vector<std::string> desc::desc_system::type_names() const
{
	std::vector<std::string> names = m_parent ? m_parent->type_names() : std::vector<std::string>{};
	names.reserve(names.size() + factory_map.size() + m_migrations.size());
	for (const auto& entry : factory_map) names.push_back(entry.first);
	for (const auto& entry : m_migrations) names.push_back(entry.first);
	for (std::string& name : m_schemas.type_names()) names.push_back(std::move(name));
	std::ranges::sort(names);
	const auto [first, last] = std::ranges::unique(names);
	names.erase(first, last);
	return names;
}

std::vector<std::string> desc::desc_system::get_component_type_names() const
{
	std::vector<std::string> names = m_parent ? m_parent->get_component_type_names() : std::vector<std::string>{};
	names.insert(names.end(), m_component_type_names.begin(), m_component_type_names.end());
	return names;
}

void desc::desc_system::register_migration(std::string_view type, int from_version, std::function<void(json::object&)> fn)
{
	if (owned_by_parent(type)) {
		egLOG_ERROR("desc/error", "Migration for '{}' not registered: the type belongs to the parent desc system", type);
		return;
	}
	auto& steps = m_migrations[std::string{ type }];
	ASSERT_MSG(!steps.contains(from_version), "Migration for '{}' from version {} is already registered!", type, from_version);
	steps.emplace(from_version, std::move(fn));
	m_schemas.set_version(type, current_version(type));
}

void desc::desc_system::unregister_migrations(std::string_view type)
{
	m_migrations.erase(std::string{ type });
	m_schemas.set_version(type, current_version(type));
}

int desc::desc_system::current_version(std::string_view type) const
{
	const auto* steps = find_migrations(type);
	if (!steps || steps->empty()) {
		return 1;
	}
	return steps->rbegin()->first + 1;
}

void desc::desc_system::migrate_tree(json::object& root, std::string_view context) const
{
	migrate_object(root, context, [](const migration_step_fn& step, json::object& obj, const std::string&, int) { step(obj); });
}

void desc::desc_system::stamp_header(json::object& obj) const
{
	auto it = obj.find("__type");
	if (it == obj.end() || !it->value().is_string()) {
		return;
	}
	obj["__version"] = current_version(std::string{ it->value().as_string() });

	constexpr std::string_view header_keys[] = { "__type", "__version", "__guid" };
	json::object ordered;
	for (auto key : header_keys) {
		if (auto found = obj.find(key); found != obj.end()) {
			ordered.emplace(found->key(), std::move(found->value()));
		}
	}
	for (auto& member : obj) {
		if (!ordered.contains(member.key())) {
			ordered.emplace(member.key(), std::move(member.value()));
		}
	}
	obj = std::move(ordered);
}

void desc::desc_system::stamp_tree(json::value& value) const
{
	if (value.is_object()) {
		auto& obj = value.as_object();
		stamp_header(obj);
		for (auto& member : obj) {
			stamp_tree(member.value());
		}
	}
	else if (value.is_array()) {
		auto& arr = value.as_array();
		if (!arr.empty() && arr.front().is_number()) {
			return;
		}
		for (auto& element : arr) {
			stamp_tree(element);
		}
	}
}

void desc::desc_system::stamp_tree(json::object& obj) const
{
	json::value wrapper = std::move(obj);
	stamp_tree(wrapper);
	obj = std::move(wrapper.as_object());
}

namespace
{
	bool is_header_key(std::string_view key)
	{
		return key.starts_with("__");
	}

	const json::value* id_of(const json::value& item)
	{
		return item.is_object() ? item.as_object().if_contains("__id") : nullptr;
	}

	json::object merge_patch(const json::object& base, const json::object& target, bool nested, bool top = true);

	// Nodes of a prefab pair with the parent's by "__id" (v2): a node equal to the parent's is left out, a changed one keeps
	// only its "__id" and its changes. A parent's node this one lacks cannot be said here: reads_back refuses such a form.
	json::array patch_by_id(const json::array& base, const json::array& target)
	{
		json::array patch;
		for (const json::value& item : target) {
			const json::value* id = id_of(item);
			const auto same_id = [id](const json::value& was) {
				const json::value* other = id_of(was);
				return other && *other == *id;
			};
			const auto same = id ? std::ranges::find_if(base, same_id) : base.end();
			if (same == base.end()) {
				patch.push_back(item);
			} else if (*same != item) {
				json::object changed;
				changed["__id"] = *id;
				for (auto& member : merge_patch(same->as_object(), item.as_object(), true, false)) {
					if (member.key() != "__id") changed.emplace(member.key(), std::move(member.value()));
				}
				patch.push_back(std::move(changed));
			}
		}
		return patch;
	}

	bool keyed_by_id(const json::array& items)
	{
		return !items.empty() && std::ranges::all_of(items, [](const json::value& item) { return id_of(item) != nullptr; });
	}

	// What `target` changes of `base`, as a merge patch: a key `target` lacks is null (#821/#823: null removes a map key and
	// resets an optional field). `nested`: objects on both sides are patched key by key (map fields), arrays of nodes by
	// "__id"; otherwise, and for an inline desc (it has a "__type"), a changed value is written whole. The file's own header
	// keys are not compared.
	json::object merge_patch(const json::object& base, const json::object& target, bool nested, bool top)
	{
		json::object patch;
		for (const auto& member : target) {
			if (top && is_header_key(member.key())) continue;
			const json::value* was = base.if_contains(member.key());
			if (was && *was == member.value()) continue;
			const json::value& now = member.value();
			if (nested && was && was->is_object() && now.is_object() && !now.as_object().contains("__type")) {
				patch[member.key()] = merge_patch(was->as_object(), now.as_object(), true, false);
			} else if (nested && was && was->is_array() && now.is_array() && keyed_by_id(was->as_array()) && keyed_by_id(now.as_array())) {
				patch[member.key()] = patch_by_id(was->as_array(), now.as_array());
			} else {
				patch[member.key()] = now;
			}
		}
		for (const auto& member : base) {
			if ((!top || !is_header_key(member.key())) && !target.contains(member.key())) patch[member.key()] = nullptr;
		}
		return patch;
	}
}

// The parent is the one loaded now: if it changed on disk after this desc was read, the file says what this desc shows
// against the new parent ("save what you see"), so a value it took from the old parent is written as its own.
void desc::desc_system::reduce_to_parent(const desc::desc_base& desc, json::object& body)
{
	const res::tag& parent_tag = desc.get_parent();
	if (!parent_tag.is_valid() || desc.get_type().empty()) {
		return;
	}
	const auto file_form = [&](const json::object& own) {
		json::object file;
		for (const auto& member : body) {
			if (is_header_key(member.key())) file.emplace(member.key(), member.value());
		}
		file["__parent"] = json::value_from(parent_tag);
		for (const auto& member : own) {
			if (!is_header_key(member.key())) file.emplace(member.key(), member.value());
		}
		return file;
	};
	const auto parent = m_res_system.require_sync<desc::desc_base>(parent_tag).get_sync();
	if (!parent) {
		// The file still needs its parent to load: the link is kept, with every field written.
		egLOG_WARN("desc", "'{}': its '__parent' '{}' did not load; written whole, '__parent' kept", desc.get_tag().view(), parent_tag.view());
		body = file_form(body);
		return;
	}
	// Compared as files are written: nested typed objects carry their __version in a file (stamp_tree) but not in serialize.
	const auto stamped = [this](json::object object) {
		stamp_tree(object);
		return object;
	};
	const json::object inherited = [&] {
		json::object out;
		parent->serialize(out);
		return stamped(std::move(out));
	}();
	const json::object target = stamped(body);
	// The form is right only if loading it does what the adapter does - the parent copied in, the file read on top - and
	// gives `body` again; each type reads its fields its own way, so that is checked rather than assumed.
	const auto reads_back = [&](const json::object& file) {
		try {
			const auto probe = create_instance(desc.get_type(), desc.get_tag());
			if (!probe) return false;
			parent->copy_to(*probe);
			probe->set_parent(parent_tag);
			probe->deserialize(*this, file);
			json::object again;
			probe->serialize(again);
			return stamped(std::move(again)) == target;
		} catch (const std::exception& e) {
			egLOG_DEBUG("desc", "'{}': a form with '__parent' does not read back: {}", desc.get_tag().view(), e.what());
			return false;
		}
	};

	// The smallest form first: key by key, for map fields and prefab nodes that merge with the parent's. Then whole values,
	// for a type that reads a field as a whole.
	for (const bool nested : { true, false }) {
		json::object file = file_form(merge_patch(inherited, target, nested));
		if (reads_back(file)) {
			body = std::move(file);
			return;
		}
	}
	egLOG_WARN("desc", "'{}': no form with '__parent' '{}' reads back the same; written whole, without '__parent'",
		desc.get_tag().view(), parent_tag.view());
}

std::shared_ptr<desc::desc_base> desc::desc_system::create_instance(std::string_view type_id, const res::tag& tag) const
{
	const auto* factory = find_factory(type_id);
	if (!factory) {
		egLOG("desc/error", "Failed to create desc instance of type '{0}': type not registered", type_id);
		return nullptr;
	}
	auto instance = (*factory)();
	instance->m_system = const_cast<desc_system*>(this);
	instance->set_tag(tag);
	instance->set_type(type_id);
	return instance;
}

const desc::desc_system::factory_fn* desc::desc_system::find_factory(std::string_view type_name) const
{
	auto it = factory_map.find(std::string{ type_name });
	if (it != factory_map.end()) {
		return &it->second;
	}
	return m_parent ? m_parent->find_factory(type_name) : nullptr;
}

const desc::desc_system::migration_steps* desc::desc_system::find_migrations(std::string_view type) const
{
	auto it = m_migrations.find(std::string{ type });
	if (it != m_migrations.end()) {
		return &it->second;
	}
	return m_parent ? m_parent->find_migrations(type) : nullptr;
}

bool desc::desc_system::owned_by_parent(std::string_view type_name) const
{
	return m_parent && (m_parent->is_registered(type_name) || m_parent->find_schema(type_name) || m_parent->find_migrations(type_name));
}

bool desc::desc_system::add_desc_entry(std::string_view type_name, schema_kind kind, factory_fn factory, std::vector<enum_schema> enums,
	std::optional<type_schema> schema)
{
	if (owned_by_parent(type_name)) {
		egLOG_ERROR("desc/error", "Desc type '{}' not registered: the type belongs to the parent desc system", type_name);
		return false;
	}
	ASSERT_MSG(!factory_map.contains(std::string{ type_name }), "That desc type is already registered!");
	factory_map[std::string{ type_name }] = std::move(factory);
	for (auto& enumeration : enums) {
		m_schemas.add_enum(std::move(enumeration));
	}
	if (schema) {
		register_schema(std::move(*schema));
	}
	if (kind == schema_kind::component) {
		m_component_type_names.push_back(std::string{ type_name });
	}
	return true;
}


void desc::desc_system::migrate_tree(json::object& root, std::string_view file, std::vector<migration_warning>& warnings) const
{
	migrate_object(root, file, [file, &warnings](const migration_step_fn& step, json::object& obj, const std::string& type, int from) {
		if (const auto* described = step.target<described_step>()) {
			migration_context context(type, from, file, &warnings);
			described->fn(obj, context);
		} else {
			step(obj);
		}
	});
}

desc::desc_system::desc_system(res::resource_system& res_system_)
	: m_res_system(res_system_)
{
}

desc::desc_system::~desc_system() = default;

desc::desc_system::desc_system(res::resource_system& res_system_, const desc_system* parent)
	: m_res_system(res_system_)
	, m_parent(parent)
	, m_schemas(parent ? &parent->m_schemas : nullptr)
{
}

std::string desc::desc_system::document_problem(const type_schema& schema, std::span<const std::string_view> codec_fields) const
{
	return desc::document_problem(schema, m_schemas, codec_fields);
}

bool desc::desc_system::mark_document(std::string_view type_name, std::span<const std::string_view> codec_fields)
{
	if (owned_by_parent(type_name)) {
		egLOG_ERROR("desc/error", "Desc type '{}' not marked as a document: the type belongs to the parent desc system", type_name);
		return false;
	}
	const type_schema* schema = m_schemas.find(type_name);
	if (!schema) {
		egLOG_ERROR("desc/error", "Desc type '{}' not marked as a document: it has no schema (it is not on the generic desc schema)", type_name);
		return false;
	}
	if (schema->is_document) {
		return true;
	}
	if (const std::string problem = document_problem(*schema, codec_fields); !problem.empty()) {
		egLOG_ERROR("desc/error", "Desc type '{}' not marked as a document: {}", type_name, problem);
		return false;
	}
	return m_schemas.set_document(type_name, true);
}

std::vector<std::string> desc::desc_system::document_types() const
{
	std::vector<std::string> names;
	for (const std::string& name : m_schemas.type_names()) {
		if (const type_schema* schema = m_schemas.find(name); schema && schema->is_document) {
			names.push_back(name);
		}
	}
	std::ranges::sort(names);
	return names;
}

res::tag desc::desc_system::resolve_reference(const res::reference& ref) const
{
	return m_res_system.resolve_reference(ref);
}

namespace
{
	bool is_reference_kind(desc::field_kind kind)
	{
		return kind == desc::field_kind::res_tag || kind == desc::field_kind::desc_ref;
	}

	json::object reference_object(const res::asset_id& guid, const res::tag& path)
	{
		json::object out;
		out["guid"] = res::to_text(guid);
		out["path"] = path.view();
		return out;
	}

	// The guid the replaced file wrote for each path. A path it wrote with two guids has none.
	using kept_guids = std::map<std::string, std::optional<res::asset_id>, std::less<>>;

	// `file` names the replaced file in what is logged.
	void collect_kept(const json::value& value, kept_guids& out, std::string_view file, int depth)
	{
		if (depth > 128) {
			return;
		}
		if (const json::object* object = value.if_object()) {
			if (!object->contains("__type")) {
				if (const auto read = res::reference_from_json(value); read && !read->guid.is_nil() && !read->path.view().empty()) {
					const auto [found, added] = out.try_emplace(std::string(read->path.view()), read->guid);
					if (!added && found->second && *found->second != read->guid) {
						egLOG_WARN("desc", "{}: '{}' is written with two guids ({} and {}): neither is kept for it",
							file.empty() ? std::string_view{ "the replaced file" } : file, read->path.view(), res::to_text(*found->second), res::to_text(read->guid));
						found->second.reset();
					}
					return;
				}
			}
			for (const auto& member : *object) {
				collect_kept(member.value(), out, file, depth + 1);
			}
		}
		else if (const json::array* items = value.if_array()) {
			if (!items->empty() && items->front().is_number()) {
				return;
			}
			for (const json::value& item : *items) {
				collect_kept(item, out, file, depth + 1);
			}
		}
	}

	struct reference_writer
	{
		const desc::desc_system& descs;
		const res::resource_system& resources;
		bool has_index = false;
		const kept_guids* kept = nullptr;
		// What the edited value replaces (a component an override record applies to): where it holds an object, a path stays a
		// string, since an object would be merged into it.
		const json::value* merged_into = nullptr;

		// The index's ID for `path`, else the guid the replaced file had for it: a writer never drops a guid the file had,
		// unless the index puts that guid on another file.
		std::optional<res::asset_id> id_for(const res::tag& path) const
		{
			if (has_index) {
				if (auto id = resources.asset_id_of(path)) {
					return id;
				}
			}
			if (!kept) {
				return std::nullopt;
			}
			const auto found = kept->find(path.view());
			if (found == kept->end() || !found->second) {
				return std::nullopt;
			}
			if (has_index) {
				if (const auto now = resources.asset_path_of(*found->second); now && !(*now == path)) {
					return std::nullopt;
				}
			}
			return found->second;
		}

		bool merged_into_object(const std::string& where) const
		{
			const json::value* held = merged_into ? desc::json_pointer_get(*merged_into, where) : nullptr;
			return held && held->is_object();
		}

		// One reference value; an inline desc (it has "__type") is left to the walk over typed objects.
		void reference(json::value& value, const std::string& where) const
		{
			if (const json::string* text = value.if_string()) {
				const res::tag path{ std::string(text->data(), text->size()) };
				if (path.protocol() != res::tag::default_protocol() || merged_into_object(where)) {
					return;
				}
				if (const auto guid = id_for(path)) {
					value = reference_object(*guid, path);
				}
				return;
			}
			const json::object* object = value.if_object();
			if (!object || object->contains("__type")) {
				return;
			}
			const auto read = res::reference_from_json(value);
			if (!read) {
				return;
			}
			if (!read->guid.is_nil()) {
				if (!has_index) {
					return;
				}
				const auto now = resources.asset_path_of(read->guid);
				// Already where the file is, however it is spelled: left as written, so no file changes for nothing.
				if (!now || now->protocol() != res::tag::default_protocol() || *now == read->path) {
					return;
				}
				value = reference_object(read->guid, *now);
				return;
			}
			if (read->path.protocol() != res::tag::default_protocol()) {
				return;
			}
			if (const auto guid = id_for(read->path)) {
				value = reference_object(*guid, read->path);
			}
		}

		void element(const desc::field_schema& field, desc::field_kind kind, json::value& value, const std::string& where, int depth) const
		{
			if (is_reference_kind(kind)) {
				reference(value, where);
			}
			else if (kind == desc::field_kind::structure && value.is_object()) {
				if (const desc::type_schema* nested = descs.find_schema(field.ref_type)) {
					fields(*nested, value, where, depth + 1);
				}
			}
		}

		// The value of `field` itself: a list or map element by element.
		void field_value(const desc::field_schema& field, json::value& value, const std::string& where, int depth) const
		{
			if (field.kind == desc::field_kind::array) {
				if (json::array* items = value.if_array()) {
					for (std::size_t i = 0; i < items->size(); ++i) {
						element(field, field.element, (*items)[i], where + "/" + std::to_string(i), depth);
					}
				}
			}
			else if (field.kind == desc::field_kind::map) {
				if (json::object* entries = value.if_object()) {
					for (auto& entry : *entries) {
						element(field, field.element, entry.value(), where + "/" + res::json_pointer_token(std::string_view(entry.key().data(), entry.key().size())), depth);
					}
				}
			}
			else {
				element(field, field.kind, value, where, depth);
			}
		}

		// The fields of one object of `schema`: a typed object or a structure in one. Inline descs are typed objects of their own.
		void fields(const desc::type_schema& schema, json::value& data, const std::string& where, int depth) const
		{
			if (depth > 32) {
				return;
			}
			for (const desc::field_schema& field : schema.fields) {
				std::vector<std::string_view> keys{ field.key };
				for (const std::string& alias : field.read_aliases) {
					keys.push_back(alias);
				}
				for (const std::string_view key : keys) {
					if (json::value* value = value_at(data, key)) {
						field_value(field, *value, where + pointer_of(key), depth);
					}
				}
			}
		}

		void tree(json::value& value, const std::string& where) const
		{
			if (json::object* object = value.if_object()) {
				if (const json::value* type = object->if_contains("__type"); type && type->is_string()) {
					if (const desc::type_schema* schema = descs.find_schema(std::string_view(type->as_string()))) {
						fields(*schema, value, where, 0);
					}
				}
				for (auto& member : *object) {
					tree(member.value(), where + "/" + res::json_pointer_token(std::string_view(member.key().data(), member.key().size())));
				}
			}
			else if (json::array* items = value.if_array()) {
				// Numeric payloads (geometry) can be megabytes long and never contain typed descs.
				if (!items->empty() && items->front().is_number()) {
					return;
				}
				for (std::size_t i = 0; i < items->size(); ++i) {
					tree((*items)[i], where + "/" + std::to_string(i));
				}
			}
		}

		// `value` is what an object of `schema` holds at `tokens` (none: the object itself).
		void at(const desc::type_schema& schema, std::span<const std::string> tokens, json::value& value, const std::string& where, int depth) const
		{
			if (depth > 32) {
				return;
			}
			if (tokens.empty()) {
				fields(schema, value, where, depth);
				return;
			}
			std::vector<std::string> key_tokens;
			for (const desc::field_schema& field : schema.fields) {
				std::vector<std::string_view> keys{ field.key };
				for (const std::string& alias : field.read_aliases) {
					keys.push_back(alias);
				}
				for (const std::string_view key : keys) {
					if (!desc::json_pointer_tokens(pointer_of(key), key_tokens) || key_tokens.empty() || key_tokens.size() > tokens.size()
						|| !std::equal(key_tokens.begin(), key_tokens.end(), tokens.begin())) {
						continue;
					}
					const std::span<const std::string> rest = tokens.subspan(key_tokens.size());
					if (rest.empty()) {
						field_value(field, value, where, depth);
					}
					else if (field.kind == desc::field_kind::array || field.kind == desc::field_kind::map) {
						if (rest.size() == 1) {
							element(field, field.element, value, where, depth);
						}
						else if (field.element == desc::field_kind::structure) {
							if (const desc::type_schema* nested = descs.find_schema(field.ref_type)) {
								at(*nested, rest.subspan(1), value, where, depth + 1);
							}
						}
					}
					else if (field.kind == desc::field_kind::structure) {
						if (const desc::type_schema* nested = descs.find_schema(field.ref_type)) {
							at(*nested, rest, value, where, depth + 1);
						}
					}
					return;
				}
			}
		}

		static std::string pointer_of(std::string_view key)
		{
			return key.starts_with('/') ? std::string(key) : "/" + std::string(key);
		}

		static json::value* value_at(json::value& root, std::string_view key)
		{
			if (!key.starts_with('/')) {
				json::object* object = root.if_object();
				return object ? object->if_contains(key) : nullptr;
			}
			return const_cast<json::value*>(desc::json_pointer_get(std::as_const(root), key));
		}
	};
}

namespace
{
	// `file` names the replaced file in what is logged; empty: it has no name here.
	void write_over(const desc::desc_system& descs, const res::resource_system& resources, json::value& root, const json::value* replaced,
		std::string_view file)
	{
		kept_guids kept;
		if (replaced) {
			collect_kept(*replaced, kept, file, 0);
		}
		const reference_writer writer{ descs, resources, resources.has_asset_index(), kept.empty() ? nullptr : &kept };
		if (writer.has_index || writer.kept) {
			writer.tree(root, {});
		}
	}

	void write_over_bytes(const desc::desc_system& descs, const res::resource_system& resources, json::value& root,
		std::span<const std::byte> bytes, std::string_view file)
	{
		const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		// A file without a {guid, path} has no guid to keep: it is not parsed again.
		if (text.find("\"guid\"") == std::string_view::npos) {
			write_over(descs, resources, root, nullptr, file);
			return;
		}
		json::parse_options options;
		options.max_depth = 128;
		boost::system::error_code ec;
		const json::value replaced = json::parse(text, ec, {}, options);
		if (ec) {
			egLOG_WARN("desc", "{}: the replaced file does not parse ({}), the guids of its references are not kept",
				file.empty() ? std::string_view{ "a desc" } : file, ec.message());
		}
		write_over(descs, resources, root, ec ? nullptr : &replaced, file);
	}
}

void desc::desc_system::write_references(json::value& root, const json::value* replaced) const
{
	write_over(*this, m_res_system, root, replaced, {});
}

void desc::desc_system::write_references(json::value& root, std::span<const std::byte> replaced_file) const
{
	write_over_bytes(*this, m_res_system, root, replaced_file, {});
}

void desc::desc_system::write_references_over(json::value& root, const res::tag& file) const
{
	if (!m_res_system.exists(file)) {
		write_references(root);
		return;
	}
	constexpr std::chrono::seconds timeout{ 5 };
	const auto raw = m_res_system.fetch_data(file);
	const bool timed_out = raw && !raw->wait_for(timeout);
	if (!raw || timed_out || !raw->is_ready()) {
		egLOG_WARN("desc", "{}: the replaced file could not be read{}, the guids of its references are not kept", file.view(),
			timed_out ? std::format(" in {} s", timeout.count()) : std::string{});
		write_references(root);
		return;
	}
	write_over_bytes(*this, m_res_system, root, std::span<const std::byte>(raw->data), file.view());
}

void desc::desc_system::write_references(const type_schema& schema, std::string_view pointer, json::value& value, const json::value* merged_into) const
{
	std::vector<std::string> tokens;
	if (!desc::json_pointer_tokens(pointer, tokens)) {
		return;
	}
	reference_writer writer{ *this, m_res_system, m_res_system.has_asset_index() };
	if (!writer.has_index) {
		return;
	}
	writer.merged_into = merged_into;
	writer.at(schema, tokens, value, {}, 0);
	writer.tree(value, {});
}

std::optional<res::tag> desc::reference_tag(const json::value& in, const read_context& ctx)
{
	const auto read = res::reference_from_json(in);
	if (!read) {
		return std::nullopt;
	}
	return ctx.system ? ctx.system->resolve_reference(*read) : read->path;
}
