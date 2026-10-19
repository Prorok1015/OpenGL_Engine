#include <snk/scene/level/scn_render_data_extractor.h>
#include <snk/scene/level/scn_world.h>
#include <memory_resource>
#include <snk/common/eng_transform_3d.hpp>
#include <snk/render/rnd_light_data.hpp>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_render_system.h>
#include <snk/render/shader/rnd_material_compiler.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_material_desc.h>
#include <snk/scene/scn_model.h>
#include <snk/common/eng_profiler.h>
#include <snk/common/mem_allocator.h>

namespace {
    // Returns the entity's compiled material, rebuilding it only when the material was marked dirty or one
    // of its inputs changed. The pointer stays valid for the frame: a replaced material is parked in `retired`.
    const rnd::compiled_material* acquire_material(
        entt::registry& reg, entt::entity ent, scn::material_desc_component& mat,
        const std::shared_ptr<scn::material_desc>& desc, bool skinned, rnd::frame_context& context,
        std::vector<std::shared_ptr<rnd::compiled_material>>& retired)
    {
        auto& cache = reg.get_or_emplace<scn::compiled_material_component>(ent);
        const auto generation = context.textures->generation();
        if (mat.dirty || !cache.material || !cache.material->is_current(desc.get(), skinned, generation)) {
            if (cache.material) {
                retired.push_back(std::move(cache.material));
            }
            cache.material = rnd::build_compiled_material(
                *context.material_compiler, context.shaders->variants(), *context.textures, desc, skinned);
            mat.dirty = false;
        }
        return cache.material.get();
    }
}

void scn::render_data_extractor::extract(rnd::frame_context& context) {
    PROFILE_SCOPE("ExtractRenderData");
    // Managers are injected into frame_context by render_system::begin_frame, which the `extract`
    // stage runs before this extractor. Catch a missing/mis-ordered injection early.
    ASSERT_MSG(context.textures, "frame_context render managers not injected (EPIC-49 US-49-6)");
    ASSERT_MSG(context.material_compiler, "frame_context material_compiler not injected (EPIC-49 US-49-5)");
    ASSERT_MSG(context.shaders, "frame_context shader manager not injected");
    m_retired_materials.clear();
    auto& variants = context.shaders->variants();

    std::pmr::vector<rnd::render_packet_t> packets(ds::frame_allocator());

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
            // Every skeleton is copied at most once per packet, so the palettes together bound the copy.
            std::size_t bone_estimate = 0;
            std::size_t skeleton_estimate = 0;
            for (auto [skel_ent, bones] : reg.view<scn::bone_matrices_component>().each()) {
                bone_estimate += bones.matrices.size();
                ++skeleton_estimate;
            }
            packet.bones.reserve(bone_estimate, skeleton_estimate);
            for (auto&& [m_ent, name, mesh, geom, mat, m_trans] : meshes_view.each()) {

                const auto mlt_ptr = mat.mlt_desc.get();
                if (!mlt_ptr) {
                    continue;
                }

                auto& mlt = *mlt_ptr;

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
                // ctx.has_skinning, which is part of the cached material's validity.
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
                            dc.skinning_tag = skin->skinning_tag;

                            has_skinning = true;
                        }
                    }
                }

                const rnd::compiled_material* material = nullptr;
                {
                    PROFILE_SCOPE("Extract.ShaderDesc");
                    material = acquire_material(reg, m_ent, mat, mlt_ptr, has_skinning, context, m_retired_materials);
                }
                dc.material = material;

                {
                    PROFILE_SCOPE("Extract.PushDraws");
                    bool needs_opaque = (queue == scn::pass_queue::OPAQUE || queue == scn::pass_queue::MIX);
                    bool needs_transparent = (queue == scn::pass_queue::TRANSPARENT || queue == scn::pass_queue::MIX);

                    if (needs_opaque) {
                        rnd::draw_call_t opaque_dc = dc;
                        opaque_dc.variant = material->opaque;
                        opaque_dc.sort_key = variants.key(opaque_dc.variant);
                        packet.opaque_draws.push_back(std::move(opaque_dc));
                    }
                    if (needs_transparent) {
                        dc.variant = material->base;
                        dc.sort_key = variants.key(dc.variant);
                        packet.transparent_draws.push_back(std::move(dc));
                    }
                }
            }

            auto skys_view = reg.view<scn::sky_component, scn::material_desc_component, scn::renderable>();
            for (auto [sky_ent, sky, mat] : skys_view.each()) {
                if (const auto sky_desc = mat.mlt_desc.get()) {
                    // Skybox: no skinning; a world transform only if the sky entity has one.
                    rnd::skybox_draw_t sky_draw;
                    if (auto* sky_trans = reg.try_get<scn::world_transform>(sky_ent)) {
                        sky_draw.world = sky_trans->world;
                    }
                    sky_draw.material = acquire_material(reg, sky_ent, mat, sky_desc, false, context, m_retired_materials);
                    sky_draw.variant = sky_draw.material->base;
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
