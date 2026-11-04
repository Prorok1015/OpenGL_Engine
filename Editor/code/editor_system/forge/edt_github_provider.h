#pragma once
#include "edt_forge_provider.h"
#include "edt_http_transport.h"
#include <string>

namespace edt::forge
{
	struct github_options
	{
		// GitHub Enterprise: the REST root (https://ghe.example.com/api/v3) and the web host it serves
		// (ghe.example.com). Both empty (the default): github.com only, through https://api.github.com. The transport's
		// host allowlist must name the API host as well.
		std::string api_base;
		std::string web_host;
	};

	// GitHub REST v3 (pull requests) on an injected http_transport.
	class github_provider final : public forge_provider
	{
	public:
		explicit github_provider(http_transport transport, github_options options = {});

		std::string_view name() const override { return "github"; }

		forge_result<pr_info> create_pr(const forge_remote& remote, const create_pr_request& request,
			const platform::secret_text& token, std::stop_token stop = {}) override;
		forge_result<pr_info> pr_status(const forge_remote& remote, std::int64_t number,
			const platform::secret_text& token, std::stop_token stop = {}) override;
		forge_result<std::vector<pr_info>> list_prs(const forge_remote& remote, const list_prs_filter& filter,
			const platform::secret_text& token, std::stop_token stop = {}) override;

	private:
		http_transport m_transport;
		github_options m_options;
	};
}
