#include <boost/test/unit_test.hpp>
#include "edt_agent_policy.h"
#include "edt_forge_account_model.h"
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/platform/plt_secret.h>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The Git Account window's model (#458 PR-4): states derived without reading a token out, the token wiped from the caller's
// buffer before any job starts, only allowed hosts, messages without the token. A fake secret backend and a fake job runner;
// nothing here touches the OS store.
namespace {

namespace json = boost::json;
using platform::secret_status;

const std::string canary = "ghp_CANARYCANARYCANARY0123456789abcdef";

// A backend with a switchable status; its store() message echoes the secret, the way a careless backend might.
class fake_backend final : public platform::secret_backend
{
public:
	explicit fake_backend(secret_status status) : m_status(status) {}

	platform::secret_result store(const platform::secret_key& key, const platform::secret_text& value) override
	{
		if (m_status == secret_status::ok) m_values[key.account] = value.reveal();
		return answer("store " + value.reveal());
	}

	platform::secret_result load(const platform::secret_key& key, platform::secret_text& out) override
	{
		const auto it = m_values.find(key.account);
		if (m_status == secret_status::ok && it != m_values.end()) {
			out = platform::secret_text(std::string(it->second));
			return { secret_status::ok, {} };
		}
		return { m_status == secret_status::ok ? secret_status::not_found : m_status, "daemon is not running" };
	}

	platform::secret_result erase(const platform::secret_key& key) override
	{
		const bool had = m_values.erase(key.account) > 0;
		if (m_status != secret_status::ok) return { m_status, "daemon is not running" };
		return { had ? secret_status::ok : secret_status::not_found, {} };
	}

	const char* name() const override { return "fake"; }
	void set_status(secret_status status) { m_status = status; }

private:
	platform::secret_result answer(const std::string& text) const
	{
		return { m_status, m_status == secret_status::ok ? std::string{} : "keychain is locked (" + text + ")" };
	}

	secret_status m_status;
	std::map<std::string, std::string> m_values;
};

struct account_fixture
{
	struct job
	{
		std::uint64_t id = 0;
		std::string label;
		std::function<json::value()> work;
	};

	explicit account_fixture(secret_status primary = secret_status::ok, std::vector<std::string> hosts = { "github.com" })
	{
		auto backend = std::make_unique<fake_backend>(primary);
		fake = backend.get();
		secrets = std::make_shared<platform::secret_store>(std::move(backend));
		edt::forge_account_model::ports ports;
		ports.secrets = secrets;
		ports.start_job = [this](std::string label, std::function<json::value()> work) -> std::optional<std::uint64_t> {
			if (refuse_jobs) return std::nullopt;
			if (on_start) on_start();
			jobs.push_back({ next_job, std::move(label), std::move(work) });
			return next_job++;
		};
		model = std::make_unique<edt::forge_account_model>(events, std::move(hosts), std::move(ports));
	}

	// Runs the queued jobs and delivers their results, as job_service would.
	void run_jobs()
	{
		std::vector<job> queued;
		queued.swap(jobs);
		for (job& entry : queued) {
			labels.push_back(entry.label);
			json::value result = entry.work();
			results.push_back(json::serialize(result));
			events.job_finished.emit(entry.id, edt::job_status::done, std::string{}, std::optional<json::value>{ std::move(result) });
		}
	}

	std::string status_of(std::string_view host) const
	{
		for (const auto& row : model->rows()) {
			if (row.host == host) return std::string(edt::forge_token_state_text(row.state));
		}
		return "no such row";
	}

	// Everything the model lets a view see.
	std::string visible_text() const
	{
		std::string out = model->message();
		for (const auto& row : model->rows()) out += row.host + std::string(edt::forge_token_state_text(row.state));
		for (const std::string& label : labels) out += label;
		for (const std::string& result : results) out += result;
		return out;
	}

	std::optional<platform::secret_text> stored(const std::string& host)
	{
		platform::secret_text out;
		if (secrets->load({ "forge", host }, out).status != secret_status::ok) return std::nullopt;
		return std::optional<platform::secret_text>(std::move(out));
	}

	edt::editor_events events;
	fake_backend* fake = nullptr;
	std::shared_ptr<platform::secret_store> secrets;
	std::unique_ptr<edt::forge_account_model> model;
	std::vector<job> jobs;
	std::vector<std::string> labels;
	std::vector<std::string> results;
	std::uint64_t next_job = 1;
	bool refuse_jobs = false;
	std::function<void()> on_start;
};

bool all_zero(const char* data, std::size_t size)
{
	return std::all_of(data, data + size, [](char c) { return c == '\0'; });
}

}

BOOST_AUTO_TEST_SUITE(EdtForgeAccountModel)

BOOST_AUTO_TEST_CASE(TheRows_AreTheAllowedHosts_LowercaseOnce)
{
	account_fixture f(secret_status::ok, { "GitHub.com", "github.com", "git.example.org", "" });
	BOOST_REQUIRE(f.model->rows().size() == 2u);
	BOOST_TEST(f.model->rows()[0].host == "github.com");
	BOOST_TEST(f.model->rows()[1].host == "git.example.org");
	BOOST_TEST(f.model->allows("GITHUB.COM"));
	BOOST_TEST(!f.model->allows("evil.example"));
}

BOOST_AUTO_TEST_CASE(AClosedWindow_AsksTheStoreForNothing)
{
	account_fixture f;
	f.model->set_visible(false);
	BOOST_TEST(f.jobs.empty());
	f.model->set_visible(true);
	BOOST_TEST(f.jobs.size() == 1u);
	f.model->set_visible(false);
	f.model->set_visible(true);
	BOOST_TEST(f.jobs.size() == 1u);
}

BOOST_AUTO_TEST_CASE(NoToken_IsNotSet)
{
	account_fixture f;
	f.model->set_visible(true);
	BOOST_TEST(f.status_of("github.com") == "checking...");
	f.run_jobs();
	BOOST_TEST(f.status_of("github.com") == "not set");
}

BOOST_AUTO_TEST_CASE(ATokenInTheKeychain_IsSetInTheKeychain_AndNeverRead)
{
	account_fixture f;
	BOOST_REQUIRE((f.secrets->store({ "forge", "github.com" }, platform::secret_text{ std::string(canary) }).status == secret_status::ok));
	f.model->set_visible(true);
	f.run_jobs();
	BOOST_TEST(f.status_of("github.com") == "set (OS keychain)");
	BOOST_TEST(f.visible_text().find(canary) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(ATokenKeptInMemory_IsSetForThisSessionOnly)
{
	account_fixture f(secret_status::unavailable);
	BOOST_REQUIRE((f.secrets->store({ "forge", "github.com" }, platform::secret_text{ std::string(canary) }).status == secret_status::ok));
	f.model->set_visible(true);
	f.run_jobs();
	BOOST_TEST(f.status_of("github.com") == "set (this session only)");
}

BOOST_AUTO_TEST_CASE(AnUnreachableKeychain_IsNotClaimedToBeEmpty)
{
	account_fixture f(secret_status::unavailable);
	f.model->set_visible(true);
	f.run_jobs();
	BOOST_TEST(f.status_of("github.com").starts_with("unknown"));
	BOOST_TEST(f.visible_text().find(canary) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(Save_StoresTheToken_AndSaysItIsInTheKeychain)
{
	account_fixture f;
	f.model->set_visible(true);
	f.run_jobs();
	BOOST_REQUIRE(f.model->save("github.com", platform::secret_text{ std::string(canary) }));
	BOOST_TEST(f.model->busy());
	f.run_jobs();
	BOOST_TEST(!f.model->busy());
	BOOST_TEST(f.status_of("github.com") == "set (OS keychain)");
	BOOST_TEST(!f.model->message_is_error());
	const auto token = f.stored("github.com");
	BOOST_REQUIRE(token.has_value());
	BOOST_TEST(token->reveal() == canary);
	BOOST_TEST(f.visible_text().find(canary) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(Save_WithTheKeychainDown_KeepsItForTheSession_AndSaysSo)
{
	account_fixture f(secret_status::unavailable);
	BOOST_REQUIRE(f.model->save("github.com", platform::secret_text{ std::string(canary) }));
	f.run_jobs();
	BOOST_TEST(f.status_of("github.com") == "set (this session only)");
	BOOST_TEST(f.model->message().find("this session only") != std::string::npos);
	// The backend echoed the token in its own message; the model cut it out.
	BOOST_TEST(f.visible_text().find(canary) == std::string::npos);
	BOOST_TEST(f.visible_text().find(canary.substr(0, 20)) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(SaveFromABuffer_WipesTheBuffer_BeforeAnyJobStarts)
{
	account_fixture f;
	std::array<char, 64> buffer{};
	std::copy(canary.begin(), canary.end(), buffer.begin());
	buffer[canary.size()] = '\n';
	bool wiped_when_started = false;
	f.on_start = [&] { wiped_when_started = all_zero(buffer.data(), buffer.size()); };

	BOOST_REQUIRE(f.model->save_from_buffer("github.com", buffer.data(), buffer.size()));

	BOOST_TEST(wiped_when_started);
	BOOST_TEST(all_zero(buffer.data(), buffer.size()));
	f.run_jobs();
	const auto token = f.stored("github.com");
	BOOST_REQUIRE(token.has_value());
	BOOST_TEST(token->reveal() == canary);
}

BOOST_AUTO_TEST_CASE(SaveFromABuffer_WipesItWhateverTheOutcome)
{
	account_fixture f;
	std::array<char, 64> refused{};
	std::copy(canary.begin(), canary.end(), refused.begin());
	BOOST_TEST(!f.model->save_from_buffer("evil.example", refused.data(), refused.size()));
	BOOST_TEST(all_zero(refused.data(), refused.size()));

	f.refuse_jobs = true;
	std::array<char, 64> unstarted{};
	std::copy(canary.begin(), canary.end(), unstarted.begin());
	BOOST_TEST(!f.model->save_from_buffer("github.com", unstarted.data(), unstarted.size()));
	BOOST_TEST(all_zero(unstarted.data(), unstarted.size()));
	BOOST_TEST(!f.stored("github.com").has_value());
	BOOST_TEST(f.visible_text().find(canary) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(ADisallowedHost_IsRefused_AndNothingIsStored)
{
	account_fixture f;
	BOOST_TEST(!f.model->save("evil.example", platform::secret_text{ std::string(canary) }));
	BOOST_TEST(f.model->message_is_error());
	BOOST_TEST(f.jobs.empty());
	BOOST_TEST(!f.stored("evil.example").has_value());
	BOOST_TEST(!f.model->remove("evil.example"));
	BOOST_TEST(f.jobs.empty());
	BOOST_TEST(f.visible_text().find(canary) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(NotAToken_IsRefused_BeforeAnyJob)
{
	account_fixture f;
	BOOST_TEST(!f.model->save("github.com", platform::secret_text{}));
	BOOST_TEST(!f.model->save("github.com", platform::secret_text{ "has a space" }));
	BOOST_TEST(!f.model->save("github.com", platform::secret_text{ std::string("line\nbreak") }));
	BOOST_TEST(!f.model->save("github.com", platform::secret_text{ std::string(edt::forge_account_model::k_max_token + 1, 'a') }));
	BOOST_TEST(f.jobs.empty());
	BOOST_TEST(f.model->message_is_error());
}

BOOST_AUTO_TEST_CASE(AHostThatIsBusy_TakesNoSecondJob)
{
	account_fixture f;
	BOOST_REQUIRE(f.model->save("github.com", platform::secret_text{ std::string(canary) }));
	BOOST_TEST(!f.model->save("github.com", platform::secret_text{ "other" }));
	BOOST_TEST(!f.model->remove("github.com"));
	BOOST_TEST(f.jobs.size() == 1u);
}

BOOST_AUTO_TEST_CASE(Remove_ForgetsTheToken)
{
	account_fixture f;
	BOOST_REQUIRE(f.model->save("github.com", platform::secret_text{ std::string(canary) }));
	f.run_jobs();
	BOOST_REQUIRE(f.model->remove("github.com"));
	f.run_jobs();
	BOOST_TEST(f.status_of("github.com") == "not set");
	BOOST_TEST(!f.model->message_is_error());
	BOOST_TEST(!f.stored("github.com").has_value());
}

BOOST_AUTO_TEST_CASE(Remove_WithTheKeychainDown_SaysACopyMayRemain)
{
	account_fixture f;
	BOOST_REQUIRE(f.model->save("github.com", platform::secret_text{ std::string(canary) }));
	f.run_jobs();
	f.fake->set_status(secret_status::unavailable);
	BOOST_REQUIRE(f.model->remove("github.com"));
	f.run_jobs();
	BOOST_TEST(f.model->message_is_error());
	BOOST_TEST(f.model->message().find("saved copy may remain") != std::string::npos);
	BOOST_TEST(f.status_of("github.com").starts_with("unknown"));
	BOOST_TEST(f.visible_text().find(canary) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(AJobThatFailed_IsTold_WithoutTheToken)
{
	account_fixture f;
	BOOST_REQUIRE(f.model->save("github.com", platform::secret_text{ std::string(canary) }));
	f.events.job_finished.emit(f.jobs.front().id, edt::job_status::failed, "boom", std::nullopt);
	BOOST_TEST(f.model->message_is_error());
	BOOST_TEST(!f.model->busy());
	BOOST_TEST(f.model->message().find(canary) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheModel_HasNoWayToReadATokenBack)
{
	// The whole public surface: rows (host, state, busy), texts and the two operations. Every text it ever produced is free of the token.
	account_fixture f(secret_status::unavailable);
	f.model->set_visible(true);
	f.run_jobs();
	BOOST_REQUIRE(f.model->save("github.com", platform::secret_text{ std::string(canary) }));
	f.run_jobs();
	BOOST_REQUIRE(f.model->remove("github.com"));
	f.run_jobs();
	BOOST_TEST(f.visible_text().find(canary) == std::string::npos);
	for (const std::string& label : f.labels) BOOST_TEST(label.find(canary) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheAccountAction_IsForAPersonOnly)
{
	edt::action_registry actions;
	int opened = 0;
	const edt::registration registered = edt::register_forge_account_action(actions, [&opened] { ++opened; });
	const cmd::command_desc* desc = actions.find("forge.account.open");
	BOOST_REQUIRE(desc != nullptr);
	BOOST_TEST((desc->category == cmd::agent_category::not_for_agents));
	BOOST_TEST(!desc->expose_as_tool);
	for (const cmd::command_desc* tool : actions.tools()) BOOST_TEST(tool->id != "forge.account.open");

	// The policy gate asks this for an agent's call: whatever the files say, it is a deny.
	const edt::policy_layer none{};
	BOOST_TEST(((edt::effective_mode(*desc, none, none).mode) == cmd::policy_mode::deny));
	edt::policy_layer allowing;
	allowing.categories[cmd::agent_category::not_for_agents] = cmd::policy_mode::allow;
	allowing.commands["forge.account.open"] = cmd::policy_mode::allow;
	BOOST_TEST(((edt::effective_mode(*desc, allowing, allowing).mode) == cmd::policy_mode::deny));

	BOOST_TEST(actions.invoke("forge.account.open", json::object{}).ok);
	BOOST_TEST(opened == 1);
}

BOOST_AUTO_TEST_CASE(NoCommandOfTheAccountAction_NamesATokenOrASecret)
{
	edt::action_registry actions;
	const edt::registration registered = edt::register_forge_account_action(actions, [] {});
	for (const cmd::command_desc* desc : actions.commands().list()) {
		for (const std::string& text : { desc->id, json::serialize(desc->args_schema), json::serialize(desc->result_schema), desc->summary }) {
			BOOST_TEST(text.find("token") == std::string::npos, desc->id);
			BOOST_TEST(text.find("secret") == std::string::npos, desc->id);
		}
	}
}

BOOST_AUTO_TEST_SUITE_END()
