#pragma once
#include "edt_desc_location.h"
#include "edt_desc_problems.h"
#include "edt_play_preflight.h"
#include "edt_world_problems.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <snk/resource/res_tag.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gs { class input_actions; }

namespace edt
{
	class asset_component;
	class editor_context;
	class problems_panel;
	class scene_document;
	class world_controller;
	struct editor_services;
	struct problem;

	// What the Problems panel shows and where a row leads, whole: the publishers of the sources "desc", "world", "play" (and the
	// input conflicts, "input"), what a double click on a row does (`go_to_problem`: select its node, or open its file), the fixes of
	// the source "assets" the panel offers, and the refresh of the world rows on every change of the level. The publishers live here
	// and are handed out by reference to whatever reports into them (the prefab editor, the level load, the drop of an asset, Play).
	// editor_system creates it before the prefab editor, which reports into it, and so declares it before everything that holds one
	// of those references.
	class problems_component
	{
	public:
		struct ports
		{
			editor_context& context;
			scene_document& state;
			editor_services& services;
			world_controller& worlds;
			// Null without a project (the asset database exists only with one): read when asked.
			std::function<asset_component*()> assets;
			// Opens the file a row leads to; false when nothing opens it (`quiet`: a link, not a double click).
			std::function<bool(const res::tag&, bool quiet)> open_asset;
			std::function<bool()> play_active;
		};

		explicit problems_component(ports from);
		~problems_component();
		problems_component(const problems_component&) = delete;
		problems_component& operator=(const problems_component&) = delete;

		// The world rows follow the level: refreshed when a document or its active world changes.
		void install();
		// The panel's double click, the fixes of the source "assets" and Edit > Undo's twin, the "Undo: <label>" button.
		void wire(problems_panel& panel);

		desc_problem_publisher& desc() { return m_desc; }
		play_preflight_publisher& play_preflight() { return m_play_preflight; }
		// The unknown systems of the worlds' plans as problems (#549); refreshed on every change of the level.
		void refresh_world();
		// Bindings two actions share are problems of the source "input" (the frame hook; only when the actions changed).
		void follow_input_actions(const gs::input_actions& actions);
		// What a row of Problems (double click, Enter, "Go to") leads to: its node selected in its world, or its file opened when an
		// asset kind opens it; nothing for a node that is gone or a level that is not open (#596).
		void go_to_problem(const std::string& source, const problem& item);
		void go_to(const desc_target& target);

	private:
		ports m_ports;
		world_problem_publisher m_world;
		// The source "desc" (#596): the scopes "level" (load and hot reload), "drop" and "prefab" (the open preview).
		desc_problem_publisher m_desc;
		// The source "play" (#597): the last check before Play.
		play_preflight_publisher m_play_preflight;
		// gs::input_actions::revision() the Problems of the source "input" were last made for.
		std::uint64_t m_input_revision = ~std::uint64_t{ 0 };
		// A fix's question holds it weakly: answered after this component went, it runs nothing.
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
		// Last, so the listeners go before what they call.
		std::vector<connection> m_connections;
	};
}
