#pragma once
#include "edt_desc_file_commands.h"
#include <functional>
#include <memory>

namespace res { class resource_system; }

namespace edt
{
	class editor_context;
	struct editor_services;
	class inspector_panel;
	class prefab_document;
	class scene_document;
	class shape_parameters_section;

	// Hangs the "Shape parameters" section (shape_parameters_section) under a mesh component of the Inspector and gives it what it asks of the
	// editor: the poly file, the commands, the open level and prefab, the person's confirmation. Made after the asset opening (desc_layout);
	// `editor_system` only creates it.
	class shape_parameters_component
	{
	public:
		struct ports
		{
			editor_context& context;
			editor_services& services;
			scene_document& state;
			res::resource_system& resources;
			inspector_panel& inspector;
			std::function<desc_file_layout()> desc_layout;
			// The prefab the prefab editor window shows (null: none); the shape section rebuilds into that one.
			std::function<prefab_document*()> shown_prefab = {};
		};

		explicit shape_parameters_component(ports from);
		~shape_parameters_component();
		shape_parameters_component(const shape_parameters_component&) = delete;
		shape_parameters_component& operator=(const shape_parameters_component&) = delete;

	private:
		ports m_ports;
		std::unique_ptr<shape_parameters_section> m_section;
	};
}
