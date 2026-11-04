#pragma once
#include "edt_forge_types.h"
#include <snk/platform/plt_secret.h>
#include <stop_token>
#include <string_view>
#include <vector>

namespace edt::forge
{
	// A code host's pull request API (GitHub today; GitLab and others implement the same calls). Every call blocks (it
	// talks to the network): run it from a job. The token is only ever read through secret_text::reveal() by the
	// transport.
	class forge_provider
	{
	public:
		virtual ~forge_provider() = default;

		virtual std::string_view name() const = 0;

		virtual forge_result<pr_info> create_pr(const forge_remote& remote, const create_pr_request& request,
			const platform::secret_text& token, std::stop_token stop = {}) = 0;
		virtual forge_result<pr_info> pr_status(const forge_remote& remote, std::int64_t number,
			const platform::secret_text& token, std::stop_token stop = {}) = 0;
		virtual forge_result<std::vector<pr_info>> list_prs(const forge_remote& remote, const list_prs_filter& filter,
			const platform::secret_text& token, std::stop_token stop = {}) = 0;
	};
}
