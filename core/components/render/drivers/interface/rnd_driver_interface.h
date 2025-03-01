#pragma once
#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>
#include <unordered_map>
#include "ds_bit_frags.hpp"

namespace rnd::driver
{
	class shader_interface;
	class texture_interface;
	struct texture_header;
	class render_context_interface;
	class vertex_array_interface;
	class uniform_buffer_interface;
	class buffer_interface;

	enum class CLEAR_FLAGS { COLOR_BUFFER, DEPTH_BUFFER, STENCIL_BUFFER, DEPTH_STENCIL_BUFFER };

	using clear_flags = ds::bit_flags<CLEAR_FLAGS>;

	// Depth state configuration
	struct depth_state {
		enum class func {
			LESS,
			LEQUAL,
			EQUAL,
			ALWAYS,
			GREATER,
			GEQUAL,
			NOTEQUAL,
			NEVER
		};
		
		bool enabled = false;
		bool write_mask = true;
		func test_func = func::LESS;
	};

	// Blend state configuration
	struct blend_state {
		enum class factor {
			ZERO,
			ONE,
			SRC_COLOR,
			ONE_MINUS_SRC_COLOR,
			DST_COLOR,
			ONE_MINUS_DST_COLOR,
			SRC_ALPHA,
			ONE_MINUS_SRC_ALPHA,
			DST_ALPHA,
			ONE_MINUS_DST_ALPHA
		};

		enum class equation {
			ADD,
			SUBTRACT,
			REVERSE_SUBTRACT,
			MIN,
			MAX
		};

		bool enabled = false;
		factor src_factor = factor::ONE;
		factor dst_factor = factor::ZERO;
		equation blend_eq = equation::ADD;
	};

	// Face culling configuration
	struct face_culling_state {
		enum class mode {
			FRONT,
			BACK,
			FRONT_AND_BACK
		};

		bool enabled = false;
		mode cull_mode = mode::BACK;
	};

	struct shader_header
	{
		enum class TYPE { VERTEX, FRAGMENT, GEOM };
		std::string title;
		std::string body;
		TYPE type;
	};

	enum class RENDER_MODE
	{
		TRIANGLE,
		TRIANGLE_STRIP,
		TRIANGLE_FAN,
		TRIANGLE_ADJ,
		TRIANGLE_STRIP_ADJ,
		LINE,
		LINE_STRIP,
		LINE_LOOP,
		LINE_ADJ,
		LINE_STRIP_ADJ,
		POINT,
	};

	// Combined render state
	struct render_state {
		depth_state depth;
		std::vector<blend_state> blend_states;
		face_culling_state face_culling;

		// Factory methods for common states
		static render_state opaque() {
			render_state state;
			state.depth.enabled = true;
			state.depth.write_mask = true;
			state.depth.test_func = depth_state::func::LESS;
			state.blend_states = { blend_state{} };
			state.face_culling.enabled = true;
			return state;
		}

		static render_state transparent() {
			render_state state;
			state.depth.enabled = true;
			state.depth.write_mask = false;
			state.depth.test_func = depth_state::func::LESS;
			
			blend_state blend;
			blend.enabled = true;
			blend.src_factor = blend_state::factor::SRC_ALPHA;
			blend.dst_factor = blend_state::factor::ONE_MINUS_SRC_ALPHA;
			state.blend_states = { blend };
			
			state.face_culling.enabled = true;
			return state;
		}

		static render_state ui() {
			render_state state;
			state.depth.enabled = false;
			state.depth.write_mask = false;
			
			blend_state blend;
			blend.enabled = true;
			blend.src_factor = blend_state::factor::SRC_ALPHA;
			blend.dst_factor = blend_state::factor::ONE_MINUS_SRC_ALPHA;
			state.blend_states = { blend };
			
			state.face_culling.enabled = false;
			return state;
		}

		// Hash for use as a key in state cache
		size_t hash() const {
			size_t h = 0;
			//TODO: hash calculation based on all fields
			return h;
		}
	};

	// State cache manager
	class render_state_cache {
	public:
		void register_state(const render_state& state) {
			m_states[state.hash()] = state;
		}

		const render_state* get_state(size_t hash) const {
			auto it = m_states.find(hash);
			return it != m_states.end() ? &it->second : nullptr;
		}

	private:
		std::unordered_map<size_t, render_state> m_states;
	};

	class driver_interface
	{
	public:
		virtual ~driver_interface() {}

		virtual void push_frame_buffer() = 0;
		virtual void pop_frame_buffer() = 0;
		virtual void set_render_target(texture_interface* color, texture_interface* depth_stencil = nullptr) = 0;
		virtual void set_render_targets(std::vector<texture_interface*> colors, texture_interface* depth_stencil = nullptr) = 0;

		virtual void set_viewport(glm::ivec4 rect) = 0;
		virtual void set_clear_color(glm::vec4 color) = 0;
		virtual void clear(clear_flags flags) = 0;
		virtual void clear(clear_flags flags, glm::vec4 color) = 0;
		virtual void clear(clear_flags flags, const std::vector<glm::vec4>& colors) = 0;
		virtual void set_activate_texture(int idx) = 0;
		virtual void set_line_size(float size) = 0;
		virtual void set_point_size(float size) = 0;
		virtual void draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, RENDER_MODE render_mode, unsigned int count, unsigned int offset = 0) = 0;
		virtual void draw_instanced_indices(const std::unique_ptr<vertex_array_interface>& vertices, RENDER_MODE render_mode, unsigned int count, unsigned int instance_count, unsigned int offset = 0) = 0;
		virtual void draw_indices(const vertex_array_interface* va, RENDER_MODE rm, unsigned int count, unsigned int base_vertex, unsigned int base_index) = 0;


		virtual std::unique_ptr<shader_interface> create_shader(const std::vector<shader_header>& headers) = 0;
		virtual std::unique_ptr<texture_interface> create_texture(const texture_header& header) = 0;
		virtual std::unique_ptr<vertex_array_interface> create_vertex_array() = 0;
		virtual std::unique_ptr<buffer_interface> create_buffer() = 0;
		virtual std::unique_ptr<uniform_buffer_interface> create_uniform_buffer(std::size_t size, std::size_t binding) = 0;

		virtual void register_render_state(const render_state& state) = 0;
		virtual void set_render_state(const render_state& state) = 0;
	};
}