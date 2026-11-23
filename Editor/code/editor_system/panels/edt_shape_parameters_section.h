#pragma once
#include "edt_shape_menu_model.h"
#include "edt_shape_parameters.h"
#include "edt_prefab_shape.h"
#include <boost/json.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace gui { struct theme_semantic_colors; }

namespace edt
{
	// "Shape parameters" under a mesh node's component in the Inspector (#746 S5): the parameters of the shape the node draws, when the
	// editor made it and nobody edited its mesh by hand, and a Rebuild button. Nothing is rebuilt while a value is typed.
	class shape_parameters_section
	{
	public:
		struct ports
		{
			std::function<desc_file_read(const std::string& poly_path)> read_poly;
			// What a rebuild to `range` does to the open level and prefab (the nodes it changes, what is unsaved).
			std::function<mesh::rebuild_facts(std::string_view geometry, const std::string& poly_path, const boost::json::object& range)> facts;
			// A person's answer: `proceed` runs on Rebuild only.
			std::function<void(std::string title, std::string text, std::function<void()> proceed)> confirm;
			// shape.rebuild, then the open prefab's working copy takes the new range; what went wrong is in the report.
			std::function<rebuild_report(const boost::json::object& arguments, std::string_view geometry)> rebuild;
			std::function<void(std::string text)> report_error;
			// Play, or an open prefab that cannot be edited.
			std::function<bool()> locked;
		};

		explicit shape_parameters_section(ports from);

		// For the mesh component `comp` of the selected node.
		void draw(const scn::prefab_desc::prefab_comp_node& comp, const gui::theme_semantic_colors& colors);
		// The file of the shape is read again at the next draw.
		void forget() { m_geometry.clear(); }

	private:
		void load(const std::string& geometry);
		void begin_rebuild(const std::string& geometry);

		ports m_ports;
		std::shared_ptr<int> m_alive = std::make_shared<int>();
		std::string m_geometry;
		mesh::shape_parameters m_shape;
		mesh::parametric_shape_form m_form;
		std::string m_problem;
	};
}
