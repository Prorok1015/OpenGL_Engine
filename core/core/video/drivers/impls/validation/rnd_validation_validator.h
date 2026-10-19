#pragma once
#include <snk/common/ds/ds_fixed_vector.hpp>
#include <snk/common/snk_api.h>
#include <snk/driver/rnd_pipeline.h>
#include <snk/driver/rnd_texture_interface.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rnd::driver::validation
{
	enum class rule : std::uint8_t
	{
		unbalanced_at_present,
		attachment_usage,
		texture_usage_flags,
		texture_not_sampled,
		attachment_size_mismatch,
		nested_render_pass,
		end_without_begin,
		invalid_render_pass,
		render_area_outside_target,
		viewport_outside_target,
		draw_outside_render_pass,
		draw_without_pipeline,
		bind_pipeline_outside_pass,
		invalid_pipeline,
		pipeline_format_mismatch,
		draw_index_range,
		draw_without_layout,
		buffer_write_range,
		dead_object,
		budget_frame_buffers,
		budget_programs,
		budget_textures,
		budget_pipelines,
		invalid_buffer,
		bind_group_outside_pass,
		invalid_bind_group,
		bind_group_layout_mismatch,
		binding_outside_table,
		transient_group_expired,
		dynamic_offset_alignment,
		draw_missing_bind_group,
		budget_bind_groups,
	};

	inline constexpr std::size_t rule_count = static_cast<std::size_t>(rule::budget_bind_groups) + 1;

	SNK_API const char* rule_name(rule r);

	// Per-frame creation budgets; `unlimited` switches a budget off.
	struct limits
	{
		static constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();

		std::size_t max_frame_buffers = unlimited;
		std::size_t max_programs = unlimited;
		std::size_t max_textures = unlimited;
		std::size_t max_pipelines = unlimited;
		// Persistent bind groups only: a transient group is meant to be made again every frame.
		std::size_t max_bind_groups = unlimited;
	};

	struct frame_stats
	{
		std::size_t frame_buffers = 0;
		std::size_t programs = 0;
		std::size_t textures = 0;
		std::size_t pipelines = 0;
		std::size_t bind_groups = 0;
		std::size_t transient_bind_groups = 0;
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
	};

	enum class trace_op : std::uint8_t
	{
		set_viewport,
		begin_pass,		// a = color attachment count, b = 1 when a depth/stencil attachment is set
		end_pass,
		bind_pipeline,	// a = pipeline index
		bind_texture,	// a = slot
		bind_group,		// a = set, b = group index
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

	// The render target the commands currently go to. The backbuffer has no known extent.
	struct target_info
	{
		bool has_extent = false;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
	};

	// Formats the render pass in progress renders to: what a pipeline bound in it has to declare.
	struct pass_formats
	{
		ds::fixed_vector<std::optional<texture_format>, pipeline_desc::max_targets> colors;
		std::optional<texture_format> depth;
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

		// The render pass in progress, if any. A pass the validator refused (nested, dead attachment) never
		// becomes current; its end_render_pass is swallowed by `consume_dropped_begin`.
		bool in_pass() const { return m_pass_active; }
		const std::string& pass_name() const { return m_pass_name; }
		void begin_pass(std::string name, target_info target, pass_formats formats, std::uint32_t colors, bool depth);
		// False (and nothing changes) when no pass is in progress.
		bool end_pass();
		void drop_begin() { ++m_dropped_begins; }
		bool consume_dropped_begin();

		const target_info& current_target() const { return m_pass_target; }
		const pass_formats& current_formats() const { return m_pass_formats; }

		// A pipeline is bound for the rest of the pass; the next pass starts without one.
		void on_pipeline_bound() { m_pipeline_bound = true; }
		bool pipeline_bound() const { return m_pipeline_bound; }
		void on_draw() { ++m_current.draws; }
		void on_texture_created();
		void on_program_created();
		void on_pipeline_created();
		void on_bind_group_created(bool transient);

		// Frames the renderer began (driver_interface::begin_frame): what a transient bind group's life is measured in.
		std::uint64_t frame_epoch() const { return m_epoch; }
		void on_begin_frame() { ++m_epoch; }

		// The end of a frame: no render pass may be open; per-frame state restarts.
		void on_present();

	private:
		std::string recent_trace(std::size_t count) const;

		limits m_budget;
		std::array<std::size_t, rule_count> m_counts{};
		std::size_t m_total = 0;
		std::vector<violation> m_violations;

		std::uint64_t m_frame = 0;
		std::uint64_t m_epoch = 0;
		frame_stats m_current;
		frame_stats m_last;
		bool m_pipeline_bound = false;
		bool m_pass_active = false;
		std::string m_pass_name;
		target_info m_pass_target;
		pass_formats m_pass_formats;
		std::size_t m_dropped_begins = 0;

		mutable std::mutex m_registry_mutex;
		std::unordered_map<const void*, object_kind> m_objects;

		std::vector<trace_entry> m_trace;
		std::size_t m_trace_head = 0;
		std::size_t m_trace_size = 0;
	};
}
