#pragma once
#include <vector>
#include <string>
#include <glm/glm.hpp>

namespace rnd::driver
{
	enum class SHADER_DATA_TYPE
	{
		UNKNOWN, FLOAT, VEC2_F, VEC3_F, VEC4_F, MAT3_F, MAT4_F, INT, VEC2_I, VEC3_I, VEC4_I, BOOL
	};

	static uint32_t shader_data_type_size(SHADER_DATA_TYPE type)
	{
		switch (type)
		{
		case SHADER_DATA_TYPE::FLOAT:    return sizeof(float);
		case SHADER_DATA_TYPE::VEC2_F:   return sizeof(float) * 2;
		case SHADER_DATA_TYPE::VEC3_F:   return sizeof(float) * 3;
		case SHADER_DATA_TYPE::VEC4_F:   return sizeof(float) * 4;
		case SHADER_DATA_TYPE::MAT3_F:   return sizeof(float) * 3 * 3;
		case SHADER_DATA_TYPE::MAT4_F:   return sizeof(float) * 4 * 4;
		case SHADER_DATA_TYPE::INT:      return sizeof(int);
		case SHADER_DATA_TYPE::VEC2_I:   return sizeof(int) * 2;
		case SHADER_DATA_TYPE::VEC3_I:   return sizeof(int) * 3;
		case SHADER_DATA_TYPE::VEC4_I:   return sizeof(glm::ivec4);
		case SHADER_DATA_TYPE::BOOL:     return sizeof(bool);
		}

		return 0;
	}

	struct BufferElement
	{
		std::string Name;
		SHADER_DATA_TYPE Type;
		uint32_t Size;
		size_t Offset;
		bool Normalized;

		BufferElement() = default;

		BufferElement(SHADER_DATA_TYPE type, const std::string& name, bool normalized = false)
			: Name(name), Type(type), Size(shader_data_type_size(type)), Offset(0), Normalized(normalized)
		{
		}

		uint32_t get_component_count() const
		{
			switch (Type)
			{
			case SHADER_DATA_TYPE::FLOAT:   return 1;
			case SHADER_DATA_TYPE::VEC2_F:  return 2;
			case SHADER_DATA_TYPE::VEC3_F:  return 3;
			case SHADER_DATA_TYPE::VEC4_F:  return 4;
			case SHADER_DATA_TYPE::MAT3_F:  return 3; // 3* float3
			case SHADER_DATA_TYPE::MAT4_F:  return 4; // 4* float4
			case SHADER_DATA_TYPE::INT:     return 1;
			case SHADER_DATA_TYPE::VEC2_I:  return 2;
			case SHADER_DATA_TYPE::VEC3_I:  return 3;
			case SHADER_DATA_TYPE::VEC4_I:  return 4;
			case SHADER_DATA_TYPE::BOOL:    return 1;
			}

			return 0;
		}
	};

	class BufferLayout
	{
	public:
		BufferLayout() {}

		BufferLayout(std::initializer_list<BufferElement> elements)
			: m_Elements(elements)
		{
			calculate_offsets_and_stride();
		}

		explicit BufferLayout(const std::vector<BufferElement>& elements)
			: m_Elements(elements)
		{
			calculate_offsets_and_stride();
		}

		uint32_t get_stride() const { return m_Stride; }
		const std::vector<BufferElement>& get_elements() const { return m_Elements; }

		std::vector<BufferElement>::iterator begin() { return m_Elements.begin(); }
		std::vector<BufferElement>::iterator end() { return m_Elements.end(); }
		std::vector<BufferElement>::const_iterator begin() const { return m_Elements.begin(); }
		std::vector<BufferElement>::const_iterator end() const { return m_Elements.end(); }
	private:
		void calculate_offsets_and_stride()
		{
			size_t offset = 0;
			m_Stride = 0;
			for (auto& element : m_Elements)
			{
				element.Offset = offset;
				offset += element.Size;
				m_Stride += element.Size;
			}
		}
	private:
		std::vector<BufferElement> m_Elements;
		uint32_t m_Stride = 0;
	};
}