#include <snk/scene/level/scn_render_data_extractor.h>
#include <snk/scene/level/scn_screen_anchor.h>
#include <snk/scene/level/scn_world.h>
#include <memory_resource>
#include <unordered_map>
#include <snk/common/eng_transform_3d.hpp>
#include <snk/render/rnd_light_data.hpp>
#include <snk/render/rnd_material_instance_cache.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_material_desc.h>
#include <snk/scene/scn_model.h>
#include <snk/common/eng_profiler.h>
#include <snk/common/mem_allocator.h>

namespace {
    rnd::clear_mode clear_mode_of(scn::camera_clear clear)
    {
        switch (clear) {
        case scn::camera_clear::depth: return rnd::clear_mode::depth;
        case scn::camera_clear::none: return rnd::clear_mode::none;
        default: return rnd::clear_mode::all;
        }
    }
}

void scn::render_data_extractor::extract(rnd::frame_context& context) {
    PROFILE_SCOPE("ExtractRenderData");
    // The services are injected into frame_context by render_system::begin_frame, which the `extract`
    // stage runs before this extractor. Catch a missing/mis-ordered injection early.
    ASSERT_MSG(context.materials, "frame_context material instance cache not injected (RHI R6b)");

    std::pmr::vector<rnd::render_packet_t> packets(ds::frame_allocator());

    // Markers over objects (WORLD_MESSAGING 3): resolved here, after every update of the frame, from the
    // current transforms and cameras, so a marker never lags behind its target.
    scn::resolve_screen_anchors(m_contexts, context.backbuffer_size);

    m_contexts.for_each([&](scn::world_context& w) {
        if (!w.extract()) return;
        auto& reg = w.state();

        // Collect per-world lights
        rnd::scene_lights_t world_lights;
        for (auto [l_ent, light] : reg.view<scn::directional_light>().each()) {
            rnd::scene_lights_t::directional_light_t dl;
            dl.direction = light.direction;
            dl.diffuse   = light.diffuse;
            dl.ambient   = light.ambient;
            dl.specular  = light.specular;
            world_lights.directional.push_back(dl);
        }

        for (auto [ent, cam] : reg.view<scn::camera_component>().each()) {
            const auto camera_viewport = rnd::effective_camera_viewport(
                cam.m_viewport, rnd::targets_backbuffer(cam.color_targets), context.backbuffer_size);
            if (!camera_viewport) {
                continue;
            }

            rnd::render_packet_t packet;
            packet.lights = world_lights;

            packet.camera.view_matrix = glm::mat4{ 1.0f };
            packet.camera.view_position = glm::vec3{ 0.0f };

            if (auto* trans = reg.try_get<scn::world_transform>(ent)) {
                packet.camera.view_matrix = glm::inverse(trans->world);
                packet.camera.view_position = glm::vec3(trans->world[3]);
            }

            const glm::ivec4 vp_rect = *camera_viewport;
            packet.camera.viewport = vp_rect;
            packet.camera.projection_matrix = scn::camera_projection_matrix(cam, vp_rect);
            packet.camera.color_targets = cam.color_targets;
            packet.camera.depth_target = cam.depth_target;
            packet.camera.tp_accum_target = cam.tp_accum_target;
            packet.camera.tp_reveal_target = cam.tp_reveal_target;
            packet.camera.render_priority = cam.layer;
            packet.camera.clear = clear_mode_of(cam.clear);

            auto meshes_view = reg.view<scn::name_component, scn::mesh_component, scn::geometry_component, scn::material_desc_component, scn::world_transform>();
            auto mesh_count = meshes_view.size_hint();
            packet.opaque_draws.reserve(mesh_count);
            packet.transparent_draws.reserve(mesh_count);
            // Every skeleton is copied at most once per packet, so the palettes together bound the copy.
            std::size_t bone_estimate = 0;
            std::size_t skeleton_estimate = 0;
            for (auto [skel_ent, bones] : reg.view<scn::bone_matrices_component>().each()) {
                bone_estimate += bones.matrices.size();
                ++skeleton_estimate;
            }
            packet.bones.reserve(bone_estimate, skeleton_estimate);
            const std::size_t skinned_mesh_estimate = reg.view<scn::skinning_component>().size();

            // Draws name their geometry and skin by index into the packet's tables; each distinct one is added
            // once. Their material is an id of the material instance cache.
            packet.geometries.reserve(mesh_count);
            packet.skins.reserve(skinned_mesh_estimate);
            std::pmr::unordered_map<res::tag, uint32_t> geometry_ids(ds::frame_allocator());
            std::pmr::unordered_map<res::tag, uint32_t> skin_ids(ds::frame_allocator());
            geometry_ids.reserve(mesh_count);
            const auto intern = [](auto& ids, auto& table, const auto& value) {
                const auto [it, added] = ids.try_emplace(value, static_cast<uint32_t>(table.size()));
                if (added) {
                    table.push_back(value);
                }
                return it->second;
            };
            for (auto&& [m_ent, name, mesh, geom, mat, m_trans] : meshes_view.each()) {

                const auto mlt_ptr = mat.mlt_desc.get();
                if (!mlt_ptr) {
                    continue;
                }

                auto& mlt = *mlt_ptr;

                rnd::draw_call_t dc;
                {
                    PROFILE_SCOPE("Extract.BuildDC");
                    dc.geometry.index = intern(geometry_ids, packet.geometries, geom.geom_tag);
                    dc.indices_count = static_cast<uint32_t>(mesh.mesh.get_indices_count());
                    dc.vx_begin = static_cast<uint32_t>(mesh.mesh.vx_begin);
                    dc.ind_begin = static_cast<uint32_t>(mesh.mesh.ind_begin);
                    dc.transform = m_trans.world;
                }

                const auto queue = mlt.queue;

                // Resolve skinning before asking for the instance: the compiler bakes USE_ANIMATION from
                // ctx.has_skinning, which is part of the instance's key.
                bool has_skinning = false;
                {
                    PROFILE_SCOPE("Extract.Skinning");
                    if (auto* skin = reg.try_get<scn::skinning_component>(m_ent)) {
                        auto skel_ent = skin->skeleton_entity;
                        if (skel_ent != entt::null &&
                            reg.valid(skel_ent) &&
                            reg.all_of<scn::bone_matrices_component>(skel_ent)) {

                            auto& matrices = reg.get<scn::bone_matrices_component>(skel_ent);

                            dc.bone_offset = packet.bones.add(&matrices.matrices, matrices.matrices);
                            dc.bone_count = static_cast<uint32_t>(matrices.matrices.size());
                            if (skin->skinning_tag.is_valid()) {
                                dc.skin.index = intern(skin_ids, packet.skins, skin->skinning_tag);
                            }

                            has_skinning = true;
                        }
                    }
                }

                const rnd::compiled_material* material = nullptr;
                {
                    PROFILE_SCOPE("Extract.ShaderDesc");
                    // Compiled (and its textures asked for) by the cache, and only when the desc, skinning or a texture changed.
                    dc.material = context.materials->acquire(mlt_ptr, has_skinning, mat.dirty);
                    mat.dirty = false;
                    material = &context.materials->get(dc.material);
                }
                // Distance along the view axis: the transparent order, and the tie-break among equal opaque draws.
                const glm::vec4 view_pos = packet.camera.view_matrix * glm::vec4(glm::vec3(m_trans.world[3]), 1.0f);
                const float view_depth = -view_pos.z;

                {
                    PROFILE_SCOPE("Extract.PushDraws");
                    bool needs_opaque = (queue == scn::pass_queue::OPAQUE || queue == scn::pass_queue::MIX);
                    bool needs_transparent = (queue == scn::pass_queue::TRANSPARENT || queue == scn::pass_queue::MIX);

                    if (needs_opaque) {
                        rnd::draw_call_t opaque_dc = dc;
                        opaque_dc.variant = material->opaque;
                        opaque_dc.sort_key = rnd::make_sort_key(
                            rnd::draw_pass::opaque, opaque_dc.variant.value, opaque_dc.material.index, view_depth);
                        packet.opaque_draws.push_back(std::move(opaque_dc));
                    }
                    if (needs_transparent) {
                        dc.variant = material->base;
                        dc.sort_key = rnd::make_sort_key(
                            rnd::draw_pass::transparent, dc.variant.value, dc.material.index, view_depth);
                        packet.transparent_draws.push_back(std::move(dc));
                    }
                }
            }

            {
                PROFILE_SCOPE("Extract.SortDraws");
                rnd::sort_draws(packet.opaque_draws);
                rnd::sort_draws(packet.transparent_draws);
            }

            auto skys_view = reg.view<scn::sky_component, scn::material_desc_component, scn::renderable>();
            for (auto [sky_ent, sky, mat] : skys_view.each()) {
                if (const auto sky_desc = mat.mlt_desc.get()) {
                    // Skybox: no skinning; a world transform only if the sky entity has one.
                    rnd::skybox_draw_t sky_draw;
                    if (auto* sky_trans = reg.try_get<scn::world_transform>(sky_ent)) {
                        sky_draw.world = sky_trans->world;
                    }
                    sky_draw.material = context.materials->acquire(sky_desc, false, mat.dirty);
                    mat.dirty = false;
                    sky_draw.variant = context.materials->get(sky_draw.material).base;
                    packet.skybox_material = std::move(sky_draw);
                }
            }

            // Bone visualization: generate debug lines for skeletons with show_skeleton enabled
            auto skeleton_view = reg.view<scn::skeleton_component>();
            for (auto [skel_ent, skel] : skeleton_view.each()) {
                if (!skel.show_skeleton) continue;

                auto bones_view = reg.view<scn::bone_component, scn::world_transform, scn::parent_component>();
                for (auto [bone_ent, bone, bone_trans, parent_comp] : bones_view.each()) {
                    // Only draw bones belonging to this skeleton
                    auto* parent_trans = reg.try_get<scn::world_transform>(parent_comp.parent);
                    if (!parent_trans) continue;

                    glm::vec3 bone_pos = glm::vec3(bone_trans.world[3]);
                    glm::vec3 parent_pos = glm::vec3(parent_trans->world[3]);

                    rnd::debug_line_t line;
                    line.start = parent_pos;
                    line.end = bone_pos;
                    line.color = glm::vec4{0.0f, 1.0f, 0.0f, 1.0f};
                    packet.debug_draws.lines.push_back(line);
                }
            }

            packets.push_back(std::move(packet));
        }
    });

    context.data.construct<std::pmr::vector<rnd::render_packet_t>>(std::move(packets));
}
