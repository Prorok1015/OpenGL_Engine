#include <snk/render/shader/rnd_shader_config_cache.h>
#include <algorithm>
#include <snk/common/engine_assert.h>

namespace
{
	using cdata_t = rnd::shader_config::constant_data;

	std::uint64_t program_hash(const rnd::shader_config::shader_program_data& program)
	{
		std::uint64_t hash = 0;
		for (const auto& tag : program) {
			hash = (hash ^ cdata_t::fnv1a(tag.view())) * cdata_t::fnv_prime;
		}
		return hash;
	}

	bool same_variant(const cdata_t& a, const cdata_t& b)
	{
		return a.program == b.program
			&& a.constants == b.constants
			&& std::is_permutation(a.defines.begin(), a.defines.end(), b.defines.begin(), b.defines.end());
	}
}

rnd::variant_id rnd::shader_config_cache::intern(const shader_config::constant_data& cdata, pass_define_mask passes)
{
	const std::uint64_t key = cdata.variant_hash(passes);
	auto [first, last] = m_by_key.equal_range(key);
	for (auto it = first; it != last; ++it) {
		const entry& e = m_entries[it->second];
		if (e.passes == passes && same_variant(e.source, cdata)) {
			return variant_id{ it->second };
		}
	}

	entry e;
	e.key = key;
	e.passes = passes;
	e.source = cdata;
	e.resolved = cdata;
	if (passes & pass_define_opaque) {
		e.resolved.defines.push_back("OPAQUE");
	}

	const auto index = static_cast<std::uint32_t>(m_entries.size());
	m_entries.push_back(std::move(e));
	m_by_key.emplace(key, index);
	return variant_id{ index };
}

rnd::variant_id rnd::shader_config_cache::with_program(variant_id source, const shader_config::shader_program_data& program)
{
	const std::uint64_t memo_key = cdata_t::mix((get(source).key * cdata_t::fnv_prime) ^ program_hash(program));

	if (auto it = m_program_memo.find(memo_key); it != m_program_memo.end()) {
		const auto& hit = get(it->second);
		const auto& from = get(source).resolved;
		if (hit.passes == pass_define_none && hit.resolved.program == program
			&& hit.resolved.defines == from.defines && hit.resolved.constants == from.constants) {
			return it->second;
		}
	}

	shader_config::constant_data derived = get(source).resolved;
	derived.program = program;
	const variant_id id = intern(derived);
	m_program_memo[memo_key] = id;
	return id;
}

const rnd::shader_config::constant_data& rnd::shader_config_cache::resolve(variant_id id) const
{
	return get(id).resolved;
}

std::uint64_t rnd::shader_config_cache::key(variant_id id) const
{
	return get(id).key;
}

const rnd::shader_config_cache::entry& rnd::shader_config_cache::get(variant_id id) const
{
	ASSERT_MSG(id.is_valid() && id.value < m_entries.size(), "unknown shader variant id");
	return m_entries[id.value];
}
