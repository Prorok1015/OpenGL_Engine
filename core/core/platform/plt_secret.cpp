#include <snk/platform/plt_secret.h>
#include <snk/common/logger/engine_log.h>
#include <cstdint>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#else
#include <dlfcn.h>
#endif

namespace platform
{
	namespace {

		// volatile, so the compiler cannot drop the stores of a buffer that is about to be freed.
		void wipe_string(std::string& value) noexcept
		{
			volatile char* p = value.data();
			for (std::size_t i = 0; i < value.capacity(); ++i) {
				p[i] = 0;
			}
			value.clear();
		}

		secret_result make_result(secret_status status, std::string message = {})
		{
			return secret_result{ status, std::move(message) };
		}

#ifdef _WIN32

		std::wstring wide_of(const std::string& utf8)
		{
			if (utf8.empty()) {
				return {};
			}
			const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
			std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
			if (n > 0) {
				MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
			}
			return out;
		}

		secret_result from_last_error(const char* what)
		{
			const DWORD err = GetLastError();
			if (err == ERROR_NOT_FOUND) {
				return make_result(secret_status::not_found);
			}
			const secret_status status = (err == ERROR_NO_SUCH_LOGON_SESSION || err == ERROR_ACCESS_DENIED)
				? secret_status::unavailable : secret_status::failed;
			return make_result(status, std::format("{} failed (Windows error {})", what, err));
		}

		class os_secret_backend final : public secret_backend
		{
		public:
			secret_result store(const secret_key& key, const secret_text& value) override
			{
				if (value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
					return make_result(secret_status::failed, "the value is too long for Credential Manager");
				}
				detail::os_target os_name;
				if (const secret_result checked = detail::make_os_target(key, os_name); checked.status != secret_status::ok) {
					return checked;
				}
				std::wstring target = wide_of(os_name.name);
				std::wstring user = wide_of(os_name.account);
				CREDENTIALW cred{};
				cred.Type = CRED_TYPE_GENERIC;
				cred.TargetName = target.data();
				cred.UserName = user.data();
				cred.CredentialBlobSize = static_cast<DWORD>(value.size());
				cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.reveal().data()));
				cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
				if (!CredWriteW(&cred, 0)) {
					return from_last_error("CredWrite");
				}
				return make_result(secret_status::ok);
			}

			secret_result load(const secret_key& key, secret_text& out) override
			{
				PCREDENTIALW cred = nullptr;
				detail::os_target os_name;
				if (const secret_result checked = detail::make_os_target(key, os_name); checked.status != secret_status::ok) {
					return checked;
				}
				const std::wstring target = wide_of(os_name.name);
				if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
					return from_last_error("CredRead");
				}
				out = secret_text(std::string(reinterpret_cast<const char*>(cred->CredentialBlob), cred->CredentialBlobSize));
				SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
				CredFree(cred);
				return make_result(secret_status::ok);
			}

			secret_result erase(const secret_key& key) override
			{
				detail::os_target os_name;
				if (const secret_result checked = detail::make_os_target(key, os_name); checked.status != secret_status::ok) {
					return checked;
				}
				const std::wstring target = wide_of(os_name.name);
				if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
					return from_last_error("CredDelete");
				}
				return make_result(secret_status::ok);
			}

			const char* name() const override { return "windows-credential-manager"; }
		};

#elif defined(__APPLE__)

		template <class T>
		class cf_ref
		{
		public:
			cf_ref() = default;
			explicit cf_ref(T ref) : m_ref(ref) {}
			cf_ref(const cf_ref&) = delete;
			cf_ref& operator=(const cf_ref&) = delete;
			cf_ref(cf_ref&& other) noexcept : m_ref(other.m_ref) { other.m_ref = nullptr; }
			~cf_ref() { if (m_ref) { CFRelease(m_ref); } }
			T get() const { return m_ref; }
			T* out() { return &m_ref; }
		private:
			T m_ref = nullptr;
		};

		// CFData that holds a secret: the bytes are zeroed before the object is released.
		class cf_secret_data
		{
		public:
			explicit cf_secret_data(const std::string& bytes)
				: m_ref(CFDataCreateMutable(kCFAllocatorDefault, static_cast<CFIndex>(bytes.size())))
			{
				if (m_ref) {
					CFDataAppendBytes(m_ref, reinterpret_cast<const UInt8*>(bytes.data()), static_cast<CFIndex>(bytes.size()));
				}
			}
			cf_secret_data(const cf_secret_data&) = delete;
			cf_secret_data& operator=(const cf_secret_data&) = delete;
			~cf_secret_data()
			{
				if (!m_ref) {
					return;
				}
				volatile UInt8* p = CFDataGetMutableBytePtr(m_ref);
				for (CFIndex i = 0, n = CFDataGetLength(m_ref); i < n; ++i) {
					p[i] = 0;
				}
				CFRelease(m_ref);
			}
			CFDataRef get() const { return m_ref; }
		private:
			CFMutableDataRef m_ref = nullptr;
		};

		cf_ref<CFStringRef> cf_string(const std::string& utf8)
		{
			return cf_ref<CFStringRef>(CFStringCreateWithBytes(kCFAllocatorDefault,
				reinterpret_cast<const UInt8*>(utf8.data()), static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false));
		}

		cf_ref<CFMutableDictionaryRef> base_query(const cf_ref<CFStringRef>& service, const cf_ref<CFStringRef>& account)
		{
			cf_ref<CFMutableDictionaryRef> query(CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
				&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
			if (query.get()) {
				CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
				CFDictionarySetValue(query.get(), kSecAttrService, service.get());
				CFDictionarySetValue(query.get(), kSecAttrAccount, account.get());
			}
			return query;
		}

		secret_result from_os_status(OSStatus status, const char* what)
		{
			if (status == errSecSuccess) {
				return make_result(secret_status::ok);
			}
			if (status == errSecItemNotFound) {
				return make_result(secret_status::not_found);
			}
			const secret_status kind = (status == errSecNotAvailable || status == errSecInteractionNotAllowed)
				? secret_status::unavailable : secret_status::failed;
			return make_result(kind, std::format("{} failed (OSStatus {})", what, static_cast<int>(status)));
		}

		class os_secret_backend final : public secret_backend
		{
		public:
			secret_result store(const secret_key& key, const secret_text& value) override
			{
				detail::os_target os_name;
				if (const secret_result checked = detail::make_os_target(key, os_name); checked.status != secret_status::ok) {
					return checked;
				}
				const auto service = cf_string(os_name.service);
				const auto account = cf_string(os_name.account);
				const auto query = base_query(service, account);
				const cf_secret_data data(value.reveal());
				if (!service.get() || !account.get() || !query.get() || !data.get()) {
					return make_result(secret_status::failed, "cannot build the Keychain query");
				}
				CFDictionarySetValue(query.get(), kSecValueData, data.get());
				OSStatus status = SecItemAdd(query.get(), nullptr);
				if (status == errSecDuplicateItem) {
					const auto match = base_query(service, account);
					cf_ref<CFMutableDictionaryRef> update(CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
						&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
					if (!match.get() || !update.get()) {
						return make_result(secret_status::failed, "cannot build the Keychain query");
					}
					CFDictionarySetValue(update.get(), kSecValueData, data.get());
					status = SecItemUpdate(match.get(), update.get());
				}
				return from_os_status(status, "SecItemAdd");
			}

			secret_result load(const secret_key& key, secret_text& out) override
			{
				detail::os_target os_name;
				if (const secret_result checked = detail::make_os_target(key, os_name); checked.status != secret_status::ok) {
					return checked;
				}
				const auto service = cf_string(os_name.service);
				const auto account = cf_string(os_name.account);
				const auto query = base_query(service, account);
				if (!service.get() || !account.get() || !query.get()) {
					return make_result(secret_status::failed, "cannot build the Keychain query");
				}
				CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
				CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
				cf_ref<CFTypeRef> result;
				const OSStatus status = SecItemCopyMatching(query.get(), result.out());
				if (status != errSecSuccess) {
					return from_os_status(status, "SecItemCopyMatching");
				}
				if (!result.get() || CFGetTypeID(result.get()) != CFDataGetTypeID()) {
					return make_result(secret_status::failed, "the Keychain returned no data");
				}
				const auto data = static_cast<CFDataRef>(result.get());
				out = secret_text(std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
					static_cast<std::size_t>(CFDataGetLength(data))));
				// The returned CFData is immutable and owned by Security (it may be shared): writing into it is
				// undefined, and zeroing a mutable copy would not clear this one, so it is only released.
				return make_result(secret_status::ok);
			}

			secret_result erase(const secret_key& key) override
			{
				detail::os_target os_name;
				if (const secret_result checked = detail::make_os_target(key, os_name); checked.status != secret_status::ok) {
					return checked;
				}
				const auto service = cf_string(os_name.service);
				const auto account = cf_string(os_name.account);
				const auto query = base_query(service, account);
				if (!service.get() || !account.get() || !query.get()) {
					return make_result(secret_status::failed, "cannot build the Keychain query");
				}
				return from_os_status(SecItemDelete(query.get()), "SecItemDelete");
			}

			const char* name() const override { return "macos-keychain"; }
		};

#else

		// libsecret's public SecretSchema (libsecret/secret-schema.h) laid out by hand, because the header is not
		// installed on CI: name, flags (an enum, int-sized), 32 {name, type} attributes, then reserved fields. Only
		// the first fields are read by the library; the tail must exist so the struct is as large as the real one.
		struct secret_schema_attribute
		{
			const char* name;
			int type;
		};

		struct secret_schema
		{
			const char* name;
			int flags;
			secret_schema_attribute attributes[32];
			int reserved;
			void* reserved1;
			void* reserved2;
			void* reserved3;
			void* reserved4;
			void* reserved5;
			void* reserved6;
			void* reserved7;
		};

		struct glib_error
		{
			std::uint32_t domain;
			int code;
			char* message;
		};

		constexpr int attribute_type_string = 0;

		using store_fn = int (*)(const secret_schema*, const char* collection, const char* label, const char* password,
			void* cancellable, glib_error** error, ...);
		using lookup_fn = char* (*)(const secret_schema*, void* cancellable, glib_error** error, ...);
		using clear_fn = int (*)(const secret_schema*, void* cancellable, glib_error** error, ...);
		using free_fn = void (*)(void*);
		using error_free_fn = void (*)(glib_error*);

		const secret_schema& snk_schema()
		{
			static const secret_schema schema = [] {
				secret_schema s{};
				s.name = "org.snk.engine.secret";
				s.flags = 0; // SECRET_SCHEMA_NONE
				s.attributes[0] = { "service", attribute_type_string };
				s.attributes[1] = { "account", attribute_type_string };
				return s;
			}();
			return schema;
		}

		// The libraries are opened once per process and never closed: libsecret/glib register GTypes, start a GDBus
		// worker thread and cache a proxy, none of which survives dlclose. RTLD_NODELETE keeps them mapped regardless.
		struct libsecret_api
		{
			store_fn store = nullptr;
			lookup_fn lookup = nullptr;
			clear_fn clear = nullptr;
			free_fn free = nullptr;
			error_free_fn error_free = nullptr;

			bool usable() const { return store && lookup && clear && free; }

			static const libsecret_api& get()
			{
				static const libsecret_api api = load();
				return api;
			}

		private:
			static libsecret_api load()
			{
				libsecret_api api;
				constexpr int flags = RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE;
				void* lib = dlopen("libsecret-1.so.0", flags);
				if (!lib) {
					return api;
				}
				api.store = reinterpret_cast<store_fn>(dlsym(lib, "secret_password_store_sync"));
				api.lookup = reinterpret_cast<lookup_fn>(dlsym(lib, "secret_password_lookup_sync"));
				api.clear = reinterpret_cast<clear_fn>(dlsym(lib, "secret_password_clear_sync"));
				api.free = reinterpret_cast<free_fn>(dlsym(lib, "secret_password_free"));
				// dlsym on a handle also searches its dependencies, which normally include glib.
				api.error_free = reinterpret_cast<error_free_fn>(dlsym(lib, "g_error_free"));
				if (!api.error_free) {
					if (void* glib = dlopen("libglib-2.0.so.0", flags)) {
						api.error_free = reinterpret_cast<error_free_fn>(dlsym(glib, "g_error_free"));
					}
				}
				return api;
			}
		};

		class os_secret_backend final : public secret_backend
		{
		public:
			secret_result store(const secret_key& key, const secret_text& value) override
			{
				detail::os_target target;
				if (const secret_result checked = detail::make_os_target(key, target); checked.status != secret_status::ok) {
					return checked;
				}
				const libsecret_api& api = libsecret_api::get();
				if (!api.usable()) {
					return unavailable();
				}
				glib_error* error = nullptr;
				const int ok = api.store(&snk_schema(), "default", target.name.c_str(), value.reveal().c_str(), nullptr, &error,
					"service", target.service.c_str(), "account", target.account.c_str(), static_cast<const char*>(nullptr));
				if (!ok || error) {
					return from_error(api, error, "secret_password_store_sync");
				}
				return make_result(secret_status::ok);
			}

			secret_result load(const secret_key& key, secret_text& out) override
			{
				detail::os_target target;
				if (const secret_result checked = detail::make_os_target(key, target); checked.status != secret_status::ok) {
					return checked;
				}
				const libsecret_api& api = libsecret_api::get();
				if (!api.usable()) {
					return unavailable();
				}
				glib_error* error = nullptr;
				char* found = api.lookup(&snk_schema(), nullptr, &error,
					"service", target.service.c_str(), "account", target.account.c_str(), static_cast<const char*>(nullptr));
				if (error) {
					if (found) {
						api.free(found);
					}
					return from_error(api, error, "secret_password_lookup_sync");
				}
				if (!found) {
					return make_result(secret_status::not_found);
				}
				out = secret_text(std::string(found));
				api.free(found);
				return make_result(secret_status::ok);
			}

			secret_result erase(const secret_key& key) override
			{
				detail::os_target target;
				if (const secret_result checked = detail::make_os_target(key, target); checked.status != secret_status::ok) {
					return checked;
				}
				const libsecret_api& api = libsecret_api::get();
				if (!api.usable()) {
					return unavailable();
				}
				glib_error* error = nullptr;
				const int removed = api.clear(&snk_schema(), nullptr, &error,
					"service", target.service.c_str(), "account", target.account.c_str(), static_cast<const char*>(nullptr));
				if (error) {
					return from_error(api, error, "secret_password_clear_sync");
				}
				return make_result(removed ? secret_status::ok : secret_status::not_found);
			}

			const char* name() const override { return "libsecret"; }

		private:
			static secret_result unavailable()
			{
				return make_result(secret_status::unavailable, "libsecret is not installed");
			}

			// No D-Bus session or no keyring daemon shows up as a GError: the caller keeps the value in RAM.
			static secret_result from_error(const libsecret_api& api, glib_error* error, const char* what)
			{
				std::string text = error && error->message ? error->message : "unknown error";
				if (error && api.error_free) {
					api.error_free(error);
				}
				return make_result(secret_status::unavailable, std::format("{} failed: {}", what, text));
			}
		};

#endif
	}

	namespace detail {

		namespace {

			bool is_valid_key_part(const std::string& part)
			{
				if (part.empty() || part.size() > 255) {
					return false;
				}
				for (const char c : part) {
					const auto u = static_cast<unsigned char>(c);
					if (c == '/' || c == ':' || u < 0x20 || u == 0x7f) {
						return false;
					}
				}
				return true;
			}
		}

		secret_result make_os_target(const secret_key& key, os_target& out)
		{
			if (!is_valid_key_part(key.service) || !is_valid_key_part(key.account)) {
				return make_result(secret_status::failed,
					"invalid secret key: service and account must be 1..255 bytes without '/', ':' or control characters");
			}
			const std::string prefix(os_name_prefix);
			out.service = prefix + key.service;
			out.account = prefix + key.account;
			out.name = prefix + key.service + ":" + key.account;
			return make_result(secret_status::ok);
		}
	}

	secret_text::secret_text(secret_text&& other) noexcept : m_value(std::move(other.m_value))
	{
		other.wipe();
	}

	secret_text& secret_text::operator=(secret_text&& other) noexcept
	{
		if (this != &other) {
			wipe_string(m_value);
			m_value = std::move(other.m_value);
			other.wipe();
		}
		return *this;
	}

	secret_text::~secret_text()
	{
		wipe_string(m_value);
	}

	void secret_text::wipe() noexcept
	{
		wipe_string(m_value);
	}

	std::unique_ptr<secret_backend> make_os_secret_backend()
	{
		return std::make_unique<os_secret_backend>();
	}

	secret_result memory_secret_backend::store(const secret_key& key, const secret_text& value)
	{
		m_values.insert_or_assign(key, value.clone());
		return make_result(secret_status::ok);
	}

	secret_result memory_secret_backend::load(const secret_key& key, secret_text& out)
	{
		const auto it = m_values.find(key);
		if (it == m_values.end()) {
			return make_result(secret_status::not_found);
		}
		out = it->second.clone();
		return make_result(secret_status::ok);
	}

	secret_result memory_secret_backend::erase(const secret_key& key)
	{
		return make_result(m_values.erase(key) ? secret_status::ok : secret_status::not_found);
	}

	secret_store::secret_store(std::unique_ptr<secret_backend> primary) : m_primary(std::move(primary)), m_last_backend("memory")
	{
	}

	secret_result secret_store::store(const secret_key& key, const secret_text& value)
	{
		std::lock_guard lock(m_mutex);
		detail::os_target target;
		if (const secret_result invalid = detail::make_os_target(key, target); invalid.status != secret_status::ok) {
			return invalid;
		}
		secret_result primary = make_result(secret_status::unavailable, "no OS secret store");
		if (m_primary) {
			primary = m_primary->store(key, value);
		}
		if (primary.status == secret_status::ok) {
			m_ram.erase(key);
			m_persisted.insert(key);
			m_last_backend = m_primary->name();
			return primary;
		}
		m_persisted.erase(key);
		m_ram.store(key, value);
		m_last_backend = m_ram.name();
		egLOG_WARN("platform/secret", "the secret store is unavailable, the value is kept in memory for this session ({})",
			primary.message);
		return make_result(secret_status::ok, "kept in memory only for this session: " + primary.message);
	}

	secret_result secret_store::load(const secret_key& key, secret_text& out)
	{
		std::lock_guard lock(m_mutex);
		// A RAM entry exists only while the primary refused the latest store(), so it is newer than anything the
		// primary may hold (e.g. an old, possibly revoked token in a keyring that unlocked later).
		const secret_result ram = m_ram.load(key, out);
		if (ram.status == secret_status::ok) {
			return ram;
		}
		return m_primary ? m_primary->load(key, out) : make_result(secret_status::not_found);
	}

	secret_result secret_store::erase(const secret_key& key)
	{
		std::lock_guard lock(m_mutex);
		secret_result primary = make_result(secret_status::not_found);
		if (m_primary) {
			primary = m_primary->erase(key);
		}
		const secret_result ram = m_ram.erase(key);
		m_persisted.erase(key);
		if (primary.status == secret_status::unavailable || primary.status == secret_status::failed) {
			// Not a success: the OS store may still hold a copy that comes back after a restart.
			return make_result(primary.status,
				"the OS secret store could not be reached, a saved copy of the secret may remain: " + primary.message);
		}
		if (primary.status == secret_status::ok || ram.status == secret_status::ok) {
			return make_result(secret_status::ok);
		}
		return make_result(secret_status::not_found);
	}

	secret_presence secret_store::presence(const secret_key& key)
	{
		std::lock_guard lock(m_mutex);
		secret_text probe;
		if (m_ram.load(key, probe).status == secret_status::ok) {
			return secret_presence::in_memory;
		}
		if (!m_primary) {
			return secret_presence::absent;
		}
		const secret_result loaded = m_primary->load(key, probe);
		if (loaded.status == secret_status::ok) {
			m_persisted.insert(key);
			return secret_presence::in_os_store;
		}
		return loaded.status == secret_status::not_found ? secret_presence::absent : secret_presence::unknown;
	}

	bool secret_store::persisted(const secret_key& key) const
	{
		std::lock_guard lock(m_mutex);
		return m_persisted.contains(key);
	}

	std::string secret_store::last_backend_name() const
	{
		std::lock_guard lock(m_mutex);
		return m_last_backend;
	}
}
