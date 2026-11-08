#include <snk/resource/res_diagnostics.h>
#include <snk/resource/resolvers/res_control_block.hpp>
#include <algorithm>
#include <format>
#include <utility>

namespace {
thread_local res::load_scope* t_current_scope = nullptr;

// Deep enough for any real nesting of inline descs; a broken chain cannot loop forever.
constexpr int k_max_origin_depth = 32;
}

std::string res::json_pointer_token(std::string_view key)
{
	std::string out;
	out.reserve(key.size());
	for (const char c : key) {
		if (c == '~') {
			out += "~0";
		} else if (c == '/') {
			out += "~1";
		} else {
			out += c;
		}
	}
	return out;
}

res::load_error::load_error(std::string_view kind, std::string_view json_path, const std::string& reason)
	: std::runtime_error(reason)
	, m_diagnostic{ diagnostic_severity::error, std::string{ kind }, {}, std::string{ json_path }, reason }
{
}

void res::load_diagnostics::set_resource(std::string resource)
{
	std::lock_guard lock(m_mutex);
	m_resource = std::move(resource);
}

void res::load_diagnostics::add(diagnostic item)
{
	std::lock_guard lock(m_mutex);
	if (m_items.size() >= max_items) {
		++m_dropped;
		return;
	}
	m_items.push_back(std::move(item));
}

void res::load_diagnostics::add_child(const std::shared_ptr<core::res::res_control_block_base>& child)
{
	if (!child) {
		return;
	}
	std::lock_guard lock(m_mutex);
	const bool known = std::ranges::any_of(m_children, [&](const auto& c) {
		return !c.owner_before(child) && !child.owner_before(c);
	});
	if (!known) {
		m_children.push_back(child);
	}
}

void res::load_diagnostics::set_origin(const std::shared_ptr<core::res::res_control_block_base>& owner, std::string json_path)
{
	std::lock_guard lock(m_mutex);
	if (!m_origin.expired()) {
		return;
	}
	m_origin = owner;
	m_origin_path = std::move(json_path);
}

res::load_diagnostics::snapshot res::load_diagnostics::read() const
{
	std::lock_guard lock(m_mutex);
	return snapshot{ m_resource, m_items, m_dropped, m_origin, m_origin_path, m_children };
}

res::load_scope::load_scope(std::shared_ptr<core::res::res_control_block_base> block)
	: m_block(std::move(block))
	, m_previous(t_current_scope)
{
	t_current_scope = this;
}

res::load_scope::~load_scope()
{
	t_current_scope = m_previous;
}

res::load_scope* res::load_scope::current()
{
	return t_current_scope;
}

void res::report_load_warning(std::string_view kind, std::string_view json_path, std::string reason)
{
	if (const auto* scope = load_scope::current(); scope && scope->block()) {
		scope->block()->diagnostics.add(
			diagnostic{ diagnostic_severity::warning, std::string{ kind }, {}, std::string{ json_path }, std::move(reason) });
	}
}

void res::note_field_load(const std::shared_ptr<core::res::res_control_block_base>& child, std::string_view json_path,
	bool inline_copy)
{
	const auto* scope = load_scope::current();
	if (!scope || !scope->block() || !child || child == scope->block()) {
		return;
	}
	scope->block()->diagnostics.add_child(child);
	if (inline_copy) {
		child->diagnostics.set_origin(scope->block(), std::string{ json_path });
	}
}

res::diagnostics_collector::diagnostics_collector(std::size_t limit)
	: m_limit(limit)
{
}

void res::diagnostics_collector::add(diagnostic item)
{
	std::lock_guard lock(m_mutex);
	add_locked(std::move(item));
}

void res::diagnostics_collector::add_locked(diagnostic item)
{
	if (std::ranges::find(m_items, item) != m_items.end()) {
		return;
	}
	if (m_items.size() >= m_limit) {
		++m_dropped;
		return;
	}
	m_items.push_back(std::move(item));
}

void res::diagnostics_collector::collect(const std::shared_ptr<core::res::res_control_block_base>& block)
{
	std::lock_guard lock(m_mutex);
	std::vector<std::shared_ptr<core::res::res_control_block_base>> pending{ block };
	while (!pending.empty()) {
		auto current = std::move(pending.back());
		pending.pop_back();
		if (!current || !m_seen.insert(current.get()).second) {
			continue;
		}
		m_walked.push_back(current);

		const auto own = current->diagnostics.read();
		// An inline copy reports at its owner's file, under the path of the field it was written in.
		std::string resource = own.resource;
		std::string prefix;
		auto owner = own.origin.lock();
		std::string step = own.origin_path;
		for (int depth = 0; owner && depth < k_max_origin_depth; ++depth) {
			prefix = step + prefix;
			const auto above = owner->diagnostics.read();
			resource = above.resource;
			step = above.origin_path;
			owner = above.origin.lock();
		}

		bool error_recorded = false;
		for (auto item : own.items) {
			error_recorded = error_recorded || item.severity == diagnostic_severity::error;
			if (item.resource.empty()) {
				item.resource = resource;
				item.json_path = prefix + item.json_path;
			}
			add_locked(std::move(item));
		}
		// Every failed load shows, also one whose failure no adapter described.
		if (current->status.load(std::memory_order_acquire) == core::res::res_status::error && !error_recorded) {
			add_locked(diagnostic{ diagnostic_severity::error, std::string{ diagnostic_kind::load }, resource, prefix,
				current->error_msg });
		}
		m_dropped += own.dropped;

		for (auto it = own.children.rbegin(); it != own.children.rend(); ++it) {
			if (auto child = it->lock()) {
				pending.push_back(std::move(child));
			}
		}
	}
}

std::vector<res::diagnostic> res::diagnostics_collector::items() const
{
	std::lock_guard lock(m_mutex);
	auto out = m_items;
	if (m_dropped > 0) {
		out.push_back(diagnostic{ diagnostic_severity::warning, std::string{ diagnostic_kind::truncated }, {}, {},
			std::format("{} more diagnostics", m_dropped) });
	}
	return out;
}

std::size_t res::diagnostics_collector::error_count() const
{
	std::lock_guard lock(m_mutex);
	return static_cast<std::size_t>(std::ranges::count_if(m_items,
		[](const diagnostic& item) { return item.severity == diagnostic_severity::error; }));
}
