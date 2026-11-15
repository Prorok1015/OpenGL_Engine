#pragma once
#include <snk/common/ds/ds_type_id.hpp>
#include <snk/common/snk_api.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ds {

	// Services are destroyed in reverse registration order, so a service may hold references
	// to services registered before it.
	class SNK_API app_data_storage
	{
	public:
		struct isolated_t {};
		static constexpr isolated_t isolated{};

		app_data_storage();
		explicit app_data_storage(app_data_storage* parent);
		// A child that reads through to the parent only the services listed with expose<T>(): a game module's layer,
		// whose module must not reach the base's registries (MODULE_LAYER.md sec. 5). Anything else of the parent is
		// refused: has_value false, require_shared null, require throws std::logic_error.
		app_data_storage(app_data_storage* parent, isolated_t);
		app_data_storage(app_data_storage&& other);
		app_data_storage& operator=(app_data_storage&& other);
		app_data_storage(const app_data_storage&) = delete;
		app_data_storage& operator=(const app_data_storage&) = delete;
		~app_data_storage();

		template<class T>
		bool has_value() const {
			return find_entry(type_key<T>(), type_name<T>()) != nullptr;
		}

		template<class T>
		std::shared_ptr<T> require_parent_shared() const {
			if (app_data_storage* up = parent()) {
				return up->template require_shared<T>();
			}
			return nullptr;
		}

		template<class T>
		std::shared_ptr<T> require_shared() const {
			if (const entry* found = find_entry(type_key<T>(), type_name<T>())) {
				return std::static_pointer_cast<T>(found->object);
			}
			return nullptr;
		}

		template<class T>
		T& require() const {
			const entry* found = find_entry(type_key<T>(), type_name<T>());
			if (!found) {
				if (hidden(type_key<T>(), type_name<T>())) {
					throw std::logic_error(std::string{ "ds::app_data_storage::require: " } + std::string{ type_name<T>() }
						+ " belongs to the base and is not exposed to this storage (register through the module's layer)");
				}
				// Hard error in every build type: a missing service must not turn into a null dereference.
				throw std::runtime_error(std::string{ "ds::app_data_storage::require: service not found: " } + std::string{ type_name<T>() });
			}
			return *static_cast<T*>(found->object.get());
		}

		template<class T, class... A>
		T& construct(A&&... args) {
			return add<T, T>(std::forward<A>(args)...);
		}

		template<class KEY, class INSTANCE, class... A>
		KEY& construct(A&&... args) {
			return add<KEY, INSTANCE>(std::forward<A>(args)...);
		}

		// Puts an object owned elsewhere under T; `object` may be an aliasing shared_ptr that owns nothing (a
		// layer's storage refers to the layer's services this way).
		template<class T>
		T& attach(std::shared_ptr<T> object) {
			T& ref = *object;
			insert(type_key<T>(), type_name<T>(), std::move(object));
			return ref;
		}

		template<class T>
		void destruct() {
			erase_key(type_key<T>(), type_name<T>());
		}

		std::size_t size() const noexcept;

		// An isolated storage reads T through to its parent.
		template<class T>
		void expose() {
			expose_key(type_key<T>());
		}

	private:
		struct entry {
			std::uint64_t key;
			std::string_view name;
			std::shared_ptr<void> object;
		};

		template<class KEY, class INSTANCE, class... A>
		KEY& add(A&&... args) {
			ensure_absent(type_key<KEY>(), type_name<KEY>());
			std::shared_ptr<KEY> object = std::make_shared<INSTANCE>(std::forward<A>(args)...);
			KEY& ref = *object;
			insert(type_key<KEY>(), type_name<KEY>(), std::move(object));
			return ref;
		}

		void ensure_absent(std::uint64_t key, std::string_view name) const;
		void insert(std::uint64_t key, std::string_view name, std::shared_ptr<void> object);
		void rebuild_index();
		// Looks in this storage first, then in the parents, up to an isolated storage that does not expose the key.
		const entry* find_entry(std::uint64_t key, std::string_view name) const;
		// Not found because an isolated storage stops the lookup before a parent that has it.
		bool hidden(std::uint64_t key, std::string_view name) const;
		void destroy_all();
		// The exported halves of expose<T> and require_parent_shared<T>: module code never touches the members.
		// Throws std::logic_error when this storage itself has no `name` under `key`.
		void erase_key(std::uint64_t key, std::string_view name);
		void expose_key(std::uint64_t key);
		app_data_storage* parent() const;

		std::vector<entry> m_entries;
		std::unordered_map<std::uint64_t, std::size_t> m_index;
		app_data_storage* m_parent = nullptr;
		bool m_isolated = false;
		std::unordered_set<std::uint64_t> m_exposed;
	};
}