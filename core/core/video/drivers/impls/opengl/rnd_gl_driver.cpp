#include "rnd_gl_driver.h"
#include "open_gl_specific.h"
#include "rnd_gl_framebuffers.h"
#include <snk/common/ds/ds_fixed_vector.hpp>
#include <glad/glad.h>
#include <rnd_gl_shader.h>
#include <rnd_gl_texture.h>
#include <rnd_gl_vertex_array.h>
#include <rnd_gl_buffer.h>
#include <rnd_gl_uniform_buffer.h>
#include <rnd_gl_ssbo_buffer.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/engine_assert.h>

const GLenum gRenderModeToGLRenderMode[] =
{
	GL_TRIANGLES,
	GL_TRIANGLE_STRIP,
	GL_LINES,
	GL_LINE_STRIP,
	GL_POINTS,
};

const GLenum gShaderTypeToGLShaderType[] =
{
	GL_VERTEX_SHADER,
	GL_FRAGMENT_SHADER,
	GL_GEOMETRY_SHADER,
};

// Conversion tables for OpenGL constants
const GLenum gDepthFuncToGL[] = {
	GL_LESS,
	GL_LEQUAL,
	GL_EQUAL,
	GL_ALWAYS,
	GL_GREATER,
	GL_GEQUAL,
	GL_NOTEQUAL,
	GL_NEVER
};

const GLenum gBlendFactorToGL[] = {
	GL_ZERO,
	GL_ONE,
	GL_SRC_COLOR,
	GL_ONE_MINUS_SRC_COLOR,
	GL_DST_COLOR,
	GL_ONE_MINUS_DST_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA
};

const GLenum gCullModeToGL[] = {
	GL_FRONT,
	GL_BACK,
	GL_FRONT_AND_BACK
};

const GLenum gBlendEquationToGL[] = {
	GL_FUNC_ADD,
	GL_FUNC_SUBTRACT,
	GL_FUNC_REVERSE_SUBTRACT,
	GL_MIN,
	GL_MAX
};

struct gl_type
{
	GLenum internal_format;
	GLenum format;
	GLenum data_type;
};

gl_type to_gl_type(rnd::driver::texture_header::TYPE type)
{
	GLenum format_internal = 0;
	GLenum format_2 = 0;
	GLenum data_type = GL_UNSIGNED_BYTE;
	switch (type)
	{
	case rnd::driver::texture_header::TYPE::R8:
	{
		format_internal = GL_R8;
		format_2 = GL_RED;
	}
	break;
	case rnd::driver::texture_header::TYPE::RGB8:
	{
		format_internal = GL_RGB8;
		format_2 = GL_RGB;
	}
	break;
	case rnd::driver::texture_header::TYPE::RGBA8:
	{
		format_internal = GL_RGBA8;
		format_2 = GL_RGBA;
	}
	break;
	case rnd::driver::texture_header::TYPE::RGBA12:
	{
		format_internal = GL_RGBA12;
		format_2 = GL_RGBA;
	}
	break;
	case rnd::driver::texture_header::TYPE::RGBA16:
	{
		format_internal = GL_RGBA16;
		format_2 = GL_RGBA;
	}
	break;
	case rnd::driver::texture_header::TYPE::RGBA16F:
	{
		format_internal = GL_RGBA16F;
		format_2 = GL_RGBA;
	}
	break;
	case rnd::driver::texture_header::TYPE::RGBA32F:
	{
		format_internal = GL_RGBA32F;
		format_2 = GL_RGBA;
	}
	break;
	case rnd::driver::texture_header::TYPE::R32I:
	{
		format_internal = GL_R32I;
		format_2 = GL_RED_INTEGER;
		data_type = GL_INT;
	}
	break;
	case rnd::driver::texture_header::TYPE::R32F:
	{
		format_internal = GL_R32F;
		format_2 = GL_RED_INTEGER;
		data_type = GL_FLOAT;
	}
	break;
	case rnd::driver::texture_header::TYPE::D24_S8:
	{
		format_internal = GL_DEPTH24_STENCIL8;
	}
	break;
	case rnd::driver::texture_header::TYPE::D32F:
	{
		format_internal = GL_DEPTH_COMPONENT32F;
	}
	break;
	case rnd::driver::texture_header::TYPE::D32F_S8:
	{
		format_internal = GL_DEPTH32F_STENCIL8;
	}
	break;
	case rnd::driver::texture_header::TYPE::D16:
	{
		format_internal = GL_DEPTH_COMPONENT16;
	}
	break;
	case rnd::driver::texture_header::TYPE::D24:
	{
		format_internal = GL_DEPTH_COMPONENT24;
	}
	break;
	case rnd::driver::texture_header::TYPE::D32:
	{
		format_internal = GL_DEPTH_COMPONENT32;
	}
	break;
	case rnd::driver::texture_header::TYPE::S1:
	{
		format_internal = GL_STENCIL_INDEX1;
	}
	break;
	case rnd::driver::texture_header::TYPE::S4:
	{
		format_internal = GL_STENCIL_INDEX4;
	}
	break;
	case rnd::driver::texture_header::TYPE::S8:
	{
		format_internal = GL_STENCIL_INDEX8;
	}
	break;

	}
	return { format_internal, format_2, data_type };
}

rnd::driver::gl::driver::driver()
	: m_framebuffers(std::make_shared<framebuffer_registry>())
{
}

rnd::driver::gl::driver::~driver()
{
	m_framebuffers->release_all();
}

namespace {
	using namespace rnd::driver;

	void attach(GLuint fbo, GLenum point, const attachment_desc& desc)
	{
		const auto* gl_texture = static_cast<const gl::texture*>(desc.texture);
		switch (gl_texture->get_type()) {
		case TEXTURE_TYPE::RENDERBUFFER:
			glNamedFramebufferRenderbuffer(fbo, point, GL_RENDERBUFFER, gl_texture->get_id());
			break;
		case TEXTURE_TYPE::TEXTURE_3D:
		case TEXTURE_TYPE::TEXTURE_CUBE_MAP:
		case TEXTURE_TYPE::TEXTURE_1D_ARRAY:
		case TEXTURE_TYPE::TEXTURE_2D_ARRAY:
		case TEXTURE_TYPE::TEXTURE_CUBE_ARRAY:
			if (desc.layer == attachment_desc::all_layers) {
				glNamedFramebufferTexture(fbo, point, gl_texture->get_id(), static_cast<GLint>(desc.mip));
			} else {
				glNamedFramebufferTextureLayer(fbo, point, gl_texture->get_id(), static_cast<GLint>(desc.mip), static_cast<GLint>(desc.layer));
			}
			break;
		default:
			glNamedFramebufferTexture(fbo, point, gl_texture->get_id(), static_cast<GLint>(desc.mip));
			break;
		}
		CHECK_GL_ERROR();
	}

	void check_framebuffer_status(GLuint fbo)
	{
		const GLenum status = glCheckNamedFramebufferStatus(fbo, GL_DRAW_FRAMEBUFFER);
		if (status == GL_FRAMEBUFFER_COMPLETE) {
			return;
		}
		const char* reason = "Unknown Framebuffer Error";
		switch (status) {
		case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: reason = "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT"; break;
		case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: reason = "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT"; break;
		case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS: reason = "GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS"; break;
		case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER: reason = "GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER"; break;
		case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE: reason = "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE"; break;
		case GL_FRAMEBUFFER_UNSUPPORTED: reason = "GL_FRAMEBUFFER_UNSUPPORTED"; break;
		default: break;
		}
		egLOG("opengl", "Framebuffer is not complete: {0}", reason);
		ASSERT_FAIL("ERROR");
	}
}

GLuint rnd::driver::gl::driver::create_framebuffer(const render_pass_desc& desc)
{
	GLuint fbo = 0;
	glCreateFramebuffers(1, &fbo);
	CHECK_GL_ERROR();

	ds::fixed_vector<GLenum, render_pass_desc::max_colors> draw_buffers;
	GLenum read_buffer = GL_NONE;
	for (std::size_t i = 0; i < desc.colors.size(); ++i) {
		const attachment_desc& color = desc.colors[i];
		if (!color.texture) {
			draw_buffers.push_back(GL_NONE);
			continue;
		}
		ASSERT_MSG(color.texture->has_usage(texture_usage::color_target), "Texture must have COLOR_TARGET usage flag");
		const GLenum point = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
		attach(fbo, point, color);
		draw_buffers.push_back(point);
		if (read_buffer == GL_NONE) {
			read_buffer = point;
		}
	}
	if (draw_buffers.empty()) {
		glNamedFramebufferDrawBuffer(fbo, GL_NONE);
	} else {
		glNamedFramebufferDrawBuffers(fbo, static_cast<GLsizei>(draw_buffers.size()), draw_buffers.data());
	}
	glNamedFramebufferReadBuffer(fbo, read_buffer);
	CHECK_GL_ERROR();

	if (desc.depth_stencil && desc.depth_stencil->texture) {
		const attachment_desc& depth = *desc.depth_stencil;
		ASSERT_MSG(depth.texture->has_usage(texture_usage::depth_target) || depth.texture->has_usage(texture_usage::stencil_target),
			"Texture must have DEPTH_TARGET or STENCIL_TARGET usage flag");
		attach(fbo, static_cast<texture*>(depth.texture)->get_attachment_type(), depth);
	}

	check_framebuffer_status(fbo);
	return fbo;
}

GLuint rnd::driver::gl::driver::framebuffer_for(const render_pass_desc& desc)
{
	const auto key = make_framebuffer_key(desc, [](texture_handle handle) -> std::uint64_t {
		return handle ? static_cast<const texture*>(handle)->framebuffer_object() : 0;
	});
	return m_framebuffers->cache.acquire(key, [&] { return create_framebuffer(desc); });
}

void rnd::driver::gl::driver::begin_render_pass(const render_pass_desc& desc)
{
	ASSERT_MSG(!m_in_pass, "begin_render_pass inside a render pass");
	m_in_pass = true;
	if (desc.debug_name && glad_glPushDebugGroup) {
		glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, desc.debug_name);
		m_pass_debug_group = true;
	}

	const bool window = desc.backbuffer;
	m_pass_fbo = window ? 0 : framebuffer_for(desc);
	glBindFramebuffer(GL_FRAMEBUFFER, m_pass_fbo);
	CHECK_GL_ERROR();

	// The window's buffers are addressed by GL_COLOR / GL_DEPTH, a framebuffer's by its attachment points.
	const auto takes_part = [window](const attachment_desc& a) { return window || a.texture != nullptr; };
	const auto color_point = [window](std::size_t i) { return window ? GL_COLOR : GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i); };
	const auto depth_point = [window](const attachment_desc& a) {
		return window ? GL_DEPTH : static_cast<texture*>(a.texture)->get_attachment_type();
	};

	std::array<GLenum, render_pass_desc::max_colors + 1> discard{};
	std::size_t discard_count = 0;
	m_pass_invalidate_count = 0;
	bool any_clear = false;
	const auto note_ops = [&](const attachment_desc& a, GLenum point) {
		any_clear = any_clear || a.load == load_op::clear;
		if (a.load == load_op::dont_care) {
			discard[discard_count++] = point;
		}
		if (a.store == store_op::dont_care) {
			m_pass_invalidate[m_pass_invalidate_count++] = point;
		}
	};
	for (std::size_t i = 0; i < desc.colors.size(); ++i) {
		if (takes_part(desc.colors[i])) {
			note_ops(desc.colors[i], color_point(i));
		}
	}
	const bool depth_takes_part = desc.depth_stencil && takes_part(*desc.depth_stencil);
	if (depth_takes_part) {
		note_ops(*desc.depth_stencil, depth_point(*desc.depth_stencil));
	}

	if (discard_count > 0) {
		glInvalidateNamedFramebufferData(m_pass_fbo, static_cast<GLsizei>(discard_count), discard.data());
		CHECK_GL_ERROR();
	}

	const bool has_area = desc.render_area.z > 0 && desc.render_area.w > 0;
	if (any_clear) {
		if (has_area) {
			glEnable(GL_SCISSOR_TEST);
			glScissor(desc.render_area.x, desc.render_area.y, desc.render_area.z, desc.render_area.w);
		} else {
			glDisable(GL_SCISSOR_TEST);
		}

		for (std::size_t i = 0; i < desc.colors.size(); ++i) {
			const attachment_desc& color = desc.colors[i];
			if (takes_part(color) && color.load == load_op::clear) {
				glClearNamedFramebufferfv(m_pass_fbo, GL_COLOR, static_cast<GLint>(i), glm::value_ptr(color.clear_color));
				CHECK_GL_ERROR();
			}
		}

		if (depth_takes_part && desc.depth_stencil->load == load_op::clear) {
			const attachment_desc& depth = *desc.depth_stencil;
			const bool with_stencil = !window && depth_point(depth) == GL_DEPTH_STENCIL_ATTACHMENT;
			if (!m_depth_write) {
				glDepthMask(GL_TRUE);
			}
			if (with_stencil) {
				glClearNamedFramebufferfi(m_pass_fbo, GL_DEPTH_STENCIL, 0, depth.clear_depth, static_cast<GLint>(depth.clear_stencil));
			} else {
				glClearNamedFramebufferfv(m_pass_fbo, GL_DEPTH, 0, &depth.clear_depth);
			}
			if (!m_depth_write) {
				glDepthMask(GL_FALSE);
			}
			CHECK_GL_ERROR();
		}

		if (has_area) {
			glDisable(GL_SCISSOR_TEST);
		}
	}

	if (has_area) {
		set_viewport(desc.render_area);
	}
}

void rnd::driver::gl::driver::end_render_pass()
{
	ASSERT_MSG(m_in_pass, "end_render_pass outside a render pass");
	if (m_pass_invalidate_count > 0) {
		glInvalidateNamedFramebufferData(m_pass_fbo, static_cast<GLsizei>(m_pass_invalidate_count), m_pass_invalidate.data());
		CHECK_GL_ERROR();
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (m_pass_debug_group) {
		glPopDebugGroup();
		m_pass_debug_group = false;
	}
	m_pass_fbo = 0;
	m_in_pass = false;
}

void rnd::driver::gl::driver::set_viewport(glm::ivec4 rect)
{
	if (viewport != rect) {
		viewport = rect;
		glViewport(rect[0], rect[1], rect[2], rect[3]);
		CHECK_GL_ERROR();
	}
}

void rnd::driver::gl::driver::set_line_size(float size)
{
	glLineWidth(size);
	CHECK_GL_ERROR();
}

void rnd::driver::gl::driver::draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, unsigned int count, unsigned int offset)
{
	if (!m_bound) {
		egLOG_ERROR("opengl", "draw without a bound pipeline is skipped");
		return;
	}
	const GLenum rm = m_bound->backend.topology;

	vertices->bind();
	if (offset > 0) {
		glDrawElementsBaseVertex(rm, count, GL_UNSIGNED_INT, 0, offset);
		CHECK_GL_ERROR();
	} else {
		glDrawElements(rm, count, GL_UNSIGNED_INT, 0);
		CHECK_GL_ERROR();
	}


	vertices->unbind();
}

void rnd::driver::gl::driver::draw_indices(const vertex_array_interface* vertices, unsigned int count, unsigned int base_vertex, unsigned int base_index)
{
	if (!m_bound) {
		egLOG_ERROR("opengl", "draw without a bound pipeline is skipped");
		return;
	}
	const GLenum rm = m_bound->backend.topology;

	vertices->bind();
	glDrawElementsBaseVertex(rm, count, GL_UNSIGNED_INT, (void*)(sizeof(GLuint) * base_index), base_vertex);
	vertices->unbind();
}

rnd::driver::pipeline_handle rnd::driver::gl::driver::create_pipeline(const pipeline_desc& desc)
{
	ASSERT_MSG(desc.shader, "create_pipeline without a shader");
	return m_pipelines.acquire(desc, [](const pipeline_desc& d) {
		return gl_pipeline{ gRenderModeToGLRenderMode[static_cast<int>(d.topology)] };
	});
}

const rnd::driver::pipeline_desc& rnd::driver::gl::driver::pipeline_info(pipeline_handle pipeline) const
{
	const auto* found = m_pipelines.find(pipeline);
	return found ? found->desc : empty_pipeline_desc();
}

void rnd::driver::gl::driver::invalidate_state_cache()
{
	m_state = {};
}

void rnd::driver::gl::driver::bind_pipeline(pipeline_handle pipeline)
{
	const auto* found = m_pipelines.find(pipeline);
	ASSERT_MSG(found, "bind_pipeline with a handle of no pipeline");
	apply_pipeline(found->desc);
	m_bound = found;
	m_depth_write = found->desc.depth.enabled && found->desc.depth.write_mask;
}

namespace {
	// Runs `apply` unless the cache already holds `wanted`.
	template <class T, class Apply>
	void change(std::optional<T>& cached, const T& wanted, Apply&& apply)
	{
		if (cached && *cached == wanted) {
			return;
		}
		apply();
		cached = wanted;
	}
}

void rnd::driver::gl::driver::apply_pipeline(const pipeline_desc& desc)
{
	const auto* program = static_cast<const gl::shader*>(desc.shader.object);
	change(m_state.program, program->program(), [&] { program->use(); });

	// Without depth test the mask is off too, a pass that does not test does not write.
	const bool depth_test = desc.depth.enabled;
	change(m_state.depth_test, depth_test, [&] {
		if (depth_test) {
			glEnable(GL_DEPTH_TEST);
		} else {
			glDisable(GL_DEPTH_TEST);
		}
	});
	if (depth_test) {
		change(m_state.depth_func, gDepthFuncToGL[static_cast<int>(desc.depth.test_func)], [&] {
			glDepthFunc(gDepthFuncToGL[static_cast<int>(desc.depth.test_func)]);
		});
	}
	const bool depth_mask = depth_test && desc.depth.write_mask;
	change(m_state.depth_mask, depth_mask, [&] { glDepthMask(depth_mask ? GL_TRUE : GL_FALSE); });
	CHECK_GL_ERROR();

	// GL_BLEND is global and follows the first target; a target that does not blend gets the pass-through function.
	const bool blend = !desc.blend.empty() && desc.blend[0].enabled;
	change(m_state.blend, blend, [&] {
		if (blend) {
			glEnable(GL_BLEND);
		} else {
			glDisable(GL_BLEND);
		}
	});
	for (std::size_t i = 0; i < desc.blend.size(); ++i) {
		const blend_state& b = desc.blend[i];
		const state_cache::blend_funcs wanted = b.enabled
			? state_cache::blend_funcs{ gBlendFactorToGL[static_cast<int>(b.src_factor)], gBlendFactorToGL[static_cast<int>(b.dst_factor)],
				gBlendEquationToGL[static_cast<int>(b.blend_eq)] }
			: state_cache::blend_funcs{ GL_ONE, GL_ZERO, GL_FUNC_ADD };
		change(m_state.target_blend[i], wanted, [&] {
			glBlendFunci(static_cast<GLuint>(i), wanted.src, wanted.dst);
			glBlendEquationi(static_cast<GLuint>(i), wanted.equation);
		});
	}
	CHECK_GL_ERROR();

	const bool cull = desc.cull.enabled;
	change(m_state.cull, cull, [&] {
		if (cull) {
			glEnable(GL_CULL_FACE);
		} else {
			glDisable(GL_CULL_FACE);
		}
	});
	if (cull) {
		change(m_state.cull_face, gCullModeToGL[static_cast<int>(desc.cull.cull_mode)], [&] {
			glCullFace(gCullModeToGL[static_cast<int>(desc.cull.cull_mode)]);
		});
	}
	CHECK_GL_ERROR();
}

std::unique_ptr<rnd::driver::shader_interface> rnd::driver::gl::driver::create_shader(const std::vector<shader_header>& headers)
{
	std::vector<GLuint> shaders_ids;
	GLint success;
	GLchar infoLog[512];

	std::string dbgLogTitle;

	for (const auto& header : headers) {
		const GLchar* code = header.body.c_str();
		GLuint shader_id;

		shader_id = glCreateShader(gShaderTypeToGLShaderType[(int)header.type]);
		glShaderSource(shader_id, 1, &code, nullptr);
		glCompileShader(shader_id);
		glGetShaderiv(shader_id, GL_COMPILE_STATUS, &success);
		if (!success) {
			glGetShaderInfoLog(shader_id, 512, nullptr, infoLog);
			egLOG("shader/load", "Error compiling shader '{0}' with log: {1}", header.title, infoLog);
			continue;
		}
		if (dbgLogTitle.empty()) {
			dbgLogTitle = "'" + header.title + "'";
		} else {
			dbgLogTitle += ", '" + header.title + "'";
		}

		shaders_ids.push_back(shader_id);
	}

	// Shader Program
	GLuint id = glCreateProgram();
	for (auto shader_id : shaders_ids) {
		glAttachShader(id, shader_id);
	}

	glLinkProgram(id);

	glGetProgramiv(id, GL_LINK_STATUS, &success);
	if (!success) {
		glGetProgramInfoLog(id, 512, nullptr, infoLog);
		for (auto shader_id : shaders_ids) {
			glDeleteShader(shader_id);
		}

		egLOG("shader/load", "Error Linking shaders {}", dbgLogTitle);
		egLOG("shader/load", "{}", infoLog);
		return nullptr;
	}

	for (auto shader_id : shaders_ids) {
		glDeleteShader(shader_id);
	}

	egLOG("shader/load", "Success loading shader {}", dbgLogTitle);
	return std::make_unique<rnd::driver::gl::shader>(id);
}

std::unique_ptr<rnd::driver::texture_interface> rnd::driver::gl::driver::create_texture(const texture_header& header)
{
	return std::make_unique<rnd::driver::gl::texture>(header, m_framebuffers);
}

std::unique_ptr<rnd::driver::vertex_array_interface> rnd::driver::gl::driver::create_vertex_array()
{
	return std::make_unique<gl::vertex_array>();
}

std::unique_ptr<rnd::driver::buffer_interface> rnd::driver::gl::driver::create_buffer()
{
	return std::make_unique<gl::buffer>();
}

std::unique_ptr<rnd::driver::uniform_buffer_interface> rnd::driver::gl::driver::create_uniform_buffer(std::size_t size, std::size_t binding)
{
	return std::make_unique<gl::uniform_buffer>(size, binding);
}

std::unique_ptr<rnd::driver::ssbo_buffer_interface> rnd::driver::gl::driver::create_ssbo_buffer(std::size_t size, std::size_t binding)
{
	static_assert(sizeof(GLuint) == sizeof(uint32_t));
	static_assert(sizeof(GLint) == sizeof(uint32_t));
	ASSERT_MSG((size / sizeof(GLuint)) == (size / sizeof(uint32_t)), "Missmatch size with OpenGL size");
	return std::make_unique<gl::gl_ssbo_buffer>(size, binding);
}

// GPU timer queries

bool rnd::driver::gl::driver::supports_timer_queries() const
{
	return glad_glQueryCounter != NULL && glad_glGetQueryObjectui64v != NULL;
}

uint32_t rnd::driver::gl::driver::create_timer_query()
{
	GLuint id = 0;
	glGenQueries(1, &id);
	return static_cast<uint32_t>(id);
}

void rnd::driver::gl::driver::destroy_timer_query(uint32_t id)
{
	GLuint gl_id = static_cast<GLuint>(id);
	glDeleteQueries(1, &gl_id);
}

void rnd::driver::gl::driver::timestamp_query(uint32_t id)
{
	glQueryCounter(static_cast<GLuint>(id), GL_TIMESTAMP);
}

bool rnd::driver::gl::driver::is_query_ready(uint32_t id) const
{
	GLint available = 0;
	glGetQueryObjectiv(static_cast<GLuint>(id), GL_QUERY_RESULT_AVAILABLE, &available);
	return available != 0;
}

uint64_t rnd::driver::gl::driver::get_query_result_ns(uint32_t id) const
{
	GLuint64 result = 0;
	glGetQueryObjectui64v(static_cast<GLuint>(id), GL_QUERY_RESULT, &result);
	return static_cast<uint64_t>(result);
}
