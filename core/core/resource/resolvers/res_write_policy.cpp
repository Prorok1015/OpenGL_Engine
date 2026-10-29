#include <snk/resource/resolvers/res_write_policy.h>
#include <snk/common/logger/engine_log.h>
#include <cctype>
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

	std::string lowered(std::string_view text)
	{
		std::string out(text);
		for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return out;
	}

	bool glob_matches(const std::vector<std::string>& glob, std::size_t g, const std::vector<std::string>& path, std::size_t p)
	{
		if (g == glob.size()) return p == path.size();
		if (glob[g] == ANY_SEGMENTS) {
			// A trailing '**' stands for what is inside the folder before it, so it needs at least one segment
			// ("settings/**" does not name "settings" itself); elsewhere it may match none ("**/x" reaches "x").
			const bool trailing = g + 1 == glob.size();
			for (std::size_t skip = trailing ? p + 1 : p; skip <= path.size(); ++skip) {
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

res::write_policy::write_policy(std::string_view protocol, const std::vector<std::string>& patterns, bool case_sensitive)
	: m_case_sensitive(case_sensitive)
{
	constexpr std::string_view separator = "://";
	for (const std::string& listed : patterns) {
		const bool deny = !listed.empty() && listed.front() == '!';
		const std::string pattern = deny ? listed.substr(1) : listed;
		const std::size_t split = pattern.find(separator);
		if (split == std::string::npos) {
			egLOG_WARN("resource/write", "write pattern '{}' ignored: it must read protocol://glob", pattern);
			continue;
		}
		const std::string_view scheme = std::string_view{ pattern }.substr(0, split);
		if (scheme != protocol) {
			if (lowered(scheme) == lowered(protocol)) {
				egLOG_WARN("resource/write", "write pattern '{}' ignored: the scheme is case-sensitive, write '{}://'", pattern, protocol);
			}
			continue;
		}
		const std::string glob_text = case_sensitive ? pattern.substr(split + separator.size()) : lowered(pattern.substr(split + separator.size()));
		auto segments = split_segments(glob_text);
		if (!is_valid_glob(segments)) {
			egLOG_WARN("resource/write", "write pattern '{}' ignored: empty segment, or '**' mixed with other characters", pattern);
			continue;
		}
		(deny ? m_denied : m_patterns).push_back(std::move(segments));
	}
}

bool res::write_policy::allows(std::string_view relative_path) const
{
	if (m_allow_all) return true;
	if (relative_path.empty()) return false;

	const auto path = split_segments(m_case_sensitive ? std::string{ relative_path } : lowered(relative_path));
	for (const auto& glob : m_denied) {
		if (glob_matches(glob, 0, path, 0)) return false;
	}
	for (const auto& glob : m_patterns) {
		if (glob_matches(glob, 0, path, 0)) return true;
	}
	return false;
}
