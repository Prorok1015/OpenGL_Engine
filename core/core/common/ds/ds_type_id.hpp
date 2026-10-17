#pragma once
#include <string_view>
#include <source_location>

namespace ds {
	struct type_id
	{
		using unique_id = size_t;
	public:
		type_id() = default;

		template<class T>
		static unique_id value() {
			static unique_id classId = next();
			return classId;
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

		static unique_id next() {
			static unique_id counter = 0;
			return counter++;
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