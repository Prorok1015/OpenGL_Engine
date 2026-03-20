#include "rnd_gl_driver.h"
#include "open_gl_specific.h"
#include "ds/ds_fixed_vector.hpp"
#include <glad/glad.h>
#include <rnd_gl_shader.h>
#include <rnd_gl_texture.h>
#include <rnd_gl_vertex_array.h>
#include <rnd_gl_buffer.h>
#include <rnd_gl_uniform_buffer.h>
#include <rnd_gl_ssbo_buffer.h>
#include <engine_log.h>
#include <engine_assert.h>

const GLenum gRenderModeToGLRenderMode[] =
{
	GL_TRIANGLES,
	GL_TRIANGLE_STRIP,
	GL_TRIANGLE_FAN,
	GL_TRIANGLES_ADJACENCY,
	GL_TRIANGLE_STRIP_ADJACENCY,
	GL_LINES,
	GL_LINE_STRIP,
	GL_LINE_LOOP,
	GL_LINES_ADJACENCY,
	GL_LINE_STRIP_ADJACENCY,
	GL_POINTS,
};

const GLenum gShaderTypeToGLShaderType[] =
{
	GL_VERTEX_SHADER,
	GL_FRAGMENT_SHADER,
	GL_GEOMETRY_SHADER,
};

const GLint gTextureFilteringToGlFiltering[] =
{
	GL_NEAREST,
	GL_LINEAR,
	GL_LINEAR_MIPMAP_LINEAR,
};

const GLint gTextureWrappingToGlWrapping[] =
{
	GL_REPEAT,
	GL_MIRRORED_REPEAT,
	GL_CLAMP_TO_EDGE,
	GL_CLAMP_TO_BORDER,
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
{
	framebuffers.push({ 0, 0 });
}

rnd::driver::gl::driver::~driver()
{
}

void rnd::driver::gl::driver::attach_frame_buffer()
{
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffers.top().first);
}

void rnd::driver::gl::driver::detach_frame_buffer()
{
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void rnd::driver::gl::driver::check_frame_buffer()
{
	attach_frame_buffer();
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		std::string errorString;
		switch (status) {
		case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
			errorString = "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
			errorString = "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS:
			errorString = "GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS";
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
			errorString = "GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER";
			break;
		case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
			errorString = "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE";
			break;
		case GL_FRAMEBUFFER_UNSUPPORTED:
			errorString = "GL_FRAMEBUFFER_UNSUPPORTED";
			break;

		default:
			errorString = "Unknown Framebuffer Error";
			break;
		}

		egLOG("opengl", "Framebuffer is not complete: {0}", errorString);
		ASSERT_FAIL("ERROR");
	}
	detach_frame_buffer();
}

void rnd::driver::gl::driver::push_frame_buffer()
{
	GLuint framebuffer = 0;
	glCreateFramebuffers(1, &framebuffer);
	CHECK_GL_ERROR();
	framebuffers.push({ framebuffer, 0 });
}

void rnd::driver::gl::driver::pop_frame_buffer()
{
	if (framebuffers.size() == 1) {
		ASSERT_FAIL("Trying to pop backbuffer");
		return;
	}

	auto [framebuffer, depth_stencil] = framebuffers.top();
	framebuffers.pop();
	glDeleteRenderbuffers(1, &depth_stencil);
	CHECK_GL_ERROR();
	glDeleteFramebuffers(1, &framebuffer);
	CHECK_GL_ERROR();
}

void rnd::driver::gl::driver::set_render_target(texture_interface* color, texture_interface* depth_stencil /* = nullptr*/)
{
	set_render_targets({ color }, depth_stencil);
}

void rnd::driver::gl::driver::set_render_targets(std::vector<texture_interface*> colors, texture_interface* depth_stencil /* = nullptr*/)
{
	auto& [fb, ds_b] = framebuffers.top();
	constexpr std::size_t MAX_COLOR_ATTACHMENTS = (GL_COLOR_ATTACHMENT31 - GL_COLOR_ATTACHMENT0 + 1);
	ASSERT_MSG(colors.size() <= MAX_COLOR_ATTACHMENTS, "Max color attachment is reached!");

	glm::ivec2 rt_size{ 0 };
	std::size_t color_count = std::min(colors.size(), MAX_COLOR_ATTACHMENTS);
	ds::fixed_vector<GLenum, MAX_COLOR_ATTACHMENTS> drawBuffers;

	for (std::size_t i = 0; i < color_count; ++i) {
		if (auto* color = colors[i]) {
			ASSERT_MSG(color->has_usage(TEXTURE_USAGE::COLOR_TARGET), "Texture must have COLOR_TARGET usage flag");
			auto* gl_color = static_cast<texture*>(color);
			
			if (color->get_type() == TEXTURE_TYPE::RENDERBUFFER) {
				glNamedFramebufferRenderbuffer(fb, GL_COLOR_ATTACHMENT0 + i, GL_RENDERBUFFER, gl_color->get_id());
			} else {
				glNamedFramebufferTexture(fb, GL_COLOR_ATTACHMENT0 + i, gl_color->get_id(), 0);
			}
			CHECK_GL_ERROR();
			rt_size = { gl_color->width(), gl_color->height() };
			drawBuffers.push_back(GL_COLOR_ATTACHMENT0 + i);
		}
		else {
			drawBuffers.push_back(GL_NONE);
		}
	}

	if (!drawBuffers.empty()) {
		glNamedFramebufferDrawBuffers(fb, drawBuffers.size(), drawBuffers.data());
	} else {
		glNamedFramebufferDrawBuffers(fb, 0, nullptr);
	}
	CHECK_GL_ERROR();

	if (depth_stencil) {
		ASSERT_MSG(depth_stencil->has_usage(TEXTURE_USAGE::DEPTH_TARGET) || 
			   depth_stencil->has_usage(TEXTURE_USAGE::STENCIL_TARGET), "Texture must have DEPTH_TARGET or STENCIL_TARGET usage flag");
		auto* gl_depth_stencil = static_cast<texture*>(depth_stencil);
		
		if (depth_stencil->get_type() == TEXTURE_TYPE::RENDERBUFFER) {
			glNamedFramebufferRenderbuffer(fb, gl_depth_stencil->get_attachment_type(), GL_RENDERBUFFER, gl_depth_stencil->get_id());
		} else {
			glNamedFramebufferTexture(fb, gl_depth_stencil->get_attachment_type(), gl_depth_stencil->get_id(), 0);
		}
		CHECK_GL_ERROR();
	}

	check_frame_buffer();
}

void rnd::driver::gl::driver::set_viewport(glm::ivec4 rect)
{
	if (viewport != rect) {
		egLOG("renderer/set_viewport", "viewport: '{},{}'", rect[2], rect[3]);
		viewport = rect;
		glViewport(rect[0], rect[1], rect[2], rect[3]);
		CHECK_GL_ERROR();
	}
}

void rnd::driver::gl::driver::set_clear_color(glm::vec4 color)
{
	glClearColor(color.r, color.g, color.b, color.a);
	CHECK_GL_ERROR();
}

void rnd::driver::gl::driver::clear(clear_flags flags)
{
	attach_frame_buffer();
	glClearDepth(1.0);
	GLbitfield gl_flags = 0;
	if (flags.has_flag(CLEAR_FLAGS::COLOR_BUFFER)) gl_flags |= GL_COLOR_BUFFER_BIT;
	if (flags.has_flag(CLEAR_FLAGS::DEPTH_BUFFER)) gl_flags |= GL_DEPTH_BUFFER_BIT;
	if (flags.has_flag(CLEAR_FLAGS::STENCIL_BUFFER)) gl_flags |= GL_STENCIL_BUFFER_BIT;
	if (flags.has_flag(CLEAR_FLAGS::DEPTH_STENCIL_BUFFER)) gl_flags |= (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glClear(gl_flags);
	CHECK_GL_ERROR();
	detach_frame_buffer();
}

void rnd::driver::gl::driver::clear(clear_flags flags, glm::vec4 color)
{
	clear(flags, std::vector<glm::vec4>{color});
}

void rnd::driver::gl::driver::clear(clear_flags flags, const std::vector<glm::vec4>& colors)
{
	if (colors.empty()) {
		return;
	}

	attach_frame_buffer();

	if (flags.has_flag(CLEAR_FLAGS::COLOR_BUFFER)) {
		int i = 0;
		for (auto& color : colors) {
			glClearBufferfv(GL_COLOR, i, glm::value_ptr(color));
			CHECK_GL_ERROR();
			i++;
		}
	}

	if (flags.has_flag(CLEAR_FLAGS::DEPTH_BUFFER)) {
		if (!colors.empty()) {
			glClearBufferfv(GL_DEPTH, 0, glm::value_ptr(colors[0]));
			CHECK_GL_ERROR();
		}
	}

	if (flags.has_flag(CLEAR_FLAGS::STENCIL_BUFFER)) {
		if (!colors.empty()) {
			glClearBufferiv(GL_STENCIL, 0, reinterpret_cast<const GLint*>(glm::value_ptr(colors[0])));
			CHECK_GL_ERROR();
		}
	}

	if (flags.has_flag(CLEAR_FLAGS::DEPTH_STENCIL_BUFFER)) {
		if (!colors.empty()) {
			glClearBufferfi(GL_DEPTH_STENCIL, 0, colors[0].r, static_cast<GLint>(colors[0].g));
			CHECK_GL_ERROR();
		}
	}

	detach_frame_buffer();
}

void rnd::driver::gl::driver::set_activate_texture(int idx)
{
	// active proper texture unit before binding
	glActiveTexture(GL_TEXTURE0 + idx);
	CHECK_GL_ERROR();
}

void rnd::driver::gl::driver::set_line_size(float size)
{
	glLineWidth(size);
	CHECK_GL_ERROR();
}

void rnd::driver::gl::driver::set_point_size(float size)
{
	glPointSize(size);
	CHECK_GL_ERROR();
}

void rnd::driver::gl::driver::draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, RENDER_MODE render_mode, unsigned int count, unsigned int offset)
{
	GLenum rm = GL_TRIANGLES;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffers.top().first);
	if (render_mode == RENDER_MODE::LINE_STRIP_ADJ) {
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	}
	else {
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		rm = gRenderModeToGLRenderMode[(int)render_mode];
	}

	vertices->bind();
	if (offset > 0) {
		glDrawElementsBaseVertex(rm, count, GL_UNSIGNED_INT, 0, offset);
		CHECK_GL_ERROR();
	} else {
		glDrawElements(rm, count, GL_UNSIGNED_INT, 0);
		CHECK_GL_ERROR();
	}


	vertices->unbind();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void rnd::driver::gl::driver::draw_instanced_indices(const std::unique_ptr<vertex_array_interface>& vertices, RENDER_MODE render_mode, unsigned int count, unsigned int instance_count, unsigned int offset)
{
	GLenum rm = GL_TRIANGLES;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffers.top().first);
	if (render_mode == RENDER_MODE::LINE_STRIP_ADJ) {
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	}
	else {
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		rm = gRenderModeToGLRenderMode[(int)render_mode];
	}
	vertices->bind();
	glDrawElementsInstanced(rm, count, GL_UNSIGNED_INT, 0, instance_count);
	CHECK_GL_ERROR();
	vertices->unbind();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void rnd::driver::gl::driver::draw_indices(const vertex_array_interface* vertices, RENDER_MODE render_mode, unsigned int count, unsigned int base_vertex, unsigned int base_index)
{
	GLenum rm = GL_TRIANGLES;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffers.top().first);
	if (render_mode == RENDER_MODE::LINE_STRIP_ADJ) {
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	}
	else {
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		rm = gRenderModeToGLRenderMode[(int)render_mode];
	}

	vertices->bind();
	glDrawElementsBaseVertex(rm, count, GL_UNSIGNED_INT, (void*)(sizeof(GLuint) * base_index), base_vertex);
	vertices->unbind();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void rnd::driver::gl::driver::set_render_state(const render_state& state) {
    // Depth state
    if (state.depth.enabled) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(gDepthFuncToGL[static_cast<int>(state.depth.test_func)]);
        glDepthMask(state.depth.write_mask ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
    CHECK_GL_ERROR();

    // Blend states for each render target
    if (!state.blend_states.empty()) {
        if (state.blend_states[0].enabled) {
            glEnable(GL_BLEND);
        } else {
            glDisable(GL_BLEND);
        }

        for (size_t i = 0; i < state.blend_states.size(); ++i) {
            const auto& blend = state.blend_states[i];
            if (blend.enabled) {
                glBlendFunci(i, 
                    gBlendFactorToGL[static_cast<int>(blend.src_factor)],
                    gBlendFactorToGL[static_cast<int>(blend.dst_factor)]
                );
                glBlendEquationi(i, gBlendEquationToGL[static_cast<int>(blend.blend_eq)]);
            }
			else {
				glBlendFunci(i, GL_ONE, GL_ZERO);
				glBlendEquationi(i, GL_FUNC_ADD);
			}
        }
    } else {
        glDisable(GL_BLEND);
    }
    CHECK_GL_ERROR();

    // Face culling state
    if (state.face_culling.enabled) {
        glEnable(GL_CULL_FACE);
        glCullFace(gCullModeToGL[static_cast<int>(state.face_culling.cull_mode)]);
    } else {
        glDisable(GL_CULL_FACE);
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
	return std::make_unique<rnd::driver::gl::texture>(header);
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

void rnd::driver::gl::driver::register_render_state(const render_state& state) {
    // OpenGL doesn't require state preregistration,
    // but we can cache them for optimization
    state_cache.register_state(state);
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
