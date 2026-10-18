#pragma once
#include <entt/fwd.hpp>
#include <vector>

namespace ecs
{
	template<class T>
	struct event
	{
		std::vector<entt::entity> const& list() const {
			return m_list;
		}

		void emit(entt::entity ent) {
			m_list.push_back(ent);
		}

		auto begin() { return m_list.begin(); }
		auto begin() const { return m_list.begin(); }
		auto cbegin() const { return m_list.cbegin(); }
		auto end() { return m_list.end(); }
		auto end() const { return m_list.end(); }
		auto cend() const { return m_list.cend(); }

		auto size() const { return m_list.size(); }

		void clear() {
			m_list.clear();
		}
	private:
		std::vector<entt::entity> m_list;
	};
}