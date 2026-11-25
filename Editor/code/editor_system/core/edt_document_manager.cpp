#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <string>

namespace {
constexpr const char* k_preview_problem_source = "preview";

using adopted_list = std::vector<std::unique_ptr<edt::document>>;

using adopted_table = std::map<const edt::document_manager*, adopted_list>;

// Never destroyed: a manager that outlives the other statics (the editor context does) still finds its table.
adopted_table& adopted_lists()
{
	static auto* lists = new adopted_table;
	return *lists;
}

// For a read: a manager that adopted nothing has no entry, and a read makes none.
const adopted_list& adopted_view(const edt::document_manager* manager)
{
	static const adopted_list none;
	const auto found = adopted_lists().find(manager);
	return found != adopted_lists().end() ? found->second : none;
}

adopted_list& adopted_of(const edt::document_manager* manager)
{
	return adopted_lists()[manager];
}

struct link_record
{
	edt::document_id id = 0;
	edt::link_kind kind = edt::link_kind::asset;
	edt::document_id parent = 0;
	std::string anchor;
};

struct host_record
{
	edt::host_id id = 0;
	edt::document_id link = 0;
};

struct link_tables
{
	std::vector<link_record> links;
	std::vector<host_record> hosts;
	edt::host_id next_host = 1;
	std::uint64_t revision = 0;
	bool sweeping = false;
};

using link_table = std::map<const edt::document_manager*, link_tables>;

// Never destroyed, like the adopted lists.
link_table& link_tables_of_all()
{
	static auto* tables = new link_table;
	return *tables;
}

link_tables& links_of(const edt::document_manager* manager)
{
	return link_tables_of_all()[manager];
}

// For a read: a manager with no links has no entry, and a read makes none.
const link_tables& links_view(const edt::document_manager* manager)
{
	static const link_tables none;
	const auto found = link_tables_of_all().find(manager);
	return found != link_tables_of_all().end() ? found->second : none;
}

const link_record* find_record(const link_tables& tables, edt::document_id id)
{
	const auto found = std::ranges::find_if(tables.links, [id](const link_record& r) { return r.id == id; });
	return found != tables.links.end() ? &*found : nullptr;
}

const host_record* find_host_showing(const link_tables& tables, edt::document_id link)
{
	const auto found = std::ranges::find_if(tables.hosts, [link](const host_record& h) { return h.link == link; });
	return found != tables.hosts.end() ? &*found : nullptr;
}

host_record* find_host_showing(link_tables& tables, edt::document_id link)
{
	return const_cast<host_record*>(find_host_showing(std::as_const(tables), link));
}

const host_record* find_host(const link_tables& tables, edt::host_id id)
{
	const auto found = std::ranges::find_if(tables.hosts, [id](const host_record& h) { return h.id == id; });
	return found != tables.hosts.end() ? &*found : nullptr;
}

host_record* find_host(link_tables& tables, edt::host_id id)
{
	return const_cast<host_record*>(find_host(std::as_const(tables), id));
}

void erase_host(link_tables& tables, edt::host_id id)
{
	std::erase_if(tables.hosts, [id](const host_record& h) { return h.id == id; });
	++tables.revision;
}

// A host that lets go of `link`: it goes back to the link's parent when that is a link too, open and without a host, else it closes
// (a root document, the level, is not a thing a host shows by itself).
void release_host_of(link_tables& tables, edt::document_id link, const std::function<bool(edt::document_id)>& is_open)
{
	host_record* host = find_host_showing(tables, link);
	if (!host) return;
	const link_record* record = find_record(tables, link);
	const edt::document_id parent = record && record->kind == edt::link_kind::instance ? record->parent : 0;
	if (parent != 0 && find_record(tables, parent) && is_open(parent) && !find_host_showing(tables, parent)) {
		host->link = parent;
		++tables.revision;
		return;
	}
	erase_host(tables, host->id);
}

std::string source_key(const res::tag& source)
{
	std::string protocol{ source.protocol() };
	std::string rest{ source.relative() };
	std::vector<std::string> parts;
	std::size_t start = 0;
	while (start <= rest.size()) {
		std::size_t end = rest.find('/', start);
		if (end == std::string::npos) end = rest.size();
		std::string part = rest.substr(start, end - start);
		start = end + 1;
		if (part.empty() || part == ".") continue;
		if (part == "..") {
			if (!parts.empty()) parts.pop_back();
			continue;
		}
		parts.push_back(std::move(part));
	}
	std::string key = protocol + "://";
	for (const std::string& part : parts) key += part + "/";
#if defined(_WIN32) || defined(__APPLE__)
	std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
	return key;
}
}

edt::document_manager::document_manager(document& scene, editor_events& events)
	: m_scene(&scene)
	, m_events(events)
{
	m_scene->assign_id(m_next_id++);
	m_scene->bind_events(&m_events);
	m_active = m_scene->id();
}

edt::document_manager::~document_manager()
{
	adopted_lists().erase(this);
	link_tables_of_all().erase(this);
}

void edt::document_manager::set_preview_kinds(asset_kind_registry& kinds, editor_services& services, problem_list* problems)
{
	m_kinds = &kinds;
	m_services = &services;
	m_problems = problems;
	m_kind_removals = kinds.on_preview_removed([this](const asset_preview_desc& preview) { preview_removed(preview); });
}

edt::document& edt::document_manager::scene()
{
	ASSERT_MSG(m_scene, "the scene document is gone: document_manager::shutdown was called");
	return *m_scene;
}

const edt::document& edt::document_manager::scene() const
{
	ASSERT_MSG(m_scene, "the scene document is gone: document_manager::shutdown was called");
	return *m_scene;
}

void edt::document_manager::shutdown()
{
	if (!m_scene) return;
	for (previewable_document* open : previews()) close(open->id());
	m_open_previews.clear();
	while (!adopted_view(this).empty()) close(adopted_view(this).back()->id());
	m_scene->bind_events(nullptr);
	m_scene = nullptr;
	m_active = 0;
}

edt::document* edt::document_manager::active()
{
	return find(m_active);
}

edt::document* edt::document_manager::find(document_id id)
{
	if (m_scene && id == m_scene->id()) return m_scene;
	for (previewable_document* open : m_open_previews) {
		if (open->is_open() && open->id() == id) return open;
	}
	const adopted_list& adopted = adopted_view(this);
	const auto kept = std::find_if(adopted.begin(), adopted.end(), [id](const std::unique_ptr<document>& d) { return d->id() == id; });
	return kept != adopted.end() ? kept->get() : nullptr;
}

void edt::document_manager::set_active(document_id id)
{
	if (!find(id) || id == m_active) return;
	m_active = id;
	m_events.document_activated.emit(id);
}

edt::document_manager::kept_preview& edt::document_manager::kind_of(const asset_preview_desc& preview)
{
	const auto known = std::find_if(m_previews.begin(), m_previews.end(), [&preview](const kept_preview& p) { return p.preview_id == preview.id; });
	if (known != m_previews.end()) return *known;
	m_previews.push_back({ preview.id, preview.multiple, {} });
	return m_previews.back();
}

edt::previewable_document* edt::document_manager::preview_by_id(document_id id)
{
	for (previewable_document* open : m_open_previews) {
		if (open->is_open() && open->id() == id) return open;
	}
	return nullptr;
}

edt::document_manager::kept_preview* edt::document_manager::kind_holding(const previewable_document* instance)
{
	for (kept_preview& kind : m_previews) {
		const auto holds = std::find_if(kind.instances.begin(), kind.instances.end(),
			[instance](const std::unique_ptr<previewable_document>& d) { return d.get() == instance; });
		if (holds != kind.instances.end()) return &kind;
	}
	return nullptr;
}

edt::previewable_document* edt::document_manager::open_with(const asset_preview_desc& preview, const res::tag& source)
{
	// A preview that closed itself is no open preview any more.
	std::erase_if(m_open_previews, [](const previewable_document* d) { return !d->is_open(); });
	// A document of a `multiple` kind that closed itself never came back through close(): it is destroyed here, not kept.
	for (kept_preview& kept : m_previews) {
		if (kept.multiple) std::erase_if(kept.instances, [](const std::unique_ptr<previewable_document>& d) { return !d->is_open(); });
	}
	forget_closed_links();
	m_failed_preview.reset();
	const bool multiple = kind_of(preview).multiple;
	previewable_document* next = nullptr;
	if (multiple) {
		const std::string wanted = source_key(source);
		for (const auto& candidate : kind_of(preview).instances) {
			if (candidate->is_open() && source_key(candidate->source()) == wanted) {
				const document_id id = candidate->id();
				set_active(id);
				return preview_by_id(id);
			}
		}
		auto made = preview.make_preview(*m_services);
		ASSERT_MSG(made, "make_preview of an asset preview made no document");
		next = made.get();
		kind_of(preview).instances.push_back(std::move(made));
	} else {
		// A single kind replaces every other single-kind preview; a `multiple` one is never closed by it. The closes are
		// announced, and a listener may register another kind, so no reference into the kinds is held across them.
		std::vector<document_id> replaced;
		for (previewable_document* open : m_open_previews) {
			const kept_preview* held = kind_holding(open);
			if (held && !held->multiple) replaced.push_back(open->id());
		}
		for (const document_id id : replaced) close(id);
		kept_preview& kind = kind_of(preview);
		if (kind.instances.empty()) {
			auto made = preview.make_preview(*m_services);
			ASSERT_MSG(made, "make_preview of an asset preview made no document");
			kind.instances.push_back(std::move(made));
		}
		next = kind_of(preview).instances.front().get();
	}
	if (m_problems) m_problems->clear(k_preview_problem_source);

	next->open(source);
	if (next->is_open()) {
		const document_id id = m_next_id++;
		next->assign_id(id);
		m_open_previews.push_back(next);
		m_events.document_opened.emit(id);
		set_active(id);
		// A listener may have closed it, and a multiple kind destroys a closed document.
		return multiple ? preview_by_id(id) : next;
	} else if (multiple) {
		// Nothing was announced; the document that did not open is kept until the next open_preview, so the pointer given back is valid.
		kept_preview& kind = kind_of(preview);
		const auto failed = std::find_if(kind.instances.begin(), kind.instances.end(),
			[next](const std::unique_ptr<previewable_document>& d) { return d.get() == next; });
		if (failed != kind.instances.end()) {
			m_failed_preview = std::move(*failed);
			kind.instances.erase(failed);
		}
	}
	return next;
}

edt::previewable_document* edt::document_manager::open_preview(const res::tag& source)
{
	ASSERT_MSG(m_kinds && m_services, "document_manager::set_preview_kinds was not called: there is no asset preview to make a document");
	const asset_preview_desc* preview = m_kinds->preview_for(source.relative());
	return preview ? open_with(*preview, source) : nullptr;
}

edt::previewable_document* edt::document_manager::open_preview(std::string_view preview_id, const res::tag& source)
{
	ASSERT_MSG(m_kinds && m_services, "document_manager::set_preview_kinds was not called: there is no asset preview to make a document");
	const asset_preview_desc* preview = m_kinds->find_preview(preview_id);
	return preview ? open_with(*preview, source) : nullptr;
}

edt::document& edt::document_manager::adopt_quiet(std::unique_ptr<document> adopted)
{
	ASSERT_MSG(adopted, "document_manager::adopt: no document");
	document& kept = *adopted;
	kept.assign_id(m_next_id++);
	kept.bind_events(&m_events);
	adopted_of(this).push_back(std::move(adopted));
	return kept;
}

edt::document& edt::document_manager::adopt(std::unique_ptr<document> adopted)
{
	document& kept = adopt_quiet(std::move(adopted));
	m_events.document_opened.emit(kept.id());
	return kept;
}

const edt::document* edt::document_manager::find_open(document_id id) const
{
	if (m_scene && id == m_scene->id()) return m_scene;
	for (const previewable_document* open : m_open_previews) {
		if (open->is_open() && open->id() == id) return open;
	}
	const adopted_list& adopted = adopted_view(this);
	const auto kept = std::ranges::find_if(adopted, [id](const std::unique_ptr<document>& d) { return d->id() == id; });
	return kept != adopted.end() ? kept->get() : nullptr;
}

edt::document_id edt::document_manager::open_instance(std::unique_ptr<document> opened, document_id parent, std::string_view anchor)
{
	ASSERT_MSG(opened, "document_manager::open_instance: no document");
	if (!find_open(parent)) return 0;
	if (const std::optional<document_id> existing = find_link(parent, anchor)) return *existing;
	const document_id id = adopt_quiet(std::move(opened)).id();
	link_tables& tables = links_of(this);
	tables.links.push_back({ id, link_kind::instance, parent, std::string(anchor) });
	++tables.revision;
	m_events.document_opened.emit(id);
	return find_open(id) ? id : 0;
}

void edt::document_manager::register_asset_link(document_id id)
{
	if (m_scene && id == m_scene->id()) return;
	link_tables& tables = links_of(this);
	if (find_record(tables, id)) return;
	tables.links.push_back({ id, link_kind::asset, 0, {} });
	++tables.revision;
}

edt::document_id edt::document_manager::link_asset(document_id id)
{
	const document* open = find_open(id);
	if (!open) return 0;
	if (m_scene && id == m_scene->id()) return 0;
	const link_tables& tables = links_view(this);
	if (find_record(tables, id)) return id;
	if (open->source().is_valid()) {
		const std::string wanted = source_key(open->source());
		for (const link_record& record : tables.links) {
			if (record.kind != link_kind::asset) continue;
			const document* other = find_open(record.id);
			if (other && other->source().is_valid() && source_key(other->source()) == wanted) return record.id;
		}
	}
	register_asset_link(id);
	return id;
}

bool edt::document_manager::has_link(document_id id) const
{
	return find_record(links_view(this), id) != nullptr;
}

edt::link_kind edt::document_manager::link_kind_of(document_id id) const
{
	const link_record* record = find_record(links_view(this), id);
	return record ? record->kind : link_kind::asset;
}

edt::document_id edt::document_manager::link_parent(document_id id) const
{
	const link_record* record = find_record(links_view(this), id);
	return record && record->kind == link_kind::instance ? record->parent : 0;
}

std::string edt::document_manager::link_anchor(document_id id) const
{
	const link_record* record = find_record(links_view(this), id);
	return record ? record->anchor : std::string{};
}

std::vector<edt::document_id> edt::document_manager::chain_of(document_id id) const
{
	std::vector<document_id> chain;
	if (!find_open(id)) return chain;
	const link_tables& tables = links_view(this);
	for (document_id at = id; at != 0 && chain.size() <= tables.links.size() + 1;) {
		chain.push_back(at);
		const link_record* record = find_record(tables, at);
		at = record && record->kind == link_kind::instance ? record->parent : 0;
	}
	std::ranges::reverse(chain);
	return chain;
}

std::optional<edt::document_id> edt::document_manager::find_link(document_id parent, std::string_view anchor) const
{
	for (const link_record& record : links_view(this).links) {
		if (record.kind == link_kind::instance && record.parent == parent && record.anchor == anchor) return record.id;
	}
	return std::nullopt;
}

std::vector<edt::document_id> edt::document_manager::descendants_of(document_id id) const
{
	const link_tables& tables = links_view(this);
	std::vector<document_id> out;
	std::set<document_id> seen{ id };
	std::vector<document_id> level{ id };
	while (!level.empty()) {
		std::vector<document_id> next;
		for (const document_id at : level) {
			for (const link_record& record : tables.links) {
				if (record.kind == link_kind::instance && record.parent == at && seen.insert(record.id).second) next.push_back(record.id);
			}
		}
		out.insert(out.end(), next.begin(), next.end());
		level = std::move(next);
	}
	std::ranges::reverse(out);
	return out;
}

std::vector<edt::document_id> edt::document_manager::unhosted_links() const
{
	const link_tables& tables = links_view(this);
	std::vector<document_id> out;
	for (const link_record& record : tables.links) {
		if (!find_host_showing(tables, record.id)) out.push_back(record.id);
	}
	return out;
}

edt::host_id edt::document_manager::open_host(document_id link)
{
	if (!find_open(link)) return 0;
	link_tables& tables = links_of(this);
	if (const host_record* shown = find_host_showing(tables, link)) return shown->id;
	register_asset_link(link);
	const host_id id = tables.next_host++;
	tables.hosts.push_back({ id, link });
	++tables.revision;
	return id;
}

void edt::document_manager::close_host(host_id host)
{
	if (!find_host(links_view(this), host)) return;
	erase_host(links_of(this), host);
}

std::optional<edt::host_id> edt::document_manager::host_of(document_id link) const
{
	const host_record* host = find_host_showing(links_view(this), link);
	return host ? std::optional<host_id>(host->id) : std::nullopt;
}

std::optional<edt::document_id> edt::document_manager::host_link(host_id host) const
{
	const host_record* found = find_host(links_view(this), host);
	return found ? std::optional<document_id>(found->link) : std::nullopt;
}

std::vector<edt::host_id> edt::document_manager::hosts() const
{
	std::vector<host_id> out;
	for (const host_record& host : links_view(this).hosts) out.push_back(host.id);
	return out;
}

edt::host_id edt::document_manager::set_host_link(host_id host, document_id link)
{
	link_tables& tables = links_of(this);
	if (!find_host(tables, host) || !find_open(link)) return 0;
	if (const host_record* shown = find_host_showing(tables, link)) return shown->id;
	register_asset_link(link);
	find_host(tables, host)->link = link;
	++tables.revision;
	return host;
}

edt::host_id edt::document_manager::move_link(document_id link, host_id to_host)
{
	link_tables& tables = links_of(this);
	if (!find_open(link) || !has_link(link)) return 0;
	if (to_host != 0 && !find_host(tables, to_host)) return 0;
	const host_record* shown = find_host_showing(tables, link);
	if (shown && shown->id == to_host) return to_host;
	const auto is_open = [this](document_id other) { return find_open(other) != nullptr; };
	if (to_host == 0) {
		release_host_of(tables, link, is_open);
		const host_id created = tables.next_host++;
		tables.hosts.push_back({ created, link });
		++tables.revision;
		return created;
	}
	if (shown) erase_host(tables, shown->id);
	find_host(tables, to_host)->link = link;
	++tables.revision;
	return to_host;
}

std::uint64_t edt::document_manager::links_revision() const
{
	return links_view(this).revision;
}

edt::document* edt::document_manager::find_by_source(const res::tag& source)
{
	if (!source.is_valid()) return nullptr;
	const std::string wanted = source_key(source);
	document* found = nullptr;
	for_each([&](document& candidate) {
		if (found || !candidate.source().is_valid() || source_key(candidate.source()) != wanted) return;
		// An instance's source() is its parent's tag: that file is not the instance's.
		const link_record* link = find_record(links_view(this), candidate.id());
		if (link && link->kind == link_kind::instance) return;
		found = &candidate;
	});
	return found;
}

edt::previewable_document* edt::document_manager::preview()
{
	previewable_document* last = nullptr;
	for (previewable_document* open : m_open_previews) {
		if (!open->is_open()) continue;
		if (open->id() == m_active) return open;
		last = open;
	}
	return last;
}

std::vector<edt::previewable_document*> edt::document_manager::previews()
{
	std::vector<previewable_document*> out;
	out.reserve(m_open_previews.size());
	for (previewable_document* open : m_open_previews) {
		if (open->is_open()) out.push_back(open);
	}
	return out;
}

void edt::document_manager::close(document_id id)
{
	if (!find(id) || (m_scene && id == m_scene->id())) {
		close_document(id);
		forget_closed_links();
		return;
	}
	close_descendants(id);
	close_document(id);
	document_closed_link(id);
	forget_closed_links();
}

// A preview that closed itself is not announced closed (nobody calls close() for it), while the documents opened from it are: they close
// here, so a listener hears about the children but never about the parent.
void edt::document_manager::forget_closed_links()
{
	link_tables& state = links_of(this);
	if (state.sweeping) return;
	state.sweeping = true;
	for (;;) {
		const link_tables& tables = links_view(this);
		const auto stale = std::ranges::find_if(tables.links, [this](const link_record& r) { return !find_open(r.id); });
		if (stale == tables.links.end()) break;
		const document_id id = stale->id;
		close_or_release_descendants(id);
		document_closed_link(id);
	}
	links_of(this).sweeping = false;
}

void edt::document_manager::close_or_release_descendants(document_id id)
{
	const std::vector<document_id> below = descendants_of(id);
	std::set<document_id> kept;
	std::vector<problem> stayed;
	// Shallowest first: a document stays when it has unsaved changes or hangs from one that stays.
	for (auto at = below.rbegin(); at != below.rend(); ++at) {
		const document* open = find_open(*at);
		if (!open) continue;
		const document_id parent = link_parent(*at);
		if (open->is_dirty() && !kept.contains(parent)) {
			kept.insert(*at);
			// A root now: nothing is above it any more.
			link_tables& tables = links_of(this);
			std::erase_if(tables.links, [at](const link_record& r) { return r.id == *at; });
			++tables.revision;
			const std::string message = "'" + open->title() + "' stayed open: its parent closed";
			egLOG_WARN("Editor/Documents", "{}", message);
			problem item;
			item.severity = problem_severity::warning;
			item.file = std::string(open->source().relative());
			item.message = message;
			stayed.push_back(std::move(item));
		} else if (kept.contains(parent)) {
			kept.insert(*at);
		}
	}
	for (const document_id member : below) {
		if (!kept.contains(member)) close(member);
	}
	// Replaces the earlier rows of this source: the list has no way to add one.
	if (m_problems && !stayed.empty()) m_problems->set("links", std::move(stayed));
}

void edt::document_manager::document_closed_link(document_id id)
{
	link_tables& tables = links_of(this);
	release_host_of(tables, id, [this](document_id other) { return find(other) != nullptr; });
	std::erase_if(tables.links, [id](const link_record& r) { return r.id == id; });
	++tables.revision;
}

void edt::document_manager::close_document(document_id id)
{
	if (m_scene && id == m_scene->id()) {
		egLOG_WARN("Editor/Documents", "The scene document cannot be closed");
		return;
	}
	const auto adopted = std::find_if(adopted_of(this).begin(), adopted_of(this).end(), [id](const std::unique_ptr<document>& d) { return d->id() == id; });
	if (adopted != adopted_of(this).end()) {
		// Out of the list before anything is announced, and destroyed after: a listener may still read the closing document.
		std::unique_ptr<document> closing = std::move(*adopted);
		adopted_of(this).erase(adopted);
		closing->bind_events(nullptr);
		if (m_active == id) {
			m_active = m_scene ? m_scene->id() : 0;
			m_events.document_activated.emit(m_active);
		}
		m_events.document_closed.emit(id);
		return;
	}
	const auto open = std::find_if(m_open_previews.begin(), m_open_previews.end(),
		[id](const previewable_document* d) { return d->is_open() && d->id() == id; });
	if (open == m_open_previews.end()) return;

	previewable_document* closing = *open;
	end_preview(*closing);
	// A `multiple` kind's document is destroyed once it is announced closed; a single kind keeps its one.
	if (kept_preview* kind = kind_holding(closing); kind && kind->multiple) {
		std::erase_if(kind->instances, [closing](const std::unique_ptr<previewable_document>& d) { return d.get() == closing; });
	}
}

void edt::document_manager::end_preview(previewable_document& closing)
{
	const document_id id = closing.id();
	closing.close();
	std::erase(m_open_previews, &closing);
	if (m_active == id) {
		m_active = m_scene ? m_scene->id() : 0;
		m_events.document_activated.emit(m_active);
	}
	m_events.document_closed.emit(id);
}

void edt::document_manager::preview_removed(const asset_preview_desc& preview)
{
	const auto known = std::find_if(m_previews.begin(), m_previews.end(), [&preview](const kept_preview& p) { return p.preview_id == preview.id; });
	if (known == m_previews.end()) return;

	m_failed_preview.reset();
	// Out of the list before anything is announced: a listener of the close may open another preview, which changes the list.
	kept_preview removed = std::move(*known);
	m_previews.erase(known);
	std::vector<problem> unsaved;
	for (const auto& instance : removed.instances) {
		if (!instance->is_open()) continue;
		if (instance->is_dirty()) {
			// Nothing saves it any more: the code that made it is going away. Said where a person looks, not only in the log.
			const std::string message = "the preview '" + instance->title() + "' had unsaved changes and was closed: its asset preview '"
				+ preview.id + "' was unloaded";
			egLOG_WARN("Editor/Documents", "{}", message);
			problem item;
			item.severity = problem_severity::warning;
			item.file = std::string(instance->source().relative());
			item.message = message;
			unsaved.push_back(std::move(item));
		}
		const document_id closing_id = instance->id();
		close_or_release_descendants(closing_id);
		end_preview(*instance);
		document_closed_link(closing_id);
	}
	if (m_problems && !unsaved.empty()) m_problems->set(k_preview_problem_source, std::move(unsaved));
	// `removed` goes here, after the announcements and while the preview's make_preview is still registered.
}

void edt::document_manager::for_each(const std::function<void(document&)>& fn)
{
	std::vector<document_id> adopted_ids;
	adopted_ids.reserve(adopted_view(this).size());
	for (const auto& kept : adopted_view(this)) adopted_ids.push_back(kept->id());

	std::vector<document_id> preview_ids;
	preview_ids.reserve(m_open_previews.size());
	for (const previewable_document* open : m_open_previews) {
		if (open->is_open()) preview_ids.push_back(open->id());
	}

	if (m_scene) fn(*m_scene);
	for (const document_id id : preview_ids) {
		if (document* still_open = find(id)) fn(*still_open);
	}
	for (const document_id id : adopted_ids) {
		if (document* still_open = find(id)) fn(*still_open);
	}
}
