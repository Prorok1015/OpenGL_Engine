#pragma once
#include <cstddef>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <snk/common/snk_api.h>

namespace platform
{
	// A secret (a forge token). Printing it is impossible by accident: std::format gives "<redacted>", there is no
	// stream operator, and the buffer is overwritten when the object dies.
	class SNK_API secret_text
	{
	public:
		secret_text() = default;
		explicit secret_text(std::string value) : m_value(std::move(value)) {}
		secret_text(const secret_text&) = delete;
		secret_text& operator=(const secret_text&) = delete;
		secret_text(secret_text&& other) noexcept;
		secret_text& operator=(secret_text&& other) noexcept;
		~secret_text();

		// The only way to the plain text: for the code that hands it to the OS store or to the HTTP header.
		const std::string& reveal() const { return m_value; }
		bool empty() const { return m_value.empty(); }
		std::size_t size() const { return m_value.size(); }
		secret_text clone() const { return secret_text(m_value); }
		// Overwrites the buffer and empties the value.
		void wipe() noexcept;

		static constexpr std::string_view redacted_text = "<redacted>";
		std::string to_string_redacted() const { return std::string(redacted_text); }

	private:
		std::string m_value;
	};

	struct secret_key
	{
		std::string service;
		std::string account;

		friend bool operator<(const secret_key& a, const secret_key& b)
		{
			return std::tie(a.service, a.account) < std::tie(b.service, b.account);
		}
	};

	enum class secret_status { ok, not_found, unavailable, failed };

	struct secret_result
	{
		secret_status status = secret_status::failed;
		// For the log and the UI; never holds the secret.
		std::string message;
	};

	class SNK_API secret_backend
	{
	public:
		virtual ~secret_backend() = default;
		virtual secret_result store(const secret_key& key, const secret_text& value) = 0;
		virtual secret_result load(const secret_key& key, secret_text& out) = 0;
		virtual secret_result erase(const secret_key& key) = 0;
		virtual const char* name() const = 0;
	};

	// The operating system's store: Windows Credential Manager, macOS Keychain, Linux libsecret (loaded with dlopen, so
	// there is no build dependency). Without libsecret or its daemon every call answers `unavailable`.
	SNK_API std::unique_ptr<secret_backend> make_os_secret_backend();

	// Secrets in RAM only; gone with the process.
	class SNK_API memory_secret_backend : public secret_backend
	{
	public:
		secret_result store(const secret_key& key, const secret_text& value) override;
		secret_result load(const secret_key& key, secret_text& out) override;
		secret_result erase(const secret_key& key) override;
		const char* name() const override { return "memory"; }

	private:
		std::map<secret_key, secret_text> m_values;
	};

	// The primary backend first; when it is `unavailable` or `failed` the value stays in RAM for the session. It is
	// never written to a file.
	class SNK_API secret_store
	{
	public:
		explicit secret_store(std::unique_ptr<secret_backend> primary);

		// `ok` also when only RAM took it; `persisted(key)` tells which.
		secret_result store(const secret_key& key, const secret_text& value);
		secret_result load(const secret_key& key, secret_text& out);
		secret_result erase(const secret_key& key);

		// True when the primary backend holds the value (it survives a restart).
		bool persisted(const secret_key& key) const;
		// The backend that took the last store(): the primary's name, or "memory".
		std::string last_backend_name() const;

	private:
		mutable std::mutex m_mutex;
		std::unique_ptr<secret_backend> m_primary;
		memory_secret_backend m_ram;
		std::set<secret_key> m_persisted;
		std::string m_last_backend;
	};
}

template <>
struct std::formatter<platform::secret_text> : std::formatter<std::string_view>
{
	auto format(const platform::secret_text&, std::format_context& ctx) const
	{
		return std::formatter<std::string_view>::format(platform::secret_text::redacted_text, ctx);
	}
};
