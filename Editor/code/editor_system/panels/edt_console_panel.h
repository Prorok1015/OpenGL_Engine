#pragma once
#include "edt_panel_base.h"
#include <string>
#include <vector>
#include <cstddef>

namespace edt
{
	enum class log_level { info, warning, error };

	struct log_entry
	{
		log_level level;
		std::string message;
	};

	class console_panel : public panel_base
	{
	public:
		console_panel();

		void add_log(log_level level, std::string message);
		void clear();

		std::size_t get_log_count() const { return m_logs.size(); }
		std::size_t get_error_count() const;
		std::size_t get_warning_count() const;

	protected:
		void on_render() override;

	private:
		std::vector<log_entry> m_logs;
		bool m_show_info = true;
		bool m_show_warnings = true;
		bool m_show_errors = true;
		bool m_auto_scroll = true;
	};
}
