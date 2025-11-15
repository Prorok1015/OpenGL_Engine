#include "gui_menu_layout.h"
#include <imgui.h>
#include <ranges>
#include "engine_assert.h"

void gui::menu_layout_manager::process()
{
	if (get_is_show_main_menu()) {
		if (ImGui::BeginMainMenuBar()) {
			for (auto& child : root_menu_item.children) {
				process_menu_item(child);
			}
			ImGui::EndMainMenuBar();
		}
	}

	std::vector<menu_tree_item*> del;
	for (int i = 0; i < checked_menu_item_list.size(); ++i) {
		menu_tree_item* item = checked_menu_item_list[i];
		item->checked = item->uiCallback();
		if (!item->checked) {
			del.push_back(item);
			item->notify_check_callback(false);
		}
	}

	for (auto& it : del) {
		checked_menu_item_list.erase(std::remove(checked_menu_item_list.begin(), checked_menu_item_list.end(), it));
	}

	std::vector<std::string> del_imp;
	for (auto& [id, callback] : implicit_callback_list) {
		if (!callback()) {
			del_imp.push_back(id);
		}
	}

	for (auto& imp : del_imp) {
		auto it = std::remove_if(implicit_callback_list.begin(), implicit_callback_list.end(), [imp](auto& i) {return imp == i.first; });
		implicit_callback_list.erase(it);
	}
}

void gui::menu_layout_manager::registrate(std::string_view path, std::function<bool()> callback)
{
	add_menu(path, callback);

}

void gui::menu_layout_manager::unregistrate(std::string_view path)
{
	std::vector<std::string> tokens = split(path, "/");
	menu_tree_item* menuItem = &root_menu_item;

	for (auto& token : tokens) {
		menu_tree_item* curMenuItem = menuItem->find(token);
		if (!curMenuItem) {
			return;
		}
		menuItem = curMenuItem;
	}

	while (true) {
		menu_tree_item* parent = menuItem->parent;
		if (parent->children.size() != 1 || parent == &root_menu_item) {
			parent->remove(menuItem);
			break;
		}
		menuItem = parent;
	}
	delete_menu(menuItem);
}

void gui::menu_layout_manager::register_implicit(const std::string_view id, UI_CALLBACK callback)
{
	auto idx = std::ranges::find_if(implicit_callback_list, [id](auto& i) { return i.first == id; });
	if (idx == implicit_callback_list.end()) {
		implicit_callback_list.push_back(std::pair<std::string, UI_CALLBACK>(id, std::move(callback)));
	}
}

void gui::menu_layout_manager::unregister_implicit(const std::string_view id)
{
	implicit_callback_list.erase(std::remove_if(implicit_callback_list.begin(), implicit_callback_list.end(), [id](auto& i) { return i.first == id; }));
}

std::vector<std::string> gui::menu_layout_manager::split(std::string_view path, std::string_view delim)
{
	std::vector<std::string> tokens;
	for (const auto& word : std::views::split(path, delim)) {
		tokens.emplace_back(std::string_view{ word.data(), word.size() });
	}

	return tokens;
}


void gui::menu_layout_manager::add_menu(std::string_view path, UI_CALLBACK cb)
{
	std::vector<std::string> tokens = split(path, "/");

	menu_tree_item* menuItem = &root_menu_item;

	for (auto& token : tokens) {
		menu_tree_item* curMenuItem = menuItem->find(token);
		if (!curMenuItem) {
			curMenuItem = new menu_tree_item(token);
			menuItem->add(curMenuItem);
		}
		menuItem = curMenuItem;
	}

	ASSERT_MSG(menuItem->uiCallback == nullptr, "Debug menu item already exist"/*, path.CStr()*/);
	menuItem->uiCallback = std::move(cb);
}

void gui::menu_layout_manager::process_menu_item(menu_tree_item* menuItem)
{
	if (!menuItem->uiCallback) {
		if (ImGui::BeginMenu(menuItem->title.c_str())) {
			for (auto& child : menuItem->children) {
				process_menu_item(child);
			}
			ImGui::EndMenu();
		}
	}
	else {
		if (ImGui::MenuItem(menuItem->title.c_str(), nullptr, &menuItem->checked)) {
			if (menuItem->checked) {
				checked_menu_item_list.push_back(menuItem);
				menuItem->notify_check_callback(true);
			}
			else {
				auto id = std::remove(checked_menu_item_list.begin(), checked_menu_item_list.end(), menuItem);
				if (id != checked_menu_item_list.end()) {
					checked_menu_item_list.erase(id);
					menuItem->notify_check_callback(false);
				}
			}
		}
	}
}

gui::menu_layout_manager::menu_tree_item* gui::menu_layout_manager::find_menu(const std::string_view path)
{
	std::vector<std::string> tokens = split(path, "/");

	menu_tree_item* menuItem = &root_menu_item;

	for (auto& token : tokens) {
		menu_tree_item* curMenuItem = menuItem->find(token);
		if (!curMenuItem) {
			return nullptr;
		}
		menuItem = curMenuItem;
	}

	if (menuItem == &root_menu_item) {
		return nullptr;
	}

	return menuItem;
}

void gui::menu_layout_manager::set_menu_checked(menu_tree_item* menuItem, bool checked)
{
	if (!menuItem->children.empty()) {
		menuItem->checked = false;
		return;
	}

	menuItem->checked = checked;
	if (menuItem->checked) {
		checked_menu_item_list.push_back(menuItem);
		menuItem->notify_check_callback(true);
	}
	else {
		auto id = std::remove(checked_menu_item_list.begin(), checked_menu_item_list.end(), menuItem);
		if (id != checked_menu_item_list.end()) {
			checked_menu_item_list.erase(id);
			menuItem->notify_check_callback(false);
		}
	}
}

void gui::menu_layout_manager::set_menu_checked(const std::string_view path, bool checked)
{
	menu_tree_item* menuItem = find_menu(path);
	if (!menuItem) {
		return;
	}

	set_menu_checked(menuItem, checked);
}

bool gui::menu_layout_manager::get_is_item_checked(const std::string_view path) const
{
	return std::ranges::find_if(checked_menu_item_list, [path](auto i) { return i->get_path() == path; }) != checked_menu_item_list.end();
}

void gui::menu_layout_manager::set_check_callback(const std::string_view path, UI_SWITCH_CALLBACK callback)
{
	menu_tree_item* menuItem = find_menu(path);
	if (!menuItem) {
		return;
	}

	menuItem->checkCallback = std::move(callback);
}

void gui::menu_layout_manager::uncheck_all()
{
	for (menu_tree_item* item : checked_menu_item_list) {
		item->checked = false;
	}
	checked_menu_item_list.clear();
}

void gui::menu_layout_manager::delete_menu(menu_tree_item* menuItem)
{
	for (menu_tree_item* child : menuItem->children) {
		delete_menu(child);
	}

	set_menu_checked(menuItem, false);
	delete menuItem;
}

gui::menu_layout_manager::menu_tree_item* gui::menu_layout_manager::menu_tree_item::find(const std::string_view title) const
{
	for (const auto& child : children) {
		if (child->title == title) {
			return child;
		}
	}
	return nullptr;
}


void gui::menu_layout_manager::menu_tree_item::add(menu_tree_item* item)
{
	if (!item) {
		return;
	}

	children.push_back(item);
	item->parent = this;
}


void gui::menu_layout_manager::menu_tree_item::remove(menu_tree_item* item)
{
	children.erase(std::remove(children.begin(), children.end(), item));
}


std::string gui::menu_layout_manager::menu_tree_item::get_path() const
{
	std::string fullName;
	if (parent && parent->parent) {
		fullName = parent->get_path() + "/" + title;
		return fullName;
	}

	return title;
}


void gui::menu_layout_manager::menu_tree_item::notify_check_callback(bool enable)
{
	if (checkCallback) {
		checkCallback(enable);
	}
}

