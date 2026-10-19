#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/rnd_bindings.h>
#include <snk/render/rnd_frame_context.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_gpu_blocks.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Bind groups on the whole engine frame: the passes bind the sets their pipelines name, the per-draw ring is
// filled and bound with aligned offsets, nothing but transient groups is created once the scene is warm, and the
// shaders the engine compiles come out of the preprocessor with their bindings from the backend's table.

namespace vl = rnd::driver::validation;
namespace fs = std::filesystem;
using namespace rnd::driver;

namespace {

struct bind_group_fixture : test_util::null_engine_fixture
{
	// Loads the smoke level and lets its assets finish, as the material cache tests do.
	void load_warm_smoke_level()
	{
		auto& levels = storage.require<scn::context_manager>();
		BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));
		test_util::pump(tasks);
		BOOST_REQUIRE(run_frames(3) == 3);
	}

	// Calls `probe` once per frame right after the passes ran, while the frame's packets are still around.
	template <class Probe>
	void run_probed(int frames, Probe&& probe)
	{
		auto& stages = storage.require<app::frame_stage_registry>();
		stages.add_stage("bind_group_probe", app::builtin_stage::render, app::stage_side::after,
			[&](app::stage_context&) { probe(); });
		BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(frames) == frames));
		stages.remove_stage("bind_group_probe");
	}

	std::pmr::vector<rnd::render_packet_t>& packets()
	{
		return storage.require<rnd::frame_context>().data.require<std::pmr::vector<rnd::render_packet_t>>();
	}

	void expect_no_violations()
	{
		const vl::validator& state = validating_driver()->state();
		for (const auto& violation : state.violations()) {
			BOOST_ERROR("frame " << violation.frame << ", " << vl::rule_name(violation.kind) << ": " << violation.message);
		}
		BOOST_TEST(state.total() == 0u);
	}

	// The set 3 binds of the log from `first` on.
	std::vector<const null::driver::bind_record*> object_binds(std::size_t first)
	{
		std::vector<const null::driver::bind_record*> result;
		const auto& log = driver()->bind_log();
		for (std::size_t i = first; i < log.size(); ++i) {
			if (log[i].set == object_set) {
				result.push_back(&log[i]);
			}
		}
		return result;
	}
};

std::string read_file(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

std::size_t count_of(const std::string& text, const std::string& what)
{
	std::size_t count = 0;
	for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) {
		++count;
	}
	return count;
}

// The source without the lines that are comments.
std::string code_of(const std::string& source)
{
	std::istringstream in(source);
	std::string line;
	std::string code;
	while (std::getline(in, line)) {
		const auto first = line.find_first_not_of(" \t\r");
		if (first != std::string::npos && line.compare(first, 2, "//") == 0) {
			continue;
		}
		code += line + "\n";
	}
	return code;
}

// The `uniform` declarations of the source that are neither blocks nor samplers: values set one by one by name,
// which the engine's shaders no longer have (a sampler gets its unit from its binding, a block from its binding).
std::vector<std::string> free_uniforms(const std::string& source)
{
	std::string without_blocks;
	for (std::size_t i = 0; i < source.size(); ++i) {
		if (source.compare(i, 2, "/*") == 0) {
			const auto end = source.find("*/", i + 2);
			i = end == std::string::npos ? source.size() : end + 1;
		} else {
			without_blocks += source[i];
		}
	}
	const std::string code = code_of(without_blocks);
	const auto is_word_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
	std::vector<std::string> result;
	for (std::size_t at = code.find("uniform"); at != std::string::npos; at = code.find("uniform", at + 7)) {
		if ((at > 0 && is_word_char(code[at - 1])) || (at + 7 < code.size() && is_word_char(code[at + 7]))) {
			continue;
		}
		const auto stop = code.find_first_of(";{", at);
		if (stop != std::string::npos && code[stop] == '{') {
			continue;	// a block
		}
		std::string declaration = code.substr(at + 7, stop == std::string::npos ? std::string::npos : stop - at - 7);
		declaration.erase(0, declaration.find_first_not_of(" \t\r\n"));
		if (declaration.rfind("sampler", 0) == 0) {
			continue;
		}
		result.push_back("uniform " + declaration);
	}
	return result;
}

}

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendBindGroups, bind_group_fixture)

BOOST_AUTO_TEST_CASE(EveryDrawHasTheSetsOfItsPipelineBound)
{
	load_warm_smoke_level();
	const auto draws_before = driver()->calls().draws;
	const auto before = driver()->draws_with_set();
	BOOST_REQUIRE(run_frames(4) == 4);

	// draw_missing_bind_group and bind_group_layout_mismatch are the validation's check that each draw has a group
	// of the pipeline's layout at every set the pipeline names.
	const vl::validator& state = validating_driver()->state();
	BOOST_TEST(state.count(vl::rule::draw_missing_bind_group) == 0u);
	BOOST_TEST(state.count(vl::rule::bind_group_layout_mismatch) == 0u);
	expect_no_violations();

	const std::size_t draws = driver()->calls().draws - draws_before;
	const auto with_set = [&](std::uint32_t set) { return driver()->draws_with_set()[set] - before[set]; };
	BOOST_TEST(draws > 0u);
	BOOST_TEST(with_set(frame_set) > 0u);	// scene, sky and debug draws
	BOOST_TEST(with_set(pass_set) > 0u);	// the sky's cubemap
	BOOST_TEST(with_set(object_set) > 0u);	// scene draws
	BOOST_TEST(with_set(material_set) == with_set(object_set));	// every scene draw has its material's group too
	// Only the composition draws lack the frame set, and they have the pass set instead.
	BOOST_TEST(with_set(frame_set) + with_set(pass_set) >= draws);
	BOOST_TEST(with_set(object_set) < draws);
}

BOOST_AUTO_TEST_CASE(ObjectOffsetsAreAlignedStartOverEveryFrameAndWrapTheFramesInFlight)
{
	load_warm_smoke_level();
	driver()->record_bindings(true);
	const std::size_t alignment = driver()->uniform_offset_alignment();

	std::size_t log_begin = 0;
	std::uint32_t previous_slot = driver()->frame_slot();
	std::set<std::uint32_t> slots;
	int frames_seen = 0;
	run_probed(8, [&] {
		BOOST_TEST_CONTEXT("frame " << frames_seen) {
			std::size_t objects = 0;
			for (const auto& packet : packets()) {
				objects += packet.opaque_draws.size() + packet.transparent_draws.size();
			}
			const auto binds = object_binds(log_begin);
			BOOST_REQUIRE(!binds.empty());

			const std::size_t stride = alignment;	// ObjectData (80 bytes) rounded up to the null backend's 256
			std::set<std::size_t> used;
			for (const auto* bind : binds) {
				BOOST_REQUIRE(bind->dynamic_offsets.size() == 1u);
				const std::uint32_t offset = bind->dynamic_offsets[0];
				BOOST_TEST(offset % alignment == 0u);
				BOOST_TEST(offset / stride < objects);
				BOOST_TEST(bind->frame_slot == driver()->frame_slot());
				used.insert(offset / stride);
			}
			// Elements are numbered from the start of the ring every frame.
			BOOST_TEST(*used.begin() == 0u);
			BOOST_TEST(used.size() > 0u);

			// Each frame moves to the next copy of the ring, and the copies wrap after three.
			BOOST_TEST(driver()->frame_slot() == (previous_slot + 1) % frames_in_flight);
			previous_slot = driver()->frame_slot();
			slots.insert(previous_slot);
			log_begin = driver()->bind_log().size();
		}
		++frames_seen;
	});
	BOOST_TEST(slots.size() == 3u);
	expect_no_violations();
}

BOOST_AUTO_TEST_CASE(TheRingAndTheBoneBufferHoldWhatExtractProduced)
{
	load_warm_smoke_level();
	driver()->record_bindings(true);
	std::size_t skinned_draws = 0;
	std::size_t checked_draws = 0;
	run_probed(4, [&] {
		const auto binds = object_binds(0);
		BOOST_REQUIRE(!binds.empty());
		const auto* group = driver()->group_info(binds.back()->group);
		BOOST_REQUIRE(group);
		BOOST_REQUIRE(group->entries.size() == 3u);
		const std::uint32_t slot = driver()->frame_slot();
		const auto* ring = driver()->buffer_bytes(group->entries[0].buffer, slot);
		const auto* bones = driver()->buffer_bytes(group->entries[1].buffer, slot);
		BOOST_REQUIRE(ring);
		BOOST_REQUIRE(bones);
		const std::size_t stride = driver()->uniform_offset_alignment();

		const auto element = [&](std::uint32_t index) {
			rnd::object_data stored;
			BOOST_REQUIRE((index + 1) * stride <= ring->size());
			std::memcpy(&stored, ring->data() + index * stride, sizeof(stored));
			return stored;
		};
		for (const auto& packet : packets()) {
			const auto check = [&](const rnd::draw_call_t& dc, std::uint32_t index) {
				const rnd::object_data stored = element(index);
				BOOST_CHECK(stored.world == dc.transform);
				BOOST_CHECK(stored.bone_offset == packet.bone_base + dc.bone_offset);
				++checked_draws;
				if (dc.skinning_tag.is_valid()) {
					++skinned_draws;
					BOOST_REQUIRE((stored.bone_offset + 1) * sizeof(glm::mat4) <= bones->size());
					glm::mat4 first_bone;
					std::memcpy(&first_bone, bones->data() + stored.bone_offset * sizeof(glm::mat4), sizeof(first_bone));
					BOOST_CHECK(first_bone == packet.bones.matrices[dc.bone_offset]);
				}
			};
			for (std::size_t i = 0; i < packet.opaque_draws.size(); ++i) {
				check(packet.opaque_draws[i], packet.opaque_object(i));
			}
			for (std::size_t i = 0; i < packet.transparent_draws.size(); ++i) {
				check(packet.transparent_draws[i], packet.transparent_object(i));
			}
		}
	});
	BOOST_TEST(checked_draws > 0u);
	BOOST_TEST_MESSAGE("draws checked " << checked_draws << ", of them skinned " << skinned_draws);
}

BOOST_AUTO_TEST_CASE(NoBindGroupOrBufferIsCreatedAfterWarmUpExceptTransientGroups)
{
	load_warm_smoke_level();
	std::vector<std::size_t> persistent;
	std::vector<std::size_t> transient;
	std::vector<std::size_t> buffers;
	std::vector<std::size_t> live_groups;
	std::vector<std::size_t> validated;
	run_probed(8, [&] {
		persistent.push_back(driver()->calls().bind_groups_created);
		transient.push_back(driver()->calls().transient_bind_groups_created);
		buffers.push_back(driver()->calls().buffers_created);
		live_groups.push_back(driver()->live_bind_groups());
		validated.push_back(validating_driver()->state().current_frame().bind_groups);
	});

	BOOST_REQUIRE(persistent.size() == 8u);
	for (std::size_t frame = 1; frame < persistent.size(); ++frame) {
		BOOST_TEST_CONTEXT("frame " << frame) {
			BOOST_TEST(persistent[frame] == persistent[0]);
			BOOST_TEST(buffers[frame] == buffers[0]);
			BOOST_TEST(live_groups[frame] == live_groups[0]);
			BOOST_TEST(validated[frame] == 0u);	// the validation's own count of persistent groups in the frame
			// The sky and the composition read textures: their input groups are made again every frame.
			BOOST_TEST(transient[frame] > transient[frame - 1]);
		}
	}
	expect_no_violations();
}

BOOST_AUTO_TEST_CASE(ShadersComeOutOfThePreprocessorWithTheBindingsOfTheBackend)
{
	driver()->record_shader_sources(true);
	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));
	test_util::pump(tasks);
	BOOST_REQUIRE(run_frames(3) == 3);

	const auto& programs = driver()->shader_sources();
	BOOST_REQUIRE(programs.size() >= 4u);	// z prepass, scene, sky, composition and the debug lines at least
	const std::string max_lights = "#define MAX_LIGHT_COUNT " + std::to_string(rnd::lights_params::MAX_LIGHT_COUNT) + "\n";
	std::set<std::string> seen;
	for (const auto& program : programs) {
		for (const shader_header& header : program) {
			BOOST_TEST_CONTEXT(header.title) {
				const std::string code = code_of(header.body);
				seen.insert(header.title.substr(0, header.title.find_first_of(".{ ")) + header.title.substr(header.title.find_last_of('.')));
				BOOST_TEST(code.rfind("#version 450 core", 0) == 0u);
				BOOST_TEST(count_of(code, max_lights) == 1u);
				BOOST_TEST(count_of(code, "#define MAX_LIGHT_COUNT") == 1u);
				BOOST_TEST(code.find("SNK_UBO(") == std::string::npos);
				BOOST_TEST(code.find("SNK_SSBO(") == std::string::npos);
				BOOST_TEST(code.find("SNK_TEX(") == std::string::npos);

				const bool vertex = header.type == shader_header::shader_type::VERTEX;
				const bool fragment = header.type == shader_header::shader_type::FRAGMENT;
				BOOST_TEST(count_of(code, "#define SNK_VERTEX_SHADER\n") == (vertex ? 1u : 0u));
				BOOST_TEST(count_of(code, "#define SNK_FRAGMENT_SHADER\n") == (fragment ? 1u : 0u));

				if (code.find("uniform Matrices") != std::string::npos) {
					BOOST_TEST(code.find("layout(std140, binding = 0) uniform Matrices") != std::string::npos);
				}
				if (code.find("uniform GlobalLights") != std::string::npos) {
					BOOST_TEST(code.find("layout(std140, binding = 1) uniform GlobalLights") != std::string::npos);
				}
				// The block sits behind #ifdef SNK_VERTEX_SHADER in the common source, so only vertex stages declare it.
				if (vertex && code.find("uniform ObjectData") != std::string::npos) {
					BOOST_TEST(code.find("layout(std140, binding = 6) uniform ObjectData") != std::string::npos);
				}
				if (code.find("buffer BoneMatrices") != std::string::npos) {
					BOOST_TEST(code.find("layout(std430, binding = 0) readonly buffer BoneMatrices") != std::string::npos);
					BOOST_TEST(code.find("layout(std430, binding = 1) readonly buffer IndexesBuffer") != std::string::npos);
				}
				if (code.find("uniform MaterialParams") != std::string::npos) {
					BOOST_TEST(code.find("layout(std140, binding = 4) uniform MaterialParams") != std::string::npos);
				}
				if (code.find("sampler2D diffuseTxm") != std::string::npos) {
					BOOST_TEST(code.find("layout(binding = 4) uniform sampler2D diffuseTxm") != std::string::npos);
				}
				if (code.find("sampler2D specularTxm") != std::string::npos) {
					BOOST_TEST(code.find("layout(binding = 5) uniform sampler2D specularTxm") != std::string::npos);
				}
				if (code.find("sampler2D normalTxm") != std::string::npos) {
					BOOST_TEST(code.find("layout(binding = 6) uniform sampler2D normalTxm") != std::string::npos);
				}
				BOOST_TEST(free_uniforms(code).empty());
				if (code.find("samplerCube diffuse") != std::string::npos) {
					BOOST_TEST(code.find("layout(binding = 0) uniform samplerCube diffuse") != std::string::npos);
				}
				if (code.find("sampler2D reveal") != std::string::npos) {
					BOOST_TEST(code.find("layout(binding = 0) uniform sampler2D accum") != std::string::npos);
					BOOST_TEST(code.find("layout(binding = 1) uniform sampler2D reveal") != std::string::npos);
				}
				BOOST_TEST(code.find("hightlight") == std::string::npos);
				BOOST_TEST(code.find("uWorldMeshMatr") == std::string::npos);
			}
		}
	}
	BOOST_TEST(seen.contains("scene.vert"));
	BOOST_TEST(seen.contains("sky.frag"));
	BOOST_TEST(seen.contains("composition.frag"));
	BOOST_TEST(seen.contains("z_prepass.vert"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ShippedShaderSources)

BOOST_AUTO_TEST_CASE(TheLightCountAndTheBindingsComeFromTheEngineNotFromTheSources)
{
	const fs::path dir = fs::path{ SNK_CORE_RES_DIR } / "shaders";
	std::size_t files = 0;
	for (const auto& entry : fs::directory_iterator(dir)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		++files;
		const std::string source = read_file(entry.path());
		const std::string code = code_of(source);
		const std::string name = entry.path().filename().string();
		BOOST_TEST_CONTEXT(name) {
			// One source of the light count: the engine injects it, GLSL only checks it is there.
			BOOST_TEST(code.find("#define MAX_LIGHT_COUNT") == std::string::npos);
			// Blocks and storage buffers take their bindings from the table.
			BOOST_TEST(code.find("std140") == std::string::npos);
			BOOST_TEST(code.find("std430") == std::string::npos);
			BOOST_TEST(code.find("hightlight") == std::string::npos);
			BOOST_TEST(code.find("uWorldMeshMatr") == std::string::npos);
			// Nothing is set by name: every uniform is a block or a sampler with a binding of the table (#121).
			const auto free = free_uniforms(source);
			BOOST_TEST(free.empty(), "free uniform: " << (free.empty() ? "" : free.front()));
			const bool has_version = code.find("#version") != std::string::npos;
			if (has_version) {
				BOOST_TEST(code.rfind("#version 450 core", 0) == 0u);
			}
		}
	}
	BOOST_TEST(files >= 10u);

	const std::string common = read_file(dir / "snk_common.glsl");
	BOOST_TEST(common.find("#ifndef MAX_LIGHT_COUNT") != std::string::npos);
	BOOST_TEST(common.find("#error") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheMaterialIsABlockAndThreeMapsOfSet2)
{
	const fs::path dir = fs::path{ SNK_CORE_RES_DIR } / "shaders";
	const std::string phong = code_of(read_file(dir / "phong_model_lib.frag"));
	BOOST_TEST(phong.find("SNK_UBO(2, 0) uniform MaterialParams") != std::string::npos);
	for (const char* member : { "vec4 diffuseColor;", "vec4 emissiveColor;", "float shininess;" }) {
		BOOST_TEST(phong.find(member) != std::string::npos, member);
	}
	BOOST_TEST(phong.find("SNK_TEX(2, 0) uniform sampler2D diffuseTxm") != std::string::npos);
	BOOST_TEST(phong.find("SNK_TEX(2, 1) uniform sampler2D specularTxm") != std::string::npos);
	for (const char* frag : { "scene.frag", "z_prepass.frag", "mix_opaque_trans_scene.frag" }) {
		BOOST_TEST(code_of(read_file(dir / frag)).find("SNK_TEX(2, 2) uniform sampler2D normalTxm") != std::string::npos, frag);
	}
}

BOOST_AUTO_TEST_CASE(SkinningNeverReadsPastTheBufferItIsGiven)
{
	// A skinned variant of a mesh without weights is bound a buffer of zeros (or one shorter than the mesh):
	// the length is checked before anything is read from it.
	const std::string skinning = code_of(read_file(fs::path{ SNK_CORE_RES_DIR } / "shaders" / "skinning_lib.glsl"));
	const auto length = skinning.find("data.length()");
	const auto first_read = skinning.find("data[0]");
	BOOST_REQUIRE(length != std::string::npos);
	BOOST_REQUIRE(first_read != std::string::npos);
	BOOST_TEST(length < first_read);
	BOOST_TEST(skinning.find("ID >= numColumns") != std::string::npos);
	BOOST_TEST(skinning.find("indicesStart + colEnd > size") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
