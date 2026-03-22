#include "edt_level_controller.h"
#include "edt_shared_state.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include "level/scn_level.h"
#include "level/scn_level_manager.h"
#include "level/scn_level_desc.h"
#include "level/scn_prefab_desc.h"
#include "desc_system.h"
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
	populate_worlds_from_level(level_res);
	m_state.level_name = "new_level";

	m_state.level_tag = res::tag{};
	scn::level& lvl = lvl_mgr->get_level();
	m_state.world_names.clear();
	lvl.for_each_world([&](scn::world& w) {
		m_state.world_names.push_back(std::string(w.get_name()));
	});

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
			}
			m_save_dialog_open = false;
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Cancel", {bw, 0.f})) {
			m_save_dialog_open = false;
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	} else if (m_save_dialog_open) {
		m_save_dialog_open = false;
		is_open = false;
	}

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
	auto level_manager = m_state.lvl_manager.lock();
	if (!level_manager) return;

	if (level_manager->load(m_state.editor_tag)) {
		m_res.watch(m_state.editor_tag, this, [this](const res::tag&) {
			if (on_editor_world_reloaded) on_editor_world_reloaded();
		});

		auto level_res = m_res.require_sync<scn::level_desc>(m_state.editor_tag);
		populate_worlds_from_level(level_res);
		m_state.level_name = std::string(m_state.level_tag.relative());
		if (auto p = m_state.level_name.rfind('.'); p != std::string::npos) m_state.level_name.resize(p);
		if (auto p = m_state.level_name.rfind('/');  p != std::string::npos) m_state.level_name = m_state.level_name.substr(p + 1);

		scn::level& lvl = level_manager->get_level();
		m_state.world_names.clear();
		lvl.for_each_world([&](scn::world& w) {
			m_state.world_names.push_back(std::string(w.get_name()));
		});

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
	return scn::level_desc::build_json(name, m_state.world_descs);
}

bool edt::level_controller::write_level_to_disk(const std::string& rel_path)
{
	m_scene_editor.sync_ecs_transforms_to_desc();

	std::filesystem::path frel(rel_path);
	const std::string stem = frel.stem().string();
	const std::string level_name = stem.empty() ? m_state.level_name : stem;

	const auto level_json = scn::level_desc::build_json(level_name, m_state.world_descs);

	std::filesystem::path abs_path = res::resource_system::get_resources_path() / frel;
	std::error_code ec;
	std::filesystem::create_directories(abs_path.parent_path(), ec);
	std::ofstream out(abs_path);
	if (!out) {
		egLOG("Editor/Save", "Failed to open file for writing: {}", abs_path.string());
		return false;
	}

	out << boost::json::serialize(level_json);
	m_state.level_name = level_name;
	m_state.is_dirty   = false;
	egLOG("Editor/Save", "Level saved to {}", abs_path.string());
	return true;
}

void edt::level_controller::populate_worlds_from_level(const res::res_handle<scn::level_desc>& level_res)
{
	m_state.world_descs.clear();
	m_state.worlds_systems_list.clear();
	if (!level_res.is_ready()) return;
	for (const auto& wh : level_res->get_worlds()) {
		m_state.world_descs.push_back(*wh);
		m_state.worlds_systems_list.push_back(wh->get_systems());
	}
}
