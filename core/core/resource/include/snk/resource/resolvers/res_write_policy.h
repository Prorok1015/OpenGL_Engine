#pragma once
#include <snk/common/snk_api.h>
#include <string>
#include <string_view>
#include <vector>

namespace res
{
	// Which paths a resolver may store into. A pattern reads `protocol://glob`; the glob is matched against the
	// normalized path under the protocol, segment by segment (split at '/'):
	//   *   inside a segment: any characters but '/'      ("settings/*" does not reach "settings/a/b")
	//   **  as a whole segment: zero or more segments     ("settings/**" reaches "settings/a/b")
	// Case matters. Patterns of another protocol are skipped, so one list can serve every resolver. A pattern that is
	// malformed (no protocol, an empty glob or segment, '**' mixed with other characters) is ignored with a warning,
	// i.e. it allows nothing.
	class SNK_API write_policy
	{
	public:
		static write_policy allow_all();
		static write_policy read_only();

		write_policy(std::string_view protocol, const std::vector<std::string>& patterns);

		// `relative_path` is already normalized (safe_path / lexically_normal): no '..', '.' or doubled '/'.
		bool allows(std::string_view relative_path) const;

	private:
		write_policy() = default;

		bool m_allow_all = false;
		std::vector<std::vector<std::string>> m_patterns;
	};
}
