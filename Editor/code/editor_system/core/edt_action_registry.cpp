#include <snk/editor/edt_action_registry.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <cctype>
#include <utility>

namespace json = boost::json;

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

struct edt::action_registry::ui_entry
{
	std::uint64_t key = 0;
	std::string id;
	action_ui ui;
	std::optional<shortcut> binding;
	// Owns the command: the entry going away unregisters it.
	cmd::registration command;
};

struct edt::action_registry::menu_decl
{
	std::uint64_t key = 0;
	std::string name;
};

edt::action_registry::action_registry()
	: m_owned(std::make_unique<cmd::command_registry>())
	, m_commands(*m_owned)
{
}

edt::action_registry::action_registry(cmd::command_registry& shared)
	: m_commands(shared)
{
}

edt::action_registry::~action_registry() = default;

std::shared_ptr<edt::action_registry::ui_entry> edt::action_registry::find_ui(std::string_view id) const
{
	const auto it = std::find_if(m_ui.begin(), m_ui.end(),
		[id](const std::shared_ptr<ui_entry>& e) { return e->id == id; });
	return it != m_ui.end() ? *it : nullptr;
}

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

	cmd::command_desc command;
	command.id = std::move(desc.id);
	command.version = desc.schema_version;
	command.summary = desc.label;
	command.args_schema = std::move(desc.args_schema);
	command.result_schema = std::move(desc.result_schema);
	command.expose_as_tool = desc.expose_as_tool;
	command.mutates = desc.mutates;
	command.invoke = [execute = std::move(desc.execute)](const cmd::call& request) { return execute(request.args); };
	command.enabled = std::move(desc.enabled);

	action_ui ui;
	ui.label = std::move(desc.label);
	ui.menu_path = std::move(desc.menu_path);
	ui.shortcut = std::move(desc.shortcut);
	ui.shortcut_hint = std::move(desc.shortcut_hint);
	ui.separator_before = desc.separator_before;
	ui.activate = std::move(desc.activate);
	return add(std::move(command), std::move(ui));
}

edt::registration edt::action_registry::add(cmd::command_desc command, std::optional<action_ui> ui)
{
	std::optional<shortcut> binding;
	if (ui) {
		if (ui->label.empty()) {
			egLOG_ERROR("Editor/Actions", "action '{}' not registered: it needs a label", command.id);
			return {};
		}
		if (ui->menu_path.find('/') != std::string::npos) {
			egLOG_ERROR("Editor/Actions", "action '{}' not registered: menu path '{}' - submenus are not supported", command.id, ui->menu_path);
			return {};
		}
		if (!ui->shortcut.empty()) {
			binding = shortcut::parse(ui->shortcut);
			if (!binding) {
				egLOG_ERROR("Editor/Actions", "action '{}' not registered: malformed shortcut '{}'", command.id, ui->shortcut);
				return {};
			}
		}
	}

	std::string id = command.id;
	cmd::registration registered = m_commands.add(std::move(command));
	if (!registered.active()) return {};

	if (ui) {
		// The registry has filled the default schemas and checked them, so `required` can be read safely.
		const json::value* required = m_commands.find(id)->args_schema.as_object().if_contains("required");
		const bool needs_args = required && !required->as_array().empty();
		if (needs_args && !ui->activate && (!ui->menu_path.empty() || !ui->shortcut.empty())) {
			egLOG_ERROR("Editor/Actions", "action '{}' not registered: it has required arguments, so a menu item or shortcut needs `activate`", id);
			return {};
		}
		if (binding) {
			const auto taken = std::find_if(m_ui.begin(), m_ui.end(),
				[&binding](const std::shared_ptr<ui_entry>& e) { return e->binding == binding; });
			if (taken != m_ui.end()) {
				egLOG_WARN("Editor/Actions", "action '{}': shortcut '{}' is already bound to '{}', which wins", id, ui->shortcut, (*taken)->id);
			}
		}
	}

	// Nothing of the editor to track: the command's own handle is the registration.
	if (!ui) return registered;
	const std::uint64_t key = m_next_key++;
	auto added = std::make_shared<ui_entry>();
	added->key = key;
	added->id = std::move(id);
	added->ui = std::move(*ui);
	added->binding = std::move(binding);
	added->command = std::move(registered);
	m_ui.push_back(std::move(added));
	return registration(m_alive, [this, key] {
		std::erase_if(m_ui, [key](const std::shared_ptr<ui_entry>& e) { return e->key == key; });
	});
}

bool edt::action_registry::contains(std::string_view id) const
{
	return m_commands.contains(id);
}

std::vector<std::string> edt::action_registry::menus() const
{
	std::vector<std::string> out;
	const auto add_unique = [&out](const std::string& name) {
		if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
	};
	for (const auto& m : m_menus) add_unique(m->name);
	for (const auto& a : m_ui) {
		if (!a->ui.menu_path.empty()) add_unique(a->ui.menu_path);
	}
	return out;
}

std::vector<edt::menu_item_info> edt::action_registry::items(std::string_view menu) const
{
	std::vector<menu_item_info> out;
	for (const auto& a : m_ui) {
		if (a->ui.menu_path != menu) continue;
		out.push_back({
			a->id,
			a->ui.label,
			a->ui.shortcut_hint.empty() ? a->ui.shortcut : a->ui.shortcut_hint,
			a->ui.separator_before,
			m_commands.is_enabled(a->id) });
	}
	return out;
}

bool edt::action_registry::is_enabled(std::string_view id) const
{
	return m_commands.is_enabled(id);
}

const cmd::command_desc* edt::action_registry::find(std::string_view id) const
{
	return m_commands.find(id);
}

std::vector<const cmd::command_desc*> edt::action_registry::tools() const
{
	std::vector<const cmd::command_desc*> out = m_commands.list();
	std::erase_if(out, [](const cmd::command_desc* c) { return !c->expose_as_tool; });
	return out;
}

std::vector<std::string> edt::action_registry::ids() const
{
	const auto commands = m_commands.list();
	std::vector<std::string> out;
	out.reserve(commands.size());
	for (const auto* c : commands) out.push_back(c->id);
	return out;
}

edt::action_result edt::action_registry::invoke(std::string_view id, const boost::json::value& args, cmd::origin from)
{
	return m_commands.invoke(id, args, std::move(from));
}

edt::action_result edt::action_registry::invoke(std::string_view id, const cmd::call& request)
{
	return m_commands.invoke(id, request);
}

bool edt::action_registry::activate(std::string_view id)
{
	if (!m_commands.is_enabled(id)) return false;
	std::shared_ptr<ui_entry> entry = find_ui(id);
	if (entry && entry->ui.activate) {
		// The callback may unregister its own action, even to add the same id again. Its own copy keeps it alive
		// while it runs; the entry is let go first, so the removal takes effect at once (the entry owns the command).
		const std::function<void()> run = entry->ui.activate;
		entry.reset();
		run();
		return true;
	}
	const action_result outcome = invoke(id, json::object{});
	if (!outcome.ok) egLOG_WARN("Editor/Actions", "{}", outcome.error);
	return true;
}

bool edt::action_registry::dispatch(const shortcut& chord)
{
	for (const auto& a : m_ui) {
		if (a->binding == chord && m_commands.is_enabled(a->id)) return activate(a->id);
	}
	return false;
}

void edt::action_registry::for_each_shortcut(const std::function<void(const shortcut&)>& visit) const
{
	for (const auto& a : m_ui) {
		if (a->binding && m_commands.is_enabled(a->id)) visit(*a->binding);
	}
}
