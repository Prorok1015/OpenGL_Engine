#pragma once
#include "common.h"
#include "ds_fixed_vector.hpp"
#include <entt/fwd.hpp>

namespace ecs
{
	/// EXAMPLE 
	/// 
	/// class job : public ecs::job_base
	/// {
	/// public:
	/// 	void init(entt::organizer& organizer, entt::registry& registry) override
	/// 	{
	///			organizer.add<&job::update>(this);
	/// 	}
	/// 
	/// 	void deinit(entt::organizer& organizer, entt::registry& registry) override
	/// 	{
	/// 		organizer.remove<&job::update>(this);
	/// 	}
	/// 
	///		void update(some_data& data) const {
	///			do_something(data);
	/// 	}
	/// };
	/// 
	/// job job_instance; // only one instance
	class job_base
	{
	public:
		enum layer { FIRST, SECOND, THIRD };

	private:
		static constexpr std::size_t MAX_LAYERS = 3;
		static constexpr std::size_t MAX_SYSTEMS = 16;
		static auto& get_jobs_layer(layer layer) {
			static std::array<ds::fixed_vector<job_base*, MAX_SYSTEMS>, MAX_LAYERS> jobs;
			return jobs[layer];
		}

	public:
		job_base() { get_jobs_layer(layer::FIRST).push_back(this); }
		virtual ~job_base() {}
		job_base(const job_base&) = delete;
		job_base(job_base&&) = delete;
		job_base& operator=(const job_base&) = delete;
		job_base& operator=(job_base&&) = delete;

		virtual void init(entt::organizer& organizer, entt::registry& registry) = 0;
		virtual void deinit(entt::organizer& organizer, entt::registry& registry) = 0;

		static const auto& get_jobs(layer layer) { return get_jobs_layer(layer); }
	};
}