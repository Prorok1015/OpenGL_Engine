#include "edt_prefab_document.h"
#include "edt_asset_open_component.h"
#include "edt_asset_browser_panel.h"
#include "edt_builtin_asset_kinds.h"
#include "edt_desc_problems.h"
#include "edt_document_creator.h"
#include "edt_editor_layer.h"
#include "edt_level_controller.h"
#include "edt_prefab_variant_creator.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <imgui.h>
#include <boost/json.hpp>
#include <filesystem>
#include <format>
#include <string>
#include <utility>
#include <vector>

edt::asset_open_component::asset_open_component(ports from)
	: m_ports(std::move(from))
{
	m_data_documents = std::make_unique<data_document_host>(m_ports.context, m_ports.services, [this] { return desc_layout(); },
		[this](document& open) {
			if (dynamic_cast<const prefab_document*>(&open)) m_ports.show_prefab();
		});
	m_data_documents->set_asset_source(m_ports.asset_catalog);
	m_creator = std::make_shared<document_creator>(document_creator::ports{
		[this] { return m_ports.services.descs(); },
		[this] { return m_ports.services.writable_resources(); },
		[this] { return m_ports.resources.get_resource_layers(); },
		[this] { return m_ports.write_blocker(); },
		[this] { return edt::desc_kind_suffixes(m_ports.context.asset_kinds()); },
		[](std::string_view type) { return edt::initial_document_fields(type); } });
	m_texture_viewers = std::make_unique<texture_viewer_host>(texture_viewer_host::ports{
		.context = m_ports.context,
		.services = m_ports.services,
		.layout = [this] { return desc_layout(); },
		.creator = [this]() -> const document_creator* { return m_creator.get(); },
		.open_texture = [this](const res::tag& tag) { open(tag, false); } });
	m_data_documents->set_picture_texture([this](const res::tag& picture) { return m_texture_viewers->texture_of_picture(picture); });
}

edt::asset_open_component::~asset_open_component() = default;

void edt::asset_open_component::install(std::vector<registration>& out)
{
	asset_kind_hooks asset_hooks;
	asset_hooks.open_prefab = m_ports.open_prefab;
	asset_hooks.desc_layout = [this] { return desc_layout(); };
	asset_hooks.edits_locked = m_ports.play_active;
	asset_hooks.add_desc_to_scene = [this](const res::tag& tag) { add_to_scene(tag); };
	asset_hooks.open_desc = [this](const res::tag& tag) { m_data_documents->open(tag, m_open_quiet); };
	asset_hooks.open_texture = [this](const res::tag& tag) { open_texture(tag); };
	asset_hooks.open_picture = [this](const res::tag& tag) { m_texture_viewers->open_picture(tag); };
	for (auto& kind : register_builtin_asset_kinds(m_ports.context.asset_kinds(), std::move(asset_hooks))) out.push_back(std::move(kind));
	m_ports.browser.set_document_creator(m_creator, [this](const res::tag& tag) { open(tag, false); });
	m_ports.browser.set_on_file_double_clicked([this](const std::filesystem::path& res_rel) {
		open(res::tag::make(res_rel.generic_string()), false);
	});
	// A material opens in the material window (#1196); this is the way to its generic data window.
	m_ports.browser.set_open_as_data([this](const res::tag& tag) { return m_data_documents->has_own_window(tag); },
		[this](const res::tag& tag) { m_data_documents->open(tag, false, data_window_kind::generic); });
	document_creator::ports names{
		[this] { return m_ports.services.descs(); },
		[this] { return m_ports.services.writable_resources(); },
		[this] { return m_ports.resources.get_resource_layers(); },
		[this] { return m_ports.write_blocker(); } };
	m_ports.browser.set_variant_creator(std::make_shared<prefab_variant_creator>(std::move(names), [this](const res::tag& tag) {
			desc::desc_system* descs = m_ports.services.descs();
			res::resource_system* resources = m_ports.services.resources();
			return descs && resources ? open_level_claim(*resources, *descs, desc_layout(), tag) : std::string{};
		}),
		[this](const res::tag& tag) { m_ports.open_prefab(tag); });
}

bool edt::asset_open_component::open(const res::tag& tag, bool quiet)
{
	m_open_quiet = quiet;
	const bool opened_by_kind = m_ports.context.asset_kinds().open(m_ports.context, tag);
	m_open_quiet = false;
	if (opened_by_kind) return true;
	if (!tag.relative().ends_with(".desc")) return false;
	m_data_documents->open(tag, quiet);
	return true;
}

void edt::asset_open_component::open_texture(const res::tag& tag)
{
	// An engine or package texture is no data document of the project: it is shown, not refused with a message.
	if (m_texture_viewers->is_lower_layer_desc(tag)) {
		m_texture_viewers->open_desc(tag);
		return;
	}
	m_data_documents->open(tag, m_open_quiet);
}

void edt::asset_open_component::release_previews()
{
	m_data_documents->release_previews();
	m_texture_viewers->release_previews();
}

void edt::asset_open_component::rebuild_previews()
{
	m_data_documents->rebuild_previews();
	m_texture_viewers->rebuild_previews();
}

void edt::asset_open_component::shutdown_windows()
{
	m_texture_viewers->shutdown();
	m_data_documents->shutdown();
}

edt::desc_file_layout edt::asset_open_component::desc_layout()
{
	desc_file_layout layout;
	layout.top_layer = [this] { return m_ports.resources.get_resources_path(); };
	layout.layers = [this] { return m_ports.resources.get_resource_layers(); };
	layout.open_level_files = [this] {
		std::vector<std::string> open;
		for (const res::tag* tag : { &m_ports.state.level_tag, &m_ports.state.editor_tag }) {
			if (!tag->view().empty()) open.emplace_back(tag->relative());
		}
		return open;
	};
	layout.write_blocker = m_ports.write_blocker;
	layout.asset_index_wait = m_ports.asset_index_wait;
	return layout;
}

void edt::asset_open_component::add_to_scene(const res::tag& tag)
{
	if (m_ports.refused_during_play("Stop Play to add an asset to the scene.")) return;
	const std::string stem = std::filesystem::path(std::string(tag.relative())).stem().string();
	start_pending(tag, m_ports.scene.make_unique_key(stem.empty() ? "Asset" : stem), "edt/drop_loading");
}

void edt::asset_open_component::add_exported_to_scene(const res::tag& tag)
{
	if (m_ports.refused_during_play("The asset is exported; stop Play to add it to the scene.")) return;
	const std::string name = std::string(tag.pure_name());
	start_pending(tag, m_ports.scene.make_unique_key(name.empty() ? "ImportedModel" : name), "edt/export_add");
}

void edt::asset_open_component::start_pending(const res::tag& tag, std::string key, std::string_view implicit_id)
{
	m_pending_handle = m_ports.services.resources()->require<desc::desc_base>(tag);
	m_pending_tag = tag;
	m_pending_key = std::move(key);
	m_ports.layer.register_implicit(implicit_id, [this] { return poll_pending(); });
}

bool edt::asset_open_component::poll_pending()
{
	// Dropped when the game layer changed under the load.
	if (!m_pending_handle.is_valid()) return false;
	if (m_pending_handle.has_error()) {
		egLOG_ERROR("Editor/Drop", "Failed to load desc '{}'", m_pending_tag.view());
		m_ports.desc_problems.report(m_ports.context.problems(), "drop", diagnostics_of(m_pending_handle));
		m_pending_handle = {};
		return false;
	}

	if (!m_pending_handle.is_ready()) {
		ImGui::OpenPopup("Loading Asset##edt_drop");
		if (ImGui::BeginPopupModal("Loading Asset##edt_drop", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
		{
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
				ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				m_pending_handle = {};
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return false;
			}
			ImGui::Text("Loading '%s'...", m_pending_key.c_str());
			ImGui::EndPopup();
		}
		return true;
	}

	ImGui::OpenPopup("Loading Asset##edt_drop");
	if (ImGui::BeginPopupModal("Loading Asset##edt_drop", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	m_ports.desc_problems.report(m_ports.context.problems(), "drop", diagnostics_of(m_pending_handle));
	m_ports.level_ctrl.add_desc_to_scene(m_pending_handle, m_pending_key);
	m_pending_handle = {};
	return false;
}
