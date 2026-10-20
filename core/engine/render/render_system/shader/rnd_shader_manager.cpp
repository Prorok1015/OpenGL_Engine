#include "rnd_shader_manager.h"
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>


rnd::ShaderManager::ShaderManager(driver::driver_interface* ptr, res::resource_system& res_sys)
	: drv(ptr)
	, m_res_sys(res_sys)
{
	m_layouts.frame = drv->create_bind_group_layout(frame_layout_desc());
	m_layouts.composition = drv->create_bind_group_layout(composition_layout_desc());
	m_layouts.sky = drv->create_bind_group_layout(sky_layout_desc());
	m_layouts.material = drv->create_bind_group_layout(material_layout_desc());
	m_layouts.object = drv->create_bind_group_layout(object_layout_desc());

	driver::buffer_desc matrices;
	matrices.size = sizeof(global_params);
	matrices.usage = driver::buffer_usage::uniform;
	matrices.update = driver::buffer_update::dynamic;
	matrices.debug_name = "Matrices";
	m_matrices = drv->create_buffer(matrices);

	driver::buffer_desc lights;
	lights.size = sizeof(lights_params);
	lights.usage = driver::buffer_usage::uniform;
	lights.update = driver::buffer_update::dynamic;
	lights.debug_name = "GlobalLights";
	m_lights = drv->create_buffer(lights);

	driver::bind_group_desc frame;
	frame.layout = m_layouts.frame;
	frame.entries.push_back({ driver::binding_type::ubo, bindings::matrices_slot, m_matrices, 0, sizeof(global_params), driver::texture_handle{} });
	frame.entries.push_back({ driver::binding_type::ubo, bindings::lights_slot, m_lights, 0, sizeof(lights_params), driver::texture_handle{} });
	m_frame_group = drv->create_bind_group(frame, driver::lifetime::persistent);
}

rnd::ShaderManager::~ShaderManager()
{
	for (const auto& user : m_users) {
		m_res_sys.unwatch(user.first, this);
	}
	drv->destroy_bind_group(m_frame_group);
	drv->destroy_buffer(m_matrices);
	drv->destroy_buffer(m_lights);
}

void rnd::ShaderManager::begin_frame() const
{
	for (const auto& program : m_retired) {
		drv->destroy_pipelines_of(program.get());
	}
	m_retired.clear();
}

rnd::driver::shader_interface* rnd::ShaderManager::require_program(variant_id variant) const
{
	if (variant.value >= m_programs.size()) {
		m_programs.resize(variant.value + 1);
	}

	auto& slot = m_programs[variant.value];
	if (slot.stale || (!slot.program && !slot.failed)) {
		slot.stale = false;
		auto source = shader_config::load(m_variants.resolve(variant), m_res_sys, drv->bindings());
		track_files(variant, source.files);
		std::unique_ptr<driver::shader_interface> fresh;
		if (source.ok()) {
			fresh = drv->create_shader(source.headers);
		}
		if (fresh) {
			if (slot.program) {
				m_retired.push_back(std::move(slot.program));
			}
			slot.program = std::move(fresh);
		}
		slot.failed = !slot.program;
	}
	return slot.program.get();
}

void rnd::ShaderManager::track_files(variant_id variant, const std::vector<res::tag>& files) const
{
	for (const res::tag& file : files) {
		auto [it, added] = m_users.try_emplace(file);
		if (added) {
			m_res_sys.watch(file, const_cast<ShaderManager*>(this), [this](const res::tag& changed) { on_file_changed(changed); });
		}
		if (std::ranges::find(it->second, variant.value) == it->second.end()) {
			it->second.push_back(variant.value);
		}
	}
}

void rnd::ShaderManager::on_file_changed(const res::tag& file) const
{
	const auto users = m_users.find(file);
	if (users == m_users.end()) {
		return;
	}
	egLOG_INFO("shader/reload", "'{}' changed: {} program(s) will be compiled again", file.view(), users->second.size());
	for (const std::uint32_t variant : users->second) {
		m_programs[variant].stale = true;
	}
}

void rnd::ShaderManager::update_global_uniform(const global_params& val) const
{
	drv->update_buffer(m_matrices, 0, std::as_bytes(std::span{ &val, 1 }));
}

void rnd::ShaderManager::update_global_sun(const lights_params& val) const
{
	drv->update_buffer(m_lights, 0, std::as_bytes(std::span{ &val, 1 }));
}

void rnd::ShaderManager::bind_frame() const
{
	drv->set_bind_group(driver::frame_set, m_frame_group);
}
