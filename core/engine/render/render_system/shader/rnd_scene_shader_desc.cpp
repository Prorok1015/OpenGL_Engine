#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/render/rnd_gpu_blocks.h>
#include <snk/render/shader/rnd_shader_bindings.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <format>
#include <sstream>

// The engine's own defines: the one source of the light array length (the std140 layout of GlobalLights depends
// on it, see lights_params) and the stage the source is compiled for.
static std::string engine_defines(rnd::driver::shader_header::shader_type type)
{
    const char* stage = "SNK_VERTEX_SHADER";
    if (type == rnd::driver::shader_header::shader_type::FRAGMENT) stage = "SNK_FRAGMENT_SHADER";
    if (type == rnd::driver::shader_header::shader_type::GEOMETRY) stage = "SNK_GEOMETRY_SHADER";
    return std::format("#define MAX_LIGHT_COUNT {}\n#define {}\n", rnd::lights_params::MAX_LIGHT_COUNT, stage);
}

// Returns what is wrong with the stages (empty when they are fit for the compiler).
static std::string preprocess_shader_code(std::vector<res::tag> tags, std::vector<rnd::driver::shader_header>& headers, const rnd::shader_config::constant_data& cdata, res::resource_system& res_sys, const rnd::driver::binding_table& bindings)
{
    ASSERT_MSG(tags.size() == headers.size(), "tags and headers sizes should be equal");

    std::size_t index = 0;
    for (auto& header : headers)
    {
        std::string_view body_view = header.body;
        std::string& body = header.body;
        std::size_t offset = body.find("#include");
        while (offset != std::string_view::npos) {
            std::size_t begin = body.find_first_of('"', offset);
            std::size_t end = body.find_first_of('"', begin + 1);

            if (begin != std::string_view::npos && end != std::string_view::npos) {
                std::string_view include = body_view.substr(begin + 1, end - begin - 1);

                auto include_code = res_sys.require_sync<res::text_resource>(tags[index] + res::tag::make(include)).get_sync();
                if (include_code) {
                    std::size_t line_end = body.find_first_of('\n', offset);
                    body_view = header.body.replace(offset, line_end - offset, include_code->c_str());
                }
            }

            offset = body.find("#include", offset);
        }
        ++index;
    }

    std::vector<std::string> dbg_addition_defines_names;

    std::ostringstream addition_defines;
    for (const auto& [name, value] : cdata.constants) {
        auto define = std::vformat("#define {0} {1}\n", std::make_format_args(name, value));
		addition_defines << define;
        dbg_addition_defines_names.push_back(name);
    }

    for (const auto& def : cdata.defines) {
        auto define = std::vformat("#define {0}\n", std::make_format_args(def));
		addition_defines << define;
        dbg_addition_defines_names.push_back(def);
    }

    if (!dbg_addition_defines_names.empty()) {
        std::string format = dbg_addition_defines_names.front();
        for (std::size_t i = 1; i < dbg_addition_defines_names.size(); ++i) {
            auto& name_ = dbg_addition_defines_names[i];
            format = std::vformat("{0} | {1}", std::make_format_args(format, name_));
        }
        std::string title = std::vformat("{{ {0} }}", std::make_format_args(format));

        for (auto& header : headers) {
            header.title.insert(header.title.find_first_of('.'), title);
        }
    }

    std::string problems;
    for (auto& header : headers) {
        // Lines count from the top of the source after the #include lines were replaced by the files.
        const std::string errors = rnd::expand_binding_macros(header.body, bindings);
        if (!errors.empty()) {
            problems += std::format("'{}': {}", header.title, errors);
        }
        if (!rnd::insert_after_version(header.body, engine_defines(header.type) + std::string{ addition_defines.view() })) {
            problems += std::format("'{}': the source has to begin with a #version directive\n", header.title);
        }
    }
    return problems;
}

rnd::shader_config::load_result rnd::shader_config::load(res::resource_system& res_sys, const driver::binding_table& bindings) const
{
	return load(cdata, res_sys, bindings);
}

rnd::shader_config::load_result rnd::shader_config::load(const constant_data& cdata, res::resource_system& res_sys, const driver::binding_table& bindings)
{
    std::vector<res::tag> tags;
    std::vector<driver::shader_header> headers;
	headers.reserve(3);
    tags.reserve(3);

	auto culc_shader_type = [](const std::string_view ext)
		{
			if (ext == "vert") return driver::shader_header::shader_type::VERTEX;
			if (ext == "frag") return driver::shader_header::shader_type::FRAGMENT;
			if (ext == "geom") return driver::shader_header::shader_type::GEOMETRY;
			return driver::shader_header::shader_type::VERTEX;
		};

	for (const auto& tag : cdata.program)
	{
		if (!tag.is_valid()) continue;

		auto shaderCode = res_sys.require_sync<res::text_resource>(tag).get_sync();
		if (shaderCode) {
            headers.push_back(driver::shader_header{ 
                .title = std::string{tag.name()}, 
                .body = shaderCode->c_str(), 
                .type = culc_shader_type(tag.extension())
                });
			tags.push_back(tag);
		}
	}

    load_result result;
    result.error = preprocess_shader_code(tags, headers, cdata, res_sys, bindings);
    if (result.error.empty() && headers.empty()) {
        result.error = "no stage of the program could be read\n";
    }
    if (!result.error.empty()) {
        egLOG_ERROR("shader/load", "shader not loaded:\n{}", result.error);
        return result;
    }
    result.headers = std::move(headers);
    return result;
}
