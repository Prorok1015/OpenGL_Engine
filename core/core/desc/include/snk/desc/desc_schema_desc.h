#pragma once
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_describe_ops.h>
#include <memory>

// Bridge from a described C++ type to desc_base: serialize, deserialize and copy_to come from the type's schema.
//
//   class light_desc : public desc::schema_desc<light_desc, desc::schema_kind::component>
//   {
//   public:
//       static constexpr std::string_view __type = "light_desc";
//       glm::vec4 color{ 1 };
//       BOOST_DESCRIBE_CLASS(light_desc, (), (color), (), ())
//   };
namespace desc
{
	template<typename T, schema_kind Kind = schema_kind::desc>
	class schema_desc : public desc_base
	{
	public:
		void copy_to(desc_base& other) const override
		{
			if (T* typed = dynamic_cast<T*>(&other)) {
				desc::copy(binding(), self(), binding(), typed);
				return;
			}
			desc_base::copy_to(other);
		}

		void deserialize(desc_system&, const json::object& object) override
		{
			desc::deserialize(binding(), self(), object);
		}

		void serialize(json::object& object) const override
		{
			desc::serialize(binding(), self(), object);
		}

		// Needs T::__type.
		static const schema_binding& binding()
		{
			static const std::shared_ptr<const schema_binding> instance = bind_described<T>(T::__type, 1, Kind);
			return *instance;
		}

	private:
		const T* self() const { return static_cast<const T*>(this); }
		T* self() { return static_cast<T*>(this); }
	};
}
