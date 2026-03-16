#include "rnd_scene_shader_desc.h"
#include "res_system.h"
#include "rnd_shader_interface.h"
#include "resources/res_resource_text.h"
#include "rnd_render_system.h"

static void preprocess_shader_code(std::vector<res::tag> tags, std::vector<rnd::driver::shader_header>& headers, const rnd::shader_config& desc)
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

                auto include_code = res::get_system().require<res::text_resource>(tags[index] + res::tag::make(include)).get_sync();
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
    for (const auto& [name, value] : desc.cdata.constants) {
        auto define = std::vformat("#define {0} {1}\n", std::make_format_args(name, value));
		addition_defines << define;
        dbg_addition_defines_names.push_back(name);
    }

    for (const auto& def : desc.cdata.defines) {
        auto define = std::vformat("#define {0}\n", std::make_format_args(def));
		addition_defines << define;
        dbg_addition_defines_names.push_back(def);
    }

    if (!dbg_addition_defines_names.empty()) {
        std::string format = dbg_addition_defines_names.front();
        for (int i = 1; i < dbg_addition_defines_names.size(); ++i) {
            auto& name_ = dbg_addition_defines_names[i];
            format = std::vformat("{0} | {1}", std::make_format_args(format, name_));
        }
        std::string title = std::vformat("{{ {0} }}", std::make_format_args(format));

        for (auto& header : headers) {
            header.title.insert(header.title.find_first_of('.'), title);
        }
    }

    for (auto& header : headers) {
        header.body.insert(header.body.find_first_of('\n') + 1, addition_defines.view());
    }

}

std::vector<rnd::driver::shader_header> rnd::shader_config::load() const
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

		auto shaderCode = res::get_system().require<res::text_resource>(tag).get_sync();
		if (shaderCode) {
            headers.push_back(driver::shader_header{ 
                .title = std::string{tag.name()}, 
                .body = shaderCode->c_str(), 
                .type = culc_shader_type(tag.extension())
                });
			tags.push_back(tag);
		}
	}

    preprocess_shader_code(tags, headers, *this);
    return headers;
}

void rnd::configure_render_pass(const shader_config& desc, rnd::driver::shader_interface* shader)
{
	for (int idx = 0; idx < desc.rdata.samplers.size(); ++idx)
	{
		if (desc.rdata.samplers[idx]) {
			desc.rdata.samplers[idx]->bind(idx);
		}
	}

	for (const auto& [name, data] : desc.rdata.uniforms)
	{
        shader->uniform(name, data);
	}

}