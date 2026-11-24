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
#include <snk/editor/edt_asset_kind_registry.h>
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

namespace {
	// The suffixes Create Document gives the types (#1263): a kind whose id names a desc type, e.g. "shader_desc".
	std::vector<std::pair<std::string, std::string>> desc_kind_suffixes(const edt::asset_kind_registry& kinds)
	{
		std::vector<std::pair<std::string, std::string>> out;
		for (const std::string& id : kinds.ids()) {
			const edt::asset_kind_desc* kind = kinds.find(id);
			if (kind && kind->suffix.ends_with(".desc")) {
				out.emplace_back(id, kind->suffix);
			}
		}
		return out;
	}
}

edt::asset_open_component::asset_open_component(ports from)
	: m_ports(std::move(from))
{
	m_data_documents = std::make_unique<data_document_host>(m_ports.context, m_ports.services, [this] { return desc_layout(); },
		[this](document& open) {
			if (dynamic_cast<const prefab_document*>(&open)) m_ports.show_prefab();
		});
	m_data_documents->set_asset_source(m_ports.asset_catalog);
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
	for (auto& kind : register_builtin_asset_kinds(m_ports.context.asset_kinds(), std::move(asset_hooks))) out.push_back(std::move(kind));
	m_ports.browser.set_document_creator(
		std::make_shared<document_creator>(document_creator::ports{
			[this] { return m_ports.services.descs(); },
			[this] { return m_ports.services.writable_resources(); },
			[this] { return m_ports.resources.get_resource_layers(); },
			[this] { return m_ports.write_blocker(); },
			[this] { return desc_kind_suffixes(m_ports.context.asset_kinds()); } }),
		[this](const res::tag& tag) { open(tag, false); });
	m_ports.browser.set_on_file_double_clicked([this](const std::filesystem::path& res_rel) {
		open(res::tag::make(res_rel.generic_string()), false);
	});
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
