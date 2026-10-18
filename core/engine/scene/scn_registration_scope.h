#pragma once
#include "scn_ecs_assembler.h"
#include <snk/desc/desc_system.h>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "snk_api.h"

namespace rnd {
	class skinning_manager;
}

namespace scn
{
	// Owns a set of desc-factory and spawner registrations and removes them, in reverse order, on
	// destruction. Spawners may capture the assembler, so destroy the scope before the assembler.
	class SNK_API registration_scope
	{
	public:
		registration_scope(desc::desc_system& desc_sys, ecs_assembler& assembler);
		~registration_scope();

		registration_scope(const registration_scope&) = delete;
		registration_scope& operator=(const registration_scope&) = delete;
		registration_scope(registration_scope&& other) noexcept;
		registration_scope& operator=(registration_scope&& other) noexcept;

		// component = true also lists the type as user-addable in the editor.
		template<class T>
		void desc(std::string_view type, bool component = false)
		{
			if (component) {
				m_desc_sys->register_component_desc<T>(type);
			} else {
				m_desc_sys->register_desc<T>(type);
			}
			m_entries.push_back({ entry_kind::desc, std::string{ type } });
		}

		// Upgrades `type` from `from_version` to the next one; removed together with the scope.
		void migration(std::string_view type, int from_version, std::function<void(json::object&)> fn)
		{
			m_desc_sys->register_migration(type, from_version, std::move(fn));
			m_entries.push_back({ entry_kind::migration, std::string{ type } });
		}

		void spawner(std::string_view type,
					 ecs_assembler::desc_spawner_fn spawn,
					 ecs_assembler::desc_patcher_fn patch = {},
					 spawner_category category = spawner_category::component);

		std::size_t size() const { return m_entries.size(); }

	private:
		enum class entry_kind { desc, spawner, migration };
		struct entry {
			entry_kind kind;
			std::string type;
		};

		void release();

		desc::desc_system* m_desc_sys;
		ecs_assembler* m_assembler;
		std::vector<entry> m_entries;
	};

	// Registers every scene desc type and its spawner. The skin_weights spawner keeps a reference
	// to `skinning`, which must outlive the returned scope.
	SNK_API registration_scope register_scene_types(desc::desc_system& desc_sys, ecs_assembler& assembler, rnd::skinning_manager& skinning);
}
