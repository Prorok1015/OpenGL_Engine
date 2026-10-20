#include <boost/test/unit_test.hpp>
#include <snk/render/shader/rnd_shader_config_cache.h>

BOOST_AUTO_TEST_SUITE(RndShaderConfigCacheTests)

namespace {

rnd::shader_config::constant_data make_cdata(std::initializer_list<const char*> defines = {})
{
	rnd::shader_config::constant_data cdata;
	cdata.program.set_vertex_shader(res::tag::make("shaders/a.vert"));
	cdata.program.set_fragment_shader(res::tag::make("shaders/a.frag"));
	for (const char* define : defines) {
		cdata.defines.emplace_back(define);
	}
	return cdata;
}

}

BOOST_AUTO_TEST_CASE(VariantHash_IsStableAcrossProcesses)
{
	auto cdata = make_cdata({ "USE_ANIMATION" });
	cdata.constants["COUNT"] = "4";
	BOOST_TEST(cdata.variant_hash() == cdata.variant_hash());
	// pinned: the key must not drift with the standard library or the platform
	BOOST_TEST(rnd::shader_config::constant_data::fnv1a(std::string_view{ "OPAQUE" }) == 0x8dac72c8206d5a3eull);
}

BOOST_AUTO_TEST_CASE(VariantHash_IgnoresDefineAndConstantOrder)
{
	auto a = make_cdata({ "A", "B" });
	auto b = make_cdata({ "B", "A" });
	a.constants["X"] = "1";
	a.constants["Y"] = "2";
	b.constants["Y"] = "2";
	b.constants["X"] = "1";
	BOOST_TEST(a.variant_hash() == b.variant_hash());
}

BOOST_AUTO_TEST_CASE(VariantHash_ConstantNamesMatter)
{
	auto a = make_cdata();
	auto b = make_cdata();
	a.constants["X"] = "1";
	b.constants["Y"] = "1";
	BOOST_TEST(a.variant_hash() != b.variant_hash());
}

BOOST_AUTO_TEST_CASE(VariantHash_ConstantValuesMatter)
{
	auto a = make_cdata();
	auto b = make_cdata();
	a.constants["COUNT"] = "4";
	b.constants["COUNT"] = "5";
	BOOST_TEST(a.variant_hash() != b.variant_hash());
}

BOOST_AUTO_TEST_CASE(VariantHash_IgnoresInsertionOrderOfManyKeys)
{
	const char* names[] = { "K0", "K1", "K2", "K3", "K4", "K5", "K6", "K7" };
	auto forward = make_cdata();
	auto backward = make_cdata();
	for (int i = 0; i < 8; ++i) {
		forward.defines.emplace_back(names[i]);
		forward.constants[names[i]] = std::to_string(i);
		backward.defines.emplace_back(names[7 - i]);
		backward.constants[names[7 - i]] = std::to_string(7 - i);
	}
	BOOST_TEST(forward.variant_hash() == backward.variant_hash());
	BOOST_TEST(forward.variant_hash(rnd::pass_define_opaque) == backward.variant_hash(rnd::pass_define_opaque));
}

// The key is FNV-1a over the bytes of the tags, defines and constants (no std::hash, no pointers, no container
// order), so the value is the same on every platform and run. If this constant has to change, every cached
// variant key changes with it: do it on purpose.
BOOST_AUTO_TEST_CASE(VariantHash_HasAKnownValueForAFixedInput)
{
	rnd::shader_config::constant_data cdata;
	cdata.program.set_vertex_shader(res::tag{ "memory://t/a.vert" });
	cdata.program.set_fragment_shader(res::tag{ "memory://t/a.frag" });
	cdata.program.set_geometry_shader(res::tag{ "memory://t/a.geom" });
	cdata.defines = { "USE_ANIMATION", "LIGHTS_ENABLED" };
	cdata.constants["COUNT"] = "4";
	BOOST_TEST(cdata.variant_hash() == 0x93a70b97ced321c8ull);
}

BOOST_AUTO_TEST_CASE(VariantHash_PassMaskMatters)
{
	const auto cdata = make_cdata();
	BOOST_TEST(cdata.variant_hash(rnd::pass_define_none) != cdata.variant_hash(rnd::pass_define_opaque));
}

BOOST_AUTO_TEST_CASE(Intern_SameConfigGivesSameId)
{
	rnd::shader_config_cache cache;
	const auto a = cache.intern(make_cdata({ "A", "B" }));
	const auto b = cache.intern(make_cdata({ "B", "A" }));
	BOOST_CHECK(a == b);
	BOOST_TEST(cache.size() == 1u);
}

BOOST_AUTO_TEST_CASE(Intern_DifferentDefinesGiveDifferentIds)
{
	rnd::shader_config_cache cache;
	BOOST_CHECK(cache.intern(make_cdata({ "A" })) != cache.intern(make_cdata({ "B" })));
	BOOST_TEST(cache.size() == 2u);
}

BOOST_AUTO_TEST_CASE(Intern_OpaquePassDefineIsAVariantNotAMutation)
{
	rnd::shader_config_cache cache;
	const auto cdata = make_cdata({ "A" });
	const auto base = cache.intern(cdata);
	const auto opaque = cache.intern(cdata, rnd::pass_define_opaque);

	BOOST_CHECK(base != opaque);
	BOOST_TEST(cache.key(base) != cache.key(opaque));
	BOOST_CHECK(cache.resolve(base).defines == (std::vector<std::string>{ "A" }));
	BOOST_CHECK(cache.resolve(opaque).defines == (std::vector<std::string>{ "A", "OPAQUE" }));
	BOOST_CHECK(cdata.defines == (std::vector<std::string>{ "A" }));
	BOOST_CHECK(cache.intern(cdata, rnd::pass_define_opaque) == opaque);
}

BOOST_AUTO_TEST_CASE(WithProgram_KeepsDefinesAndIsMemoized)
{
	rnd::shader_config_cache cache;
	const auto source = cache.intern(make_cdata({ "A" }), rnd::pass_define_opaque);
	auto program = rnd::shader_config::shader_program_data::build()
		.set_vertex_shader(res::tag::make("shaders/z.vert"))
		.set_fragment_shader(res::tag::make("shaders/z.frag"));

	const auto derived = cache.with_program(source, program);
	BOOST_CHECK(derived != source);
	BOOST_CHECK(cache.resolve(derived).program == program);
	BOOST_CHECK(cache.resolve(derived).defines == cache.resolve(source).defines);

	const auto count = cache.size();
	BOOST_CHECK(cache.with_program(source, program) == derived);
	BOOST_TEST(cache.size() == count);
}

BOOST_AUTO_TEST_CASE(CompiledMaterial_IsCurrentOnlyForSameInputs)
{
	auto desc = std::make_shared<int>(0);
	rnd::compiled_material material;
	material.source = desc;
	material.skinned = true;
	material.texture_generation = 3;

	BOOST_TEST(material.is_current(desc.get(), true, 3));
	BOOST_TEST(!material.is_current(desc.get(), false, 3));
	BOOST_TEST(!material.is_current(desc.get(), true, 4));
	BOOST_TEST(!material.is_current(nullptr, true, 3));

	material.complete = false;
	BOOST_TEST(!material.is_current(desc.get(), true, 3));
}

BOOST_AUTO_TEST_SUITE_END()
