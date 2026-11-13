#include "edt_data_document.h"
#include "edt_code_commands.h"
#include "edt_desc_file_commands.h"
#include "edt_schema_inspector.h"
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_system.h>
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/resource/res_system.h>
#include <boost/json/parse.hpp>
#include <algorithm>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
	namespace json = boost::json;

	constexpr std::size_t k_max_chain = 16;

	std::string_view stem_of(const res::tag& tag)
	{
		std::string_view name = tag.pure_name();
		if (const auto slash = name.find_last_of('/'); slash != std::string_view::npos) {
			name.remove_prefix(slash + 1);
		}
		if (const auto dot = name.find('.'); dot != std::string_view::npos) {
			name.remove_suffix(name.size() - dot);
		}
		return name;
	}

	std::string where(const res::tag& file, std::string_view pointer = {})
	{
		return std::format("'{}'{}{}", file.view(), pointer.empty() ? "" : " ", pointer);
	}

	std::optional<res::tag> parent_of(const json::object& body, const res::tag& file, std::string& error)
	{
		const json::value* field = body.if_contains("__parent");
		if (!field) {
			return std::nullopt;
		}
		if (!field->is_string() || !std::string_view{ field->as_string() }.starts_with("res://")) {
			error = std::format("{} /__parent: a parent is written as res://<path> (a path inside the project)", where(file));
			return std::nullopt;
		}
		return res::tag{ std::string_view{ field->as_string() } };
	}
}

namespace
{
	// The JSON pointers of the leaves two snapshots of `own` differ at: a changed or added or removed key, or a value that
	// is not an object on both sides (an array, a scalar) as a whole.
	void collect_changes(const std::string& prefix, const json::value& before, const json::value& after, std::vector<std::string>& out)
	{
		const json::object* was = before.if_object();
		const json::object* now = after.if_object();
		if (was && now) {
			for (const auto& entry : *now) {
				const std::string pointer = prefix + "/" + res::json_pointer_token(std::string_view{ entry.key() });
				if (const json::value* old = was->if_contains(entry.key())) {
					collect_changes(pointer, *old, entry.value(), out);
				} else {
					out.push_back(pointer);
				}
			}
			for (const auto& entry : *was) {
				if (!now->contains(entry.key())) out.push_back(prefix + "/" + res::json_pointer_token(std::string_view{ entry.key() }));
			}
			return;
		}
		if (!(before == after)) out.push_back(prefix.empty() ? std::string{ "/" } : prefix);
	}

	std::string edit_label(const std::vector<std::string>& pointers)
	{
		if (pointers.size() == 1) {
			const std::size_t slash = pointers.front().rfind('/');
			const std::string name = slash == std::string::npos ? pointers.front() : pointers.front().substr(slash + 1);
			return name.empty() ? std::string{ "Edit" } : "Edit " + name;
		}
		return std::format("Edit {} fields", pointers.size());
	}

	// What one edit of the person did to `own`: put back by restore_own, in either direction. The __guid of the file is
	// not an edit: a save may add it, and an undo must not take it away again.
	// Thrown out of a change the document cannot take back (a snapshot of another __type or __parent): the history has not moved.
	struct restore_refused {};

	class data_change final : public edt::history_change
	{
	public:
		data_change(edt::data_document& document, json::object before, json::object after, std::vector<std::string> keys)
			: m_document(document), m_before(std::move(before)), m_after(std::move(after)), m_keys(std::move(keys)) {}

		void undo() override { restore(m_before); }
		void redo() override { restore(m_after); }
		const std::vector<std::string>& keys() const override { return m_keys; }

	private:
		void restore(json::object snapshot)
		{
			if (const json::value* guid = m_document.own().if_contains("__guid")) {
				snapshot["__guid"] = *guid;
			}
			if (!m_document.restore_own(std::move(snapshot))) {
				throw restore_refused{};
			}
		}

		edt::data_document& m_document;
		json::object m_before;
		json::object m_after;
		std::vector<std::string> m_keys;
	};
}

edt::data_document::data_document(editor_services& services, desc_file_layout files)
	: m_services(services)
	, m_files(std::move(files))
{
}

const desc::type_schema* edt::data_document::schema() const
{
	const desc::desc_system* descs = m_services.descs();
	return descs ? descs->find_schema(m_type) : nullptr;
}

edt::open_result edt::data_document::read_file(const res::tag& file, json::object& out, std::string& version, desc_file_read* checks) const
{
	res::resource_system* resources = m_services.resources();
	desc::desc_system* descs = m_services.descs();
	if (!resources || !descs) {
		return open_result::failure("the project resources are not available");
	}
	desc_file_read read = read_desc_file(*resources, *descs, m_files, file);
	if (checks) {
		checks->located = read.located;
		checks->missing = read.missing;
		checks->top_layer = read.top_layer;
		checks->in_open_level = read.in_open_level;
		checks->version = read.version;
	}
	if (!read.error.empty()) {
		return open_result::failure(std::format("{} {}", where(file), read.error));
	}
	const std::string_view text(reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size());
	boost::system::error_code parse_error;
	json::parse_options options;
	options.max_depth = 128;
	json::value parsed = json::parse(text, parse_error, {}, options);
	if (parse_error || !parsed.is_object()) {
		return open_result::failure(std::format("{} is not a JSON object{}{}", where(file), parse_error ? ": " : "", parse_error ? parse_error.message() : std::string{}));
	}
	try {
		descs->migrate_tree(parsed.as_object(), file.view());
	} catch (const std::exception& e) {
		return open_result::failure(std::format("{}: {}", where(file), e.what()));
	}
	out = std::move(parsed.as_object());
	version = code_version_of(read.bytes);
	return open_result::ok();
}

edt::open_result edt::data_document::load(const res::tag& file, loaded& out) const
{
	desc::desc_system* descs = m_services.descs();
	if (!descs) {
		return open_result::failure("the project resources are not available");
	}

	desc_file_read checks;
	if (open_result read = read_file(file, out.own, out.version, &checks); !read.opened) {
		return read;
	}
	if (!checks.top_layer) {
		return open_result::not_a_document(std::format("{} belongs to a lower layer (the engine or a package), not to the project's own res/", where(file)));
	}
	if (checks.in_open_level) {
		return open_result::not_a_document(std::format("{} belongs to the open level (its file or one of its worlds)", where(file)));
	}
	const json::value* type_field = out.own.if_contains("__type");
	if (!type_field || !type_field->is_string()) {
		return open_result::not_a_document(std::format("{} /__type: missing or not a string", where(file)));
	}
	out.type = std::string{ type_field->as_string() };
	const desc::type_schema* type_schema = descs->find_schema(out.type);
	if (!type_schema) {
		return open_result::not_a_document(std::format("{} /__type: '{}' has no schema, so it cannot be edited as data", where(file), out.type));
	}
	if (!type_schema->is_document) {
		return open_result::not_a_document(std::format("{} /__type: '{}' is not a data document type", where(file), out.type));
	}
	return resolve_chain(file, out.own, out.type, out.inherited, out.parents);
}

edt::open_result edt::data_document::resolve_chain(const res::tag& file, const json::object& own, const std::string& type,
	json::object& inherited, std::vector<res::tag>& parents) const
{
	std::vector<json::object> chain;
	parents.clear();
	std::unordered_set<std::string> seen{ std::string{ file.view() } };
	res::tag current_tag = file;
	json::object current = own;
	while (true) {
		std::string error;
		const std::optional<res::tag> parent = parent_of(current, current_tag, error);
		if (!error.empty()) {
			return open_result::failure(std::move(error));
		}
		if (!parent) {
			break;
		}
		if (!seen.insert(std::string{ parent->view() }).second) {
			return open_result::failure(std::format("{} /__parent: '__parent' cycle through '{}'", where(current_tag), parent->view()));
		}
		if (chain.size() >= k_max_chain) {
			return open_result::failure(std::format("{} /__parent: the chain has more than {} parents", where(file), k_max_chain));
		}
		parents.push_back(*parent);
		json::object loaded_parent;
		std::string ignored;
		if (open_result read = read_file(*parent, loaded_parent, ignored); !read.opened) {
			return open_result::failure(std::format("{} /__parent: {}", where(current_tag), read.error));
		}
		const json::value* parent_type = loaded_parent.if_contains("__type");
		if (!parent_type || !parent_type->is_string() || std::string_view{ parent_type->as_string() } != type) {
			return open_result::failure(std::format("{} /__parent: '{}' is of another type than '{}'", where(current_tag), parent->view(), type));
		}
		current_tag = *parent;
		current = loaded_parent;
		chain.push_back(std::move(loaded_parent));
	}

	inherited.clear();
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		json::object layer;
		for (const auto& member : *it) {
			if (!member.key().starts_with("__")) {
				layer.insert_or_assign(member.key(), member.value());
			}
		}
		desc::json_deep_merge(inherited, layer);
	}
	return open_result::ok();
}

void edt::data_document::apply(const res::tag& file, loaded&& state)
{
	m_source = file;
	m_title = std::string{ stem_of(file) };
	m_type = std::move(state.type);
	m_own = std::move(state.own);
	m_saved_own = m_own;
	m_committed_own = m_own;
	m_history.clear();
	m_history.mark_saved();
	m_inherited = std::move(state.inherited);
	m_version = std::move(state.version);
	m_parents = std::move(state.parents);
	m_chain_problem.clear();
	m_changed_on_disk = false;
	m_flagged_version.clear();
	++m_generation;
	watch_files();
}

edt::open_result edt::data_document::open(const res::tag& file)
{
	m_open = false;
	unwatch_files();
	loaded state;
	if (open_result result = load(file, state); !result.opened) {
		return result;
	}
	apply(file, std::move(state));
	m_open = true;
	return open_result::ok();
}

edt::open_result edt::data_document::reload()
{
	if (!m_open) {
		return open_result::failure("the document is not open");
	}
	loaded state;
	if (open_result result = load(m_source, state); !result.opened) {
		return result;
	}
	apply(m_source, std::move(state));
	announce();
	return open_result::ok();
}

edt::save_result edt::data_document::save_over()
{
	return write(true);
}

void edt::data_document::flag_changed(const std::string& disk_version)
{
	m_changed_on_disk = true;
	if (disk_version != m_flagged_version) {
		m_flagged_version = disk_version;
		++m_disk_changes;
	}
}

edt::data_document::~data_document()
{
	unwatch_files();
}

void edt::data_document::watch_files()
{
	std::vector<res::tag> wanted{ m_source };
	wanted.insert(wanted.end(), m_parents.begin(), m_parents.end());
	sync_watches(wanted);
}

void edt::data_document::sync_watches(const std::vector<res::tag>& wanted)
{
	res::resource_system* resources = m_services.writable_resources();
	if (m_watching != resources) {
		unwatch_files();
		m_watching = resources;
	}
	if (!m_watching) {
		return;
	}
	// A watcher registered again would get a newer epoch than a signal that is still waiting for the main thread, and
	// that signal would skip it: only what changed is (un)watched.
	for (auto it = m_watched.begin(); it != m_watched.end();) {
		if (std::ranges::find(wanted, *it) == wanted.end()) {
			m_watching->unwatch(*it, this);
			it = m_watched.erase(it);
		} else {
			++it;
		}
	}
	for (const res::tag& tag : wanted) {
		if (std::ranges::find(m_watched, tag) == m_watched.end()) {
			m_watching->watch(tag, this, [this](const res::tag& changed) { on_changed(changed); });
			m_watched.push_back(tag);
		}
	}
}

void edt::data_document::unwatch_files()
{
	if (m_watching) {
		for (const res::tag& tag : m_watched) {
			m_watching->unwatch(tag, this);
		}
	}
	m_watched.clear();
	m_watching = nullptr;
}

void edt::data_document::announce() const
{
	if (m_events) {
		m_events->document_changed.emit(id(), m_generation);
	}
}

void edt::data_document::on_changed(const res::tag& tag)
{
	if (!m_open) {
		return;
	}
	if (tag == m_source) {
		json::object on_disk;
		std::string version;
		desc_file_read seen;
		const open_result read = read_file(m_source, on_disk, version, &seen);
		if (read.opened && version == m_version) {
			m_changed_on_disk = false;
			return;
		}
		// What the outside change is called when the file does not parse: its bytes, or "missing" when it is gone.
		if (!read.opened) version = seen.missing ? std::string{ "missing" } : (seen.version.empty() ? std::string{ "unreadable" } : seen.version);
		if (!is_dirty()) {
			loaded state;
			if (open_result reloaded = load(m_source, state); reloaded.opened) {
				apply(m_source, std::move(state));
				announce();
				return;
			}
		}
		flag_changed(version);
		announce();
		return;
	}

	json::object inherited;
	std::vector<res::tag> parents;
	if (open_result chain = resolve_chain(m_source, m_own, m_type, inherited, parents); !chain.opened) {
		std::vector<res::tag> wanted{ m_source };
		wanted.insert(wanted.end(), parents.begin(), parents.end());
		sync_watches(wanted);
		if (m_chain_problem != chain.error) {
			m_chain_problem = std::move(chain.error);
			egLOG_WARN("editor", "{} keeps its previous parents: {}", m_source.view(), m_chain_problem);
			announce();
		}
		return;
	}
	const bool recovered = !m_chain_problem.empty();
	m_chain_problem.clear();
	if (!recovered && inherited == m_inherited && parents == m_parents) {
		return;
	}
	m_inherited = std::move(inherited);
	m_parents = std::move(parents);
	watch_files();
	++m_generation;
	announce();
}

json::value edt::data_document::shown() const
{
	const desc::type_schema* type_schema = schema();
	const desc::desc_system* descs = m_services.descs();
	if (!type_schema || !descs) {
		return json::object{};
	}
	return schema_ui::compose(*type_schema, descs->schemas(), { &m_inherited, &m_own });
}

json::value edt::data_document::base() const
{
	const desc::type_schema* type_schema = schema();
	const desc::desc_system* descs = m_services.descs();
	if (!type_schema || !descs) {
		return json::object{};
	}
	return schema_ui::compose(*type_schema, descs->schemas(), { &m_inherited }, false);
}

bool edt::data_document::edit(const std::function<bool(json::value&)>& draw)
{
	const desc::type_schema* type_schema = schema();
	const desc::desc_system* descs = m_services.descs();
	if (!m_open || !type_schema || !descs) {
		return false;
	}
	if (!schema_ui::edit_component(*type_schema, descs->schemas(), &m_inherited, m_own, draw)) {
		return false;
	}
	++m_generation;
	return true;
}

bool edt::data_document::edit_marked(const std::function<bool(json::value&, const schema_ui::field_origin&)>& draw)
{
	const desc::type_schema* type_schema = schema();
	const desc::desc_system* descs = m_services.descs();
	if (!m_open || !type_schema || !descs) {
		return false;
	}
	const std::string parent = m_parents.empty() ? std::string{} : std::string{ m_parents.front().view() };
	if (!schema_ui::edit_component_marked(*type_schema, descs->schemas(), &m_inherited, m_own, parent, draw)) {
		return false;
	}
	++m_generation;
	return true;
}

bool edt::data_document::restore_own(json::object own)
{
	for (const std::string_view key : { "__type", "__parent" }) {
		const json::value* current = m_own.if_contains(key);
		const json::value* wanted = own.if_contains(key);
		if ((current == nullptr) != (wanted == nullptr) || (current && !(*current == *wanted))) {
			return false;
		}
	}
	if (!(own == m_own)) {
		m_own = std::move(own);
		++m_generation;
	}
	return true;
}

void edt::data_document::commit_pending()
{
	if (!has_pending_edit()) {
		return;
	}
	record_change(m_committed_own, m_own);
	m_committed_own = m_own;
}

void edt::data_document::record_change(const json::object& before, const json::object& after)
{
	std::vector<std::string> pointers;
	collect_changes({}, before, after, pointers);
	std::string label = edit_label(pointers);
	m_history.record(std::move(label), cmd::origin::ui(), std::make_unique<data_change>(*this, before, after, std::move(pointers)));
}

bool edt::data_document::undo()
{
	commit_pending();
	if (!m_open) {
		return false;
	}
	try {
		if (!m_history.undo()) {
			return false;
		}
	} catch (const restore_refused&) {
		m_history.discard_next_undo();
		egLOG_WARN("editor", "{}: an undo was refused (the __type or __parent is not the one it was recorded for), the record is dropped", m_source.view());
		return false;
	}
	m_committed_own = m_own;
	announce();
	return true;
}

bool edt::data_document::redo()
{
	commit_pending();
	if (!m_open) {
		return false;
	}
	try {
		if (!m_history.redo()) {
			return false;
		}
	} catch (const restore_refused&) {
		m_history.discard_next_redo();
		egLOG_WARN("editor", "{}: a redo was refused (the __type or __parent is not the one it was recorded for), the record is dropped", m_source.view());
		return false;
	}
	m_committed_own = m_own;
	announce();
	return true;
}

std::optional<edt::history_entry> edt::data_document::next_undo_entry() const
{
	if (has_pending_edit()) {
		std::vector<std::string> pointers;
		collect_changes({}, m_committed_own, m_own, pointers);
		return history_entry{ edit_label(pointers), cmd::origin::ui() };
	}
	const history_record* record = m_open ? m_history.next_undo() : nullptr;
	return record ? std::optional<history_entry>{ history_entry{ record->label, record->origin } } : std::nullopt;
}

std::optional<edt::history_entry> edt::data_document::next_redo_entry() const
{
	if (has_pending_edit()) {
		return std::nullopt;
	}
	const history_record* record = m_open ? m_history.next_redo() : nullptr;
	return record ? std::optional<history_entry>{ history_entry{ record->label, record->origin } } : std::nullopt;
}

edt::save_result edt::data_document::save()
{
	return write(false);
}

edt::save_result edt::data_document::write(bool over)
{
	if (!m_open) {
		return save_result::failure("the document is not open");
	}
	commit_pending();
	// An overwrite writes the document even when it has no edits: the file on disk is what the person chose to replace.
	if (!over && !is_dirty()) {
		return save_result::ok();
	}
	if (!over && m_changed_on_disk) {
		return save_result::failure(std::format("{} changed on disk since it was opened: nothing was written", where(m_source)));
	}
	desc::desc_system* descs = m_services.descs();
	res::resource_system* reader = m_services.resources();
	res::resource_system* writer = m_services.writable_resources();
	if (!descs || !reader || !writer) {
		return save_result::failure("the project resources are not available");
	}

	json::object on_disk;
	std::string disk_version;
	desc_file_read checks;
	if (open_result read = read_file(m_source, on_disk, disk_version, &checks); !read.opened) {
		// An overwrite goes on over a file that is gone or no longer parses, as long as the path itself is fine: there is no
		// version to compare, and that is the very thing the person chose to replace.
		if (!over || !checks.located) {
			return save_result::failure(std::format("{} cannot be checked before it is written: {}", where(m_source), read.error));
		}
	}
	if (!checks.top_layer) {
		return save_result::failure(std::format("{} belongs to a lower layer (the engine or a package), not to the project's own res/: nothing was written", where(m_source)));
	}
	if (checks.in_open_level) {
		return save_result::failure(std::format("{} belongs to the open level now: nothing was written", where(m_source)));
	}
	if (!over && disk_version != m_version) {
		// The watcher may not have said so yet (its signal waits for the main thread): the file is known to differ now.
		flag_changed(disk_version);
		return save_result::failure(std::format("{} changed on disk since it was opened: nothing was written", where(m_source)));
	}

	json::value content(m_own);
	for (const res::diagnostic& item : validate_desc_tree(*descs, content, m_source.view())) {
		if (item.severity == res::diagnostic_severity::error) {
			return save_result::failure(std::format("{}: {}", where(m_source, item.json_path), item.reason));
		}
	}
	descs->stamp_tree(content);
	const std::vector<std::byte> bytes = descs->serialize_to_bytes(content);
	if (bytes.size() > k_desc_file_limit) {
		return save_result::failure(std::format("{} would be {} bytes: a data document holds at most {}", where(m_source), bytes.size(), k_desc_file_limit));
	}
	if (!writer->store_sync(m_source, bytes)) {
		return save_result::failure(std::format("{} could not be written", where(m_source)));
	}
	writer->signal_changed(m_source);

	m_own = std::move(content.as_object());
	m_saved_own = m_own;
	m_committed_own = m_own;
	m_history.mark_saved();
	m_version = code_version_of(bytes);
	m_changed_on_disk = false;
	m_flagged_version.clear();
	return save_result::ok();
}
