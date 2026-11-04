#include <boost/test/unit_test.hpp>
#include <snk/platform/plt_secret.h>
#include <cstdlib>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

	using platform::secret_key;
	using platform::secret_result;
	using platform::secret_status;
	using platform::secret_text;

	const std::string canary = "ghp_CANARY_0123456789_do_not_leak";

	// Answers every call with `status` and records the calls; stores values like a real backend when it is `ok`.
	class fake_backend final : public platform::secret_backend
	{
	public:
		explicit fake_backend(secret_status status, std::string message = "daemon is not running")
			: m_status(status), m_message(std::move(message)) {}

		secret_result store(const secret_key& key, const secret_text& value) override
		{
			m_calls.push_back("store");
			if (m_status == secret_status::ok) {
				m_values[key.service + "/" + key.account] = value.reveal();
			}
			return { m_status, m_status == secret_status::ok ? std::string() : m_message };
		}

		secret_result load(const secret_key& key, secret_text& out) override
		{
			m_calls.push_back("load");
			const auto it = m_values.find(key.service + "/" + key.account);
			if (m_status == secret_status::ok && it != m_values.end()) {
				out = secret_text(std::string(it->second));
				return { secret_status::ok, {} };
			}
			return { m_status == secret_status::ok ? secret_status::not_found : m_status, m_message };
		}

		secret_result erase(const secret_key& key) override
		{
			m_calls.push_back("erase");
			const bool had = m_values.erase(key.service + "/" + key.account) > 0;
			return { m_status == secret_status::ok ? (had ? secret_status::ok : secret_status::not_found) : m_status, m_message };
		}

		const char* name() const override { return "fake"; }

		void set_status(secret_status status) { m_status = status; }

		std::vector<std::string> m_calls;

	private:
		secret_status m_status;
		std::string m_message;
		std::map<std::string, std::string> m_values;
	};

	const secret_key key{ "snk-forge", "token" };

	// Hands the fake to the store and keeps a pointer to it for the call log.
	struct harness
	{
		explicit harness(secret_status status) : harness(std::make_unique<fake_backend>(status)) {}

		fake_backend* fake;
		platform::secret_store store;

	private:
		explicit harness(std::unique_ptr<fake_backend> backend) : fake(backend.get()), store(std::move(backend)) {}
	};
}

BOOST_AUTO_TEST_SUITE(PltSecretTests)

BOOST_AUTO_TEST_CASE(MemoryBackend_RoundTrip)
{
	platform::memory_secret_backend backend;
	secret_text out;
	BOOST_TEST((backend.load(key, out).status == secret_status::not_found));
	BOOST_TEST((backend.store(key, secret_text(std::string(canary))).status == secret_status::ok));
	BOOST_TEST((backend.load(key, out).status == secret_status::ok));
	BOOST_TEST(out.reveal() == canary);
	BOOST_TEST((backend.erase(key).status == secret_status::ok));
	BOOST_TEST((backend.erase(key).status == secret_status::not_found));
}

BOOST_AUTO_TEST_CASE(SecretText_FormatsAsRedacted)
{
	const secret_text token{std::string(canary)};
	const std::string text = std::format("token={}", token);
	BOOST_TEST(text == "token=<redacted>");
	BOOST_TEST(text.find(canary) == std::string::npos);
	BOOST_TEST(token.to_string_redacted() == "<redacted>");
}

BOOST_AUTO_TEST_CASE(SecretText_MoveLeavesSourceEmpty)
{
	secret_text a{std::string(canary)};
	secret_text b(std::move(a));
	BOOST_TEST(b.reveal() == canary);
	BOOST_TEST(a.empty());
	b.wipe();
	BOOST_TEST(b.empty());
}

BOOST_AUTO_TEST_CASE(Store_FallsBackToRamWhenPrimaryUnavailable)
{
	for (const secret_status status : { secret_status::unavailable, secret_status::failed }) {
		harness h(status);
		const secret_result stored = h.store.store(key, secret_text(std::string(canary)));
		BOOST_TEST((stored.status == secret_status::ok));
		BOOST_TEST(!h.store.persisted(key));
		BOOST_TEST(h.store.last_backend_name() == "memory");
		BOOST_TEST(stored.message.find(canary) == std::string::npos);

		secret_text out;
		BOOST_TEST((h.store.load(key, out).status == secret_status::ok));
		BOOST_TEST(out.reveal() == canary);
		BOOST_TEST(h.fake->m_calls.size() == 1u);
	}
}

BOOST_AUTO_TEST_CASE(Store_UsesPrimaryWhenItWorks)
{
	harness h(secret_status::ok);
	BOOST_TEST((h.store.store(key, secret_text(std::string(canary))).status == secret_status::ok));
	BOOST_TEST(h.store.persisted(key));
	BOOST_TEST(h.store.last_backend_name() == "fake");
	secret_text out;
	BOOST_TEST((h.store.load(key, out).status == secret_status::ok));
	BOOST_TEST(out.reveal() == canary);
}

BOOST_AUTO_TEST_CASE(Store_WithoutPrimaryKeepsRamOnly)
{
	platform::secret_store store(nullptr);
	BOOST_TEST((store.store(key, secret_text(std::string(canary))).status == secret_status::ok));
	BOOST_TEST(!store.persisted(key));
	secret_text out;
	BOOST_TEST((store.load(key, out).status == secret_status::ok));
	BOOST_TEST((store.erase(key).status == secret_status::ok));
	BOOST_TEST((store.load(key, out).status == secret_status::not_found));
}

BOOST_AUTO_TEST_CASE(Erase_RemovesFromBothPlaces)
{
	harness h(secret_status::unavailable);
	h.store.store(key, secret_text(std::string(canary)));
	// The primary could not be asked, so a persisted copy may remain: not a success.
	const secret_result erased = h.store.erase(key);
	BOOST_TEST((erased.status == secret_status::unavailable));
	BOOST_TEST(erased.message.find("may remain") != std::string::npos);
	BOOST_TEST(erased.message.find(canary) == std::string::npos);
	secret_text out;
	BOOST_TEST((h.store.load(key, out).status != secret_status::ok));
	BOOST_TEST(out.empty());

	harness working_h(secret_status::ok);
	platform::secret_store& working = working_h.store;
	working.store(key, secret_text(std::string(canary)));
	BOOST_TEST((working.erase(key).status == secret_status::ok));
	BOOST_TEST(!working.persisted(key));
	BOOST_TEST((working.load(key, out).status == secret_status::not_found));
}

BOOST_AUTO_TEST_CASE(Load_RamEntryWinsOverStalePrimaryValue)
{
	harness h(secret_status::ok);
	h.store.store(key, secret_text(std::string("old_token")));

	// The keyring locks; the new token can only go to RAM.
	h.fake->set_status(secret_status::unavailable);
	BOOST_TEST((h.store.store(key, secret_text(std::string("new_token"))).status == secret_status::ok));
	BOOST_TEST(!h.store.persisted(key));

	// The keyring unlocks and still holds the old token: the newer RAM value must be returned.
	h.fake->set_status(secret_status::ok);
	secret_text out;
	BOOST_TEST((h.store.load(key, out).status == secret_status::ok));
	BOOST_TEST(out.reveal() == "new_token");

	// A later successful store to the primary replaces the RAM entry.
	h.store.store(key, secret_text(std::string("newest_token")));
	BOOST_TEST((h.store.load(key, out).status == secret_status::ok));
	BOOST_TEST(out.reveal() == "newest_token");
}

BOOST_AUTO_TEST_CASE(Erase_ReportsPrimaryFailureEvenWhenRamWasCleared)
{
	for (const secret_status status : { secret_status::unavailable, secret_status::failed }) {
		harness h(secret_status::ok);
		h.store.store(key, secret_text(std::string(canary)));
		h.fake->set_status(status);
		const secret_result erased = h.store.erase(key);
		BOOST_TEST((erased.status == status));
		BOOST_TEST(erased.message.find("may remain") != std::string::npos);
	}
}

BOOST_AUTO_TEST_CASE(OsTarget_HasFixedPrefixAndIsUnambiguous)
{
	platform::detail::os_target target;
	BOOST_TEST((platform::detail::make_os_target({ "forge", "github.com" }, target).status == secret_status::ok));
	BOOST_TEST(target.name == "snk-engine:forge:github.com");
	BOOST_TEST(target.service == "snk-engine:forge");
	BOOST_TEST(target.account == "snk-engine:github.com");
	BOOST_TEST(target.name.starts_with(platform::detail::os_name_prefix));
}

BOOST_AUTO_TEST_CASE(OsTarget_RejectsAmbiguousOrForeignKeys)
{
	const std::string too_long(256, 'a');
	const secret_key bad_keys[] = {
		{ "", "github.com" },
		{ "forge", "" },
		{ "forge", too_long },
		{ too_long, "github.com" },
		{ "forge", "git:https://github.com" },
		{ "forge", "a/b" },
		{ "forge/x", "b" },
		{ "for:ge", "b" },
		{ "forge", "tab\there" },
		{ "forge", std::string("nul\0byte", 8) },
		{ "forge", "line\nbreak" },
		{ "forge", "del\x7f" },
	};
	for (const secret_key& bad : bad_keys) {
		platform::detail::os_target target;
		const secret_result result = platform::detail::make_os_target(bad, target);
		BOOST_TEST((result.status == secret_status::failed));
		BOOST_TEST(!result.message.empty());
		BOOST_TEST((bad.account.size() < 4 || result.message.find(bad.account) == std::string::npos));
	}
	platform::detail::os_target longest;
	BOOST_TEST((platform::detail::make_os_target({ std::string(255, 's'), std::string(255, 'a') }, longest).status == secret_status::ok));
}

BOOST_AUTO_TEST_CASE(Messages_NeverContainTheSecret)
{
	for (const secret_status status : { secret_status::not_found, secret_status::unavailable, secret_status::failed }) {
		harness h(status);
		secret_text out;
		const secret_result results[] = {
			h.store.store(key, secret_text(std::string(canary))),
			h.store.load(key, out),
			h.store.erase(key),
		};
		for (const secret_result& result : results) {
			BOOST_TEST(result.message.find(canary) == std::string::npos);
		}
		if (results[1].status != secret_status::ok) BOOST_TEST(out.empty());
	}
}

// The real store is only read, and only for a key that cannot exist: CI machines may have no keychain or daemon.
BOOST_AUTO_TEST_CASE(OsBackend_LoadOfUnknownKeyNeverSucceeds)
{
#if !defined(_WIN32) && !defined(__APPLE__)
	// libsecret would try to autolaunch a D-Bus session bus, which can hang or spawn processes on CI.
	const char* bus = std::getenv("DBUS_SESSION_BUS_ADDRESS");
	if (!bus || !*bus) {
		BOOST_TEST_MESSAGE("DBUS_SESSION_BUS_ADDRESS is not set: skipping the libsecret backend");
		return;
	}
#endif
	const auto backend = platform::make_os_secret_backend();
	BOOST_REQUIRE(backend != nullptr);
	secret_text out;
	const secret_result result = backend->load({ "snk-test-never-stored", "no-such-account-7f3a91c2" }, out);
	BOOST_TEST((result.status != secret_status::ok));
	BOOST_TEST(out.empty());
}

BOOST_AUTO_TEST_SUITE_END()
