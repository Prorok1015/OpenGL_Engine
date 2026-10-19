#pragma once
#include <snk/common/snk_api.h>
#include <snk/driver/rnd_texture_interface.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rnd::driver::validation
{
	enum class rule : std::uint8_t
	{
		pop_without_push,
		unbalanced_at_present,
		render_target_without_push,
		attachment_usage,
		texture_usage_flags,
		texture_not_sampled,
		attachment_size_mismatch,
		viewport_outside_target,
		scissor_outside_target,
		draw_without_use,
		draw_index_range,
		draw_without_layout,
		buffer_write_range,
		dead_object,
		budget_frame_buffers,
		budget_programs,
		budget_textures,
	};

	inline constexpr std::size_t rule_count = static_cast<std::size_t>(rule::budget_textures) + 1;

	SNK_API const char* rule_name(rule r);

	// Per-frame creation budgets; `unlimited` switches a budget off.
	struct limits
	{
		static constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();

		std::size_t max_frame_buffers = unlimited;
		std::size_t max_programs = unlimited;
		std::size_t max_textures = unlimited;
	};

	struct frame_stats
	{
		std::size_t frame_buffers = 0;
		std::size_t programs = 0;
		std::size_t textures = 0;
		std::size_t draws = 0;
	};

	struct violation
	{
		rule kind;
		std::uint64_t frame;
		std::string message;
	};

	enum class object_kind : std::uint8_t
	{
		shader,
		texture,
		vertex_array,
		buffer,
		uniform_buffer,
		ssbo_buffer,
	};

	enum class trace_op : std::uint8_t
	{
		push_target,	// begins a pass-like scope
		pop_target,		// ends it
		set_targets,	// a = color attachment count, b = 1 when a depth/stencil attachment is set
		set_viewport,
		set_scissor,
		clear,
		use_program,
		bind_texture,	// a = slot
		bind_ssbo,		// a = slot
		draw,			// a = count
		create_object,	// a = object_kind
		present,
	};

	struct trace_entry
	{
		std::uint64_t frame = 0;
		trace_op op = trace_op::present;
		std::uint32_t a = 0;
		std::uint32_t b = 0;
	};

	SNK_API std::string format_trace_entry(const trace_entry& entry);

	// "sampled|color_target", or "none".
	SNK_API std::string usage_text(texture_usage usage);

	// The render target the commands currently go to. The backbuffer (depth 1) has no known extent.
	struct target_info
	{
		bool has_extent = false;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
	};

	// Shared by the validating driver, swapchain and every object they create: objects may outlive the
	// driver, so they keep it alive. Holds the violation counters, the live-object registry, the
	// per-frame state and the ring buffer of recent commands.
	class SNK_API validator
	{
	public:
		static constexpr std::size_t max_recorded_violations = 256;

		explicit validator(limits budget = {}, std::size_t trace_capacity = 256);

		void report(rule kind, std::string message);

		std::size_t count(rule kind) const { return m_counts[static_cast<std::size_t>(kind)]; }
		std::size_t total() const { return m_total; }
		// The first violations with their messages; the counters keep counting past the cap.
		const std::vector<violation>& violations() const { return m_violations; }
		const limits& budget() const { return m_budget; }

		// Frames finished so far (presents seen).
		std::uint64_t frame() const { return m_frame; }
		const frame_stats& current_frame() const { return m_current; }
		const frame_stats& last_frame() const { return m_last; }

		void add_object(const void* object, object_kind kind);
		void remove_object(const void* object);
		bool is_live(const void* object, object_kind kind) const;
		std::size_t live_objects(object_kind kind) const;
		// Reports dead_object unless `object` is a live object of `kind`; returns whether it is.
		bool require_live(const void* object, object_kind kind, const char* what);

		void note(trace_op op, std::uint32_t a = 0, std::uint32_t b = 0);
		// Oldest first.
		std::vector<trace_entry> trace() const;

		std::size_t target_depth() const { return m_targets.size(); }
		target_info& current_target() { return m_targets.back(); }
		void push_target();
		// False (and nothing changes) when only the backbuffer is left.
		bool pop_target();

		void on_program_used() { m_program_used = true; }
		bool program_used() const { return m_program_used; }
		void on_draw() { ++m_current.draws; }
		void on_texture_created();
		void on_program_created();

		// The end of a frame: the stack must be back at the backbuffer; per-frame state restarts.
		void on_present();

	private:
		std::string recent_trace(std::size_t count) const;

		limits m_budget;
		std::array<std::size_t, rule_count> m_counts{};
		std::size_t m_total = 0;
		std::vector<violation> m_violations;

		std::uint64_t m_frame = 0;
		frame_stats m_current;
		frame_stats m_last;
		std::vector<target_info> m_targets{ target_info{} };
		bool m_program_used = false;

		mutable std::mutex m_registry_mutex;
		std::unordered_map<const void*, object_kind> m_objects;

		std::vector<trace_entry> m_trace;
		std::size_t m_trace_head = 0;
		std::size_t m_trace_size = 0;
	};
}
