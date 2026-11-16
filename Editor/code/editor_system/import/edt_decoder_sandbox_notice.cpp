#include "edt_decoder_sandbox_notice.h"
#include <utility>

edt::decoder_sandbox_notice::decoder_sandbox_notice(problem_list& problems, sandbox_mode mode, decoder_sandbox_status expected)
	: m_problems(problems)
	, m_expected(std::move(expected))
{
	if (mode == sandbox_mode::off) publish(m_expected);
}

void edt::decoder_sandbox_notice::seen(const decoder_sandbox_status& status)
{
	if (!status.reported) return;
	if (m_seen && decoder_sandbox_gap(status) <= decoder_sandbox_gap(*m_seen)) return;
	m_seen = status;
	if (decoder_sandbox_gap(status) > 0) publish(status);
}

const edt::decoder_sandbox_status& edt::decoder_sandbox_notice::current() const
{
	return m_seen ? *m_seen : m_expected;
}

void edt::decoder_sandbox_notice::publish(const decoder_sandbox_status& status)
{
	problem row;
	row.severity = problem_severity::warning;
	row.message = decoder_sandbox_warning(status);
	if (row.message.empty()) return;
	m_problems.set(source, { std::move(row) });
}
