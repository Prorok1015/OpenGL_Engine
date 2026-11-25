#pragma once
#include "edt_data_document_host.h"
#include "edt_desc_file_commands.h"
#include "edt_document_creator.h"
#include "edt_texture_viewer_host.h"
#include <snk/desc/desc_base.hpp>
#include <snk/editor/edt_registration.h>
#include <snk/model_codec/res_tag.h>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace res { class resource_system; }

namespace edt
{
	class asset_browser_panel;
	class desc_problem_publisher;
	class editor_context;
	class editor_layer;
	class level_controller;
	class scene_document;
	class scene_editor;
	struct editor_services;
	namespace picker { class catalog; }

	// Opening an asset and adding one to the scene, whole: the data documents (EPIC-56: their windows and the questions about their
	// unsaved changes), the kinds of the asset registry (a module's `.desc` kind, the prefab editor, "add to the scene"), the asset
	// browser's double click and Create Document, where the project's desc files are (`desc_layout`), and the load of a dropped or
	// just exported asset that ends in a new node. editor_system creates it after the prefab editor, calls `install()` and asks
	// it `open()` wherever a file or a problem leads to an asset.
	class asset_open_component
	{
	public:
		struct ports
		{
			editor_context& context;
			editor_services& services;
			editor_layer& layer;
			res::resource_system& resources;
			scene_document& state;
			scene_editor& scene;
			level_controller& level_ctrl;
			desc_problem_publisher& desc_problems;
			asset_browser_panel& browser;
			std::function<void(const res::tag&)> open_prefab;
			// A data document that is the prefab editor's preview was opened: shows its window.
			std::function<void()> show_prefab;
			std::function<bool()> play_active;
			// Tells the person why Play refuses `text`; true when it did.
			std::function<bool(std::string text)> refused_during_play;
			// Why the project's files may not be written now (a migration runs or stopped half way). Null/empty: none.
			std::function<std::optional<std::string>()> write_blocker;
			// Why an agent's guid cannot be judged yet (#968); read when asked: the asset index exists only with a project.
			std::function<std::optional<std::string>()> asset_index_wait;
			// What the reference pickers choose from; null without a project.
			std::function<const picker::catalog*()> asset_catalog;
		};

		explicit asset_open_component(ports from);
		~asset_open_component();
		asset_open_component(const asset_open_component&) = delete;
		asset_open_component& operator=(const asset_open_component&) = delete;

		// The asset kinds, and the asset browser's double click and Create Document.
		void install(std::vector<registration>& out);

		// The kinds of the asset registry first, then any `.desc` as a data document. `quiet`: a problem or a link, not a double
		// click: a file that is no data document is a log line, no window. False when nothing opens the file.
		bool open(const res::tag& tag, bool quiet);
		// True while an open() asked quietly (a link, an agent, a recovery) is running: what a kind's open callback may read.
		bool open_is_quiet() const { return m_open_quiet; }
		desc_file_layout desc_layout();
		data_document_host& data_documents() { return *m_data_documents; }
		// The read-only texture viewers (#1205): pictures and the texture descs of lower layers.
		texture_viewer_host& texture_viewers() { return *m_texture_viewers; }
		// The game layer goes / came: the preview stages of the data windows and of the texture viewers.
		void release_previews();
		void rebuild_previews();
		// The windows go before the documents do.
		void shutdown_windows();

		// An asset dropped onto the scene, or just exported by the import dialog: loads it, then adds it as a new node.
		void add_to_scene(const res::tag& tag);
		void add_exported_to_scene(const res::tag& tag);
		// A game layer change or a migration: nothing of the old layer may be waited for.
		void drop_pending() { m_pending_handle = {}; }
		bool adding_to_scene() const { return m_pending_handle.is_valid(); }

	private:
		void start_pending(const res::tag& tag, std::string key, std::string_view implicit_id);
		// A texture desc: the texture window, or the read-only viewer when a lower layer holds it (#1205).
		void open_texture(const res::tag& tag);
		bool poll_pending();

		ports m_ports;
		std::unique_ptr<data_document_host> m_data_documents;
		// Create Document's, shared with the asset browser and the texture viewers' Create Texture.
		std::shared_ptr<document_creator> m_creator;
		std::unique_ptr<texture_viewer_host> m_texture_viewers;
		// The `quiet` of the open() running now: a kind's open callback has no parameter for it.
		bool m_open_quiet = false;
		res::res_handle<desc::desc_base> m_pending_handle;
		res::tag m_pending_tag;
		std::string m_pending_key;
	};
}
