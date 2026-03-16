#include "rnd_gl_vertex_array.h"
#include "glad/glad.h"
#include "open_gl_specific.h"

static GLenum ShaderDataTypeToOpenGLBaseType(rnd::driver::SHADER_DATA_TYPE type)
{
	using namespace rnd::driver;
	switch (type)
	{
	case SHADER_DATA_TYPE::FLOAT:    return GL_FLOAT;
	case SHADER_DATA_TYPE::VEC2_F:   return GL_FLOAT;
	case SHADER_DATA_TYPE::VEC3_F:   return GL_FLOAT;
	case SHADER_DATA_TYPE::VEC4_F:   return GL_FLOAT;
	case SHADER_DATA_TYPE::MAT3_F:     return GL_FLOAT;
	case SHADER_DATA_TYPE::MAT4_F:     return GL_FLOAT;
	case SHADER_DATA_TYPE::INT:      return GL_INT;
	case SHADER_DATA_TYPE::VEC2_I:     return GL_INT;
	case SHADER_DATA_TYPE::VEC3_I:     return GL_INT;
	case SHADER_DATA_TYPE::VEC4_I:     return GL_INT;
	case SHADER_DATA_TYPE::BOOL:     return GL_BOOL;
	}

	return 0;
}

rnd::driver::gl::vertex_array::vertex_array()
{
	glCreateVertexArrays(1, &vertex_array_id);
}

rnd::driver::gl::vertex_array::~vertex_array()
{
	glDeleteVertexArrays(1, &vertex_array_id);
}

void rnd::driver::gl::vertex_array::bind() const
{
	glBindVertexArray(vertex_array_id);
	CHECK_GL_ERROR();
}

void rnd::driver::gl::vertex_array::unbind() const
{
	glBindVertexArray(0);
}

void rnd::driver::gl::vertex_array::add_vertex_buffer(const std::shared_ptr<rnd::driver::buffer_interface>& vertexBuffer_in)
{
	auto vertexBuffer = std::static_pointer_cast<gl::buffer>(vertexBuffer_in);
	const auto& layout = vertexBuffer->get_layout();

	int binding_index = m_VertexBuffers.size();
	glVertexArrayVertexBuffer(vertex_array_id, binding_index, vertexBuffer->get_id(), 0/*offset*/, layout.get_stride());
	
	for (const auto& element : layout)
	{
		switch (element.Type)
		{
		case SHADER_DATA_TYPE::FLOAT:
		case SHADER_DATA_TYPE::VEC2_F:
		case SHADER_DATA_TYPE::VEC3_F:
		case SHADER_DATA_TYPE::VEC4_F:
		{
			uint32_t count = element.get_component_count();
			GLenum type = ShaderDataTypeToOpenGLBaseType(element.Type);
			GLint isNormalize = element.Normalized ? GL_TRUE : GL_FALSE;
			glVertexArrayAttribFormat(vertex_array_id, m_VertexBufferIndex, count, type, isNormalize, element.Offset);
			CHECK_GL_ERROR();
			glVertexArrayAttribBinding(vertex_array_id, m_VertexBufferIndex, binding_index);
			CHECK_GL_ERROR();
			glEnableVertexArrayAttrib(vertex_array_id, m_VertexBufferIndex);
			CHECK_GL_ERROR();
			m_VertexBufferIndex++;
			
		} break;
		case SHADER_DATA_TYPE::INT:
		case SHADER_DATA_TYPE::VEC2_I:
		case SHADER_DATA_TYPE::VEC3_I:
		case SHADER_DATA_TYPE::VEC4_I:
		case SHADER_DATA_TYPE::BOOL:
		{
			uint32_t count = element.get_component_count();
			GLenum type = ShaderDataTypeToOpenGLBaseType(element.Type);
			//GLint isNormalize = element.Normalized ? GL_TRUE : GL_FALSE;
			glVertexArrayAttribIFormat(vertex_array_id, m_VertexBufferIndex, count, type, element.Offset);
			CHECK_GL_ERROR();
			glVertexArrayAttribBinding(vertex_array_id, m_VertexBufferIndex, binding_index);
			CHECK_GL_ERROR();
			glEnableVertexArrayAttrib(vertex_array_id, m_VertexBufferIndex);
			CHECK_GL_ERROR();
			m_VertexBufferIndex++;
			
		} break;
		case SHADER_DATA_TYPE::MAT3_F:
		case SHADER_DATA_TYPE::MAT4_F:
		{
			uint32_t count = element.get_component_count();
			for (uint32_t i = 0; i < count; i++)
			{
				GLenum type = ShaderDataTypeToOpenGLBaseType(element.Type);
				GLint isNormalize = element.Normalized ? GL_TRUE : GL_FALSE;
				std::size_t offset = (element.Offset + sizeof(float) * count * i);
				glVertexArrayAttribFormat(vertex_array_id, m_VertexBufferIndex, count, type, isNormalize, offset);
				glVertexArrayAttribBinding(vertex_array_id, m_VertexBufferIndex, binding_index);
				glEnableVertexArrayAttrib(vertex_array_id, m_VertexBufferIndex);
				m_VertexBufferIndex++;
			}
			glVertexArrayBindingDivisor(vertex_array_id, binding_index, 1);
			
		} break;

		}
	}

	m_VertexBuffers.push_back(vertexBuffer);
}

void rnd::driver::gl::vertex_array::remove_vertex_buffer(const std::shared_ptr<rnd::driver::buffer_interface>& vertexBuffer)
{
}

void rnd::driver::gl::vertex_array::set_index_buffer(const std::shared_ptr<rnd::driver::buffer_interface>& indexBuffer)
{
	m_IndexBuffer = std::static_pointer_cast<gl::buffer>(indexBuffer);
	bind();
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_IndexBuffer->get_id());
	CHECK_GL_ERROR();
	unbind();
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	CHECK_GL_ERROR();
}