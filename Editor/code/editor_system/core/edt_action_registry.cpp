#include <snk/editor/edt_action_registry.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <cctype>
#include <utility>

namespace {
	std::string upper(std::string_view text)
	{
		std::string out(text);
		for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return out;
	}
}

std::optional<edt::shortcut> edt::shortcut::parse(std::string_view text)
{
	shortcut result;
	bool has_key = false;
	std::size_t begin = 0;
	while (begin <= text.size()) {
		std::size_t end = text.find('+', begin);
		if (end == std::string_view::npos) end = text.size();
		const std::string token = upper(text.substr(begin, end - begin));
		if (token.empty() || has_key) return std::nullopt;

		if (token == "CTRL") {
			result.ctrl = true;
		} else if (token == "SHIFT") {
			result.shift = true;
		} else if (token == "ALT") {
			result.alt = true;
		} else {
			result.key = token;
			has_key = true;
		}
		begin = end + 1;
	}
	if (!has_key) return std::nullopt;
	return result;
}

std::string edt::shortcut::to_string() const
{
	std::string out;
	if (ctrl) out += "Ctrl+";
	if (shift) out += "Shift+";
	if (alt) out += "Alt+";
	return out + key;
}

struct edt::action_registry::entry
{
	std::uint64_t key = 0;
	action_desc desc;
	std::optional<shortcut> binding;

	bool is_enabled() const { return !desc.enabled || desc.enabled(); }
};

struct edt::action_registry::menu_decl
{
	std::uint64_t key = 0;
	std::string name;
};

edt::action_registry::action_registry() = default;
edt::action_registry::~action_registry() = default;

edt::registration edt::action_registry::add_menu(std::string name)
{
	if (name.empty()) {
		egLOG_ERROR("Editor/Actions", "menu not declared: the name is empty");
		return {};
	}
	auto decl = std::make_shared<menu_decl>();
	decl->key = m_next_key++;
	decl->name = std::move(name);
	const std::uint64_t key = decl->key;
	m_menus.push_back(std::move(decl));
	return registration(m_alive, [this, key] {
		std::erase_if(m_menus, [key](const std::shared_ptr<menu_decl>& m) { return m->key == key; });
	});
}

edt::registration edt::action_registry::add(action_desc desc)
{
	if (desc.id.empty() || desc.label.empty() || !desc.execute) {
		egLOG_ERROR("Editor/Actions", "action '{}' not registered: it needs an id, a label and execute", desc.id);
		return {};
	}
	if (contains(desc.id)) {
		egLOG_ERROR("Editor/Actions", "action '{}' not registered: the id is already taken", desc.id);
		return {};
	}
	if (desc.menu_path.find('/') != std::string::npos) {
		egLOG_ERROR("Editor/Actions", "action '{}' not registered: menu path '{}' - submenus are not supported", desc.id, desc.menu_path);
		return {};
	}

	std::optional<shortcut> binding;
	if (!desc.shortcut.empty()) {
		binding = shortcut::parse(desc.shortcut);
		if (!binding) {
			egLOG_ERROR("Editor/Actions", "action '{}' not registered: malformed shortcut '{}'", desc.id, desc.shortcut);
			return {};
		}
		const auto taken = std::find_if(m_actions.begin(), m_actions.end(),
			[&binding](const std::shared_ptr<entry>& e) { return e->binding == binding; });
		if (taken != m_actions.end()) {
			egLOG_WARN("Editor/Actions", "action '{}': shortcut '{}' is already bound to '{}', which wins", desc.id, desc.shortcut, (*taken)->desc.id);
		}
	}

	auto added = std::make_shared<entry>();
	added->key = m_next_key++;
	added->desc = std::move(desc);
	added->binding = std::move(binding);
	const std::uint64_t key = added->key;
	m_actions.push_back(std::move(added));
	return registration(m_alive, [this, key] {
		std::erase_if(m_actions, [key](const std::shared_ptr<entry>& e) { return e->key == key; });
	});
}

bool edt::action_registry::contains(std::string_view id) const
{
	return std::any_of(m_actions.begin(), m_actions.end(),
		[id](const std::shared_ptr<entry>& e) { return e->desc.id == id; });
}

std::vector<std::string> edt::action_registry::menus() const
{
	std::vector<std::string> out;
	const auto add_unique = [&out](const std::string& name) {
		if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
	};
	for (const auto& m : m_menus) add_unique(m->name);
	for (const auto& a : m_actions) {
		if (!a->desc.menu_path.empty()) add_unique(a->desc.menu_path);
	}
	return out;
}

std::vector<edt::menu_item_info> edt::action_registry::items(std::string_view menu) const
{
	std::vector<menu_item_info> out;
	for (const auto& a : m_actions) {
		if (a->desc.menu_path != menu) continue;
		out.push_back({
			a->desc.id,
			a->desc.label,
			a->desc.shortcut_hint.empty() ? a->desc.shortcut : a->desc.shortcut_hint,
			a->desc.separator_before,
			a->is_enabled() });
	}
	return out;
}

bool edt::action_registry::is_enabled(std::string_view id) const
{
	const auto it = std::find_if(m_actions.begin(), m_actions.end(),
		[id](const std::shared_ptr<entry>& e) { return e->desc.id == id; });
	return it != m_actions.end() && (*it)->is_enabled();
}

bool edt::action_registry::run(std::string_view id)
{
	const auto it = std::find_if(m_actions.begin(), m_actions.end(),
		[id](const std::shared_ptr<entry>& e) { return e->desc.id == id; });
	if (it == m_actions.end()) return false;
	// Held while it runs: the action may unregister itself.
	const std::shared_ptr<entry> action = *it;
	if (!action->is_enabled()) return false;
	action->desc.execute();
	return true;
}

bool edt::action_registry::dispatch(const shortcut& chord)
{
	for (const auto& a : m_actions) {
		if (a->binding == chord && a->is_enabled()) return run(a->desc.id);
	}
	return false;
}

void edt::action_registry::for_each_shortcut(const std::function<void(const shortcut&)>& visit) const
{
	for (const auto& a : m_actions) {
		if (a->binding && a->is_enabled()) visit(*a->binding);
	}
}
