#pragma once

#include <GWCA/stdafx.h>
#include <GWCA/GameContainers/Array.h>
#include <GWCA/Utilities/Export.h>

#include <cstddef>
#include <cstdint>

namespace GW::Render {
    struct RenderBufferEntry {
        uint32_t type;
        uint32_t index;
        uint32_t param;
    };
    static_assert(sizeof(RenderBufferEntry) == 0xc);

    struct FrameRenderContext {
        Array<void*> render_frame_list;
        Array<void*> frame_array;
        Array<uint32_t> render_programs;
        Array<RenderBufferEntry> render_buffer;
    };
    static_assert(sizeof(FrameRenderContext) == 0x40);
    static_assert(sizeof(Array<void*>) == 0x10);
    static_assert(sizeof(Array<uint32_t>) == 0x10);
    static_assert(sizeof(Array<RenderBufferEntry>) == 0x10);

    struct SceneProgramList {
        void* data;
        uint32_t capacity;
        uint32_t size;
    };
    static_assert(sizeof(SceneProgramList) == 0xc);
    static_assert(offsetof(SceneProgramList, capacity) == 0x4);
    static_assert(offsetof(SceneProgramList, size) == 0x8);

    struct FrameRenderBindings {
        void(__cdecl* FrCacheRenderAll)(uint32_t render_target, float delta_time);
        FrameRenderContext* frame_context;
    };

    struct WorldRenderBindings {
        void(__fastcall* EnvUpdateSceneLightDir)(void* ecx, void* edx, float* sun_dir, void* old_state, void* new_state);
        void(__cdecl* GrRenderPrograms)(int renderer, uint32_t count, const uint32_t* programs, uint32_t flags);
        void(__cdecl* GrRenderProgramQueue)(uint32_t queue, void* program, void* renderer_state, int mask, uint32_t flags, void* draw_lists);
        void(__cdecl* GrTransformIdentity)(int slot);
        void(__cdecl* GrTransformSetAdjustedLookAt)(float* eye, float* target, float* up, int flip_z, int relative_xy);
        void(__cdecl* GrTransformPerspective)(int slot, float fov, float depth_scale, float aspect_ratio, int mode);
        void(__cdecl* GrTransformOrthographic)(int slot, float* bounds, float depth_scale);
        float*(__cdecl* GrTransformGetCurrent)(int slot);
        void(__cdecl* GrTransformSetCurrent)(int slot, float* matrix);
        void(__cdecl* GrTransformSnapshotRelease)(void* snapshot);
        void(__cdecl* GrRenderSceneLists)(void* programs, void* secondary_programs, int mode, void* clear_color, int render_target);
        void(__cdecl* GmViewBuildSceneLists)(void* frame, float* delta_time);
        void(__cdecl* GmWorldUpdateView)(float delta_time, float* eye, float* target, float* up, float* clear_color);
        void(__cdecl* GmWorldBuildPrimaryScenePrograms)(float delta_time, void* programs, int* first_program, int* program_count);
        void(__cdecl* GmWorldSetPrimarySceneBuildEnabled)(int enabled);
        void(__cdecl* GmWorldBuildRemainingScenePrograms)(float delta_time, void* programs, void* secondary_programs, int* has_sky, void* clear_color);
        void(__cdecl* GrSetCameraTransform)(float eye_x, float eye_y, float eye_z, float dir_x, float dir_y, float dir_z);
        void(__thiscall* TrnCollectScenePrograms)(void* terrain_view, void* terrain_map, int view, SceneProgramList* programs);
        void(__cdecl* EnvSkyBuildScenePrograms)(void* env_sky, void* camera, void* programs);
        uint32_t*(__cdecl* GetViewVisibilityBuffer)(int view, int category, uint32_t** end);
        int(__thiscall* GrModelFrustumCull)(void* bounds, float* frustum, float depth, int plane_count, int extra_planes, float* sort_depth);
        void(__cdecl* GrCullBuildVisibilityGrid)(float* cell_size, int* range, int output, void* optional);
        void(__cdecl* GmPropUpdateFade)(void* manager, float delta_time, void* prop);
        void(__fastcall* GmSelectVisibleCells)(void* ecx, void* edx, float* eye, void* vis_buf_0, void* vis_buf_1);
        void(__cdecl* GmPropsUpdateView)(void* prop_manager, float* eye, float* target, float lod_scale);
        int(__thiscall* TrnTexShadowDecompressTile)(void* context, int terrain_texture, int* budget);
        int(__stdcall* TrnTexComposeTileLighting)(void* trn_tex, void* tile_coord, void* tile, int* budget);
        void(__fastcall* TrnTexWaitForResidency)(void* ecx, void* edx, uint32_t manager, uint32_t texture, uint32_t budget);
        void(__cdecl* AvShadowBuild)(void* shadow_handle, void* mesh, float* model_pos, int param4, int param5, int param6, float scale, int* out_rendered);
        void(__cdecl* CameraRenderScope)(int enabled);
        void(__cdecl* SceneRenderScope)(int enabled);
        float(__cdecl* GetPropLodScale)(void* view);
        void(__cdecl* FrCacheRenderAll)(uint32_t render_target, float delta_time);
        FrameRenderContext* frame_context;
        SceneProgramList* world_scene_programs;
        SceneProgramList* world_secondary_scene_programs;
        uint32_t* sky_clear_colour;
    };

    GWCA_API const WorldRenderBindings* GetWorldRenderBindings();
    GWCA_API const FrameRenderBindings* GetFrameRenderBindings();
}
