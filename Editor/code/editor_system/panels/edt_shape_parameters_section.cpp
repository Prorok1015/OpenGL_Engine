#include "edt_shape_parameters_section.h"
#include "edt_shape_menu.h"
#include <snk/gui/gui_theme.h>
#include <imgui.h>
#include <format>

namespace json = boost::json;

namespace edt
{
	shape_parameters_section::shape_parameters_section(ports from)
		: m_ports(std::move(from))
	{
	}

	void shape_parameters_section::load(const std::string& geometry)
	{
		m_geometry = geometry;
		m_problem.clear();
		const std::string poly = mesh::poly_path_of_geometry(geometry);
		if (poly.empty()) {
			m_shape = {};
			return;
		}
		m_shape = mesh::load_shape_parameters(poly, m_ports.read_poly(poly));
		if (m_shape.what != mesh::shape_parameters::state::editable) return;
		std::string error;
		if (!m_form.load(m_shape.kind, m_shape.params, error)) {
			m_shape.what = mesh::shape_parameters::state::unreadable;
			m_shape.reason = error;
		}
	}

	void shape_parameters_section::draw(const scn::prefab_desc::prefab_comp_node& comp, const gui::theme_semantic_colors& colors)
	{
		const json::value* geometry = comp.overrides.if_contains("geometry");
		if (!geometry || !geometry->is_string()) return;
		const std::string tag(geometry->as_string());
		if (tag != m_geometry) load(tag);

		using state = mesh::shape_parameters::state;
		if (m_shape.what == state::not_a_shape || m_shape.what == state::lower_layer) return;

		ImGui::Separator();
		ImGui::TextDisabled("Shape parameters");
		if (m_shape.what != state::editable) {
			ImGui::TextWrapped("%s", m_shape.reason.c_str());
			return;
		}

		draw_shape_fields(m_form);
		if (!m_problem.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, colors.error);
			ImGui::TextWrapped("%s", m_problem.c_str());
			ImGui::PopStyleColor();
		}
		const bool locked = m_ports.locked && m_ports.locked();
		ImGui::BeginDisabled(locked);
		if (ImGui::Button("Rebuild...")) begin_rebuild(tag);
		ImGui::SameLine();
		if (ImGui::Button("Reset")) load(tag);
		ImGui::EndDisabled();
		if (locked) ImGui::TextDisabled("Stop Play to rebuild");
	}

	void shape_parameters_section::begin_rebuild(const std::string& geometry)
	{
		m_problem = m_form.parameters_problem();
		if (!m_problem.empty()) return;

		// The version the command checks is the one of the file as it was read: another one means the parameters shown are not the file's.
		const mesh::shape_parameters fresh = mesh::load_shape_parameters(m_shape.poly, m_ports.read_poly(m_shape.poly));
		if (fresh.version != m_shape.version) {
			load(geometry);
			m_problem = "The file of the shape changed since it was read; its parameters are read again. Check them and press Rebuild again.";
			return;
		}
		const std::optional<json::object> range = mesh::predicted_range(m_form.kind(), m_form.params(), m_problem);
		if (!range) return;

		const json::object arguments = mesh::rebuild_arguments(m_shape, m_form.params());
		const std::string text = mesh::rebuild_confirmation(m_ports.facts(geometry, m_shape.poly, *range));
		m_ports.confirm("Rebuild shape", text, [this, geometry, arguments, alive = std::weak_ptr<int>(m_alive)] {
			if (!alive.lock()) return;
			const rebuild_report report = m_ports.rebuild(arguments, geometry);
			forget();
			if (!report.error.empty()) {
				m_ports.report_error(std::format("The shape was not rebuilt:\n{}", report.error));
				return;
			}
			if (report.notes.empty()) return;
			std::string notes = "The shape was rebuilt, but:";
			for (const std::string& note : report.notes) notes += "\n" + note;
			m_ports.report_error(std::move(notes));
		});
	}
}
