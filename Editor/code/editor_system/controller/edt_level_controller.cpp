#include "edt_level_controller.h"
#include "edt_shared_state.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include "level/scn_level.h"
#include "level/scn_level_manager.h"
#include "level/scn_level_desc.h"
#include "level/scn_prefab_desc.h"
#include "desc_system.h"
#include "desc_guid.h"
#include "res_system.h"
#include "resources/res_resource_text.h"
#include <imgui.h>
#include <boost/json.hpp>
#include <fstream>
#include <filesystem>
#include <format>
#include "engine_log.h"

edt::level_controller::level_controller(
	shared_state& state, res::resource_system& res, desc::desc_system& desc,
	scene_editor& scene_ed, world_controller& world_ctrl)
	: m_state(state)
	, m_res(res)
	, m_desc(desc)
	, m_scene_editor(scene_ed)
	, m_world_ctrl(world_ctrl)
{
}

edt::level_controller::~level_controller()
{
	if (m_state.editor_tag.is_valid())
		m_res.unwatch(m_state.editor_tag, this);
	if (auto lvl_mgr = m_state.lvl_manager.lock())
		lvl_mgr->set_on_full_reload(nullptr);
}

// ─── New / Save / Quick-save ─────────────────────────────────────────────────

void edt::level_controller::new_level()
{
	auto lvl_mgr = m_state.lvl_manager.lock();
	if (!lvl_mgr) return;

	namespace json = boost::json;

	json::object level_json = load_desc_template("level.desc");
	if (level_json.empty()) {
		egLOG("Editor/NewLevel", "level.desc template missing — aborting");
		return;
	}

	res::tag mem_tag{ "memory://editor/new_level.desc" };

	if (m_state.editor_tag.is_valid())
		m_res.unwatch(m_state.editor_tag, this);
	m_state.editor_tag = mem_tag;

	m_res.store(mem_tag, m_desc.serialize_to_bytes(json::value{ level_json }));
	m_res.signal_changed(mem_tag);

	if (!lvl_mgr->load(mem_tag)) {
		egLOG("Editor/NewLevel", "Failed to create new level via desc pipeline");
		return;
	}

	m_res.watch(m_state.editor_tag, this, [this](const res::tag&) {
		if (on_editor_world_reloaded) on_editor_world_reloaded();
	});

	auto level_res = m_res.require_sync<scn::level_desc>(m_state.editor_tag);
	if (!level_res.is_ready()) {
		egLOG_ERROR("Editor/NewLevel", "Level desc '{}' failed to load after creation", m_state.editor_tag.view());
		return;
	}
	populate_worlds_from_level(level_res);
	m_state.level_name = "new_level";

	m_state.level_tag = res::tag{};
	m_state.level_guid.clear();
	// world_names/descs/systems already populated (index-aligned) by populate_worlds_from_level.

	m_state.active_world_idx = 0;
	m_state.is_dirty = true;
	m_world_ctrl.switch_to_world(0);
}

bool edt::level_controller::save_level()
{
	if (!m_state.editor_tag.is_valid()) {
		egLOG("Editor/Save", "No scene loaded — nothing to save.");
		return false;
	}

	if (!m_save_dialog_open) {
		m_save_dialog_open = true;
		std::string current = m_state.level_tag.is_valid()
			? std::string(m_state.level_tag.relative())
			: "levels/my_level.desc";
		auto cp = std::format_to_n(m_save_path_buf, sizeof(m_save_path_buf) - 1, "{}", current);
		*cp.out = '\0';
		ImGui::OpenPopup("Save Level##edt_save");
	}

	bool is_open = true;
	// Run only after the popup is closed so the action can't touch ImGui state mid-modal.
	std::function<void()> run_after_save;
	if (ImGui::BeginPopupModal("Save Level##edt_save", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::Text("Path (relative to res://):");
		ImGui::SetNextItemWidth(480.f);
		ImGui::InputText("##sp", m_save_path_buf, sizeof(m_save_path_buf));

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		const float bw = 110.f;
		const float avail = ImGui::GetContentRegionAvail().x;
		ImGui::SetCursorPosX((avail - bw * 2.f - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

		if (ImGui::Button("Save", {bw, 0.f})) {
			const std::string rel_path_str(m_save_path_buf);
			if (write_level_to_disk(rel_path_str)) {
				m_state.level_tag = res::tag::make(rel_path_str);
				run_after_save = std::move(m_pending_after_save);
			}
			m_pending_after_save = nullptr;
			m_save_dialog_open = false;
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Cancel", {bw, 0.f})) {
			m_pending_after_save = nullptr;
			m_save_dialog_open = false;
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	} else if (m_save_dialog_open) {
		m_pending_after_save = nullptr;
		m_save_dialog_open = false;
		is_open = false;
	}

	if (run_after_save)
		run_after_save();

	return is_open;
}

void edt::level_controller::quick_save_level()
{
	if (!m_state.level_tag.is_valid())
		return;

	write_level_to_disk(std::string(m_state.level_tag.relative()));
}

bool edt::level_controller::show_exit_confirm()
{
	ImGui::OpenPopup("Unsaved Changes##edt_exit");

	bool is_open = true;
	if (ImGui::BeginPopupModal("Unsaved Changes##edt_exit", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::Text("You have unsaved changes. What would you like to do?");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		const float bw = 120.f;
		const float total = bw * 3.f + ImGui::GetStyle().ItemSpacing.x * 2.f;
		ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - total) * 0.5f);

		if (ImGui::Button("Save & Continue", {bw, 0.f})) {
			if (m_state.level_tag.is_valid()) {
				quick_save_level();
				if (m_confirm_action) m_confirm_action();
			} else {
				// Untitled level: Save-As runs first, the action follows it (see save_level).
				m_pending_after_save = m_confirm_action;
				if (on_need_save_dialog) on_need_save_dialog();
			}
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Discard", {bw, 0.f})) {
			m_state.is_dirty = false;
			if (m_confirm_action) m_confirm_action();
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Cancel", {bw, 0.f})) {
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	} else {
		is_open = false;
	}

	return is_open;
}

// ─── Level load finish ──────────────────────────────────────────────────────

void edt::level_controller::finish_level_load(res::res_handle<scn::level_desc>& handle)
{
	// Cleared first so a failed load cannot leave the previous level's guid to be written into the new file.
	m_state.level_guid.clear();

	auto level_manager = m_state.lvl_manager.lock();
	if (!level_manager) return;

	if (level_manager->load(m_state.editor_tag)) {
		m_res.watch(m_state.editor_tag, this, [this](const res::tag&) {
			if (on_editor_world_reloaded) on_editor_world_reloaded();
		});

		auto level_res = m_res.require_sync<scn::level_desc>(m_state.editor_tag);
		if (!level_res.is_ready()) {
			egLOG_ERROR("Editor/LoadLevel", "Level desc '{}' failed to load", m_state.editor_tag.view());
			return;
		}
		m_state.level_guid = level_res->get_guid();
		populate_worlds_from_level(level_res);
		m_state.level_name = std::string(m_state.level_tag.relative());
		if (auto p = m_state.level_name.rfind('.'); p != std::string::npos) m_state.level_name.resize(p);
		if (auto p = m_state.level_name.rfind('/');  p != std::string::npos) m_state.level_name = m_state.level_name.substr(p + 1);

		// world_names/descs/systems already populated (index-aligned) by populate_worlds_from_level.
		m_state.active_world_idx = 0;
		m_state.is_dirty = false;
		m_world_ctrl.switch_to_world(0);
	}
}

// ─── Desc-to-scene (asset drop / export auto-add) ──────────────────────────

void edt::level_controller::add_desc_to_scene(const res::res_handle<desc::desc_base>& handle, const std::string& key)
{
	if (!m_state.editor_tag.is_valid() || !handle.is_ready()) return;

	scn::prefab_desc::prefab_comp_node comp;
	comp.parent_desc = handle;

	scn::prefab_desc::prefab_node node;
	node.name = key;
	node.components[key] = std::move(comp);

	m_scene_editor.create_entity_from_node(std::move(node));
	egLOG("Editor/Drop", "Added '{}' to scene", key);
}

// ─── Template & serialization helpers ───────────────────────────────────────

boost::json::object edt::level_controller::load_desc_template(const std::string& filename)
{
	namespace json = boost::json;
	auto tag = res::tag::make("templates/" + filename);
	auto text = m_res.require_sync<res::text_resource>(tag);
	if (text.is_ready()) {
		try {
			json::parse_options opts;
			opts.max_depth = 128;
			return json::parse(text->str(), {}, opts).as_object();
		} catch (...) {
			egLOG("Editor/Template", "Failed to parse template '{}'", filename);
		}
	}
	egLOG("Editor/Template", "Template '{}' not found", filename);
	return {};
}

boost::json::object edt::level_controller::build_level_json() const
{
	const std::string name = m_state.level_name.empty() ? "level" : m_state.level_name;
	return scn::level_desc::build_json(name, m_state.world_descs, m_desc, m_state.level_guid);
}

bool edt::level_controller::write_level_to_disk(const std::string& rel_path)
{
	m_scene_editor.sync_ecs_transforms_to_desc();

	std::filesystem::path frel(rel_path);
	const std::string stem = frel.stem().string();
	const std::string level_name = stem.empty() ? m_state.level_name : stem;

	// Saving the same file keeps its guid; saving to another path (Save As) writes a new file, which gets a new guid.
	const bool same_file = m_state.level_tag.is_valid() && rel_path == m_state.level_tag.relative();
	const std::string guid = (same_file && !m_state.level_guid.empty()) ? m_state.level_guid : desc::generate_guid();

	const auto level_json = scn::level_desc::build_json(level_name, m_state.world_descs, m_desc, guid);

	std::filesystem::path abs_path = res::resource_system::get_resources_path() / frel;
	std::error_code ec;
	std::filesystem::create_directories(abs_path.parent_path(), ec);
	std::ofstream out(abs_path, std::ios::binary);
	if (!out) {
		egLOG("Editor/Save", "Failed to open file for writing: {}", abs_path.string());
		return false;
	}

	// Same writer as every other desc save, so level files are indented and diff well.
	const auto bytes = m_desc.serialize_to_bytes(boost::json::value{ level_json });
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	m_state.level_guid = guid;
	m_state.level_name = level_name;
	m_state.is_dirty   = false;
	egLOG("Editor/Save", "Level saved to {}", abs_path.string());
	return true;
}

void edt::level_controller::resync_worlds_after_reload()
{
	auto lvl_mgr = m_state.lvl_manager.lock();
	if (!lvl_mgr) return;
	scn::level& lvl = lvl_mgr->get_level();

	auto level_res = m_res.require_sync<scn::level_desc>(lvl.get_tag());
	if (!level_res.is_ready()) {
		egLOG_WARN("Editor/Reload", "Level desc '{}' not ready after reload, world arrays not resynced", lvl.get_tag().view());
		return;
	}

	std::string active_name;
	if (m_state.active_world_idx >= 0 && m_state.active_world_idx < (int)m_state.world_names.size())
		active_name = m_state.world_names[m_state.active_world_idx];

	std::vector<scn::world_desc> desc_worlds;
	for (const auto& wh : level_res->get_worlds()) {
		const auto world = wh.get_sync();
		if (!ENGINE_VERIFY(world, "level '{}': world desc failed to load, skipping", level_res.get_tag().view())) {
			continue;
		}
		desc_worlds.push_back(*world);
	}

	auto merged = merge_reloaded_worlds(desc_worlds, m_state.world_descs,
		[&lvl](const scn::world_desc& w) { return lvl.has_world(w.get_name()); });
	// The editor needs an active world (active_world_desc() is unchecked); keep the old arrays,
	// switch_to_world already tolerates names missing from the level.
	if (merged.empty()) {
		egLOG_WARN("Editor/Reload", "Level '{}' has no worlds after reload, world arrays not resynced", lvl.get_tag().view());
		return;
	}
	m_state.world_descs = std::move(merged);
	derive_world_arrays(m_state.world_descs, m_state.world_names, m_state.worlds_systems_list);

	int idx = -1;
	for (int i = 0; i < (int)m_state.world_names.size(); ++i) {
		if (m_state.world_names[i] == active_name) { idx = i; break; }
	}
	if (idx < 0) {
		if (!active_name.empty()) {
			egLOG_WARN("Editor/Reload", "Active world '{}' is gone after reload, switching to '{}'", active_name, m_state.world_names[0]);
		}
		idx = 0;
	}
	m_state.active_world_idx = idx;
	m_world_ctrl.switch_to_world(idx); // the active world may have been rebuilt without the editor camera
}

void edt::level_controller::populate_worlds_from_level(const res::res_handle<scn::level_desc>& level_res)
{
	m_state.world_descs.clear();
	m_state.world_names.clear();
	m_state.worlds_systems_list.clear();
	if (!level_res.is_ready()) return;
	// world_descs is the single ordered source of truth (desc order); names and systems
	// are derived from it so all three parallel arrays stay index-aligned.
	for (const auto& wh : level_res->get_worlds()) {
		const auto world = wh.get_sync();
		if (!ENGINE_VERIFY(world, "level '{}': world desc failed to load, skipping", level_res.get_tag().view())) {
			continue;
		}
		m_state.world_descs.push_back(*world);
	}
	derive_world_arrays(m_state.world_descs, m_state.world_names, m_state.worlds_systems_list);
}
