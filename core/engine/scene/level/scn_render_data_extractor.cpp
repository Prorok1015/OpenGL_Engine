#include "scn_render_data_extractor.h"
#include "scn_world.h"
#include <memory_resource>
#include <snk/common/eng_transform_3d.hpp>
#include "rnd_light_data.hpp"
#include "rnd_render_packet.hpp"
#include "rnd_render_system.h"
#include "shader/rnd_material_compiler.h"
#include "scn_camera_component.hpp"
#include "scn_material_desc.h"
#include "scn_model.h"
#include <snk/common/eng_profiler.h>
#include <snk/common/mem_allocator.h>

void scn::render_data_extractor::extract(rnd::frame_context& context) {
    PROFILE_SCOPE("ExtractRenderData");
    // Managers are injected into frame_context by render_system::begin_frame, which the `extract`
    // stage runs before this extractor. Catch a missing/mis-ordered injection early.
    ASSERT_MSG(context.textures, "frame_context render managers not injected (EPIC-49 US-49-6)");
    ASSERT_MSG(context.material_compiler, "frame_context material_compiler not injected (EPIC-49 US-49-5)");
    auto& level = m_level_manager.get_level();

    std::pmr::vector<rnd::render_packet_t> packets(ds::frame_allocator());

    level.for_each_world([&](scn::world& w) {
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
            packet.camera.projection_matrix = glm::perspective(glm::radians(cam.fov), (float)vp_rect.z / (float)vp_rect.w,
                cam.near_distance, cam.far_distance);
            packet.camera.color_targets = cam.color_targets;
            packet.camera.depth_target = cam.depth_target;
            packet.camera.tp_accum_target = cam.tp_accum_target;
            packet.camera.tp_reveal_target = cam.tp_reveal_target;
            packet.camera.render_priority = 0;

            auto meshes_view = reg.view<scn::name_component, scn::mesh_component, scn::geometry_component, scn::material_desc_component, scn::world_transform>();
            auto mesh_count = meshes_view.size_hint();
            packet.opaque_draws.reserve(mesh_count);
            packet.transparent_draws.reserve(mesh_count);
            for (auto&& [m_ent, name, mesh, geom, mat, m_trans] : meshes_view.each()) {

                if (!mat.mlt_desc.is_ready()) {
                    continue;
                }

                auto& mlt = *mat.mlt_desc;

                rnd::draw_call_t dc;
                {
                    PROFILE_SCOPE("Extract.BuildDC");
                    dc.geometry_tag = geom.geom_tag;
                    dc.indices_count = static_cast<uint32_t>(mesh.mesh.get_indices_count());
                    dc.vx_begin = static_cast<uint32_t>(mesh.mesh.vx_begin);
                    dc.ind_begin = static_cast<uint32_t>(mesh.mesh.ind_begin);
                    dc.transform = m_trans.world;
                }

                const auto queue = mlt.queue;

                // Resolve skinning before compiling: the compiler bakes USE_ANIMATION from
                // ctx.has_skinning (was an extractor mutation on the returned shader_cfg).
                bool has_skinning = false;
                {
                    PROFILE_SCOPE("Extract.Skinning");
                    if (auto* skin = reg.try_get<scn::skinning_component>(m_ent)) {
                        auto skel_ent = skin->skeleton_entity;
                        if (skel_ent != entt::null &&
                            reg.valid(skel_ent) &&
                            reg.all_of<scn::bone_matrices_component>(skel_ent)) {

                            auto& matrices = reg.get<scn::bone_matrices_component>(skel_ent);

                            dc.bone_matrices = &matrices.matrices;
                            dc.skinning_tag = skin->skinning_tag;

                            has_skinning = true;
                        }
                    }
                }

                rnd::shader_config shader_cfg;
                {
                    PROFILE_SCOPE("Extract.ShaderDesc");
                    rnd::material_compile_context mctx{ &m_trans.world, context.textures, has_skinning };
                    shader_cfg = context.material_compiler->compile(mlt, mctx);
                }

                {
                    PROFILE_SCOPE("Extract.PushDraws");
                    bool needs_opaque = (queue == scn::pass_queue::OPAQUE || queue == scn::pass_queue::MIX);
                    bool needs_transparent = (queue == scn::pass_queue::TRANSPARENT || queue == scn::pass_queue::MIX);

                    if (needs_opaque && needs_transparent) {
                        rnd::draw_call_t opaque_dc = dc;
                        opaque_dc.material = shader_cfg;
                        opaque_dc.material.cdata.defines.push_back("OPAQUE");
                        opaque_dc.sort_key = opaque_dc.material.get_hash();
                        packet.opaque_draws.push_back(std::move(opaque_dc));

                        dc.material = std::move(shader_cfg);
                        dc.sort_key = dc.material.get_hash();
                        packet.transparent_draws.push_back(std::move(dc));
                    } else if (needs_opaque) {
                        dc.material = std::move(shader_cfg);
                        dc.material.cdata.defines.push_back("OPAQUE");
                        dc.sort_key = dc.material.get_hash();
                        packet.opaque_draws.push_back(std::move(dc));
                    } else if (needs_transparent) {
                        dc.material = std::move(shader_cfg);
                        dc.sort_key = dc.material.get_hash();
                        packet.transparent_draws.push_back(std::move(dc));
                    }
                }
            }

            auto skys_view = reg.view<scn::sky_component, scn::material_desc_component, scn::renderable>();
            for (auto [sky_ent, sky, mat] : skys_view.each()) {
                if (mat.mlt_desc.is_ready()) {
                    // Skybox: no directional-light/skinning baking; uWorldMeshMatr only if the sky
                    // entity has a world transform (mirrors old handle.all_of<world_transform>()).
                    const glm::mat4* sky_world = nullptr;
                    if (auto* sky_trans = reg.try_get<scn::world_transform>(sky_ent)) {
                        sky_world = &sky_trans->world;
                    }
                    rnd::material_compile_context mctx{ sky_world, context.textures, false };
                    packet.skybox_material = context.material_compiler->compile(*mat.mlt_desc, mctx);
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
