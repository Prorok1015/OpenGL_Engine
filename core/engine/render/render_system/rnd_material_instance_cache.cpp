#include <snk/render/rnd_material_instance_cache.h>
#include <snk/render/shader/rnd_material_compiler.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/common/engine_assert.h>

rnd::material_instance_cache::material_instance_cache(shader_config_cache& variants, texture_manager& textures)
	: m_variants(variants)
	, m_textures(textures)
{}

void rnd::material_instance_cache::begin_frame(material_compiler_interface* compiler)
{
	m_compiler = compiler;
	++m_frame;
	for (auto it = m_by_key.begin(); it != m_by_key.end();) {
		slot& s = m_slots[it->second];
		if (s.last_used + grace_frames < m_frame) {
			s.material.reset();
			m_free.push_back(it->second);
			it = m_by_key.erase(it);
		} else {
			++it;
		}
	}
}

void rnd::material_instance_cache::compile(slot& s, const std::shared_ptr<scn::material_desc>& desc, bool skinned)
{
	// The compiler resolves the maps (texture_manager::require_texture): the only place a material asks for a texture.
	s.material = build_compiled_material(*m_compiler, m_variants, m_textures, desc, skinned);
	s.built_frame = m_frame;
	++m_compilations;
}

rnd::material_instance_id rnd::material_instance_cache::acquire(const std::shared_ptr<scn::material_desc>& desc, bool skinned, bool desc_changed)
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
			const bool stale = !s.material->is_current(desc.get(), skinned, m_textures.generation());
			if (stale || desc_changed) {
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
