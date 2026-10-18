#pragma once
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <source_location>

namespace ds {
	// Stable across runs and binaries (a function-static counter would be numbered separately
	// in the executable and in every shared library).
	constexpr std::uint64_t fnv1a_64(std::string_view text) {
		std::uint64_t hash = 14695981039346656037ull;
		for (const char c : text) {
			hash ^= static_cast<unsigned char>(c);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	// Unique per T within one compiler; the whole function_name is used as the identity.
	template<class T>
	constexpr std::string_view type_name() {
		return std::source_location::current().function_name();
	}

	template<class T>
	concept has_service_id = requires {
		{ T::service_id } -> std::convertible_to<std::string_view>;
	};

	template<class T>
	constexpr std::uint64_t type_key() {
		if constexpr (has_service_id<T>) {
			return fnv1a_64(std::string_view{ T::service_id });
		} else {
			return fnv1a_64(type_name<T>());
		}
	}

	struct type_id
	{
		using unique_id = size_t;
	public:
		type_id() = default;

		template<class T>
		static unique_id value() {
			return static_cast<unique_id>(type_key<T>());
		}
		
		template<class T>
		static type_id make() {
			return type_id{ value<T>() };
		}

		auto operator<=>(const type_id&) const = default;

	private:
		explicit type_id(unique_id id_)
			: id(id_) {
		}

	public:
		unique_id id = 0;
	};

}

namespace std {
	template<>
	struct hash<ds::type_id> {
		std::size_t operator()(const ds::type_id& type) const {
			return type.id;
		}
	};
}