#pragma once
#include <snk/editor/edt_panel_base.h>
#include "edt_asset_filter.h"
#include "edt_asset_layers.h"
#include "edt_asset_operation_form.h"
#include "edt_asset_usages.h"
#include "edt_ui_probe.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <snk/model_codec/res_tag.h>
#include <string>
#include <vector>

namespace edt
{
	class asset_kind_registry;
	class thumbnail_service;
	class code_section;
	class document_creator;
	class document_create_form;
	class image_import_form;
	class material_creator;
	class material_create_form;
	class image_importer;
	class prefab_variant_creator;
	class prefab_variant_form;

	class asset_browser_panel : public panel_base
	{
	public:
		asset_browser_panel();

		// Physical roots behind res://, lowest layer first, top last (as the resource system mounts them).
		void set_layers(std::vector<std::filesystem::path> layers);

		// The Code tree under the res:// tree; none when the project has no code/ folder (nothing is drawn then).
		void set_code_section(std::shared_ptr<code_section> section);

		// Reports the screen rectangle of every entry (headless smoke, editor/ui_probe); null: nothing.
		void set_ui_probe(std::shared_ptr<ui_probe> probe) { m_ui_probe = std::move(probe); }

		// "Create Document..." in the context menu of a folder (a row, the blank of the open folder, a node of the tree): opens a
		// window to pick a document type and a name; `on_created` gets the tag of the new file. Without a creator the menu has no
		// such item. The same creator makes "New Material..." (#1198): a window with a free name, then `on_created` too.
		void set_document_creator(std::shared_ptr<document_creator> creator, std::function<void(const res::tag&)> on_created);

		// The thumbnails of the picture rows (#1192); null: none, a picture row looks as any other. Not owned.
		void set_thumbnails(thumbnail_service* thumbnails) { m_thumbnails = thumbnails; }

		// "Import Image..." in the same menus (#1193): `pick_file` shows the native file picker and calls back with the
		// picked file, then a window takes wrap, filter, mips and the file name. `importer` outlives the panel. Null: no
		// item.
		using image_picker = std::function<void(std::function<void(const std::filesystem::path&)> picked)>;
		void set_image_importer(image_importer* importer, image_picker pick_file);
		// The registry the "Kinds" filter reads the kinds of modules from; null: only the fixed kinds.
		void set_asset_kinds(const asset_kind_registry* kinds) { m_kinds = kinds; }

		~asset_browser_panel() override;

		// "Used by" of the file picked by a click (#774, EPIC-57 section 6.3): its usages as saved on the disk. Without a
		// source the section is not drawn.
		void set_usages(usage_list::report_source report, std::function<usage_stamp()> stamp)
		{
			m_usages.set_source(std::move(report), std::move(stamp));
		}

		// "Rename...", "Move..." and "Delete..." in the context menu of a file (#775, EPIC-57 section 6.4): a window with the preview of what
		// the operation does and why it is refused, then the report of what it did with an Undo. They run what the asset commands run
		// (`ports.run`), as a person's call. Without ports the menu has no such items.
		void set_asset_operations(asset_operation_ports ports);

		// Callback fired on double-click on a file entry; the argument is relative to res:// (layer independent).
		// "Create Variant" in the context menu of a `*.prefab.desc` file (#151): a window with the suggested name (Enter creates, Esc cancels);
		// `on_created` gets the new file. Null: no item.
		void set_variant_creator(std::shared_ptr<prefab_variant_creator> creator, std::function<void(const res::tag&)> on_created);
		void set_on_file_double_clicked(std::function<void(const std::filesystem::path&)> cb) {
			m_on_file_double_clicked = std::move(cb);
		}
		// "Open as data" in the context menu of a file `offered` says yes to (a material, #1196): `open` opens it in the generic
		// data window instead of the window of its type. `offered` is asked once, when the menu opens. Null: no item.
		void set_open_as_data(std::function<bool(const res::tag&)> offered, std::function<void(const res::tag&)> open)
		{
			m_open_as_data_offered = std::move(offered);
			m_open_as_data = std::move(open);
		}

	protected:
		void on_render() override;
		// Closed, collapsed or in a hidden tab: nothing is drawn, so the probe's items are not on the screen.
		void on_hidden() override;

	private:
		// Non-recursive merged scan of m_current_rel into m_entries.
		void refresh_entries();
		// A picture row's thumbnail and name over the selectable just drawn at screen position (x, y); `failure` gets why there is none.
		void draw_picture_row(const layer_entry& src, const std::string& label, float x, float y, float height, std::string& failure);
		// Recursive directory tree for the left panel; rel_dir is relative to res://.
		void draw_dir_tree(const std::filesystem::path& rel_dir, int depth = 0);
		// Navigate the right panel to rel_dir (relative to res://).
		void navigate_to(const std::filesystem::path& rel_dir);
		// The "Used by" section of the picked file, under the list.
		void draw_usages();
		void draw_usage_rows();
		void ask_operation(asset_operation what, const std::filesystem::path& relative);
		void draw_operation_window();
		void draw_operation_editing(asset_operation_form& form);
		void draw_operation_force(asset_operation_form& form);
		void draw_operation_report(asset_operation_form& form);
		// Lines in a framed, scrolling region; `error` draws them in the error colour.
		void draw_operation_lines(const char* id, const std::vector<std::string>& lines, bool error);
		// Short icon prefix for a file based on extension.
		const char* get_file_icon(const std::filesystem::path& path) const;

		std::vector<std::filesystem::path> m_layers;
		std::shared_ptr<code_section> m_code_section;
		std::shared_ptr<ui_probe> m_ui_probe;
		std::filesystem::path m_current_rel;
		// The file picked by a click, relative to res://; empty: none.
		std::filesystem::path m_selected_rel;
		usage_list m_usages;
		char m_search[128] = {};
		asset_filter m_filter;
		desc_type_cache m_desc_types;
		const asset_kind_registry* m_kinds = nullptr;
		thumbnail_service* m_thumbnails = nullptr;

		struct entry {
			layer_entry src;
			std::string label; // pre-built "icon filename"
		};
		std::vector<entry> m_entries;
		bool m_needs_refresh = true;
		// The labels were built with icon glyphs; a theme that loads or drops the icon font rebuilds them.
		bool m_labels_have_icons = false;
		std::function<void(const std::filesystem::path&)> m_on_file_double_clicked;
		std::function<bool(const res::tag&)> m_open_as_data_offered;
		std::function<void(const res::tag&)> m_open_as_data;
		// What `m_open_as_data_offered` said for the file whose context menu is open.
		bool m_offers_open_as_data = false;
		std::shared_ptr<prefab_variant_creator> m_variant_creator;
		std::unique_ptr<prefab_variant_form> m_variant_form;
		std::function<void(const res::tag&)> m_on_variant_created;
		bool m_variant_requested = false;

		// The menu item of one folder (`folder` is relative to res://): asks for the window to open at the end of the frame.
		void draw_create_menu_item(const std::filesystem::path& folder);
		void draw_create_window();
		void draw_material_window();
		void draw_variant_window();
		void draw_image_window();
		bool has_folder_menu() const { return m_create_form || m_image_form; }

		std::shared_ptr<document_creator> m_creator;
		std::unique_ptr<document_create_form> m_create_form;
		std::function<void(const res::tag&)> m_on_document_created;
		bool m_create_requested = false;
		std::unique_ptr<material_creator> m_material_creator;
		std::unique_ptr<material_create_form> m_material_form;
		bool m_material_requested = false;

		std::unique_ptr<image_import_form> m_image_form;
		image_picker m_pick_image;
		bool m_image_requested = false;
		// A picker's answer that comes after the panel is gone does nothing.
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);

		std::unique_ptr<asset_operation_form> m_operation_form;
		bool m_operation_requested = false;
	};
}
