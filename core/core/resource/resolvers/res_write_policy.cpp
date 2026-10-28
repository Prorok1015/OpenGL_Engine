#include <snk/resource/resolvers/res_write_policy.h>
#include <snk/common/logger/engine_log.h>
#include <utility>

namespace {

	constexpr std::string_view ANY_SEGMENTS = "**";

	std::vector<std::string> split_segments(std::string_view text)
	{
		std::vector<std::string> out;
		std::size_t begin = 0;
		while (true) {
			const std::size_t end = text.find('/', begin);
			out.emplace_back(text.substr(begin, end == std::string_view::npos ? end : end - begin));
			if (end == std::string_view::npos) break;
			begin = end + 1;
		}
		return out;
	}

	bool is_valid_glob(const std::vector<std::string>& segments)
	{
		for (const std::string& segment : segments) {
			if (segment.empty()) return false;
			if (segment != ANY_SEGMENTS && segment.find(ANY_SEGMENTS) != std::string::npos) return false;
		}
		return true;
	}

	bool segment_matches(std::string_view glob, std::string_view text)
	{
		std::size_t g = 0, t = 0;
		std::size_t star = std::string_view::npos, resume = 0;
		while (t < text.size()) {
			if (g < glob.size() && glob[g] == '*') {
				star = g++;
				resume = t;
			} else if (g < glob.size() && glob[g] == text[t]) {
				++g;
				++t;
			} else if (star != std::string_view::npos) {
				g = star + 1;
				t = ++resume;
			} else {
				return false;
			}
		}
		while (g < glob.size() && glob[g] == '*') ++g;
		return g == glob.size();
	}

	bool glob_matches(const std::vector<std::string>& glob, std::size_t g, const std::vector<std::string>& path, std::size_t p)
	{
		if (g == glob.size()) return p == path.size();
		if (glob[g] == ANY_SEGMENTS) {
			for (std::size_t skip = p; skip <= path.size(); ++skip) {
				if (glob_matches(glob, g + 1, path, skip)) return true;
			}
			return false;
		}
		return p < path.size() && segment_matches(glob[g], path[p]) && glob_matches(glob, g + 1, path, p + 1);
	}

}

res::write_policy res::write_policy::allow_all()
{
	write_policy policy;
	policy.m_allow_all = true;
	return policy;
}

res::write_policy res::write_policy::read_only()
{
	return write_policy{};
}

res::write_policy::write_policy(std::string_view protocol, const std::vector<std::string>& patterns)
{
	constexpr std::string_view separator = "://";
	for (const std::string& pattern : patterns) {
		const std::size_t split = pattern.find(separator);
		if (split == std::string::npos) {
			egLOG_WARN("resource/write", "write pattern '{}' ignored: it must read protocol://glob", pattern);
			continue;
		}
		if (std::string_view{ pattern }.substr(0, split) != protocol) {
			continue;
		}
		auto segments = split_segments(std::string_view{ pattern }.substr(split + separator.size()));
		if (!is_valid_glob(segments)) {
			egLOG_WARN("resource/write", "write pattern '{}' ignored: empty segment, or '**' mixed with other characters", pattern);
			continue;
		}
		m_patterns.push_back(std::move(segments));
	}
}

bool res::write_policy::allows(std::string_view relative_path) const
{
	if (m_allow_all) return true;
	if (relative_path.empty()) return false;

	const auto path = split_segments(relative_path);
	for (const auto& glob : m_patterns) {
		if (glob_matches(glob, 0, path, 0)) return true;
	}
	return false;
}
