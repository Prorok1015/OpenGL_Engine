#include "edt_agent_job_result.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>
#include <algorithm>

namespace {
	void redact_string(boost::json::value& value, const cmd::log_read_filter* filter, const cmd::origin& from)
	{
		if (!value.is_string()) return;
		const boost::json::string& held = value.get_string();
		value = boost::json::string(edt::agent_shown_text(filter, std::string_view(held.data(), held.size()), from));
	}

	// `objects`: how many more objects deep the fields are looked for; an array is not a level.
	void redact_job_fields(boost::json::value& value, const cmd::log_read_filter* filter, const cmd::origin& from, int objects)
	{
		if (value.is_array()) {
			for (boost::json::value& item : value.get_array()) redact_job_fields(item, filter, from, objects);
			return;
		}
		if (!value.is_object() || objects <= 0) return;
		const bool text_is_message = value.get_object().contains(edt::k_agent_job_result_text_marker);
		for (boost::json::key_value_pair& member : value.get_object()) {
			const std::string_view name(member.key().data(), member.key().size());
			boost::json::value& held = member.value();
			if (text_is_message && name == "text") redact_string(held, filter, from);
			if (std::find(edt::k_agent_job_result_fields.begin(), edt::k_agent_job_result_fields.end(), name) != edt::k_agent_job_result_fields.end()) {
				redact_string(held, filter, from);
				if (held.is_array()) {
					for (boost::json::value& item : held.get_array()) redact_string(item, filter, from);
				}
			}
			if (held.is_structured()) redact_job_fields(held, filter, from, objects - 1);
		}
	}
}

std::string edt::agent_shown_text(const cmd::log_read_filter* filter, std::string_view text, const cmd::origin& from)
{
	return filter != nullptr && filter->redact ? filter->redact(text, from) : std::string(text);
}

boost::json::value edt::agent_job_result(const boost::json::value& result, const cmd::log_read_filter* filter, const cmd::origin& from)
{
	boost::json::value out = result;
	if (filter != nullptr && filter->redact) redact_job_fields(out, filter, from, k_agent_job_result_depth);
	return out;
}
