#include "rnd_scene_shader_desc.h"
#include "res_system.h"
#include "rnd_shader_interface.h"
#include "res_resource_text_file.h"
#include "rnd_render_system.h"


void rnd::shader_desc::preprocess_shader_code(std::vector<res::tag> tags, std::vector<rnd::driver::shader_header>& headers, const auto& defines_names) const
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

                auto include_code = res::get_system().require_resource<res::TextFile>(tags[index] + res::tag::make(include), true);
                if (include_code) {
                    std::size_t line_end = body.find_first_of('\n', offset);
                    body_view = header.body.replace(offset, line_end - offset, include_code->c_str());
                }
            }

            offset = body.find("#include", offset);
        }
        ++index;
    }

    if (!defines_names.empty()) {
        std::string addition_defines;
        std::vector<std::string_view> addition_defines_names;
        for (int i = 0; i < defines.size(); ++i) {
            if (defines[i]) {
                if (defines_values[i].empty()) {
                    auto define = std::vformat("#define {0}\n", std::make_format_args(defines_names[i]));
                    addition_defines.append(define);
                } else {
                    auto define = std::vformat("#define {0} {1}\n", std::make_format_args(defines_names[i], defines_values[i]));
                    addition_defines.append(define);
                }
                addition_defines_names.push_back(defines_names[i]);
            }
        }

        if (!addition_defines_names.empty()) {
            std::string format = (std::string)addition_defines_names.front();
            for (int i = 1; i < addition_defines_names.size(); ++i) {
                auto& name_ = addition_defines_names[i];
                format = std::vformat("{0} | {1}", std::make_format_args(format, name_));
            }
            std::string title = std::vformat("{{ {0} }}", std::make_format_args(format));

            for (auto& header : headers) {
                header.title.insert(header.title.find_first_of('.'), title);
            }
        }

        for (auto& header : headers) {
            header.body.insert(header.body.find_first_of('\n') + 1, addition_defines);
        }
    }
}

std::vector<rnd::driver::shader_header> rnd::shader_scene_desc::load() const
{
    std::string shader_path = std::vformat("shaders/{0}", std::make_format_args(name));
	auto vertexCode = res::get_system().require_resource<res::TextFile>(res::tag::make(shader_path + ".vert"), true);
	auto fragmentCode = res::get_system().require_resource<res::TextFile>(res::tag::make(shader_path + ".frag"), true);
	//auto geomCode = res::get_system().require_resource<res::TextFile>(res::tag::make(name + ".geom"), true);

	std::vector<driver::shader_header> headers
	{
		driver::shader_header{.title = std::string(name) + ".vert", .body = vertexCode->c_str(), .type = driver::shader_header::shader_type::VERTEX},
		driver::shader_header{.title = std::string(name) + ".frag", .body = fragmentCode->c_str(), .type = driver::shader_header::shader_type::FRAGMENT},
	};

    std::vector<res::tag> tags
    {
        res::tag::make(shader_path + ".vert"),
        res::tag::make(shader_path + ".frag"),
    };

    defines[LIGHTS_ENABLED] = defines[DIRECTION_LIGHT_COUNT] || defines[POINT_LIGHT_COUNT];

    preprocess_shader_code(tags, headers, get_all_define_names());

    defines[LIGHTS_ENABLED] = false;
	return headers;
}

std::vector<rnd::driver::shader_header> rnd::shader_sky_desc::load() const
{
	auto vertexCode = res::get_system().require_resource<res::TextFile>(res::tag::make("shaders/sky.vert"), true);
	auto fragmentCode = res::get_system().require_resource<res::TextFile>(res::tag::make("shaders/sky.frag"), true);
	//auto geomCode = res::get_system().require_resource<res::TextFile>(res::tag::make(name + ".geom"), true);
	std::vector<driver::shader_header> headers
	{
		driver::shader_header{.title = "sky.vert", .body = vertexCode->c_str(), .type = driver::shader_header::shader_type::VERTEX},
		driver::shader_header{.title = "sky.frag", .body = fragmentCode->c_str(), .type = driver::shader_header::shader_type::FRAGMENT},
	};

	return headers;
}

std::vector<rnd::driver::shader_header> rnd::shader_scene_instance_desc::load() const
{
	auto vertexCode = res::get_system().require_resource<res::TextFile>(res::tag::make("shaders/scene_inst.vert"), true);
	auto fragmentCode = res::get_system().require_resource<res::TextFile>(res::tag::make("shaders/scene_inst.frag"), true);
	//auto geomCode = res::get_system().require_resource<res::TextFile>(res::tag::make(name + ".geom"), true);
	std::vector<driver::shader_header> headers
	{
		driver::shader_header{.title = "scene_inst.vert", .body = vertexCode->c_str(), .type = driver::shader_header::shader_type::VERTEX},
		driver::shader_header{.title = "scene_inst.frag", .body = fragmentCode->c_str(), .type = driver::shader_header::shader_type::FRAGMENT},
	};

	return headers;
}

std::vector<rnd::driver::shader_header> rnd::pass_composition_desc::load() const
{
    std::string shader_path = std::vformat("shaders/{0}", std::make_format_args(name));
    auto vertexCode = res::get_system().require_resource<res::TextFile>(res::tag::make(shader_path + ".vert"), true);
    auto fragmentCode = res::get_system().require_resource<res::TextFile>(res::tag::make(shader_path + ".frag"), true);
    //auto geomCode = res::get_system().require_resource<res::TextFile>(res::tag::make(name + ".geom"), true);

    std::vector<driver::shader_header> headers
    {
        driver::shader_header{.title = std::string(name) + ".vert", .body = vertexCode->c_str(), .type = driver::shader_header::shader_type::VERTEX},
        driver::shader_header{.title = std::string(name) + ".frag", .body = fragmentCode->c_str(), .type = driver::shader_header::shader_type::FRAGMENT},
    };

    std::vector<res::tag> tags
    {
        res::tag::make(shader_path + ".vert"),
        res::tag::make(shader_path + ".frag"),
    };

    preprocess_shader_code(tags, headers, get_all_define_names());
    return headers;
}

void rnd::configure_render_pass(const shader_desc& desc, rnd::driver::shader_interface* shader)
{
    if (desc.tex0) {
        desc.tex0->bind(0);
    }

    if (desc.tex1) {
        desc.tex1->bind(1);
    }

    if (desc.tex2) {
        desc.tex2->bind(2);
    }

    if (desc.tex3) {
        desc.tex3->bind(3);
    }
}

void rnd::configure_render_pass(const shader_scene_desc& desc, rnd::driver::shader_interface* shader)
{
    shader->uniform("uWorldModel", desc.uWorldModel);
    shader->uniform("uWorldMeshMatr", desc.uWorldMeshMatr);
    shader->uniform("diffuseColor", desc.diffuseColor);
    shader->uniform("emissiveColor", desc.emissiveColor);
    shader->uniform("shininess", desc.shininess);

    if (desc.tex0) {
        desc.tex0->bind(0);
    }

    if (desc.tex1) {
        desc.tex1->bind(1);
    }

    if (desc.tex2) {
        desc.tex2->bind(2);
    }

    if (desc.tex3) {
        desc.tex3->bind(3);
    }
}


static void preprocess_shader_code(std::vector<res::tag> tags, std::vector<rnd::driver::shader_header>& headers, const rnd::new_shader_desc& desc)
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

                auto include_code = res::get_system().require_resource<res::TextFile>(tags[index] + res::tag::make(include), true);
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

std::vector<rnd::driver::shader_header> rnd::new_shader_desc::load() const
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

		auto shaderCode = res::get_system().require_resource<res::TextFile>(tag, true);
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

void rnd::configure_render_pass(const new_shader_desc& desc, rnd::driver::shader_interface* shader)
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