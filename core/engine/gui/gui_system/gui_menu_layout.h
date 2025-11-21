#pragma once
#include <functional>
#include <vector>
#include <string>

namespace gui
{
	class menu_layout_manager
	{
	public:
		using UI_CALLBACK = std::function<bool()>;
		using UI_SWITCH_CALLBACK = std::function<void(bool)>;
		struct menu_tree_item
		{
			menu_tree_item() = default;
			menu_tree_item(const std::string_view title)
				: title(title) {};

			std::string get_path() const;
			menu_tree_item* find(const std::string_view title) const;
			void add(menu_tree_item* item);
			void remove(menu_tree_item* item);
			void notify_check_callback(bool enable);

		public:
			bool checked = false;
			menu_tree_item* parent = nullptr;
			UI_CALLBACK uiCallback;
			UI_SWITCH_CALLBACK checkCallback;
			std::string title;
			std::vector<menu_tree_item*> children;
		};

		void registrate(std::string_view path, std::function<bool()> callback);
		void unregistrate(std::string_view path);

		void register_implicit(const std::string_view id, UI_CALLBACK callback);
		void unregister_implicit(const std::string_view id);

		void set_menu_checked(const std::string_view path, bool checked);
		bool get_is_item_checked(const std::string_view path) const;
		void set_check_callback(const std::string_view path, UI_SWITCH_CALLBACK callback);

		void uncheck_all();

		void process();

		void set_show_main_menu(bool show) { is_show_main_menu = show; }
		bool get_is_show_main_menu() const { return is_show_main_menu; };

	private:
		void add_menu(std::string_view path, UI_CALLBACK cb);
		void process_menu_item(menu_tree_item* menuItem);
		void delete_menu(menu_tree_item*);
		void set_menu_checked(menu_tree_item* menuItem, bool checked);
		menu_tree_item* find_menu(const std::string_view path);
		std::vector<std::string> split(std::string_view, std::string_view);

	private:
		bool is_show_main_menu = false;
		menu_tree_item root_menu_item;

		std::vector<menu_tree_item*> checked_menu_item_list;
		std::vector<std::pair<std::string, UI_CALLBACK>> implicit_callback_list;
	};
}