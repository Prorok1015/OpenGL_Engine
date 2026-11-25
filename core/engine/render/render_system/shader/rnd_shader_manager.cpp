#include "rnd_shader_manager.h"
#include "rnd_shader_source_map.h"
#include <snk/render/rnd_shader_desc.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/engine_assert.h>
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

rnd::ShaderManager::built_program rnd::ShaderManager::build_program(const shader_config::constant_data& cdata, bool quiet) const
{
	built_program out;
	auto source = shader_config::load(cdata, m_res_sys, drv->bindings(), shader_config::load_options{ quiet });
	out.files = source.files;
	out.name = source.program;
	out.problems = std::move(source.problems);
	if (source.ok()) {
		driver::shader_build_log log;
		out.program = drv->create_shader(source.headers, &log);
		if (!out.program) {
			out.problems = problems_of_build_log(log, source.headers, source.sources, source.program);
			if (out.problems.empty()) {
				shader_problem unknown;
				unknown.file = source.sources.empty() || source.sources.front().empty() ? res::tag{} : source.sources.front().front();
				unknown.code = "shader/compile";
				unknown.message = "the driver did not build the program and said nothing";
				unknown.program = source.program;
				out.problems.push_back(std::move(unknown));
			}
		}
	}
	return out;
}

std::vector<rnd::shader_problem> rnd::ShaderManager::check(const res::tag& desc_tag, std::vector<res::tag>* files, std::string* program) const
{
	ASSERT_MSG(std::this_thread::get_id() == m_owner_thread, "ShaderManager::check: main thread only");
	auto handle = m_res_sys.require_sync<shader_desc>(desc_tag);
	const std::shared_ptr<shader_desc> desc = handle.is_ready() ? handle.get_sync() : nullptr;
	if (!desc) {
		shader_problem unreadable;
		unreadable.file = desc_tag;
		unreadable.code = "shader/load";
		unreadable.message = "the shader desc cannot be read";
		if (files) *files = { desc_tag };
		return { std::move(unreadable) };
	}
	return check(desc->config().cdata, files, program);
}

std::vector<rnd::shader_problem> rnd::ShaderManager::check(const shader_config::constant_data& cdata, std::vector<res::tag>* files, std::string* program) const
{
	ASSERT_MSG(std::this_thread::get_id() == m_owner_thread, "ShaderManager::check: main thread only");
	built_program built = build_program(cdata, true);
	if (files) *files = std::move(built.files);
	if (program) *program = std::move(built.name);
	return std::move(built.problems);
}

rnd::driver::shader_interface* rnd::ShaderManager::require_program(variant_id variant) const
{
	if (variant.value >= m_programs.size()) {
		m_programs.resize(variant.value + 1);
	}

	auto& slot = m_programs[variant.value];
	if (slot.stale || (!slot.program && !slot.failed)) {
		slot.stale = false;
		built_program built = build_program(m_variants.resolve(variant), false);
		track_files(variant, built.files);
		std::vector<shader_problem> problems = std::move(built.problems);
		std::unique_ptr<driver::shader_interface> fresh = std::move(built.program);
		if (fresh) {
			if (slot.program) {
				m_retired.push_back(std::move(slot.program));
			}
			slot.program = std::move(fresh);
		}
		slot.failed = !slot.program;
		// Even when the program it had stays in place: the file as it is now does not build.
		set_problems(slot, std::move(problems));
	}
	return slot.program.get();
}

void rnd::ShaderManager::set_problems(program_slot& slot, std::vector<shader_problem> problems) const
{
	if (slot.problems == problems) {
		return;
	}
	slot.problems = std::move(problems);
	++m_problems_generation;
}

void rnd::ShaderManager::clear_cache() const
{
	const bool had_problems = std::ranges::any_of(m_programs, [](const program_slot& slot) { return !slot.problems.empty(); });
	m_programs.clear();
	if (had_problems) {
		++m_problems_generation;
	}
}

std::vector<rnd::shader_problem> rnd::ShaderManager::problems() const
{
	std::vector<shader_problem> out;
	for (const program_slot& slot : m_programs) {
		out.insert(out.end(), slot.problems.begin(), slot.problems.end());
	}
	return out;
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
