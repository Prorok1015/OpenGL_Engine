#pragma once
// Shared headless test environment: a resource_system whose "memory" protocol is a synchronous
// in-RAM store (no disk, no threads), a desc_system with every scene desc registered, and an
// ecs_assembler with the production spawners plus one test-only component ("stat_desc") that makes
// override/merge behavior easy to observe.
#include <snk/resource/res_system.h>
#include <snk/desc/desc_system.h>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/resource/resolvers/res_resolver_interface.hpp>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_registration_scope.h>
#include <snk/render/skinning/rnd_skinning_manager.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/scn_camera_desc.h>
#include <snk/scene/scn_directional_light_desc.h>
#include <snk/scene/scn_skybox_desc.h>
#include <snk/scene/scn_anchor_desc.h>
#include <snk/scene/scn_mesh_node_desc.h>
#include <snk/scene/scn_object_desc.h>
#include <snk/scene/scn_bone_desc.h>
#include <snk/scene/scn_skinning_desc.h>
#include <snk/scene/scn_skin_weights_desc.h>
#include <snk/scene/scn_animation_controller_desc.h>
#include <snk/scene/scn_animated_node_desc.h>
#include <snk/scene/scn_animation_clip_desc.h>
#include <snk/scene/scn_animation_collection_desc.h>
#include <snk/scene/scn_material_desc.h>
#include <boost/json.hpp>
#include <entt/entt.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace test_util {

struct inline_worker : core::res::adapter_worker_base
{
	void start() override {}
	void stop() override {}
	bool is_stopped() const override { return false; }
	void post(std::function<void()> cb) override { cb(); }
};

inline std::vector<std::byte> to_bytes(std::string_view s)
{
	std::vector<std::byte> out(s.size());
	for (size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
	return out;
}

// Synchronous in-memory resolver: store() is visible to resolve()/exists() immediately.
class ram_resolver : public core::res::res_resolver_interface
{
public:
	ram_resolver(const std::string_view prt) : core::res::res_resolver_interface(prt) {}

	async_raw_data resolve(const res::tag& tag) const override
	{
		auto block = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
		auto it = files.find(tag);
		if (it == files.end()) {
			block->set_error("no such ram file: " + tag.string());
		} else {
			auto copy = it->second;
			block->set_ready(std::move(copy));
		}
		return block;
	}

	bool store(const res::tag& tag, const std::vector<std::byte>& data) override
	{
		files[tag] = data;
		return true;
	}

	bool exists(const res::tag& tag) const override { return files.contains(tag); }

	std::unordered_map<res::tag, std::vector<std::byte>> files;
};

// resource_system with the memory protocol replaced by ram_resolver and a synchronous worker.
struct ram_res_env
{
	ram_res_env()
	{
		std::filesystem::create_directories("./res"); // resource_system's vfs root
		std::filesystem::create_directories("./sandbox");
		rs = std::make_unique<res::resource_system>();
		rs->unregistrate_resolver(res::tag::memory);
		ram = &rs->registrate_resolver<ram_resolver>(res::tag::memory);
		rs->set_adapter_worker(std::make_unique<inline_worker>());
	}

	static res::tag tag_of(std::string_view name) { return res::tag{ std::string("memory://t/") + std::string(name) }; }

	res::tag put(std::string_view name, std::string_view content)
	{
		auto tag = tag_of(name);
		rs->store(tag, to_bytes(content));
		return tag;
	}

	std::unique_ptr<res::resource_system> rs;
	ram_resolver* ram = nullptr;
};

// ---- test-only component with a nested object field, to observe override merging ----

struct stat_component
{
	int hp = 0;
	int limit_min = 0;
	int limit_max = 0;
	std::string label{};
	int patch_count = 0; // incremented every time the hot-reload patcher runs on the entity
};

struct stat_desc : desc::desc_base
{
	int hp = 1;
	int limit_min = 0;
	int limit_max = 0;
	std::string label = "default";

	void copy_to(desc::desc_base& other) const override
	{
		static_cast<stat_desc&>(other) = *this;
	}
	void deserialize(desc::desc_system&, const boost::json::object& obj) override
	{
		if (auto* p = obj.if_contains("hp")) hp = static_cast<int>(p->to_number<int64_t>());
		if (auto* p = obj.if_contains("label")) label = std::string(p->as_string());
		if (auto* p = obj.if_contains("limits")) {
			const auto& l = p->as_object();
			if (auto* mn = l.if_contains("min")) limit_min = static_cast<int>(mn->to_number<int64_t>());
			if (auto* mx = l.if_contains("max")) limit_max = static_cast<int>(mx->to_number<int64_t>());
		}
	}
	void serialize(boost::json::object& obj) const override
	{
		obj["hp"] = hp;
		obj["label"] = label;
		boost::json::object l;
		l["min"] = limit_min;
		l["max"] = limit_max;
		obj["limits"] = std::move(l);
	}
};

// Test-only component. spawn replaces the component; patch (hot reload) updates it in place and
// counts how many times it ran, so tests can tell the two paths apart.
inline void register_stat_desc(scn::registration_scope& scope)
{
	scope.desc<stat_desc>("stat_desc");
	scope.spawner("stat_desc",
		[](entt::registry& reg, entt::entity e, const desc::desc_base& d, const std::string_view) {
			const auto& s = static_cast<const stat_desc&>(d);
			reg.emplace_or_replace<stat_component>(e, stat_component{ s.hp, s.limit_min, s.limit_max, s.label, 0 });
		},
		[](entt::registry& reg, entt::entity e, const desc::desc_base& d) {
			const auto& s = static_cast<const stat_desc&>(d);
			auto& c = reg.get_or_emplace<stat_component>(e);
			c.hp = s.hp;
			c.limit_min = s.limit_min;
			c.limit_max = s.limit_max;
			c.label = s.label;
			++c.patch_count;
		});
}

struct scene_env : ram_res_env
{
	scene_env()
	{
		descs = std::make_unique<desc::desc_system>(*rs);
		rs->registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(*rs), std::ref(*descs));
		assembler = std::make_unique<scn::ecs_assembler>(*descs, *rs);
		scope.emplace(scn::register_scene_types(*descs, *assembler, skinning));
		register_stat_desc(*scope);
	}

	~scene_env()
	{
		// destroy in dependency order (scope -> assembler -> desc -> resources)
		scope.reset();
		assembler.reset();
		descs.reset();
		rs.reset();
	}

	// Parses `text` as a desc object (`type` is filled in when the text has none) and upgrades it to the
	// current format version, the way the desc adapter does before create_instance.
	boost::json::object parse_migrated(std::string_view text, std::string_view type = "prefab_desc")
	{
		auto obj = boost::json::parse(text).as_object();
		if (!obj.contains("__type")) obj["__type"] = std::string{ type };
		descs->migrate_tree(obj, "test");
		return obj;
	}

	template<class T>
	std::shared_ptr<T> load(const res::tag& tag)
	{
		return rs->require_sync<T>(tag).get_sync();
	}

	std::unique_ptr<desc::desc_system> descs;
	std::unique_ptr<scn::ecs_assembler> assembler;
	rnd::skinning_manager skinning;
	std::optional<scn::registration_scope> scope;
};

}
