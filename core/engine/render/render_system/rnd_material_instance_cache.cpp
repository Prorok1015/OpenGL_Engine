#include <snk/render/rnd_material_instance_cache.h>
#include <snk/render/shader/rnd_material_compiler.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/resource/res_system.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace
{
	constexpr std::string_view k_load_code = "shader/load";
	constexpr std::string_view k_no_transparent_path_code = rnd::k_no_transparent_path_code;

	rnd::material_shader_state read_shader(const res::res_handle<rnd::shader_desc>& shader)
	{
		rnd::material_shader_state state;
		state.desc = shader.get();
		state.failed = !state.desc && shader.has_error();
		return state;
	}
}

rnd::material_instance_cache::material_instance_cache(shader_config_cache& variants, texture_manager& textures, res::resource_system* resources)
	: m_variants(variants)
	, m_textures(textures)
	, m_resources(resources)
{}

void rnd::material_instance_cache::begin_frame(material_compiler_interface* compiler)
{
	m_compiler = compiler;
	++m_frame;
	std::vector<std::pair<res::tag, std::string>> dropped;
	for (auto it = m_by_key.begin(); it != m_by_key.end();) {
		slot& s = m_slots[it->second];
		if (s.last_used + grace_frames < m_frame) {
			dropped.emplace_back(s.shader.get_tag(), std::move(s.material_tag));
			set_shader(s, {});
			s = slot{};
			m_free.push_back(it->second);
			it = m_by_key.erase(it);
		} else {
			++it;
		}
	}
	// A material nobody draws any more has no problem to show; its static and skinned instances share the entry.
	for (const auto& [shader, material] : dropped) {
		if (!any_slot(shader, material, false)) {
			clear_problem(shader, material);
		}
	}
}

bool rnd::material_instance_cache::any_slot(const res::tag& shader, std::string_view material, bool reload_failed_only) const
{
	return std::ranges::any_of(m_by_key, [&](const auto& entry) {
		const slot& s = m_slots[entry.second];
		return s.shader.get_tag() == shader && s.material_tag == material && (!reload_failed_only || s.reload_failed);
	});
}

void rnd::material_instance_cache::compile(slot& s, const std::shared_ptr<material_desc>& desc, bool skinned)
{
	if (!s.shader.is_valid() || desc->shader.get_tag() != s.shader.get_tag()) {
		const res::tag old_shader = s.shader.get_tag();
		set_shader(s, desc->shader);
		// A material pointed at another shader no longer has the old one's problem (filed under the last compile's tag).
		if (old_shader.is_valid() && !any_slot(old_shader, s.material_tag, false)) {
			clear_problem(old_shader, s.material_tag);
		}
	}
	s.material_tag = desc->get_tag().view();
	const material_shader_state shader = read_shader(s.shader);
	// The compiler resolves the maps (texture_manager::require, which gives the id): the only place a material asks for a texture.
	s.material = build_compiled_material(*m_compiler, m_variants, m_textures, desc, skinned, shader);
	s.built_frame = m_frame;
	++m_compilations;
	if (shader.failed) {
		report_problem(s.shader.get_tag(), s.material_tag, s.shader.error_text(), false);
	} else if (shader.desc && !s.reload_failed && !any_slot(s.shader.get_tag(), s.material_tag, true)) {
		clear_problem(s.shader.get_tag(), s.material_tag, k_load_code);
	}
	check_transparent_path(s, *desc, shader);
}

void rnd::material_instance_cache::check_transparent_path(const slot& s, const material_desc& desc, const material_shader_state& shader)
{
	if (!shader.desc) {
		return;
	}
	const bool blended = desc.queue == pass_queue::TRANSPARENT || desc.queue == pass_queue::MIX;
	if (!blended || shader.desc->transparent) {
		clear_problem(s.shader.get_tag(), s.material_tag, k_no_transparent_path_code);
		return;
	}
	const auto known = std::ranges::find_if(m_problems, [&](const shader_problem& p) {
		return p.file == s.shader.get_tag() && p.program == s.material_tag && p.code == k_no_transparent_path_code;
	});
	if (known != m_problems.end()) {
		return;
	}
	const std::string message = std::format("the queue {} needs a shader with a transparent path (\"transparent\": true, e.g. "
		"res://shaders/mix_opaque_trans_scene.shader.desc); the transparent pass gets nothing it can blend",
		desc.queue == pass_queue::MIX ? "mix" : "transparent");
	egLOG_WARN("render/material", "'{}': {}", s.material_tag, message);
	m_problems.push_back(shader_problem{ s.shader.get_tag(), 0, 0, std::string(k_no_transparent_path_code), message, s.material_tag });
	++m_problems_generation;
}

bool rnd::material_instance_cache::reload_shader(slot& s)
{
	if (!s.pending.is_valid()) {
		s.pending = m_resources->require<shader_desc>(s.shader.get_tag());
	}
	if (s.pending.is_ready()) {
		s.shader = std::exchange(s.pending, {});
		s.shader_changed = false;
		s.reload_failed = false;
		return true;
	}
	if (s.pending.has_error()) {
		report_problem(s.shader.get_tag(), s.material_tag, s.pending.error_text(), true);
		s.pending = {};
		s.shader_changed = false;
		s.reload_failed = true;
	}
	return false;
}

void rnd::material_instance_cache::set_shader(slot& s, res::res_handle<shader_desc> shader)
{
	const res::tag& old_tag = s.shader.get_tag();
	if (m_resources && old_tag.is_valid()) {
		if (const auto users = m_shader_users.find(old_tag); users != m_shader_users.end() && --users->second == 0) {
			m_resources->unwatch(old_tag, this);
			m_shader_users.erase(users);
		}
	}
	s.shader = std::move(shader);
	s.pending = {};
	s.shader_changed = false;
	s.reload_failed = false;
	const res::tag& tag = s.shader.get_tag();
	if (m_resources && tag.is_valid() && ++m_shader_users[tag] == 1) {
		// The captured tag, not the signalled one: a file with an asset ID may be signalled under it.
		m_resources->watch(tag, this, [this, tag](const res::tag&) { on_shader_changed(tag); });
	}
}

void rnd::material_instance_cache::on_shader_changed(const res::tag& shader)
{
	for (const auto& entry : m_by_key) {
		slot& s = m_slots[entry.second];
		if (s.shader.get_tag() == shader) {
			s.shader_changed = true;
			// A request made before this change may hold the previous file.
			s.pending = {};
		}
	}
}

void rnd::material_instance_cache::report_problem(const res::tag& shader, std::string_view material, const std::string& error, bool reload)
{
	const auto known = std::ranges::find_if(m_problems, [&](const shader_problem& p) {
		return p.file == shader && p.program == material && p.code == k_load_code;
	});
	if (known != m_problems.end()) {
		// Logged once per pair; the panel still shows the latest reason.
		if (known->message != error) {
			known->message = error;
			++m_problems_generation;
		}
		return;
	}
	if (reload) {
		egLOG_ERROR("render/material", "the shader '{}' of '{}' did not reload, the material keeps its previous program: {}",
			shader.view(), material, error);
	} else {
		egLOG_ERROR("render/material", "the shader '{}' of '{}' did not load, the material is not drawn: {}",
			shader.view(), material, error);
	}
	m_problems.push_back(shader_problem{ shader, 0, 0, std::string(k_load_code), error, std::string{ material } });
	++m_problems_generation;
}

void rnd::material_instance_cache::clear_problem(const res::tag& shader, std::string_view material, std::string_view code)
{
	const auto of_pair = [&](const shader_problem& p) {
		return p.file == shader && p.program == material && (code.empty() || p.code == code);
	};
	if (std::erase_if(m_problems, of_pair) != 0) {
		++m_problems_generation;
	}
}

rnd::material_instance_id rnd::material_instance_cache::acquire(const std::shared_ptr<material_desc>& desc, bool skinned, bool desc_changed)
{
	ASSERT_MSG(desc, "a material instance needs a loaded material");
	ASSERT_MSG(m_compiler, "material_instance_cache::begin_frame must run before acquire");
	const key identity{ desc.get(), skinned };

	// An edit of the description reaches every instance made from it (the static and the skinned one), not only
	// the one of the mesh that was marked.
	if (desc_changed) {
		if (const auto sibling = m_by_key.find(key{ desc.get(), !skinned }); sibling != m_by_key.end()) {
			slot& other = m_slots[sibling->second];
			if (other.built_frame != m_frame) {
				compile(other, desc, !skinned);
			}
		}
	}

	if (const auto found = m_by_key.find(identity); found != m_by_key.end()) {
		slot& s = m_slots[found->second];
		s.last_used = m_frame;
		if (s.built_frame != m_frame) {
			// While a changed shader loads, or after it failed, the instance stays as it is unless something else stales it.
			const bool reloaded = s.shader_changed && reload_shader(s);
			const bool stale = !s.material->is_current(desc.get(), skinned, m_textures.generation());
			if (reloaded || stale || desc_changed) {
				compile(s, desc, skinned);
			}
		}
		return material_instance_id{ found->second };
	}

	std::uint32_t index = 0;
	if (!m_free.empty()) {
		index = m_free.back();
		m_free.pop_back();
	} else {
		index = static_cast<std::uint32_t>(m_slots.size());
		m_slots.emplace_back();
	}
	slot& s = m_slots[index];
	s.last_used = m_frame;
	compile(s, desc, skinned);
	m_by_key.emplace(identity, index);
	return material_instance_id{ index };
}

bool rnd::material_instance_cache::contains(material_instance_id id) const noexcept
{
	return id.is_valid() && id.index < m_slots.size() && m_slots[id.index].material != nullptr;
}

const rnd::compiled_material& rnd::material_instance_cache::get(material_instance_id id) const
{
	ASSERT_MSG(contains(id), "material instance {} is not in the cache", id.index);
	return *m_slots[id.index].material;
}

std::size_t rnd::material_instance_cache::size() const noexcept
{
	return m_by_key.size();
}

std::uint64_t rnd::material_instance_cache::compilations() const noexcept
{
	return m_compilations;
}

std::vector<rnd::shader_problem> rnd::material_instance_cache::problems() const
{
	return m_problems;
}

std::uint64_t rnd::material_instance_cache::problems_generation() const noexcept
{
	return m_problems_generation;
}

rnd::material_instance_cache::~material_instance_cache()
{
	if (!m_resources) {
		return;
	}
	for (const auto& entry : m_shader_users) {
		m_resources->unwatch(entry.first, this);
	}
}

bool rnd::material_instance_cache::key::operator==(const key& other) const
{
	return desc == other.desc && skinned == other.skinned;
}
