#pragma once

#include <cstddef>
#include <cstdint>
#include <snk/common/logger/engine_log.h>
#include <snk/common/snk_api.h>

// Base only (include_base/, not installed in the SDK): a game module installs a log listener through
// gs::module_context::add_log_sink, which ties it to the module's layer (MODULE_LAYER.md).

namespace engine
{
	SNK_API void remove_log_sink(std::uint64_t id);

	// Move-only RAII handle of one sink: the sink is removed when the handle is destroyed or reset. After reset()
	// returns, the sink is not running and is never called again (a call in flight on another thread is waited
	// for). Harmless after shutdown_logger().
	class log_sink_handle
	{
	public:
		log_sink_handle() = default;
		explicit log_sink_handle(std::uint64_t id) noexcept : m_id(id) {}
		log_sink_handle(const log_sink_handle&) = delete;
		log_sink_handle& operator=(const log_sink_handle&) = delete;
		log_sink_handle(log_sink_handle&& other) noexcept : m_id(other.m_id) { other.m_id = 0; }
		log_sink_handle& operator=(log_sink_handle&& other) noexcept
		{
			if (this != &other) {
				reset();
				m_id = other.m_id;
				other.m_id = 0;
			}
			return *this;
		}
		~log_sink_handle() { reset(); }

		bool active() const noexcept { return m_id != 0; }
		void reset()
		{
			if (m_id != 0) {
				remove_log_sink(m_id);
				m_id = 0;
			}
		}

	private:
		std::uint64_t m_id = 0;
	};

	// Any number of sinks may be installed at once; they are called in the order they were added.
	// A null callback adds nothing (the handle is inactive).
	[[nodiscard]] SNK_API log_sink_handle add_log_sink(log_callback_t callback);
	// Sinks installed now. For tests and diagnostics (a module must leave as many as it found).
	[[nodiscard]] SNK_API std::size_t log_sink_count();
}
