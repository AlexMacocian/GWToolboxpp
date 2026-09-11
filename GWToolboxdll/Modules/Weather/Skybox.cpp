#include "stdafx.h"

#include <DirectXMath.h>
#include <intrin.h>

#include <GWCA/Context/GameContext.h>
#include <GWCA/Context/MapContext.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Camera.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/CameraMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Managers/RenderMgr.h>
#include <GWCA/Managers/WorldRenderMgr.h>

#include <GWCA/Utilities/Hooker.h>

#include <Logger.h>
#include <Modules/WeatherModule.h>
#include <Modules/Weather/Skybox.h>
#include <Modules/Weather/Wind.h>
#include <Modules/Weather/WeatherEffects.h>
#include <Timer.h>
#include <Utils/GameWorldCompositor.h>

#include <Utils/ShadowCamera.h>

#include "Widgets/Minimap/Shaders/sky_fullscreen_vs.h"
#include "Widgets/Minimap/Shaders/sky_composite_ps.h"
#include "Widgets/Minimap/Shaders/world_ambient_ps.h"
#include "Widgets/Minimap/Shaders/world_water_ps.h"
#include "Widgets/Minimap/Shaders/world_fog_ps.h"
#include "Widgets/Minimap/Shaders/shadow_terrain_replay_vs.h"
#include "Widgets/Minimap/Shaders/shadow_terrain_replay_ps.h"
#include "Widgets/Minimap/Shaders/shadow_terrain_receive_vs.h"
#include "Widgets/Minimap/Shaders/shadow_terrain_receive_ps.h"
#include "Widgets/Minimap/Shaders/shadow_screenspace_ps.h"
#include "Widgets/Minimap/Shaders/ocean_initial_ps.h"
#include "Widgets/Minimap/Shaders/ocean_phase_ps.h"
#include "Widgets/Minimap/Shaders/ocean_spectrum_ps.h"
#include "Widgets/Minimap/Shaders/ocean_fft_ps.h"
#include "Widgets/Minimap/Shaders/ocean_normal_ps.h"

namespace {
    bool hooks_ready = false;
    bool hooks_failed = false;
    std::vector<void*> engine_hooks;
    std::vector<void*> device_hooks;

    bool IsEnabled();

    template <typename Function, typename Callback>
    bool CreateOwnedHook(Function& function, Callback callback, Function& original,
                         std::vector<void*>& hooks, const char* name)
    {
        const auto result = GW::Hook::CreateHook(
            reinterpret_cast<void**>(&function), reinterpret_cast<void*>(callback),
            reinterpret_cast<void**>(&original));
        if (result != 0) {
            Log::Error("Weather: cannot hook %s (status %d). Unload standalone Rebirth before enabling Weather.", name, result);
            return false;
        }
        hooks.push_back(reinterpret_cast<void*>(function));
        return true;
    }

    float sun_elevation_deg = 28.4f;
    float sun_azimuth_deg = 203.5f;
    float sun_disk_size_deg = 1.5f;
    float sun_disk_intensity = 12.0f;
    float eye_altitude_km = 1.35f;
    bool animate_sun = false;
    float animate_speed = 0.05f;

    bool sun_cycle_enabled = false;
    float sun_cycle_hour = 9.0f;
    float sun_cycle_minutes = 30.0f;

    float sun_cycle_latitude_deg = 42.0f;
    float sun_cycle_declination_deg = 12.0f;

    DirectX::XMFLOAT3 night_sky_colour = {0.0f, 0.003922f, 0.003922f};
    bool stars_enabled = true;
    float star_brightness = 1.0f;
    float star_density = 580.0f;
    float star_threshold = 9.0f;
    float star_twinkle_speed = 1.0f;

    bool aurora_enabled = true;
    float aurora_intensity = 0.0f;
    float aurora_azimuth_deg = 0.0f;
    float aurora_spread_deg = 90.0f;
    float aurora_drift_speed = 0.06f;
    float aurora_altitude = 0.8f;
    int aurora_steps = 24;
    DirectX::XMFLOAT3 aurora_colour = {1.0f, 1.0f, 1.0f};
    bool moon_enabled = true;
    float moon_size_deg = 1.5f;
    float moon_intensity = 1.5f;
    float moon_glow = 0.2f;
    DirectX::XMFLOAT3 moon_colour = {0.90f, 0.90f, 0.65f};
    DirectX::XMFLOAT3 moon_glow_colour = {0.95f, 0.50f, 0.30f};
    bool clouds_enabled = true;
    float cloud_coverage = 0.62f;
    float cloud_scale = 2.04f;
    float cloud_speed = 0.632f;
    float cloud_sharpness = 2.17f;
    float cloud_absorption = 1.04f;
    float cloud_phase_g = 0.65f;
    float cloud_opacity = 0.72f;

    float cloud_darkness = 0.46f;

    float cloud_overcast_start = 0.5f;
    float cloud_overcast_full = 1.0f;
    float cloud_overcast_level = 0.529f;

    float cloud_night_sky_ratio = 0.19f;
    float cloud_overcast_diffuse = 0.52f;
    float cloud_overcast_cool = 0.272f;

    float cloud_ambient_fill = 0.53f;

    float cloud_variety = 0.9f;
    float cloud_weather_scale = 0.2f;
    float cloud_warp = 0.5f;
    float cloud_billow = 0.64f;

    float cloud_shadow_strength = 0.75f;
    int cloud_shadow_octaves = 2;

    float cloud_altitude = 1500.0f;

    bool rainbow_enabled = true;
    float rainbow_intensity = 0.0f;

    bool rainbow_follow_sun = true;
    float rainbow_azimuth_deg = 180.0f;
    float rainbow_elevation_deg = -10.0f;
    float rainbow_radius_deg = 42.0f;
    float rainbow_width_deg = 2.4f;
    float rainbow_secondary = 0.35f;
    float rainbow_saturation = 0.75f;

    bool godrays_enabled = true;

    float godray_intensity = 0.4f;
    float godray_glow = 0.5f;
    float godray_extinction = 0.9f;
    float godray_spread_deg = 40.0f;

    float godray_plateau = 0.64f;

    float godray_reach = 0.30f;
    float godray_contrast = 1.1f;
    int godray_steps = 8;
    DirectX::XMFLOAT3 godray_colour = {1.0f, 0.93f, 0.78f};

    bool world_overcast_dimming = true;

    bool world_fog_enabled = true;

    DirectX::XMFLOAT3 world_fog_day_colour = {0.384f, 0.388f, 0.392f};
    float world_fog_start = 3000.0f;
    float world_fog_end = 30000.0f;
    float world_fog_strength = 1.0f;

    float world_fog_night_bias = 0.3f;

    float world_fog_overcast_gain = 0.45f;

    float world_fog_edge_repair = 0.0f;
    int world_fog_edge_radius = 1;

    bool world_fog_debug_view = false;

    bool world_lights_enabled = true;
    float world_light_reach = 0.35f;
    float world_light_strength = 1.0f;
    float world_light_falloff = 3.0f;
    float world_light_cull_distance = 6000.0f;

    bool player_light_enabled = true;
    DirectX::XMFLOAT3 player_light_colour = {1.0f, 0.88f, 0.68f};
    float player_light_intensity = 0.8f;
    float player_light_radius = 1500.0f;
    float player_light_core = 0.2f;
    float player_light_height = 90.0f;
    bool water_enabled = true;

    float water_strength = 0.85f;

    float water_clarity_depth = 260.0f;

    float water_foam_depth = 30.0f;
    float water_foam_intensity = 1.5f;
    float water_scatter = 0.20f;
    float water_specular_power = 300.0f;
    float water_relief_steps = 12.0f;
    DirectX::XMFLOAT3 water_deep_colour = {0.02f, 0.09f, 0.13f};
    DirectX::XMFLOAT3 water_shallow_colour = {0.16f, 0.42f, 0.42f};
    DirectX::XMFLOAT3 water_foam_colour = {0.92f, 0.96f, 0.98f};
    bool world_ambient_enabled = true;
    DirectX::XMFLOAT3 ambient_day_colour = {1.0f, 1.0f, 1.0f};

    float day_brightness = 1.5f;
    DirectX::XMFLOAT3 ambient_night_colour = {0.337f, 0.337f, 0.400f};
    bool align_shadows = true;
    bool shadow_replay_preview = false;
    bool shadow_replay_light_camera = false;
    bool shadow_replay_terrain_only = false;
    bool shadow_gpu_terrain_replay = false;
    bool shadow_gpu_draw_shadows = true;
    bool disable_baked_terrain_shadows = true;
    bool shadow_cast_agent_shadows = true;
    bool disable_agent_baked_shadows = true;
    float shadow_gpu_bias = 4.0f;
    float shadow_gpu_slope_bias = 0.0f;
    float shadow_gpu_strength = 0.55f;
    float moon_shadow_strength = 0.25f;
    int shadow_screenspace_debug_view = 0;
    float shadow_light_camera_distance = 2552.0f;
    float shadow_light_camera_radius = 1994.0f;
    float shadow_light_camera_forward_bias = 0.2f;
    float shadow_light_camera_fov = 90.0f;
    bool shadow_light_camera_invert_direction = false;
    bool shadow_light_camera_orthographic = true;
    int debug_mode = 0;

    constexpr std::array<uint32_t, 5> kShadowMapSizes = {256, 512, 1024, 2048, 4096};
    int shadow_map_size_index = 4;
    uint32_t shadow_map_size = kShadowMapSizes[4];

    uint32_t shadow_map_size_active = 0;

    constexpr float kShadowVerticalCoverage = 2000.0f;

    int compositor_token = 0;
    int shadow_compositor_token = 0;

    IDirect3DVertexShader9* sky_vs = nullptr;
    IDirect3DPixelShader9* composite_ps = nullptr;
    DirectX::XMFLOAT2 cloud_scroll = {0.0f, 0.0f};

    DirectX::XMFLOAT2 cloud_origin = {0.0f, 0.0f};

    DirectX::XMFLOAT3 shadow_ambient_applied = {1.0f, 1.0f, 1.0f};
    uint32_t shadow_ambient_draw_count = 0;
    int shadow_world_light_count = 0;
    IDirect3DPixelShader9* world_ambient_ps_object = nullptr;
    IDirect3DPixelShader9* world_water_ps_object = nullptr;
    IDirect3DPixelShader9* world_fog_ps_object = nullptr;
    IDirect3DVertexDeclaration9* sky_decl = nullptr;
    IDirect3DTexture9* shadow_preview_texture = nullptr;
    IDirect3DTexture9* shadow_preview_depth_texture = nullptr;
    IDirect3DTexture9* shadow_camera_depth_texture = nullptr;
    IDirect3DSurface9* shadow_preview_surface = nullptr;
    IDirect3DSurface9* shadow_preview_depth = nullptr;
    IDirect3DVertexShader9* shadow_terrain_replay_vs_object = nullptr;
    IDirect3DPixelShader9* shadow_terrain_replay_ps_object = nullptr;
    IDirect3DVertexShader9* shadow_terrain_receive_vs_object = nullptr;
    IDirect3DPixelShader9* shadow_terrain_receive_ps_object = nullptr;
    IDirect3DPixelShader9* shadow_screenspace_ps_object = nullptr;

    uint32_t shadow_depth_surface_width = 0;
    uint32_t shadow_depth_surface_height = 0;
    uint32_t shadow_camera_depth_width = 0;
    uint32_t shadow_camera_depth_height = 0;
    bool shadow_camera_depth_copy_succeeded = false;

    const char* shadow_camera_depth_failure = "none";
    bool shadow_camera_depth_resz_supported = false;

    D3DFORMAT shadow_depth_surface_format = D3DFMT_UNKNOWN;
    D3DMULTISAMPLE_TYPE shadow_depth_surface_msaa = D3DMULTISAMPLE_NONE;
    DWORD shadow_depth_surface_msaa_quality = 0;
    uint32_t shadow_render_target_width = 0;
    uint32_t shadow_render_target_height = 0;
    D3DFORMAT shadow_render_target_format = D3DFMT_UNKNOWN;
    D3DMULTISAMPLE_TYPE shadow_render_target_msaa = D3DMULTISAMPLE_NONE;
    DWORD shadow_render_target_msaa_quality = 0;
    uint32_t shadow_reported_viewport_width = 0;
    uint32_t shadow_reported_viewport_height = 0;
    D3DVIEWPORT9 shadow_device_viewport{};
    bool shadow_device_viewport_valid = false;
    DWORD shadow_replay_saved_z_enable = D3DZB_TRUE;
    DWORD shadow_replay_saved_z_write = TRUE;
    DWORD shadow_replay_saved_z_func = D3DCMP_LESSEQUAL;
    bool shadow_replay_depth_guarded_this_draw = false;
    uint32_t shadow_replay_depth_wipe_blocked = 0;
    std::array<uint32_t, 36> shadow_replay_depth_histogram{};
    uint32_t shadow_screenspace_draw_count = 0;

    bool resources_failed = false;
    bool shadow_preview_failed = false;
    bool shadow_preview_depth_sampleable = false;
    D3DFORMAT shadow_preview_format = D3DFMT_UNKNOWN;
    uint32_t shadow_preview_depth_precision = 8;

    bool IsEnabled()
    {
        return hooks_ready && !resources_failed && WeatherModule::IsAtmosphereEnabled();
    }

    using SceneProgramList = GW::Render::SceneProgramList;

    using GrRenderPrograms_pt = decltype(GW::Render::WorldRenderBindings::GrRenderPrograms);
    using GrRenderProgramQueue_pt = decltype(GW::Render::WorldRenderBindings::GrRenderProgramQueue);
    using GrTransformIdentity_pt = decltype(GW::Render::WorldRenderBindings::GrTransformIdentity);
    using GrTransformSetAdjustedLookAt_pt = decltype(GW::Render::WorldRenderBindings::GrTransformSetAdjustedLookAt);
    using GrTransformPerspective_pt = decltype(GW::Render::WorldRenderBindings::GrTransformPerspective);
    using GrTransformOrthographic_pt = decltype(GW::Render::WorldRenderBindings::GrTransformOrthographic);
    using GrTransformGetCurrent_pt = decltype(GW::Render::WorldRenderBindings::GrTransformGetCurrent);
    using GrTransformSetCurrent_pt = decltype(GW::Render::WorldRenderBindings::GrTransformSetCurrent);
    using GrTransformSnapshotRelease_pt = decltype(GW::Render::WorldRenderBindings::GrTransformSnapshotRelease);
    using GrRenderSceneLists_pt = decltype(GW::Render::WorldRenderBindings::GrRenderSceneLists);
    using GmViewBuildSceneLists_pt = decltype(GW::Render::WorldRenderBindings::GmViewBuildSceneLists);
    using GmWorldUpdateView_pt = decltype(GW::Render::WorldRenderBindings::GmWorldUpdateView);
    using GmWorldBuildPrimaryScenePrograms_pt = decltype(GW::Render::WorldRenderBindings::GmWorldBuildPrimaryScenePrograms);
    using GmWorldSetPrimarySceneBuildEnabled_pt = decltype(GW::Render::WorldRenderBindings::GmWorldSetPrimarySceneBuildEnabled);
    using GmWorldBuildRemainingScenePrograms_pt = decltype(GW::Render::WorldRenderBindings::GmWorldBuildRemainingScenePrograms);
    using GrSetCameraTransform_pt = decltype(GW::Render::WorldRenderBindings::GrSetCameraTransform);
    using TrnCollectScenePrograms_pt = decltype(GW::Render::WorldRenderBindings::TrnCollectScenePrograms);

    using EnvSkyBuildScenePrograms_pt = decltype(GW::Render::WorldRenderBindings::EnvSkyBuildScenePrograms);
    using GetViewVisibilityBuffer_pt = decltype(GW::Render::WorldRenderBindings::GetViewVisibilityBuffer);
    using GrModelFrustumCull_pt = decltype(GW::Render::WorldRenderBindings::GrModelFrustumCull);
    using GrCullBuildVisibilityGrid_pt = decltype(GW::Render::WorldRenderBindings::GrCullBuildVisibilityGrid);

    GrRenderPrograms_pt GrRenderPrograms_Func = nullptr;
    GrRenderPrograms_pt GrRenderPrograms_Ret = nullptr;
    GrRenderProgramQueue_pt GrRenderProgramQueue_Func = nullptr;
    GrRenderProgramQueue_pt GrRenderProgramQueue_Ret = nullptr;
    GrTransformIdentity_pt GrTransformIdentity_Func = nullptr;
    GrTransformSetAdjustedLookAt_pt GrTransformSetAdjustedLookAt_Func = nullptr;
    GrTransformPerspective_pt GrTransformPerspective_Func = nullptr;
    GrTransformOrthographic_pt GrTransformOrthographic_Func = nullptr;
    GrTransformGetCurrent_pt GrTransformGetCurrent_Func = nullptr;
    GrTransformSetCurrent_pt GrTransformSetCurrent_Func = nullptr;
    GrTransformSnapshotRelease_pt GrTransformSnapshotRelease_Func = nullptr;
    GrRenderSceneLists_pt GrRenderSceneLists_Func = nullptr;
    GrRenderSceneLists_pt GrRenderSceneLists_Ret = nullptr;
    GmViewBuildSceneLists_pt GmViewBuildSceneLists_Func = nullptr;
    GmViewBuildSceneLists_pt GmViewBuildSceneLists_Ret = nullptr;
    GmWorldUpdateView_pt GmWorldUpdateView_Func = nullptr;
    GmWorldBuildPrimaryScenePrograms_pt GmWorldBuildPrimaryScenePrograms_Func = nullptr;
    GmWorldSetPrimarySceneBuildEnabled_pt
        GmWorldSetPrimarySceneBuildEnabled_Func = nullptr;
    GmWorldBuildRemainingScenePrograms_pt
        GmWorldBuildRemainingScenePrograms_Func = nullptr;
    GrSetCameraTransform_pt GrSetCameraTransform_Func = nullptr;
    TrnCollectScenePrograms_pt TrnCollectScenePrograms_Func = nullptr;
    TrnCollectScenePrograms_pt TrnCollectScenePrograms_Ret = nullptr;
    EnvSkyBuildScenePrograms_pt EnvSkyBuildScenePrograms_Func = nullptr;
    EnvSkyBuildScenePrograms_pt EnvSkyBuildScenePrograms_Ret = nullptr;
    GetViewVisibilityBuffer_pt GetViewVisibilityBuffer_Func = nullptr;
    GetViewVisibilityBuffer_pt GetViewVisibilityBuffer_Ret = nullptr;
    GrModelFrustumCull_pt GrModelFrustumCull_Func = nullptr;
    GrModelFrustumCull_pt GrModelFrustumCull_Ret = nullptr;
    GrCullBuildVisibilityGrid_pt GrCullBuildVisibilityGrid_Func = nullptr;
    GrCullBuildVisibilityGrid_pt GrCullBuildVisibilityGrid_Ret = nullptr;

    using GmPropUpdateFade_pt = decltype(GW::Render::WorldRenderBindings::GmPropUpdateFade);
    GmPropUpdateFade_pt GmPropUpdateFade_Func = nullptr;
    GmPropUpdateFade_pt GmPropUpdateFade_Ret = nullptr;
    bool shadow_skip_prop_fade = true;
    uint32_t shadow_prop_fades_skipped = 0;

    using GmSelectVisibleCells_pt = decltype(GW::Render::WorldRenderBindings::GmSelectVisibleCells);
    GmSelectVisibleCells_pt GmSelectVisibleCells_Func = nullptr;
    GmSelectVisibleCells_pt GmSelectVisibleCells_Ret = nullptr;

    bool shadow_cell_override_active = false;
    float shadow_cell_override_eye[3]{};
    uint32_t shadow_cell_overrides = 0;

    using GmPropsUpdateView_pt = decltype(GW::Render::WorldRenderBindings::GmPropsUpdateView);
    GmPropsUpdateView_pt GmPropsUpdateView_Func = nullptr;
    void(__cdecl* CameraRenderScope_Func)(int) = nullptr;
    void(__cdecl* SceneRenderScope_Func)(int) = nullptr;
    float(__cdecl* GetPropLodScale_Func)(void*) = nullptr;

    using TrnTexShadowDecompressTile_pt = decltype(GW::Render::WorldRenderBindings::TrnTexShadowDecompressTile);
    TrnTexShadowDecompressTile_pt TrnTexShadowDecompressTile_Func = nullptr;
    TrnTexShadowDecompressTile_pt TrnTexShadowDecompressTile_Ret = nullptr;

    using TrnTexComposeTileLighting_pt = decltype(GW::Render::WorldRenderBindings::TrnTexComposeTileLighting);
    TrnTexComposeTileLighting_pt TrnTexComposeTileLighting_Func = nullptr;
    TrnTexComposeTileLighting_pt TrnTexComposeTileLighting_Ret = nullptr;

    constexpr size_t kCoarseShadowMaskBytes = 0x80;
    uint32_t shadow_baked_blocks_cleared = 0;

    using TrnTexWaitForResidency_pt = decltype(GW::Render::WorldRenderBindings::TrnTexWaitForResidency);
    TrnTexWaitForResidency_pt TrnTexWaitForResidency_Func = nullptr;
    TrnTexWaitForResidency_pt TrnTexWaitForResidency_Ret = nullptr;
    using AvShadowBuild_pt = decltype(GW::Render::WorldRenderBindings::AvShadowBuild);
    AvShadowBuild_pt AvShadowBuild_Func = nullptr;
    AvShadowBuild_pt AvShadowBuild_Ret = nullptr;

    SceneProgramList* world_scene_programs = nullptr;
    bool shadow_scene_rebuild_active = false;
    bool shadow_explicit_camera_render_active = false;
    float shadow_scene_eye[3]{};
    float shadow_scene_target[3]{};
    float shadow_scene_up[3]{};
    bool shadow_terrain_collection_active = false;

    std::array<uint32_t, 0x1450> shadow_terrain_visibility{};

    bool shadow_cell_visibility_override_active = false;
    uint32_t shadow_cell_visibility_override_count = 0;
    uint32_t shadow_cell_visibility_too_large_count = 0;
    std::vector<uint32_t> shadow_terrain_program_handles;
    const uint32_t* shadow_program_batch_handles = nullptr;
    uint32_t shadow_program_batch_count = 0;
    uint32_t shadow_program_batch_index = 0;
    bool shadow_terrain_program_queue_active = false;
    uint32_t shadow_terrain_collect_count = 0;
    uint32_t shadow_terrain_visibility_override_count = 0;
    uint32_t shadow_terrain_queue_count = 0;
    uint32_t shadow_terrain_cull_bypass_count = 0;
    uint32_t shadow_terrain_original_absolute_count = 0;
    uint32_t shadow_terrain_original_relative_count = 0;

    using SetRenderTarget_pt = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
    using SetDepthStencilSurface_pt = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*);
    using SetRenderState_pt = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
    using SetViewport_pt = HRESULT(WINAPI*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
    using DrawPrimitive_pt = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    using DrawIndexedPrimitive_pt = HRESULT(WINAPI*)(
        IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    SetRenderTarget_pt SetRenderTarget_Func = nullptr, SetRenderTarget_Ret = nullptr;
    SetDepthStencilSurface_pt SetDepthStencilSurface_Func = nullptr, SetDepthStencilSurface_Ret = nullptr;
    SetViewport_pt SetViewport_Func = nullptr, SetViewport_Ret = nullptr;
    DrawPrimitive_pt DrawPrimitive_Func = nullptr, DrawPrimitive_Ret = nullptr;
    DrawIndexedPrimitive_pt DrawIndexedPrimitive_Func = nullptr, DrawIndexedPrimitive_Ret = nullptr;

    struct ProgramSnapshotSwap {
        void** slot;
        void* original;
    };
    inline constexpr size_t kGrRenderProgramSize = 0xb8;
    inline constexpr size_t kGrRenderModelEntrySize = 0x7c;
    struct ProgramStateBackup {
        void* program;
        std::array<uint8_t, kGrRenderProgramSize> state;
        void* model_entries;
        std::vector<uint8_t> model_entry_state;
    };
    std::vector<ProgramSnapshotSwap> program_snapshot_swaps;
    std::vector<ProgramStateBackup> program_state_backups;
    void* shadow_absolute_transform_snapshot = nullptr;
    void* shadow_relative_transform_snapshot = nullptr;
    void* shadow_original_absolute_snapshot = nullptr;
    bool shadow_replay_active = false;

    bool shadow_reuse_shadow_map = false;
    int shadow_update_interval = 1;
    bool shadow_map_valid = false;
    uint64_t shadow_map_frame = 0;
    DirectX::XMFLOAT3 shadow_map_center{};
    DirectX::XMFLOAT3 shadow_map_forward{};
    DirectX::XMFLOAT3 shadow_map_bounds{};
    uint32_t shadow_map_render_count = 0;
    uint32_t shadow_map_reuse_count = 0;

    bool shadow_light_cull_enabled = true;

    bool shadow_cull_objects = false;
    uint32_t shadow_objects_not_culled = 0;
    uint32_t shadow_terrain_cull_rejected_count = 0;

    bool shadow_light_cull_degenerate = false;
    bool shadow_skip_non_depth_draws = true;
    bool shadow_skip_water_draws = true;

    bool shadow_skip_baked_tile_decode = true;

    bool shadow_skip_shadow_pass_residency = true;
    uint32_t shadow_replay_draws_skipped = 0;
    bool shadow_target_redirect_active = false;
    bool shadow_allow_auxiliary_targets = false;
    IDirect3DSurface9* shadow_original_main_target = nullptr;
    IDirect3DSurface9* shadow_original_main_depth = nullptr;
    uint32_t shadow_replay_program_count = 0;
    uint32_t shadow_replay_renderer = 0;
    uint32_t shadow_replay_queue_count = 0;
    uint32_t shadow_replay_submission_count = 0;
    uint32_t shadow_replay_swap_count = 0;
    uint32_t shadow_replay_target_redirects = 0;
    uint32_t shadow_replay_viewport_redirects = 0;
    uint32_t shadow_replay_draw_count = 0;
    uint32_t shadow_builder_draw_count = 0;
    uint32_t shadow_primary_program_count = 0;
    uint32_t shadow_baked_tiles_voided = 0;
    uint32_t shadow_baked_tile_decodes_skipped = 0;
    uint32_t shadow_residency_waits_skipped = 0;
    uint32_t shadow_view_updates_skipped = 0;
    uint32_t shadow_prop_updates_run = 0;
    uint32_t shadow_agent_shadows_skipped = 0;
    float shadow_light_projection_fov = 0.0f;

    struct CapturedTerrainStream {
        IDirect3DVertexBuffer9* buffer = nullptr;
        UINT offset = 0;
        UINT stride = 0;
        UINT frequency = 1;
    };

    struct CapturedTerrainDraw {
        IDirect3DVertexDeclaration9* declaration = nullptr;
        IDirect3DIndexBuffer9* indices = nullptr;
        std::array<CapturedTerrainStream, 16> streams{};
        D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
        INT base_vertex = 0;
        UINT min_vertex = 0;
        UINT vertex_count = 0;
        UINT start_index = 0;
        UINT primitive_count = 0;
        std::array<float, 13> model_transform{};
        std::array<float, 16> original_wvp{};
        bool original_wvp_valid = false;
    };

    std::vector<CapturedTerrainDraw> captured_terrain_draws;
    bool shadow_gpu_capture_active = false;
    bool shadow_gpu_resource_capture_active = false;
    bool shadow_gpu_current_program_terrain = false;
    void* shadow_gpu_current_program = nullptr;
    uint32_t shadow_gpu_current_program_handle = 0;
    uint32_t shadow_gpu_current_gr_call = 0;
    uint32_t shadow_gpu_captured_draw_count = 0;
    uint32_t shadow_gpu_replayed_draw_count = 0;
    DirectX::XMFLOAT4X4A shadow_gpu_light_view{};
    DirectX::XMFLOAT4X4A shadow_gpu_light_projection{};
    bool shadow_gpu_light_matrices_ready = false;

    std::array<float, 16> shadow_native_light_wvp{};
    bool shadow_native_light_wvp_ready = false;
    float shadow_native_depth_slope = 1.0f;
    bool shadow_native_light_orthographic = false;
    std::array<float, 16> shadow_camera_wvp{};
    bool shadow_camera_wvp_ready = false;
    bool shadow_camera_wvp_capture_pending = false;
    uint32_t shadow_main_viewport_width = 0;
    uint32_t shadow_main_viewport_height = 0;
    ShadowCamera::DirectionalCamera shadow_light_camera_state{};
    bool shadow_light_camera_state_ready = false;
    bool shadow_native_vertical_clamped = false;

    struct GpuProgramDrawDiagnostic {
        uint32_t gr_call;
        uint32_t program_handle;
        uintptr_t program;
        bool terrain;
        uintptr_t vertex_shader;
        uintptr_t pixel_shader;
        uintptr_t declaration;
        DWORD alpha_blend;
        DWORD alpha_test;
        DWORD alpha_ref;
        DWORD src_blend;
        DWORD dest_blend;
        D3DPRIMITIVETYPE type;
        UINT vertex_count;
        UINT primitive_count;
        std::array<float, 13> model_transform;
        std::array<D3DVERTEXELEMENT9, MAXD3DDECLLENGTH + 1> elements;
        UINT element_count;
    };

    std::array<GpuProgramDrawDiagnostic, 1024> shadow_gpu_program_draws{};
    uint32_t shadow_gpu_program_draw_count = 0;

    struct GrRenderProgramsCall {
        uintptr_t caller;
        uint32_t renderer;
        uint32_t count;
        uint32_t flags;
        bool frcache;
    };
    std::array<GrRenderProgramsCall, 16> gr_program_calls{};
    std::array<GrRenderProgramsCall, 16> last_gr_program_calls{};
    uint32_t gr_program_call_count = 0;
    uint32_t last_gr_program_call_count = 0;
    uint64_t shadow_replayed_frame = 0;
    uintptr_t shadow_scene_list_caller = 0;
    uintptr_t shadow_trigger_scene_list_caller = 0;
    bool scene_list_submit_active = false;
    bool scene_list_submit_is_world = false;
    uintptr_t scene_list_submit_caller = 0;

    enum class Phase : uint8_t {
        ShadowFillTotal,
        WorldRenderFromCamera,
        SceneListsSubmit,
        TileDecodeHook,
        ResidencyInPass,
        ResidencyOutsidePass,
        ProgramQueueHook,
        FrustumCullHook,
        ReplayDraws,
        ScreenSpaceReceive,

        PropViewUpdate,
        SceneListBuild,
        WorldUpdateView,
        TerrainCollect,
        VisibilityBuffer,
        CullGrid,
        RenderPrograms,
        Count
    };
    constexpr const char* kPhaseNames[] = {
        "shadow fill (total)", "world render from light", "scene lists submit",
        "tile decode hook", "residency wait (in pass)", "residency wait (outside)",
        "program queue hook", "frustum cull hook", "replay draws", "in-world draw (sky+receive)",
        "  prop view update", "  scene list build", "  world update view", "  terrain collect",
        "  visibility buffer", "  cull grid", "  render programs",
    };
    static_assert(std::size(kPhaseNames) == static_cast<size_t>(Phase::Count));

    struct PhaseStat {
        double ms = 0.0;
        uint32_t calls = 0;
    };

    bool shadow_skip_view_update = true;

    bool shadow_props_follow_light = true;

    bool shadow_ignore_cell_visibility = true;
    bool shadow_profile_enabled = false;
    std::array<PhaseStat, static_cast<size_t>(Phase::Count)> phase_stats{};
    std::array<PhaseStat, static_cast<size_t>(Phase::Count)> phase_stats_last{};
    double phase_frame_ms = 0.0;
    int64_t phase_frame_mark = 0;

    int64_t PerfCounter()
    {
        LARGE_INTEGER value;
        QueryPerformanceCounter(&value);
        return value.QuadPart;
    }

    double PerfToMs(const int64_t ticks)
    {
        static const double scale = [] {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            return 1000.0 / static_cast<double>(frequency.QuadPart);
        }();
        return static_cast<double>(ticks) * scale;
    }

    struct ScopedPhase {
        Phase phase;
        int64_t start;

        explicit ScopedPhase(const Phase p)
            : phase(p), start(shadow_profile_enabled ? PerfCounter() : 0) { }

        ~ScopedPhase()
        {
            if (!start) return;
            auto& stat = phase_stats[static_cast<size_t>(phase)];
            stat.ms += PerfToMs(PerfCounter() - start);
            ++stat.calls;
        }

        ScopedPhase(const ScopedPhase&) = delete;
        ScopedPhase& operator=(const ScopedPhase&) = delete;
    };

    bool ShadowMapPassEnabled();

    bool OceanSurfaceReady();
    bool UseShadowLightCamera();
    bool UseGpuTerrainReplay();

    float compass_circle[4] = {0.f, 0.f, 0.f, 1.f};

    void RefreshCompassCircle(const uint32_t viewport_width, const uint32_t viewport_height)
    {
        compass_circle[0] = 0.f;
        compass_circle[1] = 0.f;
        compass_circle[2] = 0.f;
        compass_circle[3] = 1.f;
        if (viewport_width == 0 || viewport_height == 0) return;

        const auto* const frame = GW::UI::GetFrameByLabel(L"Compass");
        if (!frame || !frame->IsCreated() || !frame->IsVisible()) return;

        auto top_left = frame->position.GetTopLeftOnScreen(frame);
        auto bottom_right = frame->position.GetBottomRightOnScreen(frame);
        const auto height = bottom_right.y - top_left.y;
        if (height <= 0.f) return;

        constexpr auto compass_padding = 1.05f;
        const auto inset = height - height / compass_padding;
        top_left.x += inset;
        top_left.y += inset;
        bottom_right.x -= inset;
        bottom_right.y = top_left.y + (bottom_right.x - top_left.x);

        const auto radius = 0.5f * (bottom_right.x - top_left.x);
        if (radius <= 0.f) return;

        const auto width_f = static_cast<float>(viewport_width);
        const auto height_f = static_cast<float>(viewport_height);
        compass_circle[0] = (0.5f * (top_left.x + bottom_right.x)) / width_f;
        compass_circle[1] = (0.5f * (top_left.y + bottom_right.y)) / height_f;

        compass_circle[2] = radius / width_f;
        compass_circle[3] = height_f / width_f;
    }

    float depth_uv_scale[4] = {1.f, 1.f, 0.f, 0.f};

    void SetExcludedCircle(IDirect3DDevice9* device)
    {
        device->SetPixelShaderConstantF(31, compass_circle, 1);
        device->SetPixelShaderConstantF(30, depth_uv_scale, 1);
    }

    DirectX::XMFLOAT3 CurrentSkyClearColour();

    uint32_t* gw_sky_clear_colour = nullptr;
    bool recolour_gw_sky = true;

    bool recolour_gw_fog = true;

    bool suppress_gw_fog = true;

    bool water_ownership_view = false;

    bool sky_ownership_view = false;

    bool sky_depth_from_copy = false;

    float horizon_fade_start = -0.05f;
    float horizon_fade_width = 0.045f;
    float horizon_fade_floor = 0.35f;
    SetRenderState_pt SetRenderState_Func = nullptr;
    SetRenderState_pt SetRenderState_Ret = nullptr;

    uint32_t gw_fog_colour_override = 0xFF6C7BB3u;

    DWORD gw_requested_fog_enable = FALSE;
    DWORD gw_requested_fog_colour = 0xFF6C7BB3u;

    bool suppress_gw_sky = true;

    bool sky_enabled = true;

    void __cdecl OnEnvSkyBuildScenePrograms(
        void* env_sky, void* camera, void* programs)
    {
        GW::Hook::EnterHook();
        if (!suppress_gw_sky || !IsEnabled()) {
            EnvSkyBuildScenePrograms_Ret(env_sky, camera, programs);
        }
        GW::Hook::LeaveHook();
    }

    void __cdecl OnGmViewBuildSceneLists(void* frame, float* delta_time)
    {
        GW::Hook::EnterHook();
        const ScopedPhase timer(Phase::SceneListBuild);
        GmViewBuildSceneLists_Ret(frame, delta_time);

        {
            const auto sky = CurrentSkyClearColour();
            const auto channel = [](const float v) {
                return static_cast<uint32_t>(
                    std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };

            const auto packed = 0xFF000000u
                | (channel(sky.x) << 16) | (channel(sky.y) << 8) | channel(sky.z);
            if (recolour_gw_sky && gw_sky_clear_colour && IsEnabled()) {
                *gw_sky_clear_colour = packed;
            }

            gw_fog_colour_override = packed;
        }
        if (delta_time) {

            if (sun_cycle_enabled) {
                const auto hours_per_second =
                    24.0f / std::max(sun_cycle_minutes, 0.01f) / 60.0f;
                sun_cycle_hour = std::fmod(
                    sun_cycle_hour + *delta_time * hours_per_second, 24.0f);
                if (sun_cycle_hour < 0.0f) sun_cycle_hour += 24.0f;
            }
        }
        GW::Hook::LeaveHook();
    }

    void UpdateShadowView(
        const float delta_time, float* eye, float* target, float* up, float* clear_color)
    {
        const ScopedPhase timer(Phase::WorldUpdateView);
        if (shadow_explicit_camera_render_active && shadow_skip_view_update) {

            if (clear_color && eye && target) {
                const auto dx = target[0] - eye[0];
                const auto dy = target[1] - eye[1];
                const auto dz = target[2] - eye[2];
                const auto length = std::sqrt(dx * dx + dy * dy + dz * dz);
                const auto scale = length > 1e-6f ? 1.0f / length : 0.0f;
                clear_color[0] = dx * scale;
                clear_color[1] = dy * scale;
                clear_color[2] = dz * scale;
            }

            const auto map_context = GW::GetMapContext();
            if (map_context) map_context->view_flags |= 1u;

            const auto is_restore =
                map_context && eye == &map_context->view_eye.x;

            const auto prop_manager = map_context ? map_context->props : nullptr;
            const auto environment = map_context
                ? reinterpret_cast<void*>(static_cast<uintptr_t>(map_context->h00E8)) : nullptr;
            if (shadow_props_follow_light && GmPropsUpdateView_Func
                && prop_manager && environment && eye && target) {
                const auto prop_lod_scale = GetPropLodScale_Func(environment);
                const ScopedPhase prop_timer(Phase::PropViewUpdate);
                float view_eye[3] = {eye[0], eye[1], eye[2]};
                float view_target[3] = {target[0], target[1], target[2]};
                if (!is_restore) {

                    const float dir[3] = {
                        target[0] - eye[0], target[1] - eye[1], -(target[2] - eye[2])};
                    view_eye[0] = target[0];
                    view_eye[1] = target[1];
                    view_eye[2] = -target[2];
                    view_target[0] = view_eye[0] + dir[0];
                    view_target[1] = view_eye[1] + dir[1];
                    view_target[2] = view_eye[2] + dir[2];
                }
                GmPropsUpdateView_Func(prop_manager, view_eye, view_target, prop_lod_scale);
                ++shadow_prop_updates_run;
            }

            if (is_restore) shadow_cell_visibility_override_active = false;
            ++shadow_view_updates_skipped;
            return;
        }
        if (shadow_scene_rebuild_active && !shadow_explicit_camera_render_active) {
            GmWorldUpdateView_Func(
                delta_time, shadow_scene_eye, shadow_scene_target, shadow_scene_up, clear_color);
        }
        else {
            GmWorldUpdateView_Func(delta_time, eye, target, up, clear_color);
        }
    }

    bool RenderShadowScene(float* eye, float* target, float* up)
    {
        const auto map = GW::GetMapContext();
        if (!map || map->h0114 || !map->props || !map->h00E8) {
            Log::Log("[Weather] shadow scene skipped: map renderer is not ready");
            return false;
        }
        const auto saved_eye = map->view_eye;
        const auto saved_target = map->view_target;
        const auto saved_up = map->view_up;
        auto* const programs = reinterpret_cast<SceneProgramList*>(&map->h010C);
        float direction[3]{};
        uint32_t clear_colour = 0;
        int has_sky = 0;

        map->view_flags &= ~1u;
        UpdateShadowView(0.0f, eye, target, up, direction);
        CameraRenderScope_Func(1);
        SceneRenderScope_Func(1);
        GrSetCameraTransform_Func(
            eye[0], eye[1], eye[2], direction[0], direction[1], direction[2]);
        const auto saved_flags = map->flags;
        map->flags &= ~1u;
        if (shadow_cast_agent_shadows) {
            int first_program = 0;
            int program_count = 0;
            GmWorldSetPrimarySceneBuildEnabled_Func(1);
            GmWorldBuildPrimaryScenePrograms_Func(0.0f, programs, &first_program, &program_count);
            GmWorldSetPrimarySceneBuildEnabled_Func(0);
            shadow_primary_program_count = static_cast<uint32_t>(program_count);
        }
        GmWorldBuildRemainingScenePrograms_Func(0.0f, programs, nullptr, &has_sky, &clear_colour);
        map->flags = saved_flags;
        CameraRenderScope_Func(0);
        SceneRenderScope_Func(0);
        GrRenderSceneLists_Ret(programs, nullptr, has_sky, &clear_colour, 0);

        map->view_eye = saved_eye;
        map->view_target = saved_target;
        map->view_up = saved_up;
        map->view_flags &= ~1u;
        programs->size = 0;
        UpdateShadowView(0.0f, &map->view_eye.x, &map->view_target.x, &map->view_up.x, direction);
        return true;
    }

    void __fastcall OnTrnCollectScenePrograms(
        void* terrain_view, void*, void* terrain_map, const int view, SceneProgramList* programs)
    {
        GW::Hook::EnterHook();
        const ScopedPhase timer(Phase::TerrainCollect);
        const auto was_collecting = shadow_terrain_collection_active;
        shadow_terrain_collection_active = shadow_scene_rebuild_active && view == 0;
        if (shadow_terrain_collection_active) ++shadow_terrain_collect_count;
        const auto capture_gpu_terrain_handles =
            UseGpuTerrainReplay()
            && !shadow_replay_active && view == 0;
        const auto first_program = programs ? programs->size : 0;
        TrnCollectScenePrograms_Ret(terrain_view, terrain_map, view, programs);
        if ((shadow_terrain_collection_active || capture_gpu_terrain_handles)
            && programs && programs->data
            && first_program <= programs->size) {
            const auto* const handles = static_cast<const uint32_t*>(programs->data);
            shadow_terrain_program_handles.assign(
                handles + first_program, handles + programs->size);

            std::ranges::sort(shadow_terrain_program_handles);
            const auto duplicates = std::ranges::unique(shadow_terrain_program_handles);
            shadow_terrain_program_handles.erase(duplicates.begin(), duplicates.end());
        }
        shadow_terrain_collection_active = was_collecting;
        GW::Hook::LeaveHook();
    }

    uint32_t* __cdecl OnGetViewVisibilityBuffer(
        const int view, const int category, uint32_t** end)
    {
        GW::Hook::EnterHook();
        const ScopedPhase timer(Phase::VisibilityBuffer);
        uint32_t* result;
        if (shadow_terrain_collection_active && view == 0 && category == 4) {
            ++shadow_terrain_visibility_override_count;
            result = shadow_terrain_visibility.data();
            if (end) *end = result + shadow_terrain_visibility.size();
        }
        else if (shadow_cell_visibility_override_active && shadow_ignore_cell_visibility) {

            uint32_t* real_end = nullptr;
            uint32_t* const real = GetViewVisibilityBuffer_Ret(view, category, &real_end);
            const size_t size = real && real_end && real_end >= real
                ? static_cast<size_t>(real_end - real)
                : 0;
            if (size != 0 && size <= shadow_terrain_visibility.size()) {
                ++shadow_cell_visibility_override_count;
                result = shadow_terrain_visibility.data();
                if (end) *end = result + size;
            }
            else {
                ++shadow_cell_visibility_too_large_count;
                result = real;
                if (end) *end = real_end;
            }
        }
        else {
            result = GetViewVisibilityBuffer_Ret(view, category, end);
        }
        GW::Hook::LeaveHook();
        return result;
    }

    bool IsTerrainProgram(const uint32_t handle)
    {
        return handle && std::ranges::binary_search(shadow_terrain_program_handles, handle);
    }

    constexpr size_t kWaterProgramOffset = 0xAC;
    uint32_t shadow_water_program_handle = 0;
    uint32_t shadow_water_programs_skipped = 0;
    uint32_t gw_water_programs_suppressed = 0;

    void RefreshWaterProgramHandles()
    {
        shadow_water_program_handle = 0;
        const auto* const map_context = GW::GetMapContext();
        if (!map_context) return;
        const auto* const base = reinterpret_cast<const uint8_t*>(map_context);

        if ((*reinterpret_cast<const uint32_t*>(base + 0x108) & 2) == 0) return;
        const auto* const water = *reinterpret_cast<const uint8_t* const*>(base + 0x12C);
        if (!water) return;
        shadow_water_program_handle =
            *reinterpret_cast<const uint32_t*>(water + kWaterProgramOffset);
    }

    bool IsWaterProgram(const uint32_t handle)
    {
        return handle && handle == shadow_water_program_handle;
    }

    int __fastcall OnGrModelFrustumCull(
        void* bounds, void*, float* frustum, const float depth, const int plane_count,
        const int extra_planes, float* sort_depth)
    {
        GW::Hook::EnterHook();
        int result;
        if (shadow_terrain_program_queue_active) {
            const ScopedPhase timer(Phase::FrustumCullHook);

            if (shadow_light_cull_enabled && shadow_explicit_camera_render_active
                && !shadow_light_cull_degenerate && frustum) {
                result = GrModelFrustumCull_Ret(
                    bounds, frustum, depth, plane_count, extra_planes, sort_depth);
                if (result) ++shadow_terrain_cull_bypass_count;
                else ++shadow_terrain_cull_rejected_count;
            }
            else {
                ++shadow_terrain_cull_bypass_count;
                result = 1;
            }
            if (sort_depth) *sort_depth = 0.0f;
        }
        else if (shadow_replay_active && !shadow_cull_objects) {

            ++shadow_objects_not_culled;
            if (sort_depth) *sort_depth = 0.0f;
            result = 1;
        }
        else {
            result = GrModelFrustumCull_Ret(
                bounds, frustum, depth, plane_count, extra_planes, sort_depth);
        }
        GW::Hook::LeaveHook();
        return result;
    }

    float EffectiveShadowLightCameraDistance();

    void __cdecl OnGrCullBuildVisibilityGrid(
        float* cell_size, int* range, const int output, void* optional)
    {
        GW::Hook::EnterHook();
        const ScopedPhase timer(Phase::CullGrid);
        if (shadow_explicit_camera_render_active && shadow_light_camera_orthographic) {
            auto* const projection = GrTransformGetCurrent_Func(0);
            if (projection) {
                float saved_projection[13];
                std::memcpy(saved_projection, projection, sizeof(saved_projection));
                GrTransformIdentity_Func(0);
                const auto distance =
                    EffectiveShadowLightCameraDistance();
                const auto cull_fov = std::clamp(
                    2.0f * std::atan(shadow_light_camera_radius / distance),
                    DirectX::XMConvertToRadians(10.0f),
                    DirectX::XMConvertToRadians(170.0f));
                GrTransformPerspective_Func(
                    0, cull_fov,
                    distance * 2.0f, 1.0f, 0);
                GrCullBuildVisibilityGrid_Ret(cell_size, range, output, optional);
                GrTransformSetCurrent_Func(0, saved_projection);
                GW::Hook::LeaveHook();
                return;
            }
        }
        GrCullBuildVisibilityGrid_Ret(cell_size, range, output, optional);
        GW::Hook::LeaveHook();
    }

    int __fastcall OnTrnTexShadowDecompressTile(
        void* context, void*, const int terrain_texture, int* budget)
    {
        GW::Hook::EnterHook();

        constexpr size_t kTileBytes = 0x110 * 0x22;

        constexpr int kTileNoShadow = 0x00;

        const ScopedPhase hook_timer(Phase::TileDecodeHook);

        const auto void_baked_tiles = disable_baked_terrain_shadows && IsEnabled();
        if (void_baked_tiles && shadow_skip_baked_tile_decode && context
            && terrain_texture) {
            auto* const trn_tex = reinterpret_cast<uint8_t*>(terrain_texture);
            const auto tile = *reinterpret_cast<uintptr_t*>(trn_tex + 0x17c);
            if (tile && !(tile & 1)) {
                if (*budget < 300) {
                    GW::Hook::LeaveHook();
                    return 0;
                }
                *budget -= 300;

                std::memset(static_cast<uint8_t*>(context) + 0x4E4, kTileNoShadow, kTileBytes);
                *reinterpret_cast<uint32_t*>(tile + 0x14) = 2;
                *reinterpret_cast<uint32_t*>(tile + 0x18) = 0;
                ++shadow_baked_tiles_voided;
                ++shadow_baked_tile_decodes_skipped;
                GW::Hook::LeaveHook();
                return 1;
            }
        }

        const auto result = TrnTexShadowDecompressTile_Ret(
            context, terrain_texture, budget);

        if (result && void_baked_tiles && context) {
            std::memset(
                static_cast<uint8_t*>(context) + 0x4E4, kTileNoShadow, kTileBytes);
            ++shadow_baked_tiles_voided;
        }
        GW::Hook::LeaveHook();
        return result;
    }

    uint8_t* CoarseShadowMask(void* const trn_tex, void* const tile)
    {

        constexpr uintptr_t kSmallestPlausiblePointer = 0x10000;
        if (!trn_tex || !tile) return nullptr;
        const auto* const trn = static_cast<const uint8_t*>(trn_tex);
        const auto* const tile_bytes = static_cast<const uint8_t*>(tile);
        const auto grid_width = *reinterpret_cast<const uint32_t*>(trn + 0xb4);
        const auto record_count = *reinterpret_cast<const uint32_t*>(trn + 0xb0);

        constexpr uint32_t kMaxGrid = 4096;
        if (grid_width == 0 || grid_width > kMaxGrid) return nullptr;
        if (record_count == 0 || record_count > kMaxGrid * kMaxGrid) return nullptr;
        const auto tile_x = *reinterpret_cast<const uint32_t*>(tile_bytes + 0xc);
        const auto tile_y = *reinterpret_cast<const uint32_t*>(tile_bytes + 0x10);
        if (tile_x >= grid_width || tile_y > record_count / grid_width) return nullptr;

        const auto index = grid_width * tile_y + tile_x;
        if (index >= record_count) return nullptr;
        auto* const records = *reinterpret_cast<uint8_t* const*>(trn + 0xa8);
        if (reinterpret_cast<uintptr_t>(records) < kSmallestPlausiblePointer) return nullptr;
        auto* const mask =
            *reinterpret_cast<uint8_t* const*>(records + index * 0xc + 8);

        if (reinterpret_cast<uintptr_t>(mask) < kSmallestPlausiblePointer) return nullptr;
        return mask;
    }

    int __stdcall OnTrnTexComposeTileLighting(
        void* trn_tex, void* tile_coord, void* tile, int* budget)
    {
        GW::Hook::EnterHook();

        uint8_t* mask = nullptr;
        uint8_t saved[kCoarseShadowMaskBytes];
        if (disable_baked_terrain_shadows && IsEnabled()) {
            mask = CoarseShadowMask(trn_tex, tile);
        }
        if (mask) {
            std::memcpy(saved, mask, sizeof(saved));
            std::memset(mask, 0, sizeof(saved));
            ++shadow_baked_blocks_cleared;
        }
        const auto result = TrnTexComposeTileLighting_Ret(
            trn_tex, tile_coord, tile, budget);
        if (mask) std::memcpy(mask, saved, sizeof(saved));
        GW::Hook::LeaveHook();
        return result;
    }

    void __cdecl OnGmPropUpdateFade(void* manager, const float delta_time, void* prop)
    {
        GW::Hook::EnterHook();

        if (shadow_replay_active && shadow_skip_prop_fade) {
            ++shadow_prop_fades_skipped;
            GW::Hook::LeaveHook();
            return;
        }
        GmPropUpdateFade_Ret(manager, delta_time, prop);
        GW::Hook::LeaveHook();
    }

    void __fastcall OnGmSelectVisibleCells(
        void* ecx, void* edx, float* eye, void* vis_buf_0, void* vis_buf_1)
    {
        GW::Hook::EnterHook();

        if (shadow_cell_override_active && eye) {
            float override_eye[3] = {
                shadow_cell_override_eye[0], shadow_cell_override_eye[1],
                shadow_cell_override_eye[2]};
            ++shadow_cell_overrides;
            GmSelectVisibleCells_Ret(ecx, edx, override_eye, vis_buf_0, vis_buf_1);
            GW::Hook::LeaveHook();
            return;
        }
        GmSelectVisibleCells_Ret(ecx, edx, eye, vis_buf_0, vis_buf_1);
        GW::Hook::LeaveHook();
    }

    void __fastcall OnTrnTexWaitForResidency(
        void* ecx, void* edx, const uint32_t manager, const uint32_t texture, const uint32_t budget)
    {
        GW::Hook::EnterHook();

        if (shadow_replay_active && shadow_skip_shadow_pass_residency) {
            ++shadow_residency_waits_skipped;
            GW::Hook::LeaveHook();
            return;
        }
        {
            const ScopedPhase timer(
                shadow_replay_active ? Phase::ResidencyInPass : Phase::ResidencyOutsidePass);
            TrnTexWaitForResidency_Ret(ecx, edx, manager, texture, budget);
        }
        GW::Hook::LeaveHook();
    }

    void __cdecl OnAvShadowBuild(
        void* shadow_handle, void* mesh, float* model_pos, const int param4,
        const int param5, const int param6, const float scale,
        int* out_rendered)
    {
        GW::Hook::EnterHook();

        if (disable_agent_baked_shadows && shadow_gpu_draw_shadows && IsEnabled()) {
            if (out_rendered) *out_rendered = 0;
            ++shadow_agent_shadows_skipped;
            GW::Hook::LeaveHook();
            return;
        }
        AvShadowBuild_Ret(
            shadow_handle, mesh, model_pos, param4, param5, param6, scale,
            out_rendered);
        GW::Hook::LeaveHook();
    }

    void CaptureNativeShadowMatrices()
    {
        if (shadow_gpu_light_matrices_ready || !GrTransformGetCurrent_Func) return;
        const auto* const projection = GrTransformGetCurrent_Func(0);
        const auto* const view = GrTransformGetCurrent_Func(1);
        if (!projection || !view) return;

        const auto view_matrix = DirectX::XMMatrixSet(
            view[0], view[4], view[8], 0.0f,
            view[1], view[5], view[9], 0.0f,
            -view[2], -view[6], -view[10], 0.0f,
            view[3], view[7], view[11], 1.0f);
        const auto projection_matrix = DirectX::XMMatrixSet(
            projection[0], projection[4], projection[8], 0.0f,
            projection[1], projection[5], projection[9], 0.0f,
            projection[2], projection[6], projection[10], 0.0f,
            projection[3], projection[7], projection[11], 1.0f);
        DirectX::XMStoreFloat4x4A(
            &shadow_gpu_light_view,
            DirectX::XMMatrixTranspose(view_matrix));
        DirectX::XMStoreFloat4x4A(
            &shadow_gpu_light_projection,
            DirectX::XMMatrixTranspose(projection_matrix));
        shadow_gpu_light_matrices_ready = true;
    }

    void RenderShadowReplayPreview(
        IDirect3DDevice9* device, const GW::Camera* cam, void* programs,
        void* secondary_programs, int mode, void* clear_color);

    struct SkyVertex {
        float x, y, z, w;
        float u, v;
        float rx, ry, rz;
    };

    bool EnsureResources(IDirect3DDevice9* device)
    {
        if (resources_failed) return false;
        if (sky_vs && composite_ps && sky_decl && world_ambient_ps_object
            && world_water_ps_object && world_fog_ps_object) {
            return true;
        }

        constexpr D3DVERTEXELEMENT9 decl[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            {0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
            D3DDECL_END()};

        bool ok = true;
        if (!sky_decl) ok = ok && device->CreateVertexDeclaration(decl, &sky_decl) == D3D_OK;
        if (!sky_vs) ok = ok && device->CreateVertexShader(reinterpret_cast<const DWORD*>(&sky_fullscreen_vs), &sky_vs) == D3D_OK;
        if (!composite_ps) ok = ok && device->CreatePixelShader(reinterpret_cast<const DWORD*>(&sky_composite_ps), &composite_ps) == D3D_OK;
        if (!world_ambient_ps_object) {
            ok = ok && device->CreatePixelShader(
                reinterpret_cast<const DWORD*>(&world_ambient_ps),
                &world_ambient_ps_object) == D3D_OK;
        }
        if (!world_water_ps_object) {
            ok = ok && device->CreatePixelShader(
                reinterpret_cast<const DWORD*>(&world_water_ps),
                &world_water_ps_object) == D3D_OK;
        }
        if (!world_fog_ps_object) {
            ok = ok && device->CreatePixelShader(
                reinterpret_cast<const DWORD*>(&world_fog_ps),
                &world_fog_ps_object) == D3D_OK;
        }

        if (!ok) {
            resources_failed = true;
            Log::Log("[Skybox] failed to create GPU resources - disabling");
            return false;
        }
        return true;
    }

    void ReleaseShadowPreviewResources()
    {
        if (shadow_screenspace_ps_object) {
            shadow_screenspace_ps_object->Release();
            shadow_screenspace_ps_object = nullptr;
        }
        if (shadow_terrain_receive_ps_object) {
            shadow_terrain_receive_ps_object->Release();
            shadow_terrain_receive_ps_object = nullptr;
        }
        if (shadow_terrain_receive_vs_object) {
            shadow_terrain_receive_vs_object->Release();
            shadow_terrain_receive_vs_object = nullptr;
        }
        if (shadow_terrain_replay_ps_object) {
            shadow_terrain_replay_ps_object->Release();
            shadow_terrain_replay_ps_object = nullptr;
        }
        if (shadow_terrain_replay_vs_object) {
            shadow_terrain_replay_vs_object->Release();
            shadow_terrain_replay_vs_object = nullptr;
        }
        if (shadow_preview_depth) {
            shadow_preview_depth->Release();
            shadow_preview_depth = nullptr;
        }
        if (shadow_preview_depth_texture) {
            shadow_preview_depth_texture->Release();
            shadow_preview_depth_texture = nullptr;
        }
        if (shadow_camera_depth_texture) {
            shadow_camera_depth_texture->Release();
            shadow_camera_depth_texture = nullptr;
        }
        if (shadow_preview_surface) {
            shadow_preview_surface->Release();
            shadow_preview_surface = nullptr;
        }
        shadow_map_size_active = 0;
        if (shadow_preview_texture) {
            shadow_preview_texture->Release();
            shadow_preview_texture = nullptr;
        }
        shadow_preview_format = D3DFMT_UNKNOWN;
        shadow_preview_depth_precision = 8;
        shadow_preview_depth_sampleable = false;
        shadow_camera_depth_width = 0;
        shadow_camera_depth_height = 0;
    }

    void ReleaseCapturedTerrainDraws()
    {
        for (auto& draw : captured_terrain_draws) {
            if (draw.indices) draw.indices->Release();
            if (draw.declaration) draw.declaration->Release();
            for (auto& stream : draw.streams) {
                if (stream.buffer) stream.buffer->Release();
            }
        }
        captured_terrain_draws.clear();
    }

    bool IsSameCapturedTerrainDraw(
        const CapturedTerrainDraw& lhs, const CapturedTerrainDraw& rhs)
    {
        if (lhs.declaration != rhs.declaration || lhs.indices != rhs.indices
            || lhs.type != rhs.type || lhs.base_vertex != rhs.base_vertex
            || lhs.min_vertex != rhs.min_vertex
            || lhs.vertex_count != rhs.vertex_count
            || lhs.start_index != rhs.start_index
            || lhs.primitive_count != rhs.primitive_count
            || lhs.original_wvp_valid != rhs.original_wvp_valid
            || std::memcmp(
                lhs.model_transform.data(), rhs.model_transform.data(),
                sizeof(lhs.model_transform)) != 0
            || std::memcmp(
                lhs.original_wvp.data(), rhs.original_wvp.data(),
                sizeof(lhs.original_wvp)) != 0) {
            return false;
        }
        for (size_t i = 0; i < lhs.streams.size(); ++i) {
            const auto& lhs_stream = lhs.streams[i];
            const auto& rhs_stream = rhs.streams[i];
            if (lhs_stream.buffer != rhs_stream.buffer
                || lhs_stream.offset != rhs_stream.offset
                || lhs_stream.stride != rhs_stream.stride
                || lhs_stream.frequency != rhs_stream.frequency) {
                return false;
            }
        }
        return true;
    }

    void ReleaseCapturedTerrainDraw(CapturedTerrainDraw& draw)
    {
        if (draw.indices) draw.indices->Release();
        if (draw.declaration) draw.declaration->Release();
        for (auto& stream : draw.streams) {
            if (stream.buffer) stream.buffer->Release();
        }
        draw = {};
    }

    bool EnsureShadowPreviewResources(IDirect3DDevice9* device)
    {

        shadow_map_size = kShadowMapSizes[std::clamp(
            shadow_map_size_index, 0, static_cast<int>(kShadowMapSizes.size()) - 1)];
        if (shadow_map_size_active && shadow_map_size_active != shadow_map_size) {

            shadow_preview_failed = false;
            ReleaseShadowPreviewResources();
        }
        if (shadow_preview_failed) return false;
        if (shadow_preview_texture && shadow_preview_surface && shadow_preview_depth
            && shadow_terrain_replay_vs_object && shadow_terrain_replay_ps_object
            && shadow_terrain_receive_vs_object && shadow_terrain_receive_ps_object
            && shadow_screenspace_ps_object) {
            return true;
        }

        ReleaseShadowPreviewResources();
        HRESULT result = device->CreateTexture(
            shadow_map_size, shadow_map_size, 1,
            D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT,
            &shadow_preview_texture, nullptr);
        if (SUCCEEDED(result)) {
            shadow_preview_format = D3DFMT_A16B16G16R16F;
            shadow_preview_depth_precision = 10;
        }
        else {
            result = device->CreateTexture(
                shadow_map_size, shadow_map_size, 1,
                D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT,
                &shadow_preview_texture, nullptr);
            if (SUCCEEDED(result)) {
                shadow_preview_format = D3DFMT_R32F;
                shadow_preview_depth_precision = 23;
            }
            else {
                result = device->CreateTexture(
                    shadow_map_size, shadow_map_size, 1,
                    D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                    D3DPOOL_DEFAULT, &shadow_preview_texture, nullptr);
                if (SUCCEEDED(result)) {
                    shadow_preview_format = D3DFMT_A8R8G8B8;
                    shadow_preview_depth_precision = 8;
                }
            }
        }
        if (SUCCEEDED(result)) {
            result = shadow_preview_texture->GetSurfaceLevel(0, &shadow_preview_surface);
        }
        if (SUCCEEDED(result)) {
            constexpr auto intz = static_cast<D3DFORMAT>(
                MAKEFOURCC('I', 'N', 'T', 'Z'));
            result = device->CreateTexture(
                shadow_map_size, shadow_map_size, 1,
                D3DUSAGE_DEPTHSTENCIL, intz, D3DPOOL_DEFAULT,
                &shadow_preview_depth_texture, nullptr);
            if (SUCCEEDED(result)) {
                result = shadow_preview_depth_texture->GetSurfaceLevel(
                    0, &shadow_preview_depth);
                shadow_preview_depth_sampleable = SUCCEEDED(result);
            }
            if (FAILED(result)) {
                if (shadow_preview_depth_texture) {
                    shadow_preview_depth_texture->Release();
                    shadow_preview_depth_texture = nullptr;
                }
                result = device->CreateDepthStencilSurface(
                    shadow_map_size, shadow_map_size,
                    D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE,
                    &shadow_preview_depth, nullptr);
            }
        }
        if (SUCCEEDED(result)) {
            result = device->CreateVertexShader(
                reinterpret_cast<const DWORD*>(&shadow_terrain_replay_vs),
                &shadow_terrain_replay_vs_object);
        }
        if (SUCCEEDED(result)) {
            result = device->CreatePixelShader(
                reinterpret_cast<const DWORD*>(&shadow_terrain_replay_ps),
                &shadow_terrain_replay_ps_object);
        }
        if (SUCCEEDED(result)) {
            result = device->CreateVertexShader(
                reinterpret_cast<const DWORD*>(&shadow_terrain_receive_vs),
                &shadow_terrain_receive_vs_object);
        }
        if (SUCCEEDED(result)) {
            result = device->CreatePixelShader(
                reinterpret_cast<const DWORD*>(&shadow_terrain_receive_ps),
                &shadow_terrain_receive_ps_object);
        }
        if (SUCCEEDED(result)) {
            result = device->CreatePixelShader(
                reinterpret_cast<const DWORD*>(&shadow_screenspace_ps),
                &shadow_screenspace_ps_object);
        }
        if (FAILED(result)) {
            ReleaseShadowPreviewResources();
            shadow_preview_failed = true;
            shadow_map_size_active = 0;
            Log::Log(
                "[Skybox] failed to create %ux%u shadow map resources (0x%08x)",
                shadow_map_size, shadow_map_size, result);
            return false;
        }
        shadow_map_size_active = shadow_map_size;
        return true;
    }

    constexpr float kSkyDepthThreshold = 0.9999f;

    void FillFullscreenQuad(SkyVertex out[4], const DirectX::XMFLOAT3 corner_dirs[4])
    {

        const float xs[4] = {-1.f, -1.f, 1.f, 1.f};
        const float ys[4] = {1.f, -1.f, 1.f, -1.f};
        const float us[4] = {0.f, 0.f, 1.f, 1.f};
        const float vs[4] = {0.f, 1.f, 0.f, 1.f};
        for (int i = 0; i < 4; ++i) {
            out[i].x = xs[i];
            out[i].y = ys[i];

            out[i].z = kSkyDepthThreshold;
            out[i].w = 1.0f;
            out[i].u = us[i];
            out[i].v = vs[i];
            out[i].rx = corner_dirs[i].x;
            out[i].ry = corner_dirs[i].y;
            out[i].rz = corner_dirs[i].z;
        }
    }

    void FillFullscreenTriangleFromBottomRight(SkyVertex out[3])
    {

        const float xs[3] = {1.f, -5.f, 1.f};
        const float ys[3] = {-1.f, -1.f, 5.f};
        for (int i = 0; i < 3; ++i) {
            out[i] = {};
            out[i].x = xs[i];
            out[i].y = ys[i];
            out[i].z = 1.0f;
            out[i].w = 1.0f;

            out[i].u = (xs[i] + 1.f) * 0.5f;
            out[i].v = (1.f - ys[i]) * 0.5f;
        }
    }

    void FillFullscreenTriangle(SkyVertex out[3])
    {
        const float xs[3] = {-1.f, 3.f, -1.f};
        const float ys[3] = {1.f, 1.f, -3.f};
        const float us[3] = {0.f, 2.f, 0.f};
        const float vs[3] = {0.f, 0.f, 2.f};
        for (int i = 0; i < 3; ++i) {
            out[i] = {};
            out[i].x = xs[i];
            out[i].y = ys[i];
            out[i].z = 1.0f;
            out[i].w = 1.0f;
            out[i].u = us[i];
            out[i].v = vs[i];
        }
    }

    DirectX::XMFLOAT3 DirFromElevAzim(float elev_deg, float azim_deg)
    {
        const float e = DirectX::XMConvertToRadians(elev_deg);
        const float a = DirectX::XMConvertToRadians(azim_deg);
        const float ce = std::cos(e);
        return DirectX::XMFLOAT3(ce * std::cos(a), ce * std::sin(a), -std::sin(e));
    }

    void CurrentSunCyclePosition(float& elevation_deg, float& azimuth_deg)
    {
        using namespace DirectX;
        const auto hour_angle = XMConvertToRadians((sun_cycle_hour - 12.0f) * 15.0f);
        const auto declination = XMConvertToRadians(sun_cycle_declination_deg);
        const auto latitude = XMConvertToRadians(sun_cycle_latitude_deg);

        const auto sin_elevation = std::clamp(
            std::sin(declination) * std::sin(latitude)
                + std::cos(declination) * std::cos(latitude) * std::cos(hour_angle),
            -1.0f, 1.0f);
        elevation_deg = XMConvertToDegrees(std::asin(sin_elevation));

        const auto north = std::sin(declination) * std::cos(latitude)
            - std::cos(declination) * std::sin(latitude) * std::cos(hour_angle);
        const auto east = -std::sin(hour_angle) * std::cos(declination);

        const auto bearing = (std::fabs(north) < 1e-6f && std::fabs(east) < 1e-6f)
            ? 0.0f
            : XMConvertToDegrees(std::atan2(east, north));

        azimuth_deg = std::fmod(90.0f - bearing + 360.0f, 360.0f);
    }

    float CurrentSunElevation()
    {

        constexpr auto limit = 89.5f;
        if (sun_cycle_enabled) {
            float elevation = 0.0f;
            float azimuth = 0.0f;
            CurrentSunCyclePosition(elevation, azimuth);
            return std::clamp(elevation, -limit, limit);
        }
        if (!animate_sun) return std::clamp(sun_elevation_deg, -limit, limit);

        const float t = static_cast<float>(TIMER_INIT()) * 0.001f * animate_speed;
        return std::clamp(
            DirectX::XMConvertToDegrees(
                std::asin(std::clamp(std::sin(t), -1.f, 1.f))),
            -limit, limit);
    }

    float CurrentSunAzimuth()
    {
        if (!sun_cycle_enabled) return sun_azimuth_deg;
        float elevation = 0.0f;
        float azimuth = 0.0f;
        CurrentSunCyclePosition(elevation, azimuth);
        return azimuth;
    }

    DirectX::XMFLOAT3 GetSunDirWorld()
    {
        return DirFromElevAzim(CurrentSunElevation(), CurrentSunAzimuth());
    }

    constexpr float kKeyLightCrossoverDeg = -16.26f;

    constexpr float kKeyLightMinElevationDeg = 1.5f;

    DirectX::XMFLOAT3 GetSceneLightDirWorld()
    {
        const float sun_elev = CurrentSunElevation();
        const float azimuth = CurrentSunAzimuth();

        constexpr float sweep = 6.0f;
        const auto distance = sun_elev - kKeyLightCrossoverDeg;

        if (std::fabs(distance) < sweep) {

            const auto t = 1.0f - std::fabs(distance) / sweep;
            const auto smooth = t * t * (3.0f - 2.0f * t);
            const auto edge = std::max(
                std::fabs(kKeyLightCrossoverDeg) - sweep, kKeyLightMinElevationDeg);
            const auto elevation = edge + (90.0f - edge) * smooth;
            return DirFromElevAzim(
                elevation, distance > 0.0f ? azimuth : azimuth + 180.0f);
        }
        if (sun_elev > kKeyLightCrossoverDeg) {
            return DirFromElevAzim(
                std::max(sun_elev, kKeyLightMinElevationDeg), azimuth);
        }
        return DirFromElevAzim(
            std::max(-sun_elev, kKeyLightMinElevationDeg),
            azimuth + 180.0f);
    }

    float CurrentExposure()
    {
        const auto elevation = std::clamp(CurrentSunElevation(), -90.0f, 90.0f);
        return elevation / 90.0f * 4.0f;
    }

    float CurrentDaylightBlend()
    {
        const auto sun_height = std::sin(
            DirectX::XMConvertToRadians(CurrentSunElevation()));
        const auto twilight = std::clamp(
            (sun_height + 0.28f) / 0.34f, 0.0f, 1.0f);
        const auto daylight = twilight * twilight * (3.0f - 2.0f * twilight);

        const auto elevation_gain =
            0.85f + 0.15f * std::clamp(sun_height, 0.0f, 1.0f);
        return daylight * elevation_gain;
    }

    float CurrentStarVisibility()
    {
        constexpr auto appear_start = -2.0f;
        constexpr auto appear_full = -14.0f;
        const auto elevation = CurrentSunElevation();
        if (elevation >= appear_start) return 0.0f;
        if (elevation <= appear_full) return 1.0f;
        const auto t = (appear_start - elevation) / (appear_start - appear_full);
        return t * t * (3.0f - 2.0f * t);
    }

    float CurrentMoonVisibility()
    {
        constexpr auto appear_start = 2.0f;
        constexpr auto appear_full = -8.0f;
        const auto elevation = CurrentSunElevation();
        if (elevation >= appear_start) return 0.0f;
        if (elevation <= appear_full) return 1.0f;
        const auto t = (appear_start - elevation) / (appear_start - appear_full);
        return t * t * (3.0f - 2.0f * t);
    }

    WeatherEffects::LightningFrame lightning_frame;

    float CurrentOvercast()
    {
        const auto weather = std::clamp(WeatherEffects::AmbientStrength(), 0.0f, 1.0f);
        if (!clouds_enabled || cloud_opacity <= 0.0f) return weather;
        const auto span = std::max(cloud_overcast_full - cloud_overcast_start, 1e-4f);
        const auto t = std::clamp(
            (cloud_coverage - cloud_overcast_start) / span, 0.0f, 1.0f);
        return std::max(t * t * (3.0f - 2.0f * t), weather);
    }

    DirectX::XMFLOAT3 CurrentOvercastDim()
    {
        if (!world_overcast_dimming) return {1.0f, 1.0f, 1.0f};
        const auto overcast = CurrentOvercast() * std::clamp(cloud_darkness, 0.0f, 1.0f);
        const auto level = 1.0f - (1.0f - cloud_overcast_level) * overcast;
        const auto cool = cloud_overcast_cool * overcast;
        return {level * (1.0f - cool), level, level * (1.0f + cool * 0.8f)};
    }

    DirectX::XMFLOAT3 CurrentSkyClearColour()
    {
        const auto blend = std::clamp(CurrentDaylightBlend(), 0.0f, 1.0f);

        constexpr DirectX::XMFLOAT3 day = {0.55f, 0.68f, 0.86f};
        const auto dim = CurrentOvercastDim();
        return {
            (night_sky_colour.x + (day.x - night_sky_colour.x) * blend) * dim.x,
            (night_sky_colour.y + (day.y - night_sky_colour.y) * blend) * dim.y,
            (night_sky_colour.z + (day.z - night_sky_colour.z) * blend) * dim.z};
    }

    DirectX::XMFLOAT3 CurrentAmbientTint()
    {
        const auto blend = CurrentDaylightBlend();
        const auto dim = CurrentOvercastDim();

        const auto strength = std::max(day_brightness, 0.0f);
        const DirectX::XMFLOAT3 day = {
            ambient_day_colour.x * strength,
            ambient_day_colour.y * strength,
            ambient_day_colour.z * strength};
        return {
            (ambient_night_colour.x + (day.x - ambient_night_colour.x) * blend) * dim.x,
            (ambient_night_colour.y + (day.y - ambient_night_colour.y) * blend) * dim.y,
            (ambient_night_colour.z + (day.z - ambient_night_colour.z) * blend) * dim.z};
    }

    float GodrayBaseline()
    {

        const auto billow = std::clamp(cloud_billow, 0.0f, 1.0f);
        const auto shape_mean = 0.5f - 0.272f * billow;
        const auto shape_dev = 0.0483f - 0.062f * billow + 0.073f * billow * billow;

        constexpr auto weather_mean = 0.502f;
        constexpr auto weather_dev = 0.134f;
        constexpr float node[3] = {-1.732f, 0.0f, 1.732f};
        constexpr float weight[3] = {1.0f / 6.0f, 2.0f / 3.0f, 1.0f / 6.0f};

        auto total = 0.0f;
        for (auto wi = 0; wi < 3; wi++) {
            const auto weather =
                std::clamp(weather_mean + node[wi] * weather_dev, 0.0f, 1.0f);
            const auto cover = std::clamp(
                cloud_coverage * (1.0f + (weather * 2.0f - 1.0f) * cloud_variety),
                0.0f, 1.0f);
            const auto centre = (shape_mean - (1.0f - cover)) * cloud_sharpness;
            const auto spread = shape_dev * cloud_sharpness;
            for (auto si = 0; si < 3; si++) {
                const auto density =
                    std::clamp(centre + node[si] * spread, 0.0f, 1.0f);
                total += weight[wi] * weight[si]
                    * std::exp(-density * std::max(godray_extinction, 0.0f));
            }
        }
        return std::clamp(total, 0.0f, 1.0f);
    }

    float ShadowSunFade()    {
        const auto elevation = CurrentSunElevation();
        const auto smooth = [](const float t) {
            const auto c = std::clamp(t, 0.0f, 1.0f);
            return c * c * (3.0f - 2.0f * c);
        };
        if (elevation > kKeyLightCrossoverDeg) {
            constexpr auto fade_start = kKeyLightCrossoverDeg + 5.0f;
            return smooth(
                (elevation - kKeyLightCrossoverDeg)
                / (fade_start - kKeyLightCrossoverDeg));
        }
        return moon_shadow_strength
            * smooth((kKeyLightCrossoverDeg - elevation) / 5.0f);
    }

    bool ShadowMapPassEnabled()
    {
        if (!IsEnabled() || !GW::Map::GetIsMapLoaded()
            || GW::Map::GetInstanceType() == GW::Constants::InstanceType::Loading) return false;
        return shadow_replay_preview
            || (shadow_gpu_draw_shadows && ShadowSunFade() > 0.0f);
    }

    bool UseShadowLightCamera()
    {
        if (!IsEnabled()) return false;
        return shadow_replay_light_camera
            || (shadow_gpu_draw_shadows && ShadowSunFade() > 0.0f);
    }

    bool UseGpuTerrainReplay()
    {
        if (!IsEnabled()) return false;
        return !shadow_gpu_draw_shadows
            && shadow_replay_preview && shadow_gpu_terrain_replay;
    }

    float EffectiveShadowLightCameraDistance()
    {
        const auto light = GetSceneLightDirWorld();
        const auto horizontal = std::sqrt(
            light.x * light.x + light.y * light.y);
        const auto fitted_depth =
            std::max(shadow_light_camera_radius, 1.0f) * horizontal
            + kShadowVerticalCoverage * std::fabs(light.z) + 1000.0f;
        return std::max(
            std::max(std::fabs(shadow_light_camera_distance), 1.0f),
            fitted_depth);
    }

    bool BuildShadowLightCamera(
        const GW::Agent* player, const bool native_compatible,
        ShadowCamera::DirectionalCamera& camera, bool* vertical_clamped = nullptr)
    {
        if (!player) return false;

        const auto direction_to_light = GetSceneLightDirWorld();
        const auto direction_sign =
            shadow_light_camera_invert_direction ? 1.0f : -1.0f;
        DirectX::XMFLOAT3 light_direction = {
            direction_to_light.x * direction_sign,
            direction_to_light.y * direction_sign,
            direction_to_light.z * direction_sign};
        if (native_compatible) {
            light_direction.z = -light_direction.z;
        }

        auto clamped = false;
        constexpr auto minimum_horizontal = 0.017452406f;
        const auto horizontal_length = std::sqrt(
            light_direction.x * light_direction.x
            + light_direction.y * light_direction.y);
        const auto effective_azimuth = DirectX::XMConvertToRadians(
            CurrentSunAzimuth()
            + (CurrentSunElevation() <= kKeyLightCrossoverDeg ? 180.0f : 0.0f));
        if (native_compatible && horizontal_length < minimum_horizontal) {
            light_direction.x =
                direction_sign * std::cos(effective_azimuth) * minimum_horizontal;
            light_direction.y =
                direction_sign * std::sin(effective_azimuth) * minimum_horizontal;
            light_direction.z = std::copysign(
                std::sqrt(1.0f - minimum_horizontal * minimum_horizontal),
                light_direction.z);
            clamped = true;
        }
        if (vertical_clamped) *vertical_clamped = clamped;

        const auto horizontal_forward_x =
            direction_sign * std::cos(effective_azimuth);
        const auto horizontal_forward_y =
            direction_sign * std::sin(effective_azimuth);
        const auto light_horizontal = std::sqrt(
            light_direction.x * light_direction.x
            + light_direction.y * light_direction.y);
        const auto half_width = std::max(shadow_light_camera_radius, 1.0f);
        const auto half_height = native_compatible
            ? half_width
            : std::max(
                half_width * std::fabs(light_direction.z)
                    + kShadowVerticalCoverage * light_horizontal,
                250.0f);

        DirectX::XMFLOAT3 center = {player->x, player->y, player->z};

        if (native_compatible) {
            center.z = -center.z;
        }
        if (shadow_light_camera_forward_bias > 0.0f) {
            if (const auto* const game_camera = GW::CameraMgr::GetCamera()) {
                const auto view_x =
                    game_camera->look_at_target.x - game_camera->position.x;
                const auto view_y =
                    game_camera->look_at_target.y - game_camera->position.y;
                const auto view_length =
                    std::sqrt(view_x * view_x + view_y * view_y);
                if (view_length > 1e-3f) {
                    const auto offset =
                        half_width * shadow_light_camera_forward_bias;
                    center.x += view_x / view_length * offset;
                    center.y += view_y / view_length * offset;
                }
            }
        }

        const ShadowCamera::DirectionalCameraConfig config = {
            .light_direction = light_direction,
            .center = center,
            .right_hint = {
                horizontal_forward_y, -horizontal_forward_x, 0.0f},
            .half_width = half_width,
            .half_height = half_height,
            .depth_half_range = EffectiveShadowLightCameraDistance(),
            .texture_width = shadow_map_size,
            .texture_height = shadow_map_size,
            .depth_precision = shadow_preview_depth_precision,
            .snap_to_texels =
                !native_compatible && shadow_light_camera_orthographic};
        return ShadowCamera::BuildDirectional(config, camera);
    }

    using EnvUpdateSceneLightDir_pt = decltype(GW::Render::WorldRenderBindings::EnvUpdateSceneLightDir);
    EnvUpdateSceneLightDir_pt EnvUpdateSceneLightDir_Func = nullptr, EnvUpdateSceneLightDir_Ret = nullptr;

    void __fastcall OnEnvUpdateSceneLightDir(void* ecx, void* edx, float* sun_dir, void* old_state, void* new_state)
    {
        GW::Hook::EnterHook();
        if (align_shadows && sun_dir && IsEnabled()) {
            const DirectX::XMFLOAT3 dir = GetSceneLightDirWorld();
            sun_dir[0] = dir.x;
            sun_dir[1] = dir.y;
            sun_dir[2] = dir.z;
        }
        EnvUpdateSceneLightDir_Ret(ecx, edx, sun_dir, old_state, new_state);
        GW::Hook::LeaveHook();
    }

    void __cdecl OnGrRenderProgramQueue(
        uint32_t queue, void* program, void* renderer_state, int mask, uint32_t flags, void* draw_lists)
    {
        GW::Hook::EnterHook();
        if (shadow_gpu_resource_capture_active && program) {
            const auto batch_index = shadow_program_batch_index++;
            const auto program_handle =
                shadow_program_batch_handles && batch_index < shadow_program_batch_count
                    ? shadow_program_batch_handles[batch_index]
                    : 0;
            const auto terrain_program = IsTerrainProgram(program_handle);
            if (terrain_program) {
                const auto existing = std::ranges::find(
                    program_state_backups, program, &ProgramStateBackup::program);
                if (existing == program_state_backups.end()) {
                    auto& backup = program_state_backups.emplace_back();
                    backup.program = program;
                    std::memcpy(backup.state.data(), program, backup.state.size());
                    const auto* const bytes = static_cast<uint8_t*>(program);
                    backup.model_entries =
                        *reinterpret_cast<void* const*>(bytes + 0x9c);
                    const auto model_entry_count =
                        *reinterpret_cast<const uint32_t*>(bytes + 0xa4);
                    if (backup.model_entries && model_entry_count) {
                        backup.model_entry_state.resize(
                            static_cast<size_t>(model_entry_count)
                            * kGrRenderModelEntrySize);
                        std::memcpy(
                            backup.model_entry_state.data(),
                            backup.model_entries,
                            backup.model_entry_state.size());
                    }
                }
                const auto was_program = shadow_gpu_current_program;
                const auto was_program_handle = shadow_gpu_current_program_handle;
                const auto was_terrain = shadow_gpu_current_program_terrain;
                shadow_gpu_current_program = program;
                shadow_gpu_current_program_handle = program_handle;
                shadow_gpu_current_program_terrain = true;
                GrRenderProgramQueue_Ret(
                    queue, program, renderer_state, mask, flags, draw_lists);
                shadow_gpu_current_program = was_program;
                shadow_gpu_current_program_handle = was_program_handle;
                shadow_gpu_current_program_terrain = was_terrain;
            }
            GW::Hook::LeaveHook();
            return;
        }
        if (shadow_gpu_capture_active && !shadow_replay_active && program) {
            const auto batch_index = shadow_program_batch_index++;
            const auto program_handle =
                shadow_program_batch_handles && batch_index < shadow_program_batch_count
                    ? shadow_program_batch_handles[batch_index]
                    : 0;
            const auto was_program = shadow_gpu_current_program;
            const auto was_program_handle = shadow_gpu_current_program_handle;
            const auto was_terrain = shadow_gpu_current_program_terrain;
            shadow_gpu_current_program = program;
            shadow_gpu_current_program_handle = program_handle;
            shadow_gpu_current_program_terrain = IsTerrainProgram(program_handle);
            GrRenderProgramQueue_Ret(
                queue, program, renderer_state, mask, flags, draw_lists);
            shadow_gpu_current_program = was_program;
            shadow_gpu_current_program_handle = was_program_handle;
            shadow_gpu_current_program_terrain = was_terrain;
            GW::Hook::LeaveHook();
            return;
        }
        if (shadow_replay_active && program) {
            const ScopedPhase timer(Phase::ProgramQueueHook);
            ++shadow_replay_queue_count;
            const auto batch_index = shadow_program_batch_index++;
            const auto terrain_program =
                shadow_program_batch_handles && batch_index < shadow_program_batch_count
                && IsTerrainProgram(shadow_program_batch_handles[batch_index]);
            const auto was_terrain_program = shadow_terrain_program_queue_active;
            shadow_terrain_program_queue_active = terrain_program;
            if (terrain_program) ++shadow_terrain_queue_count;

            if (shadow_skip_water_draws && shadow_program_batch_handles
                && batch_index < shadow_program_batch_count
                && IsWaterProgram(shadow_program_batch_handles[batch_index])) {
                ++shadow_water_programs_skipped;
                shadow_terrain_program_queue_active = was_terrain_program;
                GW::Hook::LeaveHook();
                return;
            }
            if (shadow_replay_terrain_only && !terrain_program) {
                shadow_terrain_program_queue_active = was_terrain_program;
                GW::Hook::LeaveHook();
                return;
            }
            if (shadow_explicit_camera_render_active) {
                GrRenderProgramQueue_Ret(
                    queue, program, renderer_state, mask, flags, draw_lists);
                shadow_terrain_program_queue_active = was_terrain_program;
                GW::Hook::LeaveHook();
                return;
            }
            const auto existing = std::ranges::find(
                program_state_backups, program, &ProgramStateBackup::program);
            if (existing == program_state_backups.end()) {
                auto& backup = program_state_backups.emplace_back();
                backup.program = program;
                std::memcpy(backup.state.data(), program, backup.state.size());
                const auto* const bytes = static_cast<uint8_t*>(program);
                backup.model_entries = *reinterpret_cast<void* const*>(bytes + 0x9c);
                const auto model_entry_count = *reinterpret_cast<const uint32_t*>(bytes + 0xa4);
                if (backup.model_entries && model_entry_count) {
                    backup.model_entry_state.resize(
                        static_cast<size_t>(model_entry_count) * kGrRenderModelEntrySize);
                    std::memcpy(
                        backup.model_entry_state.data(), backup.model_entries,
                        backup.model_entry_state.size());
                }
            }
            auto* const original_snapshot =
                *reinterpret_cast<void**>(static_cast<uint8_t*>(program) + 0x78);
            if (!shadow_original_absolute_snapshot) {
                shadow_original_absolute_snapshot = original_snapshot;
            }
            auto* const shadow_transform_snapshot =
                terrain_program || original_snapshot == shadow_original_absolute_snapshot
                    ? shadow_absolute_transform_snapshot
                    : shadow_relative_transform_snapshot;
            if (terrain_program) {
                if (original_snapshot == shadow_original_absolute_snapshot) {
                    ++shadow_terrain_original_absolute_count;
                }
                else {
                    ++shadow_terrain_original_relative_count;
                }
            }
            if (shadow_transform_snapshot) {
                auto** const snapshot_slot =
                    reinterpret_cast<void**>(static_cast<uint8_t*>(program) + 0x78);
                if (*snapshot_slot != shadow_transform_snapshot) {
                    program_snapshot_swaps.push_back({snapshot_slot, *snapshot_slot});
                    *snapshot_slot = shadow_transform_snapshot;
                    ++shadow_replay_swap_count;
                }
            }
            GrRenderProgramQueue_Ret(queue, program, renderer_state, mask, flags, draw_lists);
            shadow_terrain_program_queue_active = was_terrain_program;
            GW::Hook::LeaveHook();
            return;
        }

        if (water_enabled && OceanSurfaceReady() && shadow_water_program_handle
            && IsEnabled()
            && shadow_program_batch_handles
            && shadow_program_batch_index < shadow_program_batch_count
            && IsWaterProgram(shadow_program_batch_handles[shadow_program_batch_index])) {
            ++shadow_program_batch_index;
            ++gw_water_programs_suppressed;
            GW::Hook::LeaveHook();
            return;
        }
        if (shadow_program_batch_handles) ++shadow_program_batch_index;
        GrRenderProgramQueue_Ret(queue, program, renderer_state, mask, flags, draw_lists);
        GW::Hook::LeaveHook();
    }

    void __cdecl OnGrRenderPrograms(
        const int renderer, const uint32_t count, const uint32_t* programs, const uint32_t flags)
    {
        GW::Hook::EnterHook();
        const ScopedPhase timer(Phase::RenderPrograms);
        const bool frcache = GameWorldCompositor::ProgramsBelongToFrCache(programs, count);
        if (scene_list_submit_active && !frcache && renderer == 0 && flags == 0
            && count >= 128) {
            scene_list_submit_is_world = true;
        }
        const auto replaying = shadow_replay_active;
        const auto resource_capture = shadow_gpu_resource_capture_active;
        if (replaying) {
            shadow_replay_renderer = static_cast<uint32_t>(renderer);
            shadow_replay_program_count = count;
        }
        const auto* const saved_batch_handles = shadow_program_batch_handles;
        const auto saved_batch_count = shadow_program_batch_count;
        const auto saved_batch_index = shadow_program_batch_index;
        const auto saved_gr_call = shadow_gpu_current_gr_call;
        shadow_program_batch_handles = programs;
        shadow_program_batch_count = count;
        shadow_program_batch_index = 0;
        if (!replaying && !resource_capture && shadow_gpu_capture_active) {
            shadow_gpu_current_gr_call = gr_program_call_count;
        }
        GrRenderPrograms_Ret(renderer, count, programs, flags);
        shadow_program_batch_handles = saved_batch_handles;
        shadow_program_batch_count = saved_batch_count;
        shadow_program_batch_index = saved_batch_index;
        shadow_gpu_current_gr_call = saved_gr_call;
        if (!replaying && !resource_capture && gr_program_call_count < gr_program_calls.size()) {
            gr_program_calls[gr_program_call_count++] = {
                reinterpret_cast<uintptr_t>(_ReturnAddress()),
                static_cast<uint32_t>(renderer), count, flags, frcache};
        }
        GW::Hook::LeaveHook();
    }

    void __cdecl OnGrRenderSceneLists(
        void* programs, void* secondary_programs, const int mode, void* clear_color,
        const int render_target)
    {
        GW::Hook::EnterHook();
        const auto submit_caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        shadow_scene_list_caller = submit_caller;
        scene_list_submit_active = true;
        scene_list_submit_is_world = programs == world_scene_programs;
        scene_list_submit_caller = submit_caller;
        GrRenderSceneLists_Ret(programs, secondary_programs, mode, clear_color, render_target);
        scene_list_submit_active = false;

        const auto frame_id = GameWorldCompositor::FrameId();
        const auto replay_every_world_submission =
            UseShadowLightCamera() && !UseGpuTerrainReplay();
        if (!shadow_replay_active && ShadowMapPassEnabled()
            && scene_list_submit_is_world
            && (replay_every_world_submission
                || frame_id != shadow_replayed_frame)) {
            shadow_replayed_frame = frame_id;
            ++shadow_replay_submission_count;
            shadow_trigger_scene_list_caller = submit_caller;
            if (auto* const device = GW::Render::GetDevice()) {
                if (const auto* const cam = GW::CameraMgr::GetCamera()) {
                    RenderShadowReplayPreview(
                        device, cam, programs, secondary_programs, mode, clear_color);
                }
            }
        }
        GW::Hook::LeaveHook();
    }

    HRESULT WINAPI OnSetRenderTarget(
        IDirect3DDevice9* device, const DWORD index, IDirect3DSurface9* surface)
    {
        GW::Hook::EnterHook();
        const auto redirect =
            (shadow_replay_active || shadow_target_redirect_active)
            && index == 0 && shadow_preview_surface
            && (!shadow_allow_auxiliary_targets
                || surface == shadow_original_main_target);
        if (redirect) {
            surface = shadow_preview_surface;
            ++shadow_replay_target_redirects;
        }
        const auto result = SetRenderTarget_Ret(device, index, surface);
        if (redirect && shadow_allow_auxiliary_targets && SUCCEEDED(result)) {
            const D3DVIEWPORT9 shadow_viewport = {
                0, 0, shadow_map_size, shadow_map_size, 0.0f, 1.0f};
            device->SetViewport(&shadow_viewport);
        }
        GW::Hook::LeaveHook();
        return result;
    }

    HRESULT WINAPI OnSetDepthStencilSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface)
    {
        GW::Hook::EnterHook();
        if ((shadow_replay_active || shadow_target_redirect_active)
            && shadow_preview_depth
            && (!shadow_allow_auxiliary_targets
                || surface == shadow_original_main_depth)) {
            surface = shadow_preview_depth;
            ++shadow_replay_target_redirects;
        }
        const auto result = SetDepthStencilSurface_Ret(device, surface);
        GW::Hook::LeaveHook();
        return result;
    }

    HRESULT WINAPI OnSetViewport(
        IDirect3DDevice9* device, const D3DVIEWPORT9* viewport)
    {
        GW::Hook::EnterHook();
        D3DVIEWPORT9 shadow_viewport = {
            0, 0, shadow_map_size, shadow_map_size, 0.0f, 1.0f};
        if (shadow_replay_active && shadow_allow_auxiliary_targets
            && shadow_preview_surface) {
            IDirect3DSurface9* current_target = nullptr;
            if (SUCCEEDED(device->GetRenderTarget(0, &current_target))
                && current_target) {
                if (current_target == shadow_preview_surface) {
                    viewport = &shadow_viewport;
                    ++shadow_replay_viewport_redirects;
                }
                current_target->Release();
            }
        }
        const auto result = SetViewport_Ret(device, viewport);
        GW::Hook::LeaveHook();
        return result;
    }

    bool ShouldSkipShadowDraw(IDirect3DDevice9* device)
    {
        if (!shadow_skip_non_depth_draws || !shadow_replay_active) return false;
        DWORD z_write = FALSE;
        device->GetRenderState(D3DRS_ZWRITEENABLE, &z_write);
        if (z_write) return false;
        ++shadow_replay_draws_skipped;
        return true;
    }

    void GuardReplayDepthWrites(IDirect3DDevice9* device)
    {
        shadow_replay_depth_guarded_this_draw = false;
        DWORD z_func = D3DCMP_LESSEQUAL;
        DWORD z_write = FALSE;
        DWORD z_enable = D3DZB_FALSE;
        device->GetRenderState(D3DRS_ZFUNC, &z_func);
        device->GetRenderState(D3DRS_ZWRITEENABLE, &z_write);
        device->GetRenderState(D3DRS_ZENABLE, &z_enable);
        if (z_func < shadow_replay_depth_histogram.size() / 4) {
            const auto slot = z_func * 4 + (z_write ? 2 : 0)
                + (z_enable == D3DZB_TRUE ? 1 : 0);
            ++shadow_replay_depth_histogram[slot];
        }
        if (z_func != D3DCMP_ALWAYS && z_func != D3DCMP_NEVER) return;
        shadow_replay_saved_z_write = z_write;
        if (!z_write) return;
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        shadow_replay_depth_guarded_this_draw = true;
        ++shadow_replay_depth_wipe_blocked;
    }

    void RestoreReplayDepthWrites(IDirect3DDevice9* device)
    {
        if (!shadow_replay_depth_guarded_this_draw) return;
        device->SetRenderState(D3DRS_ZWRITEENABLE, shadow_replay_saved_z_write);
        shadow_replay_depth_guarded_this_draw = false;
    }

    HRESULT WINAPI OnDrawPrimitive(
        IDirect3DDevice9* device, const D3DPRIMITIVETYPE type, const UINT start_vertex,
        const UINT primitive_count)
    {
        GW::Hook::EnterHook();
        if (shadow_replay_active) {
            ++shadow_replay_draw_count;
        }
        else if (shadow_target_redirect_active) {
            ++shadow_builder_draw_count;
        }
        else {
        }
        if (shadow_replay_active && ShouldSkipShadowDraw(device)) {
            GW::Hook::LeaveHook();
            return D3D_OK;
        }
        HRESULT result;
        if (shadow_replay_active) {
            GuardReplayDepthWrites(device);
            {
                const ScopedPhase timer(Phase::ReplayDraws);
                result = DrawPrimitive_Ret(device, type, start_vertex, primitive_count);
            }
            RestoreReplayDepthWrites(device);
        }
        else {
            result = DrawPrimitive_Ret(device, type, start_vertex, primitive_count);
        }
        GW::Hook::LeaveHook();
        return result;
    }

    void CaptureTerrainDraw(
        IDirect3DDevice9* device, const D3DPRIMITIVETYPE type, const INT base_vertex,
        const UINT min_vertex, const UINT vertex_count, const UINT start_index,
        const UINT primitive_count)
    {
        if (!shadow_gpu_current_program_terrain
            || captured_terrain_draws.size() >= 256
            || type != D3DPT_TRIANGLELIST) {
            return;
        }

        CapturedTerrainDraw draw;
        draw.type = type;
        draw.base_vertex = base_vertex;
        draw.min_vertex = min_vertex;
        draw.vertex_count = vertex_count;
        draw.start_index = start_index;
        draw.primitive_count = primitive_count;

        if (FAILED(device->GetVertexDeclaration(&draw.declaration))
            || !draw.declaration
            || FAILED(device->GetIndices(&draw.indices))
            || !draw.indices) {
            if (draw.indices) draw.indices->Release();
            if (draw.declaration) draw.declaration->Release();
            return;
        }

        std::array<D3DVERTEXELEMENT9, MAXD3DDECLLENGTH + 1> elements{};
        auto element_count = static_cast<UINT>(elements.size());
        if (FAILED(draw.declaration->GetDeclaration(elements.data(), &element_count))) {
            draw.indices->Release();
            draw.declaration->Release();
            return;
        }

        std::array<bool, 16> used_streams{};
        for (UINT i = 0; i < element_count && elements[i].Stream != 0xff; ++i) {
            if (elements[i].Stream < used_streams.size()) {
                used_streams[elements[i].Stream] = true;
            }
        }
        for (UINT stream_index = 0; stream_index < used_streams.size(); ++stream_index) {
            if (!used_streams[stream_index]) continue;
            auto& stream = draw.streams[stream_index];
            if (FAILED(device->GetStreamSource(
                    stream_index, &stream.buffer, &stream.offset, &stream.stride))
                || !stream.buffer) {
                for (auto& captured_stream : draw.streams) {
                    if (captured_stream.buffer) captured_stream.buffer->Release();
                }
                draw.indices->Release();
                draw.declaration->Release();
                return;
            }
            device->GetStreamSourceFreq(stream_index, &stream.frequency);
        }

        if (GrTransformGetCurrent_Func) {
            if (const auto* const model = GrTransformGetCurrent_Func(2)) {
                std::memcpy(
                    draw.model_transform.data(), model,
                    sizeof(draw.model_transform));
            }
        }
        draw.original_wvp_valid = SUCCEEDED(
            device->GetVertexShaderConstantF(
                0, draw.original_wvp.data(), 4));
        if (std::ranges::any_of(
                captured_terrain_draws,
                [&draw](const CapturedTerrainDraw& existing) {
                    return IsSameCapturedTerrainDraw(existing, draw);
                })) {
            ReleaseCapturedTerrainDraw(draw);
            return;
        }

        captured_terrain_draws.push_back(std::move(draw));
        shadow_gpu_captured_draw_count =
            static_cast<uint32_t>(captured_terrain_draws.size());
    }

    void CaptureGpuProgramDrawDiagnostic(
        IDirect3DDevice9* device, const D3DPRIMITIVETYPE type,
        const UINT vertex_count, const UINT primitive_count)
    {
        if (!shadow_gpu_current_program
            || shadow_gpu_program_draw_count >= shadow_gpu_program_draws.size()) {
            return;
        }

        auto& draw = shadow_gpu_program_draws[shadow_gpu_program_draw_count++];
        draw.gr_call = shadow_gpu_current_gr_call;
        draw.program_handle = shadow_gpu_current_program_handle;
        draw.program = reinterpret_cast<uintptr_t>(shadow_gpu_current_program);
        draw.terrain = shadow_gpu_current_program_terrain;
        draw.type = type;
        draw.vertex_count = vertex_count;
        draw.primitive_count = primitive_count;

        IDirect3DVertexShader9* vertex_shader = nullptr;
        if (SUCCEEDED(device->GetVertexShader(&vertex_shader)) && vertex_shader) {
            draw.vertex_shader = reinterpret_cast<uintptr_t>(vertex_shader);
            vertex_shader->Release();
        }
        IDirect3DPixelShader9* pixel_shader = nullptr;
        if (SUCCEEDED(device->GetPixelShader(&pixel_shader)) && pixel_shader) {
            draw.pixel_shader = reinterpret_cast<uintptr_t>(pixel_shader);
            pixel_shader->Release();
        }
        IDirect3DVertexDeclaration9* declaration = nullptr;
        if (SUCCEEDED(device->GetVertexDeclaration(&declaration)) && declaration) {
            draw.declaration = reinterpret_cast<uintptr_t>(declaration);
            draw.element_count = static_cast<UINT>(draw.elements.size());
            if (FAILED(declaration->GetDeclaration(
                    draw.elements.data(), &draw.element_count))) {
                draw.element_count = 0;
            }
            declaration->Release();
        }
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &draw.alpha_blend);
        device->GetRenderState(D3DRS_ALPHATESTENABLE, &draw.alpha_test);
        device->GetRenderState(D3DRS_ALPHAREF, &draw.alpha_ref);
        device->GetRenderState(D3DRS_SRCBLEND, &draw.src_blend);
        device->GetRenderState(D3DRS_DESTBLEND, &draw.dest_blend);
        if (const auto* const model = GrTransformGetCurrent_Func(2)) {
            std::memcpy(
                draw.model_transform.data(), model,
                sizeof(draw.model_transform));
        }
    }

    void CaptureNativeLightWorldViewProjection(IDirect3DDevice9* device)
    {
        if (shadow_native_light_wvp_ready || !GrTransformGetCurrent_Func) return;
        D3DVIEWPORT9 viewport{};
        if (FAILED(device->GetViewport(&viewport))
            || viewport.Width != shadow_map_size
            || viewport.Height != shadow_map_size) {
            return;
        }
        const auto* const view = GrTransformGetCurrent_Func(1);
        if (!view) return;

        float constants[16];
        if (FAILED(device->GetVertexShaderConstantF(0, constants, 4))) return;
        const auto* const row_w = constants + 12;
        const auto matches_view_z =
            std::fabs(row_w[0] - view[8]) <= 1e-3f
            && std::fabs(row_w[1] - view[9]) <= 1e-3f
            && std::fabs(row_w[2] - view[10]) <= 1e-3f
            && std::fabs(row_w[3] - view[11]) <= 1e-3f;
        const auto is_affine_w =
            std::fabs(row_w[0]) <= 1e-6f && std::fabs(row_w[1]) <= 1e-6f
            && std::fabs(row_w[2]) <= 1e-6f
            && std::fabs(row_w[3] - 1.0f) <= 1e-3f;
        if (!matches_view_z && !is_affine_w) return;
        std::memcpy(
            shadow_native_light_wvp.data(), constants,
            sizeof(shadow_native_light_wvp));
        shadow_native_light_wvp_ready = true;

        if (is_affine_w) {
            const auto* const row_z = shadow_native_light_wvp.data() + 8;

            shadow_native_depth_slope = std::sqrt(
                row_z[0] * row_z[0] + row_z[1] * row_z[1] + row_z[2] * row_z[2]);
            shadow_native_light_orthographic = true;
            return;
        }
        shadow_native_light_orthographic = false;
        const auto* const row_z = shadow_native_light_wvp.data() + 8;
        size_t best = 0;
        for (size_t i = 1; i < 3; ++i) {
            if (std::fabs(row_w[i]) > std::fabs(row_w[best])) best = i;
        }
        if (std::fabs(row_w[best]) > 1e-6f) {
            shadow_native_depth_slope = row_z[best] / row_w[best];
        }
    }

    void CaptureCameraWorldViewProjection(IDirect3DDevice9* device)
    {
        if (!shadow_camera_wvp_capture_pending || !GrTransformGetCurrent_Func
            || !shadow_main_viewport_width) {
            return;
        }
        D3DVIEWPORT9 viewport{};
        if (FAILED(device->GetViewport(&viewport))
            || viewport.Width != shadow_main_viewport_width
            || viewport.Height != shadow_main_viewport_height) {
            return;
        }
        const auto* const view = GrTransformGetCurrent_Func(1);
        if (!view) return;
        float constants[16];
        if (FAILED(device->GetVertexShaderConstantF(0, constants, 4))) return;
        for (size_t i = 0; i < 4; ++i) {
            if (std::fabs(constants[12 + i] - view[8 + i]) > 1e-3f) return;
        }
        std::memcpy(
            shadow_camera_wvp.data(), constants, sizeof(shadow_camera_wvp));
        shadow_camera_wvp_ready = true;
        shadow_camera_wvp_capture_pending = false;
    }

    HRESULT WINAPI OnDrawIndexedPrimitive(
        IDirect3DDevice9* device, const D3DPRIMITIVETYPE type, const INT base_vertex,
        const UINT min_vertex, const UINT vertex_count, const UINT start_index,
        const UINT primitive_count)
    {
        GW::Hook::EnterHook();
        if (shadow_gpu_resource_capture_active) {
            CaptureGpuProgramDrawDiagnostic(
                device, type, vertex_count, primitive_count);
            CaptureTerrainDraw(
                device, type, base_vertex, min_vertex, vertex_count,
                start_index, primitive_count);
        }
        else if (shadow_gpu_capture_active) {
            CaptureGpuProgramDrawDiagnostic(
                device, type, vertex_count, primitive_count);
            CaptureTerrainDraw(
                device, type, base_vertex, min_vertex, vertex_count,
                start_index, primitive_count);
        }
        if (shadow_replay_active) {
            ++shadow_replay_draw_count;
            CaptureNativeLightWorldViewProjection(device);
        }
        else if (shadow_target_redirect_active) {
            ++shadow_builder_draw_count;
        }
        else {
            CaptureCameraWorldViewProjection(device);
        }
        if (shadow_replay_active && ShouldSkipShadowDraw(device)) {
            GW::Hook::LeaveHook();
            return D3D_OK;
        }
        HRESULT result;
        if (shadow_replay_active) {
            GuardReplayDepthWrites(device);
            {
                const ScopedPhase timer(Phase::ReplayDraws);
                result = DrawIndexedPrimitive_Ret(
                    device, type, base_vertex, min_vertex, vertex_count, start_index, primitive_count);
            }
            RestoreReplayDepthWrites(device);
        }
        else {
            result = DrawIndexedPrimitive_Ret(
                device, type, base_vertex, min_vertex, vertex_count, start_index, primitive_count);
        }
        GW::Hook::LeaveHook();
        return result;
    }

    HRESULT WINAPI OnSetRenderState(
        IDirect3DDevice9* device, const D3DRENDERSTATETYPE state, const DWORD value)
    {

        if (state == D3DRS_FOGENABLE) {

            gw_requested_fog_enable = value;

            if (suppress_gw_fog && IsEnabled()) {
                return SetRenderState_Ret(device, state, FALSE);
            }
        }
        if (state == D3DRS_FOGCOLOR) {
            gw_requested_fog_colour = value;
            if (recolour_gw_fog && IsEnabled()) {
                return SetRenderState_Ret(device, state, gw_fog_colour_override);
            }
        }
        return SetRenderState_Ret(device, state, value);
    }

    void SyncGwFogState(IDirect3DDevice9* const device)
    {
        if (!device || !SetRenderState_Ret) return;
        const auto atmosphere_enabled = IsEnabled();
        SetRenderState_Ret(
            device, D3DRS_FOGENABLE,
            suppress_gw_fog && atmosphere_enabled ? FALSE : gw_requested_fog_enable);
        SetRenderState_Ret(
            device, D3DRS_FOGCOLOR,
            recolour_gw_fog && atmosphere_enabled ? gw_fog_colour_override : gw_requested_fog_colour);
    }

    bool EnsureD3DReplayHooks(IDirect3DDevice9* device)
    {
        if (!device_hooks.empty()) return true;
        if (hooks_failed || !device) return false;
        auto* const vtable = *reinterpret_cast<uintptr_t**>(device);
        SetRenderTarget_Func = reinterpret_cast<SetRenderTarget_pt>(vtable[37]);
        SetDepthStencilSurface_Func = reinterpret_cast<SetDepthStencilSurface_pt>(vtable[39]);
        SetRenderState_Func = reinterpret_cast<SetRenderState_pt>(vtable[57]);
        SetViewport_Func = reinterpret_cast<SetViewport_pt>(vtable[47]);
        DrawPrimitive_Func = reinterpret_cast<DrawPrimitive_pt>(vtable[81]);
        DrawIndexedPrimitive_Func = reinterpret_cast<DrawIndexedPrimitive_pt>(vtable[82]);
        if (!CreateOwnedHook(SetRenderTarget_Func, OnSetRenderTarget, SetRenderTarget_Ret, device_hooks, "SetRenderTarget")
            || !CreateOwnedHook(SetDepthStencilSurface_Func, OnSetDepthStencilSurface, SetDepthStencilSurface_Ret, device_hooks, "SetDepthStencilSurface")
            || !CreateOwnedHook(SetRenderState_Func, OnSetRenderState, SetRenderState_Ret, device_hooks, "SetRenderState")
            || !CreateOwnedHook(SetViewport_Func, OnSetViewport, SetViewport_Ret, device_hooks, "SetViewport")
            || !CreateOwnedHook(DrawPrimitive_Func, OnDrawPrimitive, DrawPrimitive_Ret, device_hooks, "DrawPrimitive")
            || !CreateOwnedHook(DrawIndexedPrimitive_Func, OnDrawIndexedPrimitive, DrawIndexedPrimitive_Ret, device_hooks, "DrawIndexedPrimitive")) {
            for (const auto hook : device_hooks) GW::Hook::RemoveHook(hook);
            device_hooks.clear();
            SetRenderState_Ret = nullptr;
            hooks_ready = false;
            hooks_failed = true;
            return false;
        }
        device->GetRenderState(D3DRS_FOGENABLE, &gw_requested_fog_enable);
        device->GetRenderState(D3DRS_FOGCOLOR, &gw_requested_fog_colour);
        for (const auto hook : device_hooks) GW::Hook::EnableHooks(hook);
        return true;
    }

    void CaptureTerrainResources(
        IDirect3DDevice9* device, void* programs, void* secondary_programs,
        const int mode, void* clear_color)
    {
        ReleaseCapturedTerrainDraws();
        shadow_gpu_captured_draw_count = 0;
        shadow_gpu_replayed_draw_count = 0;
        shadow_gpu_program_draw_count = 0;
        if (shadow_terrain_program_handles.empty()) return;

        IDirect3DStateBlock9* state_block = nullptr;
        IDirect3DSurface9* old_target = nullptr;
        IDirect3DSurface9* old_depth = nullptr;
        D3DVIEWPORT9 old_viewport{};
        const auto state_ready =
            SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &state_block))
            && SUCCEEDED(state_block->Capture())
            && SUCCEEDED(device->GetRenderTarget(0, &old_target))
            && SUCCEEDED(device->GetDepthStencilSurface(&old_depth))
            && SUCCEEDED(device->GetViewport(&old_viewport));
        if (!state_ready) {
            if (old_depth) old_depth->Release();
            if (old_target) old_target->Release();
            if (state_block) state_block->Release();
            return;
        }

        const D3DVIEWPORT9 shadow_viewport = {
            0, 0, shadow_map_size, shadow_map_size, 0.0f, 1.0f};
        device->SetRenderTarget(0, shadow_preview_surface);
        device->SetDepthStencilSurface(shadow_preview_depth);
        device->SetViewport(&shadow_viewport);
        device->Clear(
            0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
            0xffffffff, 1.0f, 0);

        program_state_backups.clear();
        shadow_target_redirect_active = true;
        shadow_gpu_resource_capture_active = true;
        GrRenderSceneLists_Ret(
            programs, secondary_programs, mode, clear_color, 0);
        shadow_gpu_resource_capture_active = false;
        shadow_target_redirect_active = false;

        for (auto& backup : program_state_backups) {
            if (!backup.model_entry_state.empty()) {
                std::memcpy(
                    backup.model_entries, backup.model_entry_state.data(),
                    backup.model_entry_state.size());
            }
            std::memcpy(
                backup.program, backup.state.data(), backup.state.size());
        }
        program_state_backups.clear();

        device->SetRenderTarget(0, old_target);
        device->SetDepthStencilSurface(old_depth);
        device->SetViewport(&old_viewport);
        state_block->Apply();
        old_depth->Release();
        old_target->Release();
        state_block->Release();
    }

    void RenderCapturedTerrainPreview(IDirect3DDevice9* device)
    {
        IDirect3DStateBlock9* state_block = nullptr;
        IDirect3DSurface9* old_target = nullptr;
        IDirect3DSurface9* old_depth = nullptr;
        D3DVIEWPORT9 old_viewport{};
        const auto state_ready =
            SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &state_block))
            && SUCCEEDED(state_block->Capture())
            && SUCCEEDED(device->GetRenderTarget(0, &old_target))
            && SUCCEEDED(device->GetDepthStencilSurface(&old_depth))
            && SUCCEEDED(device->GetViewport(&old_viewport));
        if (!state_ready) {
            if (old_depth) old_depth->Release();
            if (old_target) old_target->Release();
            if (state_block) state_block->Release();
            ReleaseCapturedTerrainDraws();
            return;
        }

        const D3DVIEWPORT9 shadow_viewport = {
            0, 0, shadow_map_size, shadow_map_size, 0.0f, 1.0f};
        device->SetRenderTarget(0, shadow_preview_surface);
        device->SetDepthStencilSurface(shadow_preview_depth);
        device->SetViewport(&shadow_viewport);
        device->Clear(
            0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
            0xffffffff, 1.0f, 0);
        shadow_gpu_light_matrices_ready = false;
        shadow_light_camera_state_ready = false;

        ShadowCamera::DirectionalCamera camera{};
        if (BuildShadowLightCamera(
                GW::Agents::GetControlledCharacter(), false, camera)
            && !captured_terrain_draws.empty()) {
            shadow_light_camera_state = camera;
            shadow_light_camera_state_ready = true;
            shadow_native_vertical_clamped = false;
            DirectX::XMFLOAT4X4A view_matrix{};
            DirectX::XMStoreFloat4x4A(
                &view_matrix,
                DirectX::XMMatrixTranspose(
                    DirectX::XMLoadFloat4x4(&camera.view)));
            DirectX::XMFLOAT4X4A projection_matrix{};
            const auto projection = shadow_light_camera_orthographic
                ? DirectX::XMLoadFloat4x4(&camera.projection)
                : DirectX::XMMatrixPerspectiveFovLH(
                    DirectX::XMConvertToRadians(std::clamp(
                        shadow_light_camera_fov, 10.0f, 170.0f)),
                    1.0f, 1.0f, GameWorldCompositor::kZFar);
            DirectX::XMStoreFloat4x4A(
                &projection_matrix, DirectX::XMMatrixTranspose(projection));
            shadow_gpu_light_view = view_matrix;
            shadow_gpu_light_projection = projection_matrix;
            shadow_gpu_light_matrices_ready = true;

            device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            device->SetRenderState(D3DRS_FOGENABLE, FALSE);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
            device->SetVertexShader(shadow_terrain_replay_vs_object);
            device->SetPixelShader(shadow_terrain_replay_ps_object);
            device->SetVertexShaderConstantF(
                0, reinterpret_cast<const float*>(&view_matrix), 4);
            device->SetVertexShaderConstantF(
                4, reinterpret_cast<const float*>(&projection_matrix), 4);

            shadow_gpu_replayed_draw_count = 0;
            for (const auto& draw : captured_terrain_draws) {
                device->SetVertexDeclaration(draw.declaration);
                for (UINT stream_index = 0; stream_index < draw.streams.size(); ++stream_index) {
                    const auto& stream = draw.streams[stream_index];
                    if (!stream.buffer) continue;
                    device->SetStreamSource(
                        stream_index, stream.buffer, stream.offset, stream.stride);
                    device->SetStreamSourceFreq(stream_index, stream.frequency);
                }
                device->SetIndices(draw.indices);
                if (SUCCEEDED(device->DrawIndexedPrimitive(
                        draw.type, draw.base_vertex, draw.min_vertex,
                        draw.vertex_count, draw.start_index,
                        draw.primitive_count))) {
                    ++shadow_gpu_replayed_draw_count;
                }
            }
        }

        if (!shadow_gpu_draw_shadows) {
            struct MarkerVertex {
                float x;
                float y;
                float z;
                float rhw;
                D3DCOLOR color;
            };
            const auto center = static_cast<float>(shadow_map_size) * 0.5f;
            constexpr auto marker_radius = 8.0f;
            const MarkerVertex marker[] = {
                {center - marker_radius, center, 0.0f, 1.0f, 0xffff3030},
                {center + marker_radius, center, 0.0f, 1.0f, 0xffff3030},
                {center, center - marker_radius, 0.0f, 1.0f, 0xffff3030},
                {center, center + marker_radius, 0.0f, 1.0f, 0xffff3030}};
            device->SetRenderState(D3DRS_ZENABLE, FALSE);
            device->SetVertexShader(nullptr);
            device->SetPixelShader(nullptr);
            device->SetVertexDeclaration(nullptr);
            device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            device->SetTexture(0, nullptr);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->DrawPrimitiveUP(D3DPT_LINELIST, 2, marker, sizeof(MarkerVertex));
        }

        device->SetRenderTarget(0, old_target);
        device->SetDepthStencilSurface(old_depth);
        device->SetViewport(&old_viewport);
        state_block->Apply();
        old_depth->Release();
        old_target->Release();
        state_block->Release();
    }

    bool ShadowReplayFunctionsReady()
    {
        return GrRenderPrograms_Func && GrRenderProgramQueue_Func
            && GrTransformIdentity_Func
            && GrTransformSetAdjustedLookAt_Func
            && GrTransformPerspective_Func && GrTransformOrthographic_Func
            && GrTransformGetCurrent_Func && GrTransformSetCurrent_Func
            && GrRenderSceneLists_Func && CameraRenderScope_Func && SceneRenderScope_Func
            && GmWorldUpdateView_Func && GrSetCameraTransform_Func
            && GmPropsUpdateView_Func && GetPropLodScale_Func
            && TrnCollectScenePrograms_Ret && GetViewVisibilityBuffer_Ret
            && GrModelFrustumCull_Ret && GrCullBuildVisibilityGrid_Ret;
    }

    bool ReuseExistingShadowMap(const ShadowCamera::DirectionalCamera& camera)
    {

        if (!shadow_reuse_shadow_map) return false;

        if (!shadow_map_valid || !shadow_native_light_wvp_ready) return false;

        constexpr float kEpsilon = 1e-3f;
        const auto same = [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
            return std::fabs(a.x - b.x) < kEpsilon && std::fabs(a.y - b.y) < kEpsilon
                && std::fabs(a.z - b.z) < kEpsilon;
        };

        const bool identical = same(camera.snapped_center, shadow_map_center)
            && same(camera.forward, shadow_map_forward)
            && same(camera.bounds, shadow_map_bounds);
        if (identical) return true;

        const auto interval = static_cast<uint64_t>(std::max(shadow_update_interval, 1));
        return GameWorldCompositor::FrameId() - shadow_map_frame < interval;
    }

    void RenderShadowReplayPreview(
        IDirect3DDevice9* device, const GW::Camera*, void* programs,
        void* secondary_programs, const int mode, void* clear_color)
    {
        if (!ShadowMapPassEnabled() || !EnsureShadowPreviewResources(device)
            || !EnsureD3DReplayHooks(device)) {
            return;
        }
        const ScopedPhase fill_timer(Phase::ShadowFillTotal);
        if (UseGpuTerrainReplay()) {
            CaptureTerrainResources(
                device, programs, secondary_programs, mode, clear_color);
            RenderCapturedTerrainPreview(device);
            return;
        }
        if (!ShadowReplayFunctionsReady()) return;

        std::array<std::array<float, 13>, 5> saved_transforms{};
        std::array<bool, 5> saved_transform_valid{};
        for (size_t slot = 0; slot < saved_transforms.size(); ++slot) {
            if (const auto* const transform =
                    GrTransformGetCurrent_Func(static_cast<int>(slot))) {
                std::memcpy(
                    saved_transforms[slot].data(), transform,
                    sizeof(saved_transforms[slot]));
                saved_transform_valid[slot] = true;
            }
        }
        if (!saved_transform_valid[0] || !saved_transform_valid[1]) return;

        float eye[3]{};
        float target[3]{};
        float up[3]{};
        shadow_light_camera_state_ready = false;
        shadow_native_vertical_clamped = false;
        if (UseShadowLightCamera()) {
            ShadowCamera::DirectionalCamera camera{};
            auto vertical_clamped = false;
            if (!BuildShadowLightCamera(
                    GW::Agents::GetControlledCharacter(), true, camera,
                    &vertical_clamped)) {
                return;
            }
            shadow_light_camera_state = camera;
            shadow_light_camera_state_ready = true;
            shadow_native_vertical_clamped = vertical_clamped;
            eye[0] = camera.eye.x;
            eye[1] = camera.eye.y;
            eye[2] = camera.eye.z;
            target[0] = camera.target.x;
            target[1] = camera.target.y;
            target[2] = camera.target.z;

            up[0] = -camera.up.x;
            up[1] = -camera.up.y;
            up[2] = camera.up.z;

            if (ReuseExistingShadowMap(camera)) {
                ++shadow_map_reuse_count;
                return;
            }
            shadow_map_center = camera.snapped_center;
            shadow_map_forward = camera.forward;
            shadow_map_bounds = camera.bounds;
            shadow_map_frame = GameWorldCompositor::FrameId();
            shadow_map_valid = true;
            ++shadow_map_render_count;

            shadow_cell_override_eye[0] = camera.snapped_center.x;
            shadow_cell_override_eye[1] = camera.snapped_center.y;
            shadow_cell_override_eye[2] = -camera.snapped_center.z;
        }

        IDirect3DStateBlock9* state_block = nullptr;
        IDirect3DSurface9* old_target = nullptr;
        IDirect3DSurface9* old_depth = nullptr;
        D3DVIEWPORT9 old_viewport{};
        const bool state_ready = SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &state_block))
            && SUCCEEDED(state_block->Capture())
            && SUCCEEDED(device->GetRenderTarget(0, &old_target))
            && SUCCEEDED(device->GetDepthStencilSurface(&old_depth))
            && SUCCEEDED(device->GetViewport(&old_viewport));
        if (state_ready) {
            const D3DVIEWPORT9 shadow_viewport = {
                0, 0, shadow_map_size, shadow_map_size, 0.0f, 1.0f};
            device->SetRenderTarget(0, shadow_preview_surface);
            device->SetDepthStencilSurface(shadow_preview_depth);
            device->SetViewport(&shadow_viewport);
            device->Clear(
                0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                0xffff00ff, 1.0f, 0);

            program_snapshot_swaps.clear();
            program_state_backups.clear();
            shadow_original_absolute_snapshot = nullptr;
            shadow_replay_queue_count = 0;
            shadow_replay_swap_count = 0;
            shadow_replay_target_redirects = 0;
            shadow_replay_viewport_redirects = 0;
            shadow_replay_draw_count = 0;
            shadow_terrain_queue_count = 0;
            shadow_water_programs_skipped = 0;

            RefreshWaterProgramHandles();
            shadow_terrain_cull_bypass_count = 0;
            shadow_terrain_cull_rejected_count = 0;
            shadow_cell_overrides = 0;
            shadow_objects_not_culled = 0;
            shadow_prop_fades_skipped = 0;
            shadow_terrain_original_absolute_count = 0;
            shadow_terrain_original_relative_count = 0;
            shadow_terrain_collect_count = 0;
            shadow_terrain_visibility_override_count = 0;
            shadow_cell_visibility_override_count = 0;
            shadow_cell_visibility_too_large_count = 0;
            shadow_terrain_program_handles.clear();
            shadow_builder_draw_count = 0;
            shadow_primary_program_count = 0;
            shadow_gpu_light_matrices_ready = false;
            shadow_native_light_wvp_ready = false;
            shadow_replay_depth_wipe_blocked = 0;
            shadow_replay_depth_histogram.fill(0);
            shadow_replay_draws_skipped = 0;

            if (UseShadowLightCamera()) {
                GrTransformIdentity_Func(0);
                if (shadow_light_camera_orthographic) {
                    shadow_light_projection_fov = 0.0f;
                    float bounds[4] = {
                        -shadow_light_camera_radius, -shadow_light_camera_radius,
                        shadow_light_camera_radius, shadow_light_camera_radius};
                    GrTransformOrthographic_Func(
                        0, bounds, shadow_light_camera_state.bounds.z);
                }
                else {
                    const auto fov = DirectX::XMConvertToRadians(std::clamp(
                        shadow_light_camera_fov, 10.0f, 170.0f));
                    shadow_light_projection_fov = fov;
                    GrTransformPerspective_Func(
                        0, fov, GameWorldCompositor::kZFar, 1.0f, 0);
                }
                GrTransformSetAdjustedLookAt_Func(eye, target, up, 1, 0);
                shadow_gpu_light_matrices_ready = false;
                CaptureNativeShadowMatrices();
                std::memcpy(shadow_scene_eye, eye, sizeof(eye));
                std::memcpy(shadow_scene_target, target, sizeof(target));
                std::memcpy(shadow_scene_up, up, sizeof(up));
                shadow_scene_rebuild_active = true;
                shadow_explicit_camera_render_active = true;
                shadow_replay_active = true;
                shadow_allow_auxiliary_targets = true;
                shadow_cell_override_active = shadow_props_follow_light;
                shadow_cell_visibility_override_active = true;
                shadow_original_main_target = old_target;
                shadow_original_main_depth = old_depth;
                {
                    const ScopedPhase timer(Phase::WorldRenderFromCamera);
                    if (!RenderShadowScene(eye, target, up)) {
                        shadow_map_valid = false;
                        shadow_native_light_wvp_ready = false;
                    }
                }
                shadow_cell_visibility_override_active = false;
                shadow_cell_override_active = false;
                shadow_original_main_depth = nullptr;
                shadow_original_main_target = nullptr;
                shadow_allow_auxiliary_targets = false;
                shadow_replay_active = false;
                shadow_explicit_camera_render_active = false;
                shadow_scene_rebuild_active = false;
            }
            else {
                shadow_replay_active = true;
                {
                    const ScopedPhase timer(Phase::SceneListsSubmit);
                    GrRenderSceneLists_Ret(
                        programs, secondary_programs, mode, clear_color, 0);
                }
                shadow_replay_active = false;
            }
            for (auto it = program_snapshot_swaps.rbegin(); it != program_snapshot_swaps.rend(); ++it) {
                *it->slot = it->original;
            }
            program_snapshot_swaps.clear();
            for (auto& backup : program_state_backups) {
                if (!backup.model_entry_state.empty()) {
                    std::memcpy(
                        backup.model_entries, backup.model_entry_state.data(),
                        backup.model_entry_state.size());
                }
                std::memcpy(backup.program, backup.state.data(), backup.state.size());
            }
            program_state_backups.clear();

            if (shadow_light_cull_enabled && !shadow_light_cull_degenerate
                && shadow_terrain_cull_rejected_count > 0
                && shadow_terrain_cull_bypass_count == 0) {
                shadow_light_cull_degenerate = true;
                Log::Log(
                    "[Skybox] light-box culling rejected all %u terrain tiles - disabling it "
                    "(frustum was not the light camera's)",
                    shadow_terrain_cull_rejected_count);
            }

            // Applying the saved state block here would desynchronise GW's render-state cache.
            device->SetRenderTarget(0, old_target);
            device->SetDepthStencilSurface(old_depth);
            device->SetViewport(&old_viewport);

        }
        if (old_depth) old_depth->Release();
        if (old_target) old_target->Release();
        if (state_block) state_block->Release();

        for (size_t slot = saved_transforms.size(); slot-- > 0;) {
            if (saved_transform_valid[slot]) {
                GrTransformSetCurrent_Func(
                    static_cast<int>(slot), saved_transforms[slot].data());
            }
        }
        if (shadow_absolute_transform_snapshot) {
            GrTransformSnapshotRelease_Func(shadow_absolute_transform_snapshot);
        }
        if (shadow_relative_transform_snapshot) {
            GrTransformSnapshotRelease_Func(shadow_relative_transform_snapshot);
        }
        shadow_absolute_transform_snapshot = nullptr;
        shadow_relative_transform_snapshot = nullptr;
        shadow_original_absolute_snapshot = nullptr;
        shadow_replay_active = false;
        shadow_target_redirect_active = false;
        shadow_allow_auxiliary_targets = false;
        shadow_original_main_target = nullptr;
        shadow_original_main_depth = nullptr;
        shadow_scene_rebuild_active = false;
        shadow_explicit_camera_render_active = false;
    }

    void DrawCapturedTerrainShadows(IDirect3DDevice9* device)
    {
        auto* const shadow_map = shadow_preview_texture;
        if (!shadow_gpu_draw_shadows || !shadow_gpu_light_matrices_ready
            || captured_terrain_draws.empty() || !shadow_map) {
            ReleaseCapturedTerrainDraws();
            return;
        }

        IDirect3DStateBlock9* state_block = nullptr;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state_block))
            || FAILED(state_block->Capture())) {
            if (state_block) state_block->Release();
            ReleaseCapturedTerrainDraws();
            return;
        }

        device->SetVertexShader(shadow_terrain_receive_vs_object);
        device->SetPixelShader(shadow_terrain_receive_ps_object);
        if (!GameWorldCompositor::SetWorldViewProj(device)) {
            state_block->Apply();
            state_block->Release();
            ReleaseCapturedTerrainDraws();
            return;
        }
        device->SetVertexShaderConstantF(
            8, reinterpret_cast<const float*>(&shadow_gpu_light_view), 4);
        device->SetVertexShaderConstantF(
            12, reinterpret_cast<const float*>(&shadow_gpu_light_projection), 4);
        const float settings[4] = {
            shadow_gpu_bias, shadow_gpu_strength,
            1.0f / static_cast<float>(shadow_map_size),
            shadow_gpu_slope_bias};
        device->SetPixelShaderConstantF(0, settings, 1);
        device->SetTexture(0, shadow_map);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
        device->SetRenderState(
            D3DRS_DEPTHBIAS, std::bit_cast<DWORD>(-0.00001f));

        for (const auto& draw : captured_terrain_draws) {
            device->SetVertexDeclaration(draw.declaration);
            for (UINT stream_index = 0; stream_index < draw.streams.size(); ++stream_index) {
                const auto& stream = draw.streams[stream_index];
                if (!stream.buffer) continue;
                device->SetStreamSource(
                    stream_index, stream.buffer, stream.offset, stream.stride);
                device->SetStreamSourceFreq(stream_index, stream.frequency);
            }
            device->SetIndices(draw.indices);
            device->DrawIndexedPrimitive(
                draw.type, draw.base_vertex, draw.min_vertex,
                draw.vertex_count, draw.start_index, draw.primitive_count);
        }

        state_block->Apply();
        state_block->Release();
    }

    bool EnsureCameraDepthTexture(
        IDirect3DDevice9* device, const uint32_t width, const uint32_t height)
    {

        IDirect3DSurface9* depth_surface = nullptr;
        D3DSURFACE_DESC depth_desc{};
        auto surface_width = width;
        auto surface_height = height;
        if (SUCCEEDED(device->GetDepthStencilSurface(&depth_surface)) && depth_surface) {
            if (SUCCEEDED(depth_surface->GetDesc(&depth_desc))) {
                shadow_depth_surface_format = depth_desc.Format;
                shadow_depth_surface_msaa = depth_desc.MultiSampleType;
                shadow_depth_surface_msaa_quality = depth_desc.MultiSampleQuality;
                if (depth_desc.Width > 0 && depth_desc.Height > 0) {
                    surface_width = depth_desc.Width;
                    surface_height = depth_desc.Height;
                }
            }
            depth_surface->Release();
        }
        shadow_depth_surface_width = surface_width;
        shadow_depth_surface_height = surface_height;

        if (shadow_camera_depth_texture
            && shadow_camera_depth_width == surface_width
            && shadow_camera_depth_height == surface_height) {
            return true;
        }
        if (shadow_camera_depth_texture) {
            shadow_camera_depth_texture->Release();
            shadow_camera_depth_texture = nullptr;
        }
        shadow_camera_depth_width = 0;
        shadow_camera_depth_height = 0;
        constexpr auto intz = static_cast<D3DFORMAT>(
            MAKEFOURCC('I', 'N', 'T', 'Z'));
        if (FAILED(device->CreateTexture(
                surface_width, surface_height, 1, D3DUSAGE_DEPTHSTENCIL, intz,
                D3DPOOL_DEFAULT, &shadow_camera_depth_texture, nullptr))) {
            return false;
        }
        shadow_camera_depth_width = surface_width;
        shadow_camera_depth_height = surface_height;
        IDirect3D9* d3d = nullptr;
        if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
            D3DDEVICE_CREATION_PARAMETERS params{};
            device->GetCreationParameters(&params);
            shadow_camera_depth_resz_supported = SUCCEEDED(d3d->CheckDeviceFormat(
                params.AdapterOrdinal, params.DeviceType, D3DFMT_X8R8G8B8,
                D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE,
                static_cast<D3DFORMAT>(MAKEFOURCC('R', 'E', 'S', 'Z'))));
            d3d->Release();
        }
        return true;
    }

    bool CopyCameraDepth(
        IDirect3DDevice9* device, const uint32_t width, const uint32_t height)
    {
        if (!EnsureCameraDepthTexture(device, width, height)) {
            shadow_camera_depth_failure = "texture creation";
            shadow_camera_depth_copy_succeeded = false;
            return false;
        }

        shadow_reported_viewport_width = width;
        shadow_reported_viewport_height = height;
        shadow_render_target_width = 0;
        shadow_render_target_height = 0;
        if (IDirect3DSurface9* render_target = nullptr;
            SUCCEEDED(device->GetRenderTarget(0, &render_target)) && render_target) {
            if (D3DSURFACE_DESC rt_desc{}; SUCCEEDED(render_target->GetDesc(&rt_desc))) {
                shadow_render_target_width = rt_desc.Width;
                shadow_render_target_height = rt_desc.Height;
                shadow_render_target_format = rt_desc.Format;
                shadow_render_target_msaa = rt_desc.MultiSampleType;
                shadow_render_target_msaa_quality = rt_desc.MultiSampleQuality;
            }
            render_target->Release();
        }
        shadow_device_viewport_valid =
            SUCCEEDED(device->GetViewport(&shadow_device_viewport));
        IDirect3DStateBlock9* state_block = nullptr;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state_block))
            || FAILED(state_block->Capture())) {
            if (state_block) state_block->Release();
            shadow_camera_depth_failure = "state block";
            shadow_camera_depth_copy_succeeded = false;
            return false;
        }

        constexpr DWORD kReszTrigger = 0x7fa05000;
        constexpr float dummy_point[3] = {0.0f, 0.0f, 0.0f};
        constexpr D3DMATRIX identity = {
            1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
        device->SetTransform(D3DTS_WORLD, &identity);
        device->SetTransform(D3DTS_VIEW, &identity);
        device->SetTransform(D3DTS_PROJECTION, &identity);
        device->SetTexture(0, shadow_camera_depth_texture);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetVertexDeclaration(nullptr);
        device->SetFVF(D3DFVF_XYZ);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        device->SetRenderState(D3DRS_CLIPPING, TRUE);
        device->DrawPrimitiveUP(
            D3DPT_POINTLIST, 1, dummy_point, sizeof(dummy_point));
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_ZENABLE, TRUE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        const auto result =
            device->SetRenderState(D3DRS_POINTSIZE, kReszTrigger);
        device->SetTexture(0, nullptr);

        state_block->Apply();
        state_block->Release();

        depth_uv_scale[0] = shadow_camera_depth_width > 0
            ? static_cast<float>(width) / static_cast<float>(shadow_camera_depth_width)
            : 1.f;
        depth_uv_scale[1] = shadow_camera_depth_height > 0
            ? static_cast<float>(height) / static_cast<float>(shadow_camera_depth_height)
            : 1.f;
        shadow_camera_depth_copy_succeeded = SUCCEEDED(result);
        shadow_camera_depth_failure =
            shadow_camera_depth_copy_succeeded ? "none" : "RESZ trigger rejected";
        return shadow_camera_depth_copy_succeeded;
    }

    constexpr int kMaxWorldLights = 8;

    struct WorldLightSample {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 colour;
        float inner;
        float outer;
    };

    int AddPlayerLight(WorldLightSample* out)
    {
        if (!player_light_enabled || player_light_intensity <= 0.0f) return 0;
        if (player_light_radius <= 0.0f) return 0;
        const auto* const player = GW::Agents::GetControlledCharacter();
        if (!player) return 0;
        const auto x = player->pos.x;
        const auto y = player->pos.y;
        const auto z = player->z;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return 0;
        out[0].position = {x, y, z - player_light_height};
        out[0].colour = {
            player_light_colour.x * player_light_intensity,
            player_light_colour.y * player_light_intensity,
            player_light_colour.z * player_light_intensity};
        out[0].outer = player_light_radius;
        out[0].inner = player_light_radius * std::clamp(player_light_core, 0.0f, 0.95f);
        return 1;
    }

    int GatherWorldLights(
        const DirectX::XMFLOAT3& camera_position, WorldLightSample* out)
    {
        const auto reserved = AddPlayerLight(out);
        if (!world_lights_enabled) return reserved;
        const auto* const map_context = GW::GetMapContext();
        if (!map_context) return reserved;
        const auto* const base = reinterpret_cast<const uint8_t*>(map_context);
        const auto* const manager =
            *reinterpret_cast<const uint8_t* const*>(base + 0xEC);
        if (!manager || reinterpret_cast<uintptr_t>(manager) < 0x10000) return reserved;

        const auto* const descriptors =
            *reinterpret_cast<const uint8_t* const*>(manager + 0x00);
        const auto count = *reinterpret_cast<const uint32_t*>(manager + 0x08);
        const auto handle_capacity = *reinterpret_cast<const uint32_t*>(manager + 0x14);
        const auto handle_count = *reinterpret_cast<const uint32_t*>(manager + 0x18);
        if (!descriptors || reinterpret_cast<uintptr_t>(descriptors) < 0x10000) return reserved;
        if (count == 0 || count > 8192) return reserved;
        if (count != handle_count || handle_capacity < handle_count) return reserved;

        struct Candidate {
            float score;
            int index;
        };
        Candidate best[kMaxWorldLights]{};
        auto found = 0;

        const auto limit = kMaxWorldLights - reserved;

        for (auto i = 0u; i < count; ++i) {
            const auto* const d = descriptors + i * 0x1C;
            const auto x = *reinterpret_cast<const float*>(d + 0x00);
            const auto y = *reinterpret_cast<const float*>(d + 0x04);
            const auto z = *reinterpret_cast<const float*>(d + 0x08);
            const auto intensity = *reinterpret_cast<const float*>(d + 0x10);
            const auto outer = *reinterpret_cast<const float*>(d + 0x18);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)
                || !std::isfinite(outer) || outer <= 0.0f || intensity <= 0.0f) {
                continue;
            }
            const auto dx = x - camera_position.x;
            const auto dy = y - camera_position.y;
            const auto dz = z - camera_position.z;
            const auto score =
                std::sqrt(dx * dx + dy * dy + dz * dz) - outer * world_light_reach;
            if (score > world_light_cull_distance) continue;

            auto slot = found;
            if (found < limit) {
                ++found;
            }
            else {
                auto worst = 0;
                for (auto j = 1; j < limit; ++j) {
                    if (best[j].score > best[worst].score) worst = j;
                }
                if (best[worst].score <= score) continue;
                slot = worst;
            }
            best[slot] = {score, static_cast<int>(i)};
        }

        for (auto i = 0; i < found; ++i) {
            auto* const slot_out = out + reserved + i;
            const auto* const d = descriptors + best[i].index * 0x1C;

            const auto intensity = *reinterpret_cast<const float*>(d + 0x10);
            const auto scale = intensity / 255.0f;
            slot_out->position = {
                *reinterpret_cast<const float*>(d + 0x00),
                *reinterpret_cast<const float*>(d + 0x04),
                *reinterpret_cast<const float*>(d + 0x08)};
            slot_out->colour = {
                static_cast<float>(d[0x0C]) * scale,
                static_cast<float>(d[0x0D]) * scale,
                static_cast<float>(d[0x0E]) * scale};
            slot_out->inner = *reinterpret_cast<const float*>(d + 0x14);
            slot_out->outer = *reinterpret_cast<const float*>(d + 0x18);
        }
        return reserved + found;
    }

    DirectX::XMFLOAT3 CurrentFogColour()
    {
        const auto blend = CurrentDaylightBlend();
        const auto dim = CurrentOvercastDim();

        const auto night = night_sky_colour;
        return {
            (night.x + (world_fog_day_colour.x - night.x) * blend) * dim.x,
            (night.y + (world_fog_day_colour.y - night.y) * blend) * dim.y,
            (night.z + (world_fog_day_colour.z - night.z) * blend) * dim.z};
    }

    bool GetWorldWaterPlane(float& plane_z)
    {
        const auto* const map_context = GW::GetMapContext();
        if (!map_context) return false;
        const auto* const base = reinterpret_cast<const uint8_t*>(map_context);
        if ((*reinterpret_cast<const uint32_t*>(base + 0x108) & 2) == 0) {
            return false;
        }
        const auto* const water =
            *reinterpret_cast<const uint8_t* const*>(base + 0x12C);
        if (!water) return false;
        plane_z = *reinterpret_cast<const float*>(water + 0xEC);
        return std::isfinite(plane_z);
    }

    constexpr uint32_t kOceanResolution = 256;
    constexpr uint32_t kOceanStages = 8;

    constexpr float kWorldUnitsPerMetre = 58.0f;

    constexpr float kOceanDetailScale = 4.37f;

    constexpr float kOceanFoamSharpness = 1.2f;

    float ocean_patch_size = 2968.0f;

    float ocean_wave_length = 332.0f;

    float ocean_wave_height = 6.0f;
    float ocean_choppiness = 2.46f;
    float ocean_time_scale = 1.0f;
    float ocean_detail_weight = 0.34f;

    float ocean_upwind_energy = 0.4f;

    float swell_height = 162.0f;
    float swell_length = 3143.0f;
    float swell_spread_deg = 26.0f;
    float swell_steepness = 0.79f;
    float ocean_time = 0.0f;

    IDirect3DTexture9* ocean_initial_tex = nullptr;
    IDirect3DTexture9* ocean_phase_tex[2] = {nullptr, nullptr};
    IDirect3DTexture9* ocean_spectrum_tex = nullptr;
    IDirect3DTexture9* ocean_fft_tex[2] = {nullptr, nullptr};
    IDirect3DTexture9* ocean_displacement_tex = nullptr;
    IDirect3DTexture9* ocean_gradient_tex = nullptr;
    IDirect3DPixelShader9* ocean_initial_ps_object = nullptr;
    IDirect3DPixelShader9* ocean_phase_ps_object = nullptr;
    IDirect3DPixelShader9* ocean_spectrum_ps_object = nullptr;
    IDirect3DPixelShader9* ocean_fft_ps_object = nullptr;
    IDirect3DPixelShader9* ocean_normal_ps_object = nullptr;
    int ocean_phase_index = 0;
    bool ocean_resources_ready = false;
    bool ocean_resources_failed = false;
    bool ocean_needs_seed = true;
    float ocean_spectrum_wind = -1.0f;
    float ocean_spectrum_patch = -1.0f;
    float ocean_spectrum_direction = 0.0f;
    float ocean_spectrum_upwind = -1.0f;
    const char* ocean_failure = nullptr;

    bool OceanSurfaceReady() { return ocean_resources_ready; }

    float OceanShapeWind()
    {
        constexpr auto kInverseWaveAge = 0.84f;
        const auto metres =
            std::max(ocean_wave_length, 1.0f) / kWorldUnitsPerMetre;

        return std::sqrt(
            metres * 9.81f * kInverseWaveAge * kInverseWaveAge / 6.2831853f);
    }

    float DominantWavelength() { return std::max(ocean_wave_length, 1.0f); }

    float CharacteristicWaveHeight() { return ocean_wave_height; }

    float OceanAmplitudeScale()
    {
        const auto shape_wind = OceanShapeWind();
        const auto reference_metres = 0.021f * shape_wind * shape_wind;
        if (reference_metres < 1e-6f) return 0.0f;
        const auto wanted_metres = CharacteristicWaveHeight() / kWorldUnitsPerMetre;
        return wanted_metres / reference_metres;
    }

    void ReleaseOceanResources()
    {
        const auto release = [](auto*& resource) {
            if (resource) {
                resource->Release();
                resource = nullptr;
            }
        };
        release(ocean_initial_tex);
        release(ocean_phase_tex[0]);
        release(ocean_phase_tex[1]);
        release(ocean_spectrum_tex);
        release(ocean_fft_tex[0]);
        release(ocean_fft_tex[1]);
        release(ocean_displacement_tex);
        release(ocean_gradient_tex);
        release(ocean_initial_ps_object);
        release(ocean_phase_ps_object);
        release(ocean_spectrum_ps_object);
        release(ocean_fft_ps_object);
        release(ocean_normal_ps_object);
        ocean_resources_ready = false;
        ocean_needs_seed = true;
        ocean_spectrum_wind = -1.0f;
        ocean_spectrum_patch = -1.0f;
    }

    bool EnsureOceanResources(IDirect3DDevice9* const device)
    {
        if (ocean_resources_ready) return true;
        if (ocean_resources_failed) return false;

        IDirect3D9* d3d = nullptr;
        if (FAILED(device->GetDirect3D(&d3d)) || !d3d) {
            ocean_resources_failed = true;
            ocean_failure = "no IDirect3D9";
            return false;
        }
        D3DDEVICE_CREATION_PARAMETERS params{};
        device->GetCreationParameters(&params);
        const auto supports = [&](const D3DFORMAT format, const DWORD usage) {
            return SUCCEEDED(d3d->CheckDeviceFormat(
                params.AdapterOrdinal, params.DeviceType, D3DFMT_X8R8G8B8, usage,
                D3DRTYPE_TEXTURE, format));
        };
        const auto simulation_format = D3DFMT_A32B32G32R32F;
        if (!supports(simulation_format, D3DUSAGE_RENDERTARGET)) {
            d3d->Release();
            ocean_resources_failed = true;
            ocean_failure = "no float render targets";
            return false;
        }

        const auto filterable = D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_FILTER;
        const auto sampled_format =
            supports(D3DFMT_A32B32G32R32F, filterable) ? D3DFMT_A32B32G32R32F
            : supports(D3DFMT_A16B16G16R16F, filterable) ? D3DFMT_A16B16G16R16F
            : D3DFMT_UNKNOWN;
        d3d->Release();
        if (sampled_format == D3DFMT_UNKNOWN) {
            ocean_resources_failed = true;
            ocean_failure = "no filterable float format";
            return false;
        }

        const auto create = [&](IDirect3DTexture9** out, const D3DFORMAT format) {
            return SUCCEEDED(device->CreateTexture(
                kOceanResolution, kOceanResolution, 1, D3DUSAGE_RENDERTARGET, format,
                D3DPOOL_DEFAULT, out, nullptr));
        };
        auto ok = create(&ocean_initial_tex, simulation_format)
            && create(&ocean_phase_tex[0], simulation_format)
            && create(&ocean_phase_tex[1], simulation_format)
            && create(&ocean_spectrum_tex, simulation_format)
            && create(&ocean_fft_tex[0], simulation_format)
            && create(&ocean_fft_tex[1], simulation_format)
            && create(&ocean_displacement_tex, sampled_format)
            && create(&ocean_gradient_tex, sampled_format);
        ok = ok
            && SUCCEEDED(device->CreatePixelShader(
                reinterpret_cast<const DWORD*>(&ocean_initial_ps), &ocean_initial_ps_object))
            && SUCCEEDED(device->CreatePixelShader(
                reinterpret_cast<const DWORD*>(&ocean_phase_ps), &ocean_phase_ps_object))
            && SUCCEEDED(device->CreatePixelShader(
                reinterpret_cast<const DWORD*>(&ocean_spectrum_ps), &ocean_spectrum_ps_object))
            && SUCCEEDED(device->CreatePixelShader(
                reinterpret_cast<const DWORD*>(&ocean_fft_ps), &ocean_fft_ps_object))
            && SUCCEEDED(device->CreatePixelShader(
                reinterpret_cast<const DWORD*>(&ocean_normal_ps), &ocean_normal_ps_object));
        if (!ok) {
            ReleaseOceanResources();
            ocean_resources_failed = true;
            ocean_failure = "resource creation";
            return false;
        }
        ocean_resources_ready = true;
        ocean_needs_seed = true;
        return true;
    }

    void RenderOceanPass(
        IDirect3DDevice9* const device, IDirect3DTexture9* const target,
        IDirect3DPixelShader9* const shader, IDirect3DTexture9* const source0 = nullptr,
        IDirect3DTexture9* const source1 = nullptr)
    {
        IDirect3DSurface9* surface = nullptr;
        if (FAILED(target->GetSurfaceLevel(0, &surface)) || !surface) return;
        device->SetTexture(0, nullptr);
        device->SetTexture(1, nullptr);
        device->SetRenderTarget(0, surface);
        surface->Release();
        device->SetTexture(0, source0);
        device->SetTexture(1, source1);
        const D3DVIEWPORT9 viewport = {0, 0, kOceanResolution, kOceanResolution, 0.0f, 1.0f};
        device->SetViewport(&viewport);
        device->SetPixelShader(shader);
        SkyVertex quad[4];
        constexpr DirectX::XMFLOAT3 no_directions[4] = {};
        FillFullscreenQuad(quad, no_directions);
        for (auto& vertex : quad) vertex.z = 0.5f;
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(SkyVertex));
    }

    void StepOceanSimulation(IDirect3DDevice9* const device, const float delta_seconds)
    {
        if (!EnsureOceanResources(device)) return;

        IDirect3DStateBlock9* state_block = nullptr;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state_block))
            || FAILED(state_block->Capture())) {
            if (state_block) state_block->Release();
            return;
        }
        IDirect3DSurface9* previous_target = nullptr;
        IDirect3DSurface9* previous_depth = nullptr;
        device->GetRenderTarget(0, &previous_target);
        device->GetDepthStencilSurface(&previous_depth);

        device->SetDepthStencilSurface(nullptr);

        device->SetVertexShader(sky_vs);
        device->SetVertexDeclaration(sky_decl);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        for (DWORD stage = 0; stage < 3; ++stage) {
            device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            device->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        }

        const auto resolution = static_cast<float>(kOceanResolution);
        const auto patch_metres = std::max(ocean_patch_size / kWorldUnitsPerMetre, 0.5f);

        const auto wind_speed = OceanShapeWind();
        float wind_dir[2] = {1.f, 0.f};
        Wind::DirectionVector(wind_dir);

        const auto direction_drift = std::abs(wind_dir[0] - ocean_spectrum_direction);
        if (std::abs(wind_speed - ocean_spectrum_wind) > 0.05f
            || std::abs(ocean_patch_size - ocean_spectrum_patch) > 0.5f
            || std::abs(ocean_upwind_energy - ocean_spectrum_upwind) > 0.005f
            || direction_drift > 0.01f) {
            const float initial_params[4] = {resolution, patch_metres, 0.f, 0.f};
            const float wind[4] = {
                wind_dir[0] * wind_speed, wind_dir[1] * wind_speed,
                std::clamp(ocean_upwind_energy, 0.f, 1.f), 0.f};
            device->SetPixelShaderConstantF(0, initial_params, 1);
            device->SetPixelShaderConstantF(1, wind, 1);
            RenderOceanPass(device, ocean_initial_tex, ocean_initial_ps_object);
            ocean_spectrum_wind = wind_speed;
            ocean_spectrum_patch = ocean_patch_size;
            ocean_spectrum_direction = wind_dir[0];
            ocean_spectrum_upwind = ocean_upwind_energy;
        }

        const auto step = ocean_needs_seed
            ? 0.0f
            : std::clamp(delta_seconds * ocean_time_scale, 0.0f, 0.25f);
        const float phase_params[4] = {
            resolution, patch_metres, step, ocean_needs_seed ? 1.f : 0.f};
        device->SetPixelShaderConstantF(0, phase_params, 1);
        const auto next_phase = 1 - ocean_phase_index;
        RenderOceanPass(
            device, ocean_phase_tex[next_phase], ocean_phase_ps_object,
            ocean_phase_tex[ocean_phase_index]);
        ocean_phase_index = next_phase;
        ocean_needs_seed = false;

        const float spectrum_params[4] = {
            resolution, patch_metres, OceanAmplitudeScale(),
            std::max(ocean_choppiness, 0.f)};
        device->SetPixelShaderConstantF(0, spectrum_params, 1);
        RenderOceanPass(
            device, ocean_spectrum_tex, ocean_spectrum_ps_object, ocean_initial_tex,
            ocean_phase_tex[ocean_phase_index]);

        auto* source = ocean_spectrum_tex;
        for (uint32_t pass = 0; pass < kOceanStages * 2; ++pass) {
            const auto horizontal = pass < kOceanStages;
            const auto stage = horizontal ? pass : pass - kOceanStages;
            const float fft_params[4] = {
                resolution, static_cast<float>(2u << stage), horizontal ? 1.f : 0.f, 0.f};
            device->SetPixelShaderConstantF(0, fft_params, 1);
            auto* const target = pass == kOceanStages * 2 - 1
                ? ocean_displacement_tex
                : ocean_fft_tex[pass & 1];
            RenderOceanPass(device, target, ocean_fft_ps_object, source);
            source = target;
        }

        const float normal_params[4] = {
            resolution, patch_metres, kOceanFoamSharpness, 0.f};
        device->SetPixelShaderConstantF(0, normal_params, 1);
        RenderOceanPass(
            device, ocean_gradient_tex, ocean_normal_ps_object, ocean_displacement_tex);

        device->SetTexture(0, nullptr);
        if (previous_target) {
            device->SetRenderTarget(0, previous_target);
            previous_target->Release();
        }
        device->SetDepthStencilSurface(previous_depth);
        if (previous_depth) previous_depth->Release();
        state_block->Apply();
        state_block->Release();
    }

    void DrawWorldWater(
        IDirect3DDevice9* device, const GW::Camera* camera,
        const uint32_t viewport_width, const uint32_t viewport_height)
    {
        float plane_z = 0.0f;
        if (!water_enabled || !camera || !shadow_camera_depth_texture
            || !world_water_ps_object || !shadow_camera_wvp_ready
            || !OceanSurfaceReady() || !GetWorldWaterPlane(plane_z)) {
            return;
        }

        if (shadow_replay_active || shadow_explicit_camera_render_active
            || shadow_replay_preview) {
            return;
        }

        IDirect3DSurface9* current_target = nullptr;
        D3DSURFACE_DESC target_desc{};
        const auto main_target_bound =
            SUCCEEDED(device->GetRenderTarget(0, &current_target))
            && current_target
            && current_target != shadow_preview_surface
            && SUCCEEDED(current_target->GetDesc(&target_desc))
            && target_desc.Width == viewport_width
            && target_desc.Height == viewport_height;
        if (current_target) current_target->Release();
        if (!main_target_bound) return;

        IDirect3DStateBlock9* state_block = nullptr;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state_block))
            || FAILED(state_block->Capture())) {
            if (state_block) state_block->Release();
            return;
        }

        const auto camera_view_projection = DirectX::XMMatrixSet(
            shadow_camera_wvp[0], shadow_camera_wvp[1],
            shadow_camera_wvp[2], shadow_camera_wvp[3],
            shadow_camera_wvp[4], shadow_camera_wvp[5],
            shadow_camera_wvp[6], shadow_camera_wvp[7],
            shadow_camera_wvp[8], shadow_camera_wvp[9],
            shadow_camera_wvp[10], shadow_camera_wvp[11],
            shadow_camera_wvp[12], shadow_camera_wvp[13],
            shadow_camera_wvp[14], shadow_camera_wvp[15]);
        DirectX::XMFLOAT4X4A inverse_matrix{};
        DirectX::XMStoreFloat4x4A(
            &inverse_matrix,
            DirectX::XMMatrixInverse(nullptr, camera_view_projection));

        device->SetVertexShader(sky_vs);
        device->SetPixelShader(world_water_ps_object);
        device->SetVertexDeclaration(sky_decl);
        SetExcludedCircle(device);
        device->SetPixelShaderConstantF(
            0, reinterpret_cast<const float*>(&inverse_matrix), 4);
        const float plane[4] = {plane_z, 0.f, 0.f, water_strength};
        device->SetPixelShaderConstantF(4, plane, 1);
        const float eye[4] = {
            camera->position.x, camera->position.y, camera->position.z, 0.f};
        device->SetPixelShaderConstantF(5, eye, 1);

        const auto daylight = CurrentDaylightBlend();
        const auto light = GetSceneLightDirWorld();
        const float light_dir[4] = {light.x, light.y, light.z, daylight};
        device->SetPixelShaderConstantF(6, light_dir, 1);
        const auto lit = 0.2f + 0.8f * daylight;
        const float deep[4] = {
            water_deep_colour.x * lit, water_deep_colour.y * lit,
            water_deep_colour.z * lit, 0.f};
        device->SetPixelShaderConstantF(7, deep, 1);
        const float shallow[4] = {
            water_shallow_colour.x * lit, water_shallow_colour.y * lit,
            water_shallow_colour.z * lit, 0.f};
        device->SetPixelShaderConstantF(8, shallow, 1);

        const auto foam_lit = 0.12f + 0.88f * daylight;
        const float foam_rgb[4] = {
            water_foam_colour.x * foam_lit, water_foam_colour.y * foam_lit,
            water_foam_colour.z * foam_lit, 0.f};
        device->SetPixelShaderConstantF(9, foam_rgb, 1);

        const float horizon[4] = {
            0.62f * daylight + night_sky_colour.x * 3.0f,
            0.72f * daylight + night_sky_colour.y * 3.0f,
            0.86f * daylight + night_sky_colour.z * 3.0f, 0.f};
        device->SetPixelShaderConstantF(10, horizon, 1);
        const float zenith[4] = {
            0.24f * daylight + night_sky_colour.x * 2.0f,
            0.42f * daylight + night_sky_colour.y * 2.0f,
            0.78f * daylight + night_sky_colour.z * 2.0f, 0.f};
        device->SetPixelShaderConstantF(11, zenith, 1);
        const float sun_rgb[4] = {
            1.0f * daylight + 0.16f * (1.0f - daylight),
            0.95f * daylight + 0.17f * (1.0f - daylight),
            0.85f * daylight + 0.22f * (1.0f - daylight),
            water_specular_power};
        device->SetPixelShaderConstantF(12, sun_rgb, 1);
        const float depth_params[4] = {
            std::max(water_clarity_depth, 1.f), std::max(water_foam_depth, 0.5f),
            std::max(water_foam_intensity, 0.f), std::max(water_scatter, 0.f)};
        device->SetPixelShaderConstantF(13, depth_params, 1);

        const auto ocean_ready = OceanSurfaceReady();
        const float ocean_sample[4] = {
            std::max(ocean_patch_size, 1.f), kWorldUnitsPerMetre,
            std::max(CharacteristicWaveHeight(), 0.01f), kOceanDetailScale};
        device->SetPixelShaderConstantF(14, ocean_sample, 1);

        const auto swell_amplitude =
            swell_height * (0.25f + 0.75f * Wind::BaseStrength());
        const float swell[4] = {
            swell_amplitude, 6.2831853f / std::max(swell_length, 1.f),
            std::clamp(swell_steepness, 0.f, 1.f), ocean_time};
        device->SetPixelShaderConstantF(16, swell, 1);

        const float envelope =
            swell_amplitude + CharacteristicWaveHeight() * 0.5f;
        const float ocean_detail[4] = {
            std::clamp(ocean_detail_weight, 0.f, 1.f), std::round(water_relief_steps), envelope,
            water_ownership_view ? 1.f : 0.f};
        device->SetPixelShaderConstantF(15, ocean_detail, 1);

        float swell_direction[2] = {1.f, 0.f};
        Wind::DirectionVector(swell_direction);
        const float swell_wind_params[4] = {
            swell_direction[0], swell_direction[1],
            DirectX::XMConvertToRadians(swell_spread_deg), 9.81f * kWorldUnitsPerMetre};
        device->SetPixelShaderConstantF(17, swell_wind_params, 1);
        if (ocean_ready) {
            device->SetTexture(1, ocean_displacement_tex);
            device->SetTexture(2, ocean_gradient_tex);
            for (DWORD stage = 1; stage <= 2; ++stage) {
                device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                device->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                device->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            }
        }

        device->SetTexture(0, shadow_camera_depth_texture);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);

        SkyVertex triangle[3];
        FillFullscreenTriangle(triangle);
        device->DrawPrimitiveUP(
            D3DPT_TRIANGLELIST, 1, triangle, sizeof(SkyVertex));

        state_block->Apply();
        state_block->Release();
    }

    void DrawWorldFog(
        IDirect3DDevice9* device, const GW::Camera* camera,
        const uint32_t viewport_width, const uint32_t viewport_height)
    {
        if (!world_fog_enabled || !camera || !shadow_camera_depth_texture
            || !world_fog_ps_object || !shadow_camera_wvp_ready

            || (world_fog_strength <= 0.0f && world_fog_edge_repair <= 0.0f
                && !world_fog_debug_view)

            || shadow_replay_active || shadow_explicit_camera_render_active
            || shadow_replay_preview) {
            return;
        }

        IDirect3DSurface9* current_target = nullptr;
        D3DSURFACE_DESC target_desc{};
        const auto main_target_bound =
            SUCCEEDED(device->GetRenderTarget(0, &current_target))
            && current_target
            && current_target != shadow_preview_surface
            && SUCCEEDED(current_target->GetDesc(&target_desc))
            && target_desc.Width == viewport_width
            && target_desc.Height == viewport_height;
        if (current_target) current_target->Release();
        if (!main_target_bound) return;

        IDirect3DStateBlock9* state_block = nullptr;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state_block))
            || FAILED(state_block->Capture())) {
            if (state_block) state_block->Release();
            return;
        }

        using namespace DirectX;
        const auto camera_view_projection = XMMatrixSet(
            shadow_camera_wvp[0], shadow_camera_wvp[1],
            shadow_camera_wvp[2], shadow_camera_wvp[3],
            shadow_camera_wvp[4], shadow_camera_wvp[5],
            shadow_camera_wvp[6], shadow_camera_wvp[7],
            shadow_camera_wvp[8], shadow_camera_wvp[9],
            shadow_camera_wvp[10], shadow_camera_wvp[11],
            shadow_camera_wvp[12], shadow_camera_wvp[13],
            shadow_camera_wvp[14], shadow_camera_wvp[15]);
        XMFLOAT4X4A inverse_matrix{};
        XMStoreFloat4x4A(
            &inverse_matrix, XMMatrixInverse(nullptr, camera_view_projection));

        device->SetVertexShader(sky_vs);
        device->SetPixelShader(world_fog_ps_object);
        device->SetVertexDeclaration(sky_decl);
        SetExcludedCircle(device);
        device->SetPixelShaderConstantF(
            0, reinterpret_cast<const float*>(&inverse_matrix), 4);
        const auto fog = CurrentFogColour();
        const float colour[4] = {fog.x, fog.y, fog.z, 0.f};
        device->SetPixelShaderConstantF(4, colour, 1);

        const auto overcast = CurrentOvercast() * std::clamp(world_fog_overcast_gain, 0.f, 1.f);
        const auto start = std::max(world_fog_start, 1.0f) * (1.0f - overcast * 0.6f);
        const auto end = std::max(world_fog_end, start + 1.0f) * (1.0f - overcast * 0.5f);

        const auto bias = std::clamp(world_fog_night_bias, 0.0f, 1.0f);
        const auto night = 1.0f - std::clamp(CurrentDaylightBlend(), 0.0f, 1.0f);
        const auto strength = std::clamp(world_fog_strength, 0.f, 1.f)
            * (1.0f - bias + bias * night);
        const float range[4] = {start, end, strength, 0.f};
        device->SetPixelShaderConstantF(5, range, 1);
        const float eye[4] = {
            camera->position.x, camera->position.y, camera->position.z, 0.f};
        device->SetPixelShaderConstantF(6, eye, 1);

        const float edge[4] = {
            1.0f / static_cast<float>(std::max(viewport_width, 1u)),
            1.0f / static_cast<float>(std::max(viewport_height, 1u)),
            std::max(world_fog_edge_repair, 0.0f),
            static_cast<float>(std::clamp(world_fog_edge_radius, 1, 4))};
        device->SetPixelShaderConstantF(7, edge, 1);
        const float debug[4] = {world_fog_debug_view ? 1.0f : 0.0f, 0.f, 0.f, 0.f};
        device->SetPixelShaderConstantF(8, debug, 1);

        device->SetTexture(0, shadow_camera_depth_texture);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);

        device->SetRenderState(
            D3DRS_ALPHABLENDENABLE, world_fog_debug_view ? FALSE : TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);

        constexpr DirectX::XMFLOAT3 directions[4] = {};
        SkyVertex quad[4];
        FillFullscreenQuad(quad, directions);
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(SkyVertex));

        state_block->Apply();
        state_block->Release();
    }

    DirectX::XMFLOAT3 CurrentAmbientTintWithFlash()
    {
        const auto tint = CurrentAmbientTint();
        const auto flash = std::clamp(
            lightning_frame.flash * lightning_frame.world_flash, 0.0f, 1.0f);
        return {
            tint.x + std::max(1.0f - tint.x, 0.0f) * flash,
            tint.y + std::max(1.0f - tint.y, 0.0f) * flash,
            tint.z + std::max(1.0f - tint.z, 0.0f) * flash};
    }

    void UploadWorldLightConstants(
        IDirect3DDevice9* device, const WorldLightSample* lights, const int light_count)
    {
        if (light_count <= 0) {

            const float params[4] = {0.f, 1.f, 1.f, 1.f};
            device->SetPixelShaderConstantF(6, params, 1);
            return;
        }

        const float params[4] = {
            1.0f, std::max(world_light_strength, 0.0f),
            std::max(world_light_reach, 0.01f),
            std::clamp(world_light_falloff, 0.5f, 8.0f)};
        device->SetPixelShaderConstantF(6, params, 1);

        float positions[kMaxWorldLights * 4]{};
        float colours[kMaxWorldLights * 4]{};
        for (auto i = 0; i < light_count && i < kMaxWorldLights; ++i) {
            positions[i * 4 + 0] = lights[i].position.x;
            positions[i * 4 + 1] = lights[i].position.y;
            positions[i * 4 + 2] = lights[i].position.z;
            positions[i * 4 + 3] = lights[i].outer;
            colours[i * 4 + 0] = lights[i].colour.x;
            colours[i * 4 + 1] = lights[i].colour.y;
            colours[i * 4 + 2] = lights[i].colour.z;
            colours[i * 4 + 3] = lights[i].inner;
        }
        device->SetPixelShaderConstantF(7, positions, kMaxWorldLights);
        device->SetPixelShaderConstantF(15, colours, kMaxWorldLights);
    }

    void DrawWorldAmbient(
        IDirect3DDevice9* device, const GW::Camera* camera,
        const uint32_t viewport_width, const uint32_t viewport_height)
    {
        if (!world_ambient_enabled || !shadow_camera_depth_texture
            || !world_ambient_ps_object

            || shadow_replay_preview) {
            return;
        }

        IDirect3DSurface9* current_target = nullptr;
        D3DSURFACE_DESC target_desc{};
        const auto main_target_bound =
            SUCCEEDED(device->GetRenderTarget(0, &current_target))
            && current_target
            && current_target != shadow_preview_surface
            && SUCCEEDED(current_target->GetDesc(&target_desc))
            && target_desc.Width == viewport_width
            && target_desc.Height == viewport_height;
        if (current_target) current_target->Release();
        if (!main_target_bound) return;

        IDirect3DStateBlock9* state_block = nullptr;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state_block))
            || FAILED(state_block->Capture())) {
            if (state_block) state_block->Release();
            return;
        }

        device->SetVertexShader(sky_vs);
        device->SetPixelShader(world_ambient_ps_object);
        device->SetVertexDeclaration(sky_decl);
        SetExcludedCircle(device);
        const auto tint = CurrentAmbientTint();

        const auto lit = CurrentAmbientTintWithFlash();
        const float wanted[3] = {lit.x, lit.y, lit.z};

        float multiply[4] = {0.f, 0.f, 0.f, 1.f};
        float boost[4] = {0.f, 0.f, 0.f, 1.f};
        auto needs_boost = false;
        for (int i = 0; i < 3; ++i) {
            multiply[i] = std::min(wanted[i], 1.0f);
            boost[i] = multiply[i] > 1e-4f ? wanted[i] / multiply[i] - 1.0f : 0.0f;
            if (boost[i] > 1e-4f) needs_boost = true;
        }
        shadow_ambient_applied = {wanted[0], wanted[1], wanted[2]};
        ++shadow_ambient_draw_count;
        device->SetPixelShaderConstantF(0, multiply, 1);

        WorldLightSample lights[kMaxWorldLights]{};
        auto light_count = 0;
        if (camera && shadow_camera_wvp_ready && world_light_strength > 0.0f) {
            const DirectX::XMFLOAT3 camera_position = {
                camera->position.x, camera->position.y, camera->position.z};
            light_count = GatherWorldLights(camera_position, lights);
        }
        if (light_count > 0) {
            using namespace DirectX;
            const auto camera_view_projection = XMMatrixSet(
                shadow_camera_wvp[0], shadow_camera_wvp[1],
                shadow_camera_wvp[2], shadow_camera_wvp[3],
                shadow_camera_wvp[4], shadow_camera_wvp[5],
                shadow_camera_wvp[6], shadow_camera_wvp[7],
                shadow_camera_wvp[8], shadow_camera_wvp[9],
                shadow_camera_wvp[10], shadow_camera_wvp[11],
                shadow_camera_wvp[12], shadow_camera_wvp[13],
                shadow_camera_wvp[14], shadow_camera_wvp[15]);
            XMFLOAT4X4A inverse_matrix{};
            XMStoreFloat4x4A(
                &inverse_matrix, XMMatrixInverse(nullptr, camera_view_projection));
            device->SetPixelShaderConstantF(
                2, reinterpret_cast<const float*>(&inverse_matrix), 4);

        }

        UploadWorldLightConstants(device, lights, light_count);

        shadow_world_light_count = light_count;
        device->SetTexture(0, shadow_camera_depth_texture);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);

        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);

        constexpr DirectX::XMFLOAT3 directions[4] = {};
        SkyVertex quad[4];
        FillFullscreenQuad(quad, directions);
        device->DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP, 2, quad, sizeof(SkyVertex));

        if (needs_boost) {

            device->SetPixelShaderConstantF(0, boost, 1);

            if (light_count > 0) {
                const float params[4] = {0.f, 1.f, 1.f, 1.f};
                device->SetPixelShaderConstantF(6, params, 1);
            }
            device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
            device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            device->DrawPrimitiveUP(
                D3DPT_TRIANGLESTRIP, 2, quad, sizeof(SkyVertex));
        }

        state_block->Apply();
        state_block->Release();
    }

    void DrawScreenSpaceShadows(
        IDirect3DDevice9* device, const GW::Camera* camera,
        const uint32_t viewport_width, const uint32_t viewport_height)
    {
        if (!shadow_gpu_draw_shadows || !camera
            || !shadow_camera_depth_texture
            || !shadow_preview_depth_texture
            || !shadow_native_light_wvp_ready
            || !shadow_camera_wvp_ready
            || !shadow_screenspace_ps_object) {
            return;
        }

        IDirect3DSurface9* current_target = nullptr;
        D3DSURFACE_DESC current_target_desc{};
        const auto main_target_bound =
            SUCCEEDED(device->GetRenderTarget(0, &current_target))
            && current_target
            && current_target != shadow_preview_surface
            && SUCCEEDED(current_target->GetDesc(&current_target_desc))
            && current_target_desc.Width == viewport_width
            && current_target_desc.Height == viewport_height;
        if (current_target) current_target->Release();
        if (!main_target_bound) return;

        IDirect3DStateBlock9* state_block = nullptr;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state_block))
            || FAILED(state_block->Capture())) {
            if (state_block) state_block->Release();
            return;
        }

        const auto camera_view_projection = DirectX::XMMatrixSet(
            shadow_camera_wvp[0], shadow_camera_wvp[1],
            shadow_camera_wvp[2], shadow_camera_wvp[3],
            shadow_camera_wvp[4], shadow_camera_wvp[5],
            shadow_camera_wvp[6], shadow_camera_wvp[7],
            shadow_camera_wvp[8], shadow_camera_wvp[9],
            shadow_camera_wvp[10], shadow_camera_wvp[11],
            shadow_camera_wvp[12], shadow_camera_wvp[13],
            shadow_camera_wvp[14], shadow_camera_wvp[15]);
        DirectX::XMFLOAT4X4A inverse_matrix{};
        DirectX::XMStoreFloat4x4A(
            &inverse_matrix,
            DirectX::XMMatrixInverse(nullptr, camera_view_projection));

        device->SetVertexShader(sky_vs);
        device->SetPixelShader(shadow_screenspace_ps_object);
        device->SetVertexDeclaration(sky_decl);
        SetExcludedCircle(device);
        device->SetPixelShaderConstantF(
            0, reinterpret_cast<const float*>(&inverse_matrix), 4);
        device->SetPixelShaderConstantF(4, shadow_native_light_wvp.data(), 4);
        const float settings[4] = {
            shadow_gpu_bias, shadow_gpu_strength * ShadowSunFade(),
            1.0f / static_cast<float>(shadow_map_size),
            static_cast<float>(shadow_screenspace_debug_view)};
        device->SetPixelShaderConstantF(8, settings, 1);
        const float depth_params[4] = {
            shadow_native_depth_slope, shadow_gpu_slope_bias,
            shadow_native_light_orthographic ? 1.0f : 0.0f, 0.0f};
        device->SetPixelShaderConstantF(9, depth_params, 1);

        float water_plane_z = 0.0f;
        const auto water_drawing =
            water_enabled && OceanSurfaceReady() && GetWorldWaterPlane(water_plane_z);
        const float water_surface[4] = {
            water_plane_z, water_drawing ? 1.f : 0.f,
            std::max(water_clarity_depth * 0.35f, 1.f), 0.f};
        device->SetPixelShaderConstantF(16, water_surface, 1);

        const float cloud_c10[4] = {
            cloud_coverage, cloud_scale, cloud_scroll.x, cloud_sharpness};
        device->SetPixelShaderConstantF(10, cloud_c10, 1);
        const float cloud_c11[4] = {
            cloud_absorption, cloud_phase_g,
            clouds_enabled ? cloud_opacity : 0.0f, cloud_scroll.y};
        device->SetPixelShaderConstantF(11, cloud_c11, 1);
        const float cloud_c19[4] = {
            cloud_variety, cloud_weather_scale, cloud_warp, cloud_billow};
        device->SetPixelShaderConstantF(19, cloud_c19, 1);

        const auto deck_altitude = std::max(cloud_altitude, 1.0f);
        const auto pattern_per_world = cloud_scale / deck_altitude;
        const auto clouds_active =
            clouds_enabled && cloud_opacity > 0.0f && cloud_shadow_strength > 0.0f;
        const float cloud_shadow_params[4] = {
            clouds_active ? cloud_shadow_strength * ShadowSunFade() : 0.0f,
            pattern_per_world, deck_altitude,
            static_cast<float>(std::clamp(cloud_shadow_octaves, 1, 4))};
        device->SetPixelShaderConstantF(12, cloud_shadow_params, 1);

        const auto shadow_light = GetSceneLightDirWorld();
        const float cloud_shadow_light[4] = {
            shadow_light.x, shadow_light.y, shadow_light.z, 0.0f};
        device->SetPixelShaderConstantF(13, cloud_shadow_light, 1);

        const auto sx = DirectX::XMVectorSet(1.f, 0.f, 0.f, 0.f);
        const auto sy = DirectX::XMVector3Cross(
            DirectX::XMVectorSet(0.f, 0.f, -1.f, 0.f), sx);
        const float cloud_sky_basis[4] = {
            DirectX::XMVectorGetX(sx), DirectX::XMVectorGetY(sx),
            DirectX::XMVectorGetX(sy), DirectX::XMVectorGetY(sy)};
        device->SetPixelShaderConstantF(14, cloud_sky_basis, 1);
        const float cloud_camera[4] = {
            camera->position.x, camera->position.y, camera->position.z, 0.0f};
        device->SetPixelShaderConstantF(15, cloud_camera, 1);

        const float cloud_origin_params[4] = {cloud_origin.x, cloud_origin.y, 0.0f, 0.0f};
        device->SetPixelShaderConstantF(20, cloud_origin_params, 1);
        device->SetTexture(0, shadow_camera_depth_texture);
        device->SetTexture(1, shadow_preview_depth_texture);
        for (DWORD sampler = 0; sampler < 2; ++sampler) {
            device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

        SkyVertex triangle[3];
        FillFullscreenTriangleFromBottomRight(triangle);
        if (SUCCEEDED(device->DrawPrimitiveUP(
                D3DPT_TRIANGLELIST, 1, triangle, sizeof(SkyVertex)))) {
            ++shadow_screenspace_draw_count;
        }

        state_block->Apply();
        state_block->Release();
    }

}

static void DrawInWorld(IDirect3DDevice9* device)
{
    const ScopedPhase timer(Phase::ScreenSpaceReceive);
    if (!device || !IsEnabled() || !EnsureResources(device)) return;

    if (!GW::Map::GetIsMapLoaded() || GW::Map::GetInstanceType() == GW::Constants::InstanceType::Loading) return;

    const uint32_t vp_w = GW::Render::GetViewportWidth();
    const uint32_t vp_h = GW::Render::GetViewportHeight();
    if (vp_w < 16 || vp_h < 16) return;

    RefreshCompassCircle(vp_w, vp_h);

    const GW::Camera* cam = GW::CameraMgr::GetCamera();
    if (!cam) return;
    shadow_main_viewport_width = vp_w;
    shadow_main_viewport_height = vp_h;
    const auto camera_depth_ready =
        (!shadow_gpu_draw_shadows && !world_ambient_enabled && !water_enabled)
        || CopyCameraDepth(device, vp_w, vp_h);

    using namespace DirectX;

    const XMVECTOR eye = XMVectorSet(cam->position.x, cam->position.y, cam->position.z, 0.f);
    const XMVECTOR target = XMVectorSet(cam->look_at_target.x, cam->look_at_target.y, cam->look_at_target.z, 0.f);
    const XMVECTOR world_up = XMVectorSet(0.f, 0.f, -1.f, 0.f);
    XMVECTOR forward = XMVector3Normalize(XMVectorSubtract(target, eye));

    XMVECTOR right = XMVector3Cross(world_up, forward);
    if (XMVectorGetX(XMVector3LengthSq(right)) < 1e-8f) right = XMVectorSet(1.f, 0.f, 0.f, 0.f);
    right = XMVector3Normalize(right);
    const XMVECTOR cam_up = XMVector3Normalize(XMVector3Cross(forward, right));

    const float fov = GW::Render::GetFieldOfView();
    const float aspect = static_cast<float>(vp_w) / static_cast<float>(std::max<uint32_t>(1, vp_h));
    const float th = std::tan(fov * 0.5f);

    const XMFLOAT3 sun_w3 = GetSunDirWorld();
    const XMVECTOR sun_w = XMVectorSet(sun_w3.x, sun_w3.y, sun_w3.z, 0.f);
    const XMVECTOR sz = world_up;
    const XMVECTOR sx = XMVectorSet(1.f, 0.f, 0.f, 0.f);
    const XMVECTOR sy = XMVector3Cross(sz, sx);

    const auto to_sky_frame = [&](XMVECTOR v) {
        return XMFLOAT3(XMVectorGetX(XMVector3Dot(v, sx)), XMVectorGetX(XMVector3Dot(v, sy)), XMVectorGetX(XMVector3Dot(v, sz)));
    };

    lightning_frame = WeatherEffects::CurrentLightning();

    {
        static clock_t last_wave_time = TIMER_INIT();
        const auto now = TIMER_INIT();
        const auto delta = std::clamp(
            static_cast<float>(now - last_wave_time) * 0.001f, 0.0f, 0.25f);
        last_wave_time = now;

        ocean_time += delta * ocean_time_scale;
        if (water_enabled) StepOceanSimulation(device, delta);
    }

    {
        static clock_t last_scroll_time = TIMER_INIT();
        const auto now = TIMER_INIT();
        const auto delta = std::clamp(
            static_cast<float>(now - last_scroll_time) * 0.001f, 0.0f, 0.25f);
        last_scroll_time = now;
        const auto wind = XMConvertToRadians(Wind::Direction());
        const auto wind_world = XMVectorSet(
            std::cos(wind), std::sin(wind), 0.f, 0.f);
        const auto wind_sky = to_sky_frame(wind_world);
        const auto wind_length = std::sqrt(
            wind_sky.x * wind_sky.x + wind_sky.y * wind_sky.y);
        if (wind_length > 1e-5f) {

            const auto rate = cloud_speed * Wind::Strength();

            cloud_scroll.x -= wind_sky.x / wind_length * rate * delta;
            cloud_scroll.y -= wind_sky.y / wind_length * rate * delta;
        }
    }

    {
        static XMFLOAT3 last_camera = {0.f, 0.f, 0.f};
        static bool camera_seeded = false;
        const XMFLOAT3 camera_now = {
            cam->position.x, cam->position.y, cam->position.z};
        if (camera_seeded) {
            const auto move_x = camera_now.x - last_camera.x;
            const auto move_y = camera_now.y - last_camera.y;

            constexpr auto kTeleportDistance = 5000.0f;
            if (std::fabs(move_x) < kTeleportDistance
                && std::fabs(move_y) < kTeleportDistance) {
                const auto pattern_per_world =
                    cloud_scale / std::max(cloud_altitude, 1.0f);
                const auto move_sky = to_sky_frame(
                    XMVectorSet(move_x, move_y, 0.f, 0.f));
                cloud_origin.x += move_sky.x * pattern_per_world;
                cloud_origin.y += move_sky.y * pattern_per_world;
            }
        }
        last_camera = camera_now;
        camera_seeded = true;
    }

    const float cx[4] = {-1.f, -1.f, 1.f, 1.f};
    const float cy[4] = {1.f, -1.f, 1.f, -1.f};
    XMFLOAT3 corner_dirs[4];
    for (int i = 0; i < 4; ++i) {
        XMVECTOR dir = XMVectorAdd(forward, XMVectorAdd(XMVectorScale(right, cx[i] * th * aspect), XMVectorScale(cam_up, cy[i] * th)));
        dir = XMVector3Normalize(dir);
        corner_dirs[i] = to_sky_frame(dir);
    }

    IDirect3DStateBlock9* state_block = nullptr;
    if (device->CreateStateBlock(D3DSBT_ALL, &state_block) != D3D_OK) return;

    if (sky_enabled
        && device->SetVertexShader(sky_vs) == D3D_OK && device->SetPixelShader(composite_ps) == D3D_OK && device->SetVertexDeclaration(sky_decl) == D3D_OK) {
        SetExcludedCircle(device);

        const bool use_copy_sky = sky_depth_from_copy
            && shadow_camera_depth_texture && shadow_camera_depth_copy_succeeded;

        device->SetRenderState(D3DRS_ZENABLE, use_copy_sky ? D3DZB_FALSE : D3DZB_TRUE);
        if (use_copy_sky) {

            device->SetTexture(0, shadow_camera_depth_texture);
            device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);

        const XMFLOAT3 sun_p = to_sky_frame(sun_w);
        const float c0[4] = {sun_p.x, sun_p.y, sun_p.z, 0.f};
        device->SetPixelShaderConstantF(0, c0, 1);
        const float c1[4] = {CurrentExposure(), sun_disk_size_deg, sun_disk_intensity, eye_altitude_km};
        device->SetPixelShaderConstantF(1, c1, 1);
        const float c2[4] = {
            sky_ownership_view ? 2.f : static_cast<float>(debug_mode),
            use_copy_sky ? 1.f : 0.f, 0.f, 0.f};
        device->SetPixelShaderConstantF(2, c2, 1);
        const float c3[4] = {
            night_sky_colour.x, night_sky_colour.y, night_sky_colour.z, 0.f};
        device->SetPixelShaderConstantF(3, c3, 1);
        const float c4[4] = {
            stars_enabled ? star_brightness : 0.0f, star_density, star_threshold,
            CurrentStarVisibility()};
        device->SetPixelShaderConstantF(4, c4, 1);
        const float c5[4] = {
            static_cast<float>(TIMER_INIT()) * 0.001f, star_twinkle_speed,
            0.f, 0.f};
        device->SetPixelShaderConstantF(5, c5, 1);

        const auto moon_w3 = DirFromElevAzim(
            -CurrentSunElevation(), CurrentSunAzimuth() + 180.0f);
        const XMFLOAT3 moon_p = to_sky_frame(
            XMVectorSet(moon_w3.x, moon_w3.y, moon_w3.z, 0.f));
        const float c6[4] = {moon_p.x, moon_p.y, moon_p.z, 0.f};
        device->SetPixelShaderConstantF(6, c6, 1);
        const float c7[4] = {
            moon_size_deg, moon_intensity,
            moon_enabled ? CurrentMoonVisibility() : 0.0f, moon_glow};
        device->SetPixelShaderConstantF(7, c7, 1);
        const float c8[4] = {
            moon_colour.x, moon_colour.y, moon_colour.z, 0.f};
        device->SetPixelShaderConstantF(8, c8, 1);
        const float c9[4] = {
            moon_glow_colour.x, moon_glow_colour.y, moon_glow_colour.z, 0.f};
        device->SetPixelShaderConstantF(9, c9, 1);

        const auto daylight = CurrentDaylightBlend();
        const auto sun_height = std::sin(
            XMConvertToRadians(CurrentSunElevation()));
        const auto night = 1.0f - daylight;
        const auto darkness = std::clamp(cloud_darkness, 0.0f, 1.0f);

        const auto overcast = CurrentOvercast() * darkness;
        const auto overcast_grey =
            1.0f - (1.0f - cloud_overcast_level) * overcast;

        const auto warmth = std::clamp(1.0f - sun_height * 2.5f, 0.0f, 1.0f)
            * (1.0f - 0.85f * overcast);

        const auto cool = cloud_overcast_cool * overcast;

        const float sun_key[3] = {
            1.0f, 1.0f - 0.35f * warmth, 1.0f - 0.62f * warmth};
        constexpr float moon_key[3] = {0.78f, 0.86f, 1.0f};
        float key[3];
        for (int i = 0; i < 3; ++i) {
            key[i] = moon_key[i] + (sun_key[i] - moon_key[i]) * daylight;
        }
        key[0] *= 1.0f - cool;
        key[2] *= 1.0f + cool * 0.8f;

        const auto night_scale = cloud_night_sky_ratio * overcast_grey;
        float level[3];
        for (int i = 0; i < 3; ++i) {
            const auto day_level = key[i] * overcast_grey;
            const auto night_level =
                (&night_sky_colour.x)[i] * night_scale;
            level[i] = night_level + (day_level - night_level) * daylight;
        }

        const auto fill = std::clamp(
            std::max({cloud_ambient_fill, cloud_overcast_diffuse * overcast, night}),
            0.0f, 1.0f);
        const auto direct_scale = 1.0f - fill;

        const float cloud_light_rgb[4] = {
            level[0] * direct_scale,
            level[1] * direct_scale,
            level[2] * direct_scale,

            overcast};
        device->SetPixelShaderConstantF(12, cloud_light_rgb, 1);

        const float cloud_ambient_rgb[4] = {
            level[0] * fill, level[1] * fill, level[2] * fill, 0.f};
        device->SetPixelShaderConstantF(13, cloud_ambient_rgb, 1);
        const float c10[4] = {
            cloud_coverage, cloud_scale, cloud_scroll.x, cloud_sharpness};
        device->SetPixelShaderConstantF(10, c10, 1);

        const float c11[4] = {
            cloud_absorption, cloud_phase_g,
            clouds_enabled ? cloud_opacity : 0.0f, cloud_scroll.y};
        device->SetPixelShaderConstantF(11, c11, 1);
        const float c19[4] = {
            cloud_variety, cloud_weather_scale, cloud_warp, cloud_billow};
        device->SetPixelShaderConstantF(19, c19, 1);
        const float c20[4] = {cloud_origin.x, cloud_origin.y, 0.f, 0.f};
        device->SetPixelShaderConstantF(20, c20, 1);
        const float c14[4] = {
            lightning_frame.flash, lightning_frame.flash_reach, lightning_frame.seed,
            lightning_frame.bolt};
        device->SetPixelShaderConstantF(14, c14, 1);
        const auto& strike = lightning_frame.direction_world;
        const XMFLOAT3 strike_sky = to_sky_frame(
            XMVectorSet(strike.x, strike.y, strike.z, 0.f));
        const float c15[4] = {
            strike_sky.x, strike_sky.y, strike_sky.z, 0.f};
        device->SetPixelShaderConstantF(15, c15, 1);

        const auto aurora_visible = aurora_enabled
            ? aurora_intensity * CurrentStarVisibility()
            : 0.0f;
        const float c16[4] = {
            aurora_visible, XMConvertToRadians(std::max(aurora_spread_deg, 1.0f)),
            aurora_drift_speed, static_cast<float>(std::clamp(aurora_steps, 4, 64))};
        device->SetPixelShaderConstantF(16, c16, 1);

        const auto aurora_w3 = DirFromElevAzim(0.0f, aurora_azimuth_deg);
        const XMFLOAT3 aurora_sky = to_sky_frame(
            XMVectorSet(aurora_w3.x, aurora_w3.y, aurora_w3.z, 0.f));
        const auto aurora_bearing = std::sqrt(
            aurora_sky.x * aurora_sky.x + aurora_sky.y * aurora_sky.y);
        const auto aurora_scale = aurora_bearing > 1e-5f ? 1.0f / aurora_bearing : 0.0f;
        const float c17[4] = {
            aurora_sky.x * aurora_scale, aurora_sky.y * aurora_scale, 0.f, 0.f};
        device->SetPixelShaderConstantF(17, c17, 1);
        const float c18[4] = {
            aurora_colour.x, aurora_colour.y, aurora_colour.z, aurora_altitude};
        device->SetPixelShaderConstantF(18, c18, 1);

        const auto godray_active = godrays_enabled && clouds_enabled && cloud_opacity > 0.0f;
        const auto sun_above = std::clamp(
            (std::sin(XMConvertToRadians(CurrentSunElevation())) + 0.05f) / 0.15f, 0.0f, 1.0f);
        const auto godray_cos_spread =
            std::cos(XMConvertToRadians(std::clamp(godray_spread_deg, 1.0f, 179.0f)));
        const float c21[4] = {
            godray_active ? godray_intensity * sun_above : 0.0f, godray_extinction,
            godray_cos_spread,
            static_cast<float>(std::clamp(godray_steps, 2, 24))};
        device->SetPixelShaderConstantF(21, c21, 1);

        const auto godray_cos_full = godray_cos_spread
            + (1.0f - godray_cos_spread) * std::clamp(godray_plateau, 0.01f, 0.99f);
        const float c22[4] = {
            godray_colour.x, godray_colour.y, godray_colour.z, godray_cos_full};
        device->SetPixelShaderConstantF(22, c22, 1);
        const float c23[4] = {
            std::max(godray_reach, 0.05f), GodrayBaseline(),
            std::max(godray_contrast, 0.0f), std::max(godray_glow, 0.0f)};
        device->SetPixelShaderConstantF(23, c23, 1);
        const float horizon_fade[4] = {
            horizon_fade_start, std::max(horizon_fade_width, 1e-4f),
            std::clamp(horizon_fade_floor, 0.0f, 1.0f), 0.f};
        device->SetPixelShaderConstantF(29, horizon_fade, 1);

        const auto sun_up = std::clamp((CurrentSunElevation() + 1.0f) / 5.0f, 0.0f, 1.0f);
        const auto rainbow_daylight = sun_up * sun_up * (3.0f - 2.0f * sun_up);
        const auto rainbow_visible = rainbow_enabled
            ? std::clamp(rainbow_intensity * rainbow_daylight, 0.0f, 4.0f)
            : 0.0f;
        const float c24[4] = {
            rainbow_visible, XMConvertToRadians(std::clamp(rainbow_radius_deg, 1.0f, 89.0f)),
            XMConvertToRadians(std::max(rainbow_width_deg, 0.05f)),
            std::max(rainbow_secondary, 0.0f)};
        device->SetPixelShaderConstantF(24, c24, 1);
        const auto antisolar_w = rainbow_follow_sun
            ? XMFLOAT3{-sun_w3.x, -sun_w3.y, -sun_w3.z}
            : DirFromElevAzim(rainbow_elevation_deg, rainbow_azimuth_deg);
        const XMFLOAT3 antisolar_sky = to_sky_frame(
            XMVector3Normalize(XMVectorSet(antisolar_w.x, antisolar_w.y, antisolar_w.z, 0.f)));
        const float c25[4] = {
            antisolar_sky.x, antisolar_sky.y, antisolar_sky.z,
            std::clamp(rainbow_saturation, 0.0f, 1.0f)};
        device->SetPixelShaderConstantF(25, c25, 1);

        SkyVertex quad[4];
        FillFullscreenQuad(quad, corner_dirs);
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(SkyVertex));
    }

    state_block->Apply();
    state_block->Release();

    if (shadow_gpu_draw_shadows && camera_depth_ready
        && ShadowSunFade() > 0.0f) {
        DrawScreenSpaceShadows(device, cam, vp_w, vp_h);
    }

    if (camera_depth_ready) {
        DrawWorldWater(device, cam, vp_w, vp_h);
        DrawWorldFog(device, cam, vp_w, vp_h);
        DrawWorldAmbient(device, cam, vp_w, vp_h);
    }

    shadow_camera_wvp_capture_pending = true;
}

static void DrawShadowReplay(IDirect3DDevice9* device)
{

    const auto now = PerfCounter();
    phase_frame_ms = phase_frame_mark ? PerfToMs(now - phase_frame_mark) : 0.0;
    phase_frame_mark = now;
    phase_stats_last = phase_stats;
    phase_stats.fill({});

    last_gr_program_calls = gr_program_calls;
    last_gr_program_call_count = gr_program_call_count;
    gr_program_call_count = 0;
    shadow_replay_submission_count = 0;

    shadow_trigger_scene_list_caller = 0;
    shadow_camera_depth_copy_succeeded = false;
    shadow_screenspace_draw_count = 0;
    if (ShadowMapPassEnabled() && device) {
        EnsureShadowPreviewResources(device);
        EnsureD3DReplayHooks(device);
    }

    RefreshWaterProgramHandles();
}

void Skybox::Initialize()
{
    if (hooks_ready || !engine_hooks.empty()) return;
    hooks_failed = false;
    const auto bindings = GW::Render::GetWorldRenderBindings();
    if (!bindings) {
        hooks_failed = true;
        Log::Error("Weather: this Guild Wars build is not supported by GWCA's world renderer.");
        return;
    }

#define BIND_WORLD_FUNCTION(name) name##_Func = bindings->name
    BIND_WORLD_FUNCTION(EnvUpdateSceneLightDir);
    BIND_WORLD_FUNCTION(GrRenderPrograms);
    BIND_WORLD_FUNCTION(GrRenderProgramQueue);
    BIND_WORLD_FUNCTION(GrTransformIdentity);
    BIND_WORLD_FUNCTION(GrTransformSetAdjustedLookAt);
    BIND_WORLD_FUNCTION(GrTransformPerspective);
    BIND_WORLD_FUNCTION(GrTransformOrthographic);
    BIND_WORLD_FUNCTION(GrTransformGetCurrent);
    BIND_WORLD_FUNCTION(GrTransformSetCurrent);
    BIND_WORLD_FUNCTION(GrTransformSnapshotRelease);
    BIND_WORLD_FUNCTION(GrRenderSceneLists);
    BIND_WORLD_FUNCTION(GmViewBuildSceneLists);
    BIND_WORLD_FUNCTION(GmWorldUpdateView);
    BIND_WORLD_FUNCTION(GmWorldBuildPrimaryScenePrograms);
    BIND_WORLD_FUNCTION(GmWorldSetPrimarySceneBuildEnabled);
    BIND_WORLD_FUNCTION(GmWorldBuildRemainingScenePrograms);
    BIND_WORLD_FUNCTION(GrSetCameraTransform);
    BIND_WORLD_FUNCTION(TrnCollectScenePrograms);
    BIND_WORLD_FUNCTION(EnvSkyBuildScenePrograms);
    BIND_WORLD_FUNCTION(GetViewVisibilityBuffer);
    BIND_WORLD_FUNCTION(GrModelFrustumCull);
    BIND_WORLD_FUNCTION(GrCullBuildVisibilityGrid);
    BIND_WORLD_FUNCTION(GmPropUpdateFade);
    BIND_WORLD_FUNCTION(GmSelectVisibleCells);
    BIND_WORLD_FUNCTION(GmPropsUpdateView);
    BIND_WORLD_FUNCTION(TrnTexShadowDecompressTile);
    BIND_WORLD_FUNCTION(TrnTexComposeTileLighting);
    BIND_WORLD_FUNCTION(TrnTexWaitForResidency);
    BIND_WORLD_FUNCTION(AvShadowBuild);
    BIND_WORLD_FUNCTION(CameraRenderScope);
    BIND_WORLD_FUNCTION(SceneRenderScope);
    BIND_WORLD_FUNCTION(GetPropLodScale);
#undef BIND_WORLD_FUNCTION
    world_scene_programs = bindings->world_scene_programs;
    gw_sky_clear_colour = bindings->sky_clear_colour;
    shadow_terrain_visibility.fill(0xffffffffu);

#define HOOK_WORLD_FUNCTION(name) CreateOwnedHook(name##_Func, On##name, name##_Ret, engine_hooks, #name)
    const auto created =
        HOOK_WORLD_FUNCTION(EnvUpdateSceneLightDir)
        && HOOK_WORLD_FUNCTION(GrRenderPrograms)
        && HOOK_WORLD_FUNCTION(GrRenderProgramQueue)
        && HOOK_WORLD_FUNCTION(GrRenderSceneLists)
        && HOOK_WORLD_FUNCTION(GmViewBuildSceneLists)
        && HOOK_WORLD_FUNCTION(TrnCollectScenePrograms)
        && HOOK_WORLD_FUNCTION(EnvSkyBuildScenePrograms)
        && HOOK_WORLD_FUNCTION(GetViewVisibilityBuffer)
        && HOOK_WORLD_FUNCTION(GrModelFrustumCull)
        && HOOK_WORLD_FUNCTION(GrCullBuildVisibilityGrid)
        && HOOK_WORLD_FUNCTION(GmPropUpdateFade)
        && HOOK_WORLD_FUNCTION(GmSelectVisibleCells)
        && HOOK_WORLD_FUNCTION(TrnTexShadowDecompressTile)
        && HOOK_WORLD_FUNCTION(TrnTexComposeTileLighting)
        && HOOK_WORLD_FUNCTION(TrnTexWaitForResidency)
        && HOOK_WORLD_FUNCTION(AvShadowBuild);
#undef HOOK_WORLD_FUNCTION
    if (!created) {
        for (const auto hook : engine_hooks) GW::Hook::RemoveHook(hook);
        engine_hooks.clear();
        hooks_failed = true;
        return;
    }
    hooks_ready = true;
    compositor_token = GameWorldCompositor::RegisterDraw(&DrawInWorld, -100);
    shadow_compositor_token = GameWorldCompositor::RegisterPreWorldDraw(&DrawShadowReplay);
    for (const auto hook : engine_hooks) GW::Hook::EnableHooks(hook);
}

namespace {
    constexpr auto kWarn = ImVec4(0.94f, 0.20f, 0.20f, 1.0f);

    void DrawSunSettings()
    {
        ImGui::Checkbox("Draw our sky", &sky_enabled);
        ImGui::Separator();
        ImGui::Checkbox("Day/night cycle", &sun_cycle_enabled);
        if (sun_cycle_enabled) {
            ImGui::SliderFloat("Time of day", &sun_cycle_hour, 0.f, 24.f, "%.2f h");
            ImGui::SliderFloat(
                "Day length", &sun_cycle_minutes, 0.25f, 240.f, "%.2f real min");
            ImGui::SliderFloat(
                "Latitude", &sun_cycle_latitude_deg, -66.f, 66.f, "%.1f deg");
            ImGui::SliderFloat(
                "Season", &sun_cycle_declination_deg, -23.44f, 23.44f, "%.2f deg");
            float cycle_elevation = 0.0f;
            float cycle_azimuth = 0.0f;
            CurrentSunCyclePosition(cycle_elevation, cycle_azimuth);
            ImGui::TextDisabled(
                "Now: elevation %.1f deg, azimuth %.1f deg.", cycle_elevation, cycle_azimuth);
        }
        else {
            ImGui::SliderFloat(
                "Sun elevation", &sun_elevation_deg, -90.f, 90.f, "%.1f deg");
            ImGui::SliderFloat("Sun azimuth", &sun_azimuth_deg, 0.f, 360.f, "%.1f deg");
            ImGui::Checkbox("Animate sun", &animate_sun);
            if (animate_sun) {
                ImGui::SliderFloat("Animate speed", &animate_speed, 0.001f, 1.f, "%.3f");
            }
        }
        ImGui::SliderFloat("Sun disk size", &sun_disk_size_deg, 0.1f, 5.f, "%.2f deg");
        ImGui::SliderFloat("Sun disk intensity", &sun_disk_intensity, 0.f, 40.f, "%.1f");
        ImGui::SliderFloat("Eye altitude", &eye_altitude_km, 0.f, 20.f, "%.2f km");
        ImGui::Text("Sky exposure: %+.2f EV (driven by sun elevation)", CurrentExposure());
        ImGui::ColorEdit3(
            "Night sky colour", reinterpret_cast<float*>(&night_sky_colour),
            ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
    }

    void DrawStarSettings()
    {
        ImGui::Checkbox("Stars", &stars_enabled);
        if (!stars_enabled) return;
        ImGui::SliderFloat("Star brightness", &star_brightness, 0.f, 4.f, "%.2f");
        ImGui::SliderFloat("Star density", &star_density, 100.f, 3000.f, "%.0f");
        ImGui::SliderFloat("Star sparsity", &star_threshold, 4.f, 90.f, "%.1f");
        ImGui::SliderFloat("Star twinkle speed", &star_twinkle_speed, 0.f, 5.f, "%.2f");
        ImGui::Text("Star visibility: %.2f", CurrentStarVisibility());
    }

    void DrawAuroraSettings()
    {
        ImGui::Checkbox("Aurora", &aurora_enabled);
        if (!aurora_enabled) return;
        ImGui::SliderFloat("Aurora intensity", &aurora_intensity, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat("Aurora bearing", &aurora_azimuth_deg, 0.f, 360.f, "%.0f deg");
        ImGui::SliderFloat("Aurora spread", &aurora_spread_deg, 5.f, 180.f, "%.0f deg");
        ImGui::SliderFloat("Curtain drift", &aurora_drift_speed, 0.f, 0.3f, "%.3f");
        ImGui::SliderFloat("Curtain altitude", &aurora_altitude, 0.2f, 2.f, "%.2f");
        ImGui::ColorEdit3("Aurora tint", reinterpret_cast<float*>(&aurora_colour));
        ImGui::SliderInt("March steps", &aurora_steps, 4, 64);
        ImGui::Text(
            "Showing at %.2f (intensity %.2f x star visibility %.2f)",
            aurora_intensity * CurrentStarVisibility(), aurora_intensity,
            CurrentStarVisibility());
    }

    void DrawMoonSettings()
    {
        ImGui::Checkbox("Moon", &moon_enabled);
        if (!moon_enabled) return;
        ImGui::SliderFloat("Moon size", &moon_size_deg, 0.2f, 8.f, "%.2f deg");
        ImGui::SliderFloat("Moon brightness", &moon_intensity, 0.f, 4.f, "%.2f");
        ImGui::SliderFloat("Moon glow", &moon_glow, 0.f, 1.f, "%.3f");
        ImGui::ColorEdit3("Moon colour", reinterpret_cast<float*>(&moon_colour));
        ImGui::ColorEdit3("Moon glow colour", reinterpret_cast<float*>(&moon_glow_colour));
        ImGui::TextDisabled("Sits opposite the Sun, so it is always full.");
    }

    void DrawRainbowSettings()
    {
        ImGui::Checkbox("Rainbow", &rainbow_enabled);
        if (!rainbow_enabled) return;
        ImGui::SliderFloat("Bow intensity", &rainbow_intensity, 0.f, 2.f, "%.2f");
        ImGui::Checkbox("Follow sun", &rainbow_follow_sun);
        if (!rainbow_follow_sun) {
            ImGui::SliderFloat(
                "Bow azimuth", &rainbow_azimuth_deg, 0.f, 360.f, "%.0f deg");
            ImGui::SliderFloat(
                "Bow elevation", &rainbow_elevation_deg, -60.f, 20.f, "%.0f deg");
        }
        ImGui::SliderFloat("Bow spread", &rainbow_radius_deg, 5.f, 89.f, "%.1f deg");
        ImGui::SliderFloat("Bow width", &rainbow_width_deg, 0.2f, 12.f, "%.2f deg");
        ImGui::SliderFloat("Secondary bow", &rainbow_secondary, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat("Bow saturation", &rainbow_saturation, 0.f, 1.f, "%.2f");
    }

    void DrawGodRaySettings()
    {
        ImGui::Checkbox("Crepuscular rays", &godrays_enabled);
        if (!godrays_enabled) return;
        ImGui::SliderFloat("Ray shadow", &godray_intensity, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat("Ray glow", &godray_glow, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat("Ray extinction", &godray_extinction, 0.5f, 16.f, "%.2f");
        ImGui::SliderFloat("Ray spread", &godray_spread_deg, 5.f, 179.f, "%.0f deg");
        ImGui::SliderFloat("Ray core", &godray_plateau, 0.01f, 0.99f, "%.2f");
        ImGui::SliderFloat("Ray reach", &godray_reach, 0.1f, 8.f, "%.2f clouds");
        ImGui::SliderFloat("Ray contrast", &godray_contrast, 0.f, 8.f, "%.2f");
        ImGui::SliderInt("Ray steps", &godray_steps, 2, 24);
        ImGui::ColorEdit3("Ray tint", reinterpret_cast<float*>(&godray_colour));
    }

    void DrawCloudSettings()
    {
        ImGui::Checkbox("Clouds", &clouds_enabled);
        if (!clouds_enabled) return;
        ImGui::SliderFloat("Cloud coverage", &cloud_coverage, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat("Cloud scale", &cloud_scale, 0.2f, 8.f, "%.2f");
        ImGui::SliderFloat("Cloud speed at strong wind", &cloud_speed, 0.f, 2.f, "%.3f");
        ImGui::SliderFloat("Cloud sharpness", &cloud_sharpness, 1.f, 8.f, "%.2f");
        ImGui::SliderFloat("Cloud opacity", &cloud_opacity, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat("Cloud light absorption", &cloud_absorption, 0.f, 4.f, "%.2f");
        ImGui::SliderFloat("Cloud forward scatter", &cloud_phase_g, 0.f, 0.95f, "%.2f");
        ImGui::SliderFloat("Cloud overcast darkening", &cloud_darkness, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat(
            "Overcast starts at coverage", &cloud_overcast_start, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat(
            "Overcast full at coverage", &cloud_overcast_full, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat("Overcast grey", &cloud_overcast_level, 0.f, 1.f, "%.3f");
        ImGui::SliderFloat(
            "Night deck vs sky", &cloud_night_sky_ratio, 0.f, 3.f, "%.2fx");
        ImGui::SliderFloat(
            "Overcast diffuse shift", &cloud_overcast_diffuse, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat("Overcast cool shift", &cloud_overcast_cool, 0.f, 0.3f, "%.3f");
        ImGui::SliderFloat("Cloud skylight fill", &cloud_ambient_fill, 0.f, 1.f, "%.2f");

        ImGui::SeparatorText("Shape variety");
        ImGui::SliderFloat("Size variety", &cloud_variety, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat("Weather region size", &cloud_weather_scale, 0.05f, 1.f, "%.2f");
        ImGui::SliderFloat("Domain warp", &cloud_warp, 0.f, 2.f, "%.2f");
        ImGui::SliderFloat("Billow", &cloud_billow, 0.f, 1.f, "%.2f");

        ImGui::SeparatorText("Cloud shadows");
        ImGui::SliderFloat("Deck altitude", &cloud_altitude, 500.f, 20000.f, "%.0f");
        ImGui::SliderFloat("Shadow strength", &cloud_shadow_strength, 0.f, 1.f, "%.2f");
        ImGui::SliderInt("Shadow detail", &cloud_shadow_octaves, 1, 4);
        {
            const auto deck = std::max(cloud_altitude, 1.0f);
            const auto light = GetSceneLightDirWorld();
            const auto climb = std::max(-light.z, 0.15f);
            const auto throw_distance = deck / climb;
            const auto cell = deck / std::max(cloud_scale, 1e-3f);
            ImGui::TextDisabled(
                "Shadows thrown %.0f units downwind of their cloud; one cloud spans about %.0f\n"
                "units on the ground.",
                throw_distance, cell);
        }
        {
            const auto darkness = std::clamp(cloud_darkness, 0.0f, 1.0f);
            const auto overcast = CurrentOvercast() * darkness;
            const auto daylight = CurrentDaylightBlend();
            const auto overcast_grey =
                1.0f - (1.0f - cloud_overcast_level) * overcast;
            const auto night_level =
                night_sky_colour.y * cloud_night_sky_ratio * overcast_grey;
            const auto level =
                night_level + (overcast_grey - night_level) * daylight;
            const auto core = 1.0f - std::exp(-cloud_opacity * 8.0f);
            const auto edge = 1.0f - std::exp(-0.15f * cloud_opacity * 8.0f);
            ImGui::Text(
                "Overcast %.2f -> deck %.3f (%d/255)",
                overcast, level, static_cast<int>(level * 255.0f + 0.5f));
            ImGui::Text("Alpha: core %.3f, wisp %.3f", core, edge);
        }

        ImGui::Separator();
        ImGui::TextDisabled(
            "Lightning has moved to the weather effects, where it is a composable effect in its\n"
            "own right - so a storm can rage without rain, or rain fall without one. The sky\n"
            "only renders the strike in force. Flash now: %.2f, bolt %.2f.",
            lightning_frame.flash, lightning_frame.bolt);
    }

    void DrawWorldLightingSettings()
    {
        ImGui::Checkbox("Day/night world lighting", &world_ambient_enabled);
        if (!world_ambient_enabled) return;
        ImGui::ColorEdit3("Daylight tint", reinterpret_cast<float*>(&ambient_day_colour));
        ImGui::ColorEdit3("Night tint", reinterpret_cast<float*>(&ambient_night_colour));
        ImGui::SliderFloat("Daylight strength", &day_brightness, 0.f, 2.f, "%.2fx");
        const auto tint = CurrentAmbientTint();
        ImGui::Text("Current world tint: %.2f %.2f %.2f", tint.x, tint.y, tint.z);
        ImGui::Text(
            "Actually applied: %.2f %.2f %.2f (%u draws, daylight %.2f)",
            shadow_ambient_applied.x, shadow_ambient_applied.y, shadow_ambient_applied.z,
            shadow_ambient_draw_count, CurrentDaylightBlend());
        if (lightning_frame.flash > 0.001f) {
            ImGui::TextDisabled(
                "A strike is lifting it by %.2f right now.",
                std::clamp(lightning_frame.flash * lightning_frame.world_flash, 0.0f, 1.0f));
        }
        ImGui::Checkbox("Dim the world under overcast", &world_overcast_dimming);
        const auto dim = CurrentOvercastDim();
        ImGui::TextDisabled(
            "Clear-sky noon is ~100,000 lux against ~1,000-20,000 under a thick deck. Eye\n"
            "adaptation swallows nearly all of that, so it lands as a modest multiply,\n"
            "tinted slightly cool because overcast light is multiply-scattered skylight\n"
            "(~7000K) rather than direct sun (~5500K). Currently %.2f %.2f %.2f.",
            dim.x, dim.y, dim.z);

        ImGui::SeparatorText("Player light");
        ImGui::Checkbox("Light on the player", &player_light_enabled);
        if (player_light_enabled) {
            ImGui::SliderFloat("Player light reach", &player_light_radius, 100.f, 4000.f, "%.0f");
            ImGui::SliderFloat("Player light brightness", &player_light_intensity, 0.f, 2.f, "%.2f");
            ImGui::SliderFloat("Player light core", &player_light_core, 0.f, 0.95f, "%.2f");
            ImGui::TextDisabled("Fraction of the reach held at full strength before it falls away.");
            ImGui::SliderFloat("Player light height", &player_light_height, 0.f, 400.f, "%.0f");
            ImGui::ColorEdit3(
                "Player light colour", reinterpret_cast<float*>(&player_light_colour));
            ImGui::TextDisabled(
                "Takes the first of the eight light slots, so a crowded room cannot push it out.");
        }
        ImGui::Separator();
        ImGui::Checkbox("Suppress GW's own sky", &suppress_gw_sky);
        ImGui::TextDisabled(
            "GW draws a real skybox - a dome with cloud and star layers - before the world.\n"
            "Terrain and props are then drawn over it with anti-aliased edges, so every\n"
            "silhouette pixel keeps a share of whatever the skybox painted behind it. Our sky\n"
            "replaces only pixels that are ENTIRELY sky, so it can never reach the share baked\n"
            "into an edge - which is exactly the pale rim that survives into the night.\n\n"
            "Not drawing it removes the contaminant instead of chasing it: the frame keeps the\n"
            "clear colour below, which is already ours, so GW's own anti-aliasing blends edges\n"
            "against the right colour and there is nothing left to repair.%s",
            EnvSkyBuildScenePrograms_Func
                ? "" : "\n\nHOOK UNAVAILABLE in this build - GW's sky is still drawing.");

        ImGui::SliderFloat("Horizon band start", &horizon_fade_start, -0.05f, 0.20f, "%.3f");
        ImGui::SliderFloat("Horizon band width", &horizon_fade_width, 0.001f, 0.40f, "%.3f");
        ImGui::SliderFloat("Horizon band floor", &horizon_fade_floor, 0.f, 1.f, "%.2f");
        ImGui::Checkbox("Debug: sky ownership", &sky_ownership_view);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Paints every pixel the SKY pass writes flat orange, using the REAL Z-buffer the "
                "sky tests against - so it sees what the water ownership view (which reads the "
                "RESZ depth copy) cannot.\n\n"
                "Turn the water ownership view OFF while using this, or the water pass paints "
                "over the orange. If the thin horizon line turns orange the sky owns it; if it "
                "stays light blue while the rest of the sky goes orange, nothing of ours paints "
                "it and GW's own pixel is surviving.");
        }
        ImGui::Checkbox("Sky claims horizon via depth copy", &sky_depth_from_copy);
        if (sky_depth_from_copy) {
            ImGui::TextColored(
                kWarn,
                "Draws a bright rim along every silhouette: the depth copy and the hardware\n"
                "disagree by about a pixel at any depth discontinuity, and the sky paints the\n"
                "difference. The horizon line this was for is handled by the water pass now.");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Candidate fix for the thin light line. The sky pass normally decides what is "
                "sky with the hardware Z-test against the REAL depth buffer, while the water, "
                "fog and tint passes read the RESZ depth COPY. The two disagree at the sea "
                "horizon - the real buffer sees GW's distant water, the copy reads it as sky - "
                "so that band is claimed by nobody and GW's own pixel survives.\n\n"
                "With this on, the sky classifies from the SAME copy, so all five passes agree "
                "and the sky itself paints the band. If the line disappears with this on and the "
                "sky ownership view (above) shows the line turning orange, this is the fix.");
        }
        ImGui::Checkbox("Suppress GW's fog", &suppress_gw_fog);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "GW's fog is authored for its own short draw distance, so it starts early and a "
                "clear day still looks hazy.\n\n"
                "Off entirely, the distance fade is ours alone - use Replace distance fog's "
                "start, end and strength to decide how far you can see. Makes 'Match GW's fog to "
                "our sky' irrelevant, since there is no fog left to tint.");
        }
        ImGui::BeginDisabled(suppress_gw_fog);
        ImGui::Checkbox("Match GW's fog to our sky", &recolour_gw_fog);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "GW fogs distant geometry toward a colour authored for ITS sky - a pale "
                "periwinkle blue. With our sky in place, distant objects fade toward a colour "
                "that no longer matches what is behind them, which reads as a pale blue rim on "
                "horizon silhouettes.\n\n"
                "On, geometry fades into exactly the sky behind it.");
        }
        ImGui::EndDisabled();
        ImGui::Checkbox("Recolour GW's sky clear", &recolour_gw_sky);
        ImGui::TextDisabled(
            "GW's sky is a CLEAR COLOUR, not geometry: three bytes from the map's environment\n"
            "data, written once a frame and used to clear the frame buffer. That is the pale\n"
            "blue behind everything - and the reason a silhouette against the sky picks up a\n"
            "pale rim. GW clears to it, then draws geometry over it with anti-aliased edges, so\n"
            "every edge pixel keeps a share of it. Our sky pass only replaces pixels that are\n"
            "ENTIRELY sky, so it can never reach the share baked into an edge.\n\n"
            "Setting the clear colour to ours fixes it at the source: the blend GW performs is\n"
            "then already against the right colour and there is nothing left to repair.%s",
            gw_sky_clear_colour ? "" : "\n\nNOT FOUND in this build - the address scan failed.");
        if (gw_sky_clear_colour && recolour_gw_sky) {
            const auto sky = CurrentSkyClearColour();
            ImGui::TextDisabled(
                "Currently %.3f %.3f %.3f -> 0x%08X.",
                sky.x, sky.y, sky.z, *gw_sky_clear_colour);
        }

        ImGui::SeparatorText("Distance haze");
        ImGui::Checkbox("Replace distance fog", &world_fog_enabled);
        if (world_fog_enabled) {
            ImGui::ColorEdit3(
                "Haze colour (day)", reinterpret_cast<float*>(&world_fog_day_colour));
            ImGui::SliderFloat("Haze start", &world_fog_start, 0.f, 30000.f, "%.0f");
            ImGui::SliderFloat("Haze end", &world_fog_end, 100.f, 60000.f, "%.0f");
            ImGui::SliderFloat("Haze strength", &world_fog_strength, 0.f, 1.f, "%.2f");
            ImGui::SliderFloat("Night bias", &world_fog_night_bias, 0.f, 1.f, "%.2f");
            ImGui::SliderFloat(
                "Silhouette repair", &world_fog_edge_repair, 0.f, 8.f, "%.2f");
            ImGui::SliderInt("Repair radius", &world_fog_edge_radius, 1, 4, "%d px");
            ImGui::Checkbox("Debug: depth classification", &world_fog_debug_view);
            if (world_fog_debug_view) {
            }
            ImGui::SliderFloat(
                "Overcast thickening", &world_fog_overcast_gain, 0.f, 1.f, "%.2f");
            const auto fog = CurrentFogColour();
            ImGui::TextDisabled(
                "Currently %.3f %.3f %.3f.", fog.x, fog.y, fog.z);
        }

        ImGui::SeparatorText("Light sources");
        ImGui::Checkbox("Lamps resist the night", &world_lights_enabled);
        if (world_lights_enabled) {
            ImGui::SliderFloat("Lamp authority", &world_light_strength, 0.f, 4.f, "%.2f");
            ImGui::SliderFloat("Lamp radius", &world_light_reach, 0.02f, 2.f, "%.2fx");
            ImGui::SliderFloat("Lamp falloff", &world_light_falloff, 0.5f, 8.f, "%.2f");
            ImGui::SliderFloat(
                "Lamp cull distance", &world_light_cull_distance, 500.f, 20000.f, "%.0f");
            ImGui::TextDisabled("%d of the map's lights in range this frame (max %d).",
                shadow_world_light_count, kMaxWorldLights);
        }
    }

    void DrawShadowSettings()
    {
        ImGui::Checkbox("Align shadows with sun/moon", &align_shadows);
        ImGui::Checkbox("Dynamic sun shadows", &shadow_gpu_draw_shadows);
        if (!shadow_gpu_draw_shadows) return;
        ImGui::Combo(
            "Shadow map size", &shadow_map_size_index,
            "256 x 256\0" "512 x 512\0" "1024 x 1024\0" "2048 x 2048\0" "4096 x 4096\0");
        {

            const auto coverage = std::max(shadow_light_camera_radius * 2.0f, 1.0f);
            const auto texels_per_unit =
                static_cast<float>(shadow_map_size) / coverage;
            ImGui::TextDisabled(
                "%u x %u covering %.0f units: %.3f texels per unit (%.1f units per texel).",
                shadow_map_size, shadow_map_size, coverage, texels_per_unit,
                1.0f / texels_per_unit);
            if (shadow_map_size_active && shadow_map_size_active != shadow_map_size) {
                ImGui::TextDisabled("Rebuilding at the new size on the next frame.");
            }
        }
        ImGui::SliderFloat("Shadow bias", &shadow_gpu_bias, 0.0f, 300.0f, "%.0f units");
        ImGui::SliderFloat("Receiver-plane bias", &shadow_gpu_slope_bias, 0.0f, 8.0f, "%.2f");
        ImGui::SliderFloat("Shadow strength", &shadow_gpu_strength, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Moonlight shadow strength", &moon_shadow_strength, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Scales shadows once the Sun sets; 0 disables the night light pass.");
        ImGui::SliderFloat("Shadow coverage radius", &shadow_light_camera_radius, 250.0f, 12000.0f, "%.0f");
        ImGui::SliderFloat("Coverage forward bias", &shadow_light_camera_forward_bias, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled(
            "Forward bias shifts coverage toward the camera's facing, so less of "
            "the map is spent behind the player.");
        ImGui::Checkbox("Characters cast shadows", &shadow_cast_agent_shadows);
        ImGui::Checkbox("Disable baked terrain shadows", &disable_baked_terrain_shadows);
        if (disable_baked_terrain_shadows) {
            ImGui::Indent();
            ImGui::TextDisabled(
                "Two layers: a per-texel mask and a per-block 'whole block shadowed' mask.\n"
                "%u tiles cleared, %u block masks suppressed.",
                shadow_baked_tiles_voided, shadow_baked_blocks_cleared);
            if (!TrnTexComposeTileLighting_Func) {
                ImGui::TextColored(kWarn, "Block-shadow hook missing: building shadows will remain.");
            }
            ImGui::Checkbox("Skip the baked tile decode", &shadow_skip_baked_tile_decode);
            ImGui::TextDisabled(
                "GW entropy-decodes a 272x272 shadow tile that we then overwrite with 'fully lit'.\n"
                "Skipping it is the main cost of terrain streaming churn. %u skipped, %u voided.",
                shadow_baked_tile_decodes_skipped, shadow_baked_tiles_voided);
            ImGui::Unindent();
        }
        ImGui::Checkbox("Disable baked character shadows", &disable_agent_baked_shadows);
        ImGui::TextDisabled("Baked shadows clear as terrain tiles reload (zone or move around).");
        ImGui::TextDisabled("Uses native orthographic caster pass and screen-space receiving.");

        ImGui::Separator();
        ImGui::Checkbox("Cull casters to the light box", &shadow_light_cull_enabled);
        ImGui::TextDisabled(
            "The shadow map only covers the coverage radius, but GW collects casters using the "
            "player's frustum - so without this every terrain tile in the map is drawn into it.");
        if (shadow_light_cull_enabled) {
            const auto tested = shadow_terrain_cull_bypass_count + shadow_terrain_cull_rejected_count;
            ImGui::Text(
                "Terrain tiles: %u drawn, %u culled (%.0f%% skipped)",
                shadow_terrain_cull_bypass_count, shadow_terrain_cull_rejected_count,
                tested ? 100.0 * shadow_terrain_cull_rejected_count / tested : 0.0);
            if (shadow_light_cull_degenerate) {
                ImGui::TextColored(kWarn, "Disabled: a fill culled every tile, so the frustum was not the light's.");
            }
        }
        ImGui::Separator();
        ImGui::Checkbox("Skip view update in shadow pass", &shadow_skip_view_update);
        ImGui::TextDisabled(
            "GmWorldRenderFromCamera flips terrain LOD/streaming, sky and environment to the\n"
            "light's viewpoint and back every frame - the measured 60+ms. This keeps them on the\n"
            "player while still rendering from the light. EXPERIMENTAL: check for missing\n"
            "casters. %u skipped.", shadow_view_updates_skipped);
        ImGui::Checkbox("Cull objects in the shadow pass", &shadow_cull_objects);
        ImGui::TextDisabled(
            "Off by default: the frustum available during the pass is the PLAYER's, so culling\n"
            "drops casters the light can see and their shadows pop as you turn. %u objects drawn\n"
            "unculled last fill. Terrain is still culled to the light box.",
            shadow_objects_not_culled);
        ImGui::Checkbox("Props follow the light", &shadow_props_follow_light);
        ImGui::Checkbox(
            "Ignore per-cell visibility in the shadow pass", &shadow_ignore_cell_visibility);
        ImGui::TextDisabled(
            "The real fix for casters popping as you turn. Props and agents never reach the\n"
            "frustum cull at all - GmPropsUpdateView admits a cell only if it is set in the\n"
            "player-view bitmasks from GetViewVisibilityBuffer, so unlit cells generate no\n"
            "programs. Substitutes an all-ones mask for the light pass. %u masks replaced,\n"
            "%u left alone (too large for the buffer).",
            shadow_cell_visibility_override_count, shadow_cell_visibility_too_large_count);
        ImGui::Checkbox("Keep prop fade off the shadow pass", &shadow_skip_prop_fade);
        ImGui::TextDisabled(
            "GW fades props that block your view, using a ray from the manager's current eye.\n"
            "Filling the shadow map pointed that at the light and faded props out. %u skipped.",
            shadow_prop_fades_skipped);
        ImGui::TextDisabled(
            "Props are picked by cell from the player's eye and facing, so without this a tree's\n"
            "shadow vanishes as soon as you turn away from it. Re-selects them from the light\n"
            "during the shadow pass. %u prop updates, %u cell selections re-centred.",
            shadow_prop_updates_run, shadow_cell_overrides);

        ImGui::Separator();
        ImGui::Checkbox("Profile shadow phases", &shadow_profile_enabled);
        if (shadow_profile_enabled) {
            ImGui::Text("Frame: %.1f ms (%.0f fps)", phase_frame_ms,
                        phase_frame_ms > 0.0 ? 1000.0 / phase_frame_ms : 0.0);
            if (ImGui::BeginTable("phases", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("phase");
                ImGui::TableSetupColumn("ms");
                ImGui::TableSetupColumn("calls");
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < phase_stats_last.size(); ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(kPhaseNames[i]);
                    ImGui::TableNextColumn();
                    const auto ms = phase_stats_last[i].ms;
                    if (ms > 0.05 * phase_frame_ms && phase_frame_ms > 0.0)
                        ImGui::TextColored(kWarn, "%.2f", ms);
                    else
                        ImGui::Text("%.2f", ms);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", phase_stats_last[i].calls);
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Phases nest, so inner ones are included in their parent's total.");
        }

        ImGui::Separator();
        ImGui::Checkbox("Skip terrain streaming waits", &shadow_skip_shadow_pass_residency);
        ImGui::TextDisabled(
            "GW blocks the render thread to stream in a terrain texture the moment it's drawn.\n"
            "The shadow map only reads depth, so waiting for one is a pure stall. %u skipped.",
            shadow_residency_waits_skipped);
        ImGui::Checkbox("Skip non-depth-writing draws", &shadow_skip_non_depth_draws);
        ImGui::Checkbox("Skip water in the shadow pass", &shadow_skip_water_draws);
        ImGui::TextDisabled(
            "GW's water contributes one program to the scene list (the handle at water+0xAC,\n"
            "appended by MapWater's collector), not the four shader programs it also builds.\n"
            "Transparent surfaces cannot be the nearest opaque occluder, so they have no place\n"
            "in a shadow map. %u skipped last fill.",
            shadow_water_programs_skipped);
        ImGui::TextDisabled(
            "Transparent/decal passes can't be the nearest occluder, so they don't belong in a "
            "shadow map. %u skipped in the last fill.", shadow_replay_draws_skipped);

        ImGui::Separator();
        ImGui::Checkbox("Reuse identical shadow maps", &shadow_reuse_shadow_map);
        if (shadow_reuse_shadow_map) {
            ImGui::SliderInt("Shadow update interval", &shadow_update_interval, 1, 8, "%d frames");
        }
        const auto total = shadow_map_render_count + shadow_map_reuse_count;
        ImGui::Text(
            "Shadow map: %u filled, %u reused (%.0f%% skipped)",
            shadow_map_render_count, shadow_map_reuse_count,
            total ? 100.0 * shadow_map_reuse_count / total : 0.0);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset counters")) {
            shadow_map_render_count = 0;
            shadow_map_reuse_count = 0;
        }
        ImGui::Text("Last fill: %u draws, %u terrain tiles", shadow_replay_draw_count, shadow_terrain_queue_count);

        ImGui::Separator();
        const char* const debug_views[] = {
            "Off", "Solid red (quad reaches screen)", "Camera depth",
            "Reconstructed world position", "Shadow-map UV + depth",
            "Raw shadow-map depth", "Shadow-map depth (amplified)",
            "Receiver vs caster depth"};
        ImGui::Combo("Shadow debug view", &shadow_screenspace_debug_view, debug_views, IM_ARRAYSIZE(debug_views));
    }

    void DrawShadowReplaySettings()
    {
        ImGui::Checkbox("Shadow-map replay preview", &shadow_replay_preview);
        if (!shadow_replay_preview) return;

        ImGui::Checkbox("Debug replay from light camera", &shadow_replay_light_camera);
        ImGui::Checkbox("Replay terrain only", &shadow_replay_terrain_only);
        if (!shadow_gpu_draw_shadows) {
            ImGui::Checkbox("GPU terrain replay (MiniEngine camera)", &shadow_gpu_terrain_replay);
        }
        if (UseShadowLightCamera() && shadow_light_camera_orthographic && !shadow_gpu_terrain_replay) {
            ImGui::TextColored(kWarn, "Native orthographic collection still uses GW's perspective culler.");
        }
        if (UseShadowLightCamera() || shadow_gpu_terrain_replay) {
            ImGui::SliderFloat("Minimum light half-depth", &shadow_light_camera_distance, 250.0f, 24000.0f, "%.0f");
            ImGui::Checkbox("Invert light camera direction", &shadow_light_camera_invert_direction);
            ImGui::Checkbox("Orthographic light projection", &shadow_light_camera_orthographic);
            if (shadow_light_camera_orthographic) {
                ImGui::SliderFloat("Light camera coverage radius", &shadow_light_camera_radius, 250.0f, 12000.0f, "%.0f");
            }
            else {
                ImGui::SliderFloat("Light camera field of view", &shadow_light_camera_fov, 10.0f, 170.0f, "%.0f deg");
            }
        }

        if (!shadow_preview_texture) return;

        ImGui::Separator();
        ImGui::Text(
            "Renderer %u, programs %u, queued %u, swaps %u",
            shadow_replay_renderer, shadow_replay_program_count, shadow_replay_queue_count,
            shadow_replay_swap_count);
        ImGui::Text(
            "RT redirects %u, builder draws %u, replay draws %u",
            shadow_replay_target_redirects, shadow_builder_draw_count, shadow_replay_draw_count);
        ImGui::Text(
            "GPU terrain captured %u, replayed %u",
            shadow_gpu_captured_draw_count, shadow_gpu_replayed_draw_count);
        ImGui::Text(
            "Primary caster programs %u, baked tiles voided %u",
            shadow_primary_program_count, shadow_baked_tiles_voided);
        ImGui::Text("GPU program draws diagnosed %u", shadow_gpu_program_draw_count);
        ImGui::Text(
            "Terrain collectors %u, mask overrides %u",
            shadow_terrain_collect_count, shadow_terrain_visibility_override_count);
        ImGui::Text(
            "Terrain handles %u, queues %u, cull bypasses %u",
            static_cast<uint32_t>(shadow_terrain_program_handles.size()),
            shadow_terrain_queue_count, shadow_terrain_cull_bypass_count);
        ImGui::Text(
            "Terrain original snapshots: absolute %u, other %u",
            shadow_terrain_original_absolute_count, shadow_terrain_original_relative_count);
        ImGui::Separator();
        if (!shadow_camera_depth_copy_succeeded) {
            ImGui::TextColored(
                kWarn, "Camera depth copy FAILED (%s) - fog, water, the day/night tint and "
                "the shadows are all skipped.", shadow_camera_depth_failure);
        }
        ImGui::Separator();
        ImGui::Text("Scene-list caller %08X", static_cast<uint32_t>(shadow_scene_list_caller));

        const auto preview_min = ImGui::GetCursorScreenPos();
        ImGui::Image(reinterpret_cast<ImTextureID>(shadow_preview_texture), ImVec2(256.0f, 256.0f));
        const auto preview_center = ImVec2(preview_min.x + 128.0f, preview_min.y + 128.0f);
        auto* const draw_list = ImGui::GetWindowDrawList();
        draw_list->AddLine(
            ImVec2(preview_center.x - 6.0f, preview_center.y),
            ImVec2(preview_center.x + 6.0f, preview_center.y), IM_COL32(255, 64, 64, 255));
        draw_list->AddLine(
            ImVec2(preview_center.x, preview_center.y - 6.0f),
            ImVec2(preview_center.x, preview_center.y + 6.0f), IM_COL32(255, 64, 64, 255));

        if (shadow_native_light_wvp_ready) {
            if (const auto* const player = GW::Agents::GetControlledCharacter()) {
                const float world[4] = {player->x, player->y, player->z, 1.0f};
                float clip[4] = {};
                for (size_t row = 0; row < 4; ++row) {
                    for (size_t column = 0; column < 4; ++column) {
                        clip[row] += shadow_native_light_wvp[row * 4 + column] * world[column];
                    }
                }
                if (std::fabs(clip[3]) > 1e-6f) {
                    const auto u = (clip[0] / clip[3]) * 0.5f + 0.5f;
                    const auto v = (clip[1] / clip[3]) * -0.5f + 0.5f;
                    draw_list->AddCircle(
                        ImVec2(preview_min.x + u * 256.0f, preview_min.y + v * 256.0f),
                        5.0f, IM_COL32(64, 255, 64, 255), 0, 2.0f);
                    ImGui::Text("Player in shadow map: u=%.3f v=%.3f", u, v);
                }
            }
        }
        ImGui::Text("Native GR calls:");
        for (uint32_t i = 0; i < last_gr_program_call_count; ++i) {
            const auto& call = last_gr_program_calls[i];
            ImGui::Text(
                "%u: caller %08X, renderer %u, count %u, flags %u, %s",
                i, static_cast<uint32_t>(call.caller), call.renderer, call.count, call.flags,
                call.frcache ? "FrCache" : "non-FrCache");
        }
    }

    void DrawWaterSettings()
    {
        ImGui::Checkbox("Replace water surface", &water_enabled);
        ImGui::TextDisabled(
            "Suppresses GW's own water and draws ours from the water plane down to the seabed.\n"
            "%u GW water draws suppressed.", gw_water_programs_suppressed);
        if (!water_enabled) return;
        float plane_z = 0.0f;
        if (GetWorldWaterPlane(plane_z)) {
            ImGui::Text("Water plane at z = %.1f", plane_z);
        }
        else {
            ImGui::TextDisabled("This map has no water plane.");
        }
        if (ocean_resources_failed) {
            ImGui::TextColored(
                kWarn, "Simulation unavailable (%s) - GW's own water is left in place.",
                ocean_failure ? ocean_failure : "unknown");
            return;
        }
        ImGui::TextDisabled(
            "%ux%u spectrum, %u passes/frame.",
            kOceanResolution, kOceanResolution, kOceanStages * 2 + 3);

        ImGui::SeparatorText("Sea state");
        ImGui::SliderFloat("Patch size", &ocean_patch_size, 400.f, 32000.f, "%.0f");
        ImGui::SliderFloat("Wave length", &ocean_wave_length, 40.f, 8000.f, "%.0f");
        ImGui::SliderFloat("Wave height", &ocean_wave_height, 0.f, 60.f, "%.1f");
        {
            const auto wavelength = DominantWavelength();
            const auto waves_per_tile = ocean_patch_size / std::max(wavelength, 1.f);
            const auto texels_per_wave =
                wavelength / (ocean_patch_size / static_cast<float>(kOceanResolution));
            ImGui::Text(
                "%.0f units between crests, %.1f high. %.1f waves per tile, %.0f texels each.",
                wavelength, CharacteristicWaveHeight(), waves_per_tile, texels_per_wave);

            if (waves_per_tile < 2.0f) {
                ImGui::TextColored(
                    kWarn,
                    "The waves do not fit in the tile, so only the short ones survive and the sea\n"
                    "reads as noise. Raise the patch above %.0f, or shorten the waves.",
                    wavelength * 3.0f);
            }
            else if (texels_per_wave < 6.0f) {
                ImGui::TextColored(
                    kWarn,
                    "Waves are smaller than the grid can draw - a few texels each, which reads as\n"
                    "sparkle rather than water. Lower the patch below %.0f, or lengthen the waves.",
                    wavelength * static_cast<float>(kOceanResolution) / 6.0f);
            }
        }
        ImGui::SliderFloat("Choppiness", &ocean_choppiness, 0.f, 4.f, "%.2f");
        ImGui::TextDisabled("Sharpens crests and broadens troughs; 0 leaves plain round swells.");
        ImGui::SliderFloat("Upwind energy", &ocean_upwind_energy, 0.f, 1.f, "%.2f");

        ImGui::SeparatorText("Swell");
        ImGui::SliderFloat("Swell height", &swell_height, 0.f, 200.f, "%.0f");
        ImGui::SliderFloat("Swell length", &swell_length, 200.f, 12000.f, "%.0f");
        ImGui::SliderFloat("Swell spread", &swell_spread_deg, 0.f, 60.f, "%.0f");
        ImGui::TextDisabled("How far the set fans out either side of the wind; 0 is a single front.");
        ImGui::SliderFloat("Swell steepness", &swell_steepness, 0.f, 1.f, "%.2f");
        {

            const auto k = 6.2831853f / std::max(swell_length, 1.f);
            const auto period = 6.2831853f / std::sqrt(9.81f * kWorldUnitsPerMetre * k);
            ImGui::TextDisabled(
                "%.1f s between crests at full wind, travelling %.0f units/s.",
                period, swell_length / std::max(period, 0.01f));
        }
        ImGui::SliderFloat("Time scale", &ocean_time_scale, 0.f, 4.f, "%.2f");
        ImGui::SliderFloat("Detail overlay", &ocean_detail_weight, 0.f, 1.f, "%.2f");
        ImGui::TextDisabled("A second sample at an unrelated scale, to hide the tile repeat.");

        ImGui::SeparatorText("Depth and colour");
        ImGui::SliderFloat("Water strength", &water_strength, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat("Clarity depth", &water_clarity_depth, 10.f, 1200.f, "%.0f");
        ImGui::TextDisabled(
            "How deep the water has to be before it hides the bottom. Drives both the fade to\n"
            "the deep colour and the fade to opaque, so the shallows stay see-through.");
        ImGui::ColorEdit3("Deep water", reinterpret_cast<float*>(&water_deep_colour));
        ImGui::ColorEdit3("Shallow water", reinterpret_cast<float*>(&water_shallow_colour));
        ImGui::SliderFloat("Sun glint tightness", &water_specular_power, 4.f, 900.f, "%.0f");
        ImGui::SliderFloat("Crest scatter", &water_scatter, 0.f, 1.f, "%.2f");

        ImGui::SeparatorText("Foam");
        ImGui::SliderFloat("Foam", &water_foam_intensity, 0.f, 2.f, "%.2f");
        ImGui::SliderFloat("Surf depth", &water_foam_depth, 1.f, 300.f, "%.0f");
        ImGui::TextDisabled(
            "How shallow counts as shore. Whitecaps further out come from the simulation's own\n"
            "fold detection, so they land where the surface is actually breaking.");
        ImGui::ColorEdit3("Foam colour", reinterpret_cast<float*>(&water_foam_colour));

        ImGui::SeparatorText("Quality");
        ImGui::SliderFloat("Wave march steps", &water_relief_steps, 0.f, 24.f, "%.0f");
        ImGui::Checkbox("Ownership view", &water_ownership_view);
        ImGui::TextDisabled("Green = this pass owns the pixel, magenta = the scene's, blue = no water.");
    }

    struct Section {
        const char* label;
        void (*draw)();
        bool default_open;
    };

    constexpr Section kSections[] = {
        {"Sun & atmosphere", DrawSunSettings, true},
        {"Stars", DrawStarSettings, false},
        {"Aurora", DrawAuroraSettings, false},
        {"Rainbow", DrawRainbowSettings, false},
        {"Moon", DrawMoonSettings, false},
        {"Clouds", DrawCloudSettings, false},
        {"God rays", DrawGodRaySettings, false},
        {"World lighting", DrawWorldLightingSettings, false},
        {"Shadows", DrawShadowSettings, false},
        {"Shadow replay (diagnostics)", DrawShadowReplaySettings, false},
        {"Water", DrawWaterSettings, false},
    };
}

void Skybox::DrawSettings()
{
    if (hooks_failed) {
        ImGui::TextColored(kWarn, "Atmosphere unavailable; check the Toolbox log. Disable and re-enable Weather after resolving the error.");
    }
    if (!GameWorldCompositor::IsActive()) {
        ImGui::TextColored(
            kWarn, GameWorldCompositor::HasFailed()
                       ? "In-world compositor FAILED to install - nothing will be drawn."
                       : "In-world compositor: not installed yet.");
        if (GameWorldCompositor::HasFailed()) {
            ImGui::TextWrapped(
                "Unload standalone Rebirth before using Toolbox's shared in-world renderer.");
        }
    }
    else {
        if (compass_circle[2] > 0.f) {
            ImGui::TextDisabled(
                "Compass exclusion: centre %.3f,%.3f radius %.3f", compass_circle[0],
                compass_circle[1], compass_circle[2]);
        }
        else {
            ImGui::TextDisabled("Compass exclusion: compass not found (excluding nothing).");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "UV of the compass terrain disc our fullscreen passes skip, because the compass "
                "renders its own 3D through the same command stream and would otherwise be "
                "painted over. Radius is normalised by viewport width.");
        }
    }
    if (resources_failed) {
        ImGui::TextColored(kWarn, "Sky GPU resources failed to initialise.");
    }

    for (const auto& [label, draw, default_open] : kSections) {
        if (!ImGui::CollapsingHeader(label, default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) continue;
        ImGui::PushID(label);
        ImGui::Indent();
        draw();
        ImGui::Unindent();
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::Combo("Ray debug view", &debug_mode, "Off\0Ray direction\0");

}

void Skybox::UploadWorldLighting(
    IDirect3DDevice9* const device, const unsigned int tint_register)
{
    if (!device) return;
    const auto tint = IsEnabled() && world_ambient_enabled
        ? CurrentAmbientTintWithFlash()
        : DirectX::XMFLOAT3{1.f, 1.f, 1.f};
    const float tint_constant[4] = {tint.x, tint.y, tint.z, 1.f};
    device->SetPixelShaderConstantF(tint_register, tint_constant, 1);

    WorldLightSample lights[kMaxWorldLights]{};
    auto light_count = 0;
    if (IsEnabled() && world_light_strength > 0.0f) {
        if (const auto* const camera = GW::CameraMgr::GetCamera()) {
            const DirectX::XMFLOAT3 camera_position = {
                camera->position.x, camera->position.y, camera->position.z};
            light_count = GatherWorldLights(camera_position, lights);
        }
    }
    UploadWorldLightConstants(device, lights, light_count);
}

void Skybox::SyncMasterSwitch(IDirect3DDevice9* const device)
{
    static bool was_enabled = true;
    if (const auto enabled = IsEnabled(); enabled != was_enabled) {
        was_enabled = enabled;

        phase_frame_mark = 0;
        Log::Log("[Weather] %s", enabled ? "enabled" : "disabled - stock game");
    }

    SyncGwFogState(device);
}

void Skybox::InvalidateDeviceResources()
{
    ReleaseCapturedTerrainDraws();
    ReleaseShadowPreviewResources();

    ReleaseOceanResources();
    ocean_resources_failed = false;
    shadow_preview_failed = false;

    shadow_map_valid = false;
    shadow_native_light_wvp_ready = false;
    shadow_camera_wvp_ready = false;
}

void Skybox::SignalTerminate()
{
    hooks_ready = false;
    SyncGwFogState(GW::Render::GetDevice());
    if (compositor_token) {
        GameWorldCompositor::UnregisterDraw(compositor_token);
        compositor_token = 0;
    }
    if (shadow_compositor_token) {
        GameWorldCompositor::UnregisterPreWorldDraw(shadow_compositor_token);
        shadow_compositor_token = 0;
    }
    for (const auto hook : engine_hooks) GW::Hook::RemoveHook(hook);
    engine_hooks.clear();
    for (const auto hook : device_hooks) GW::Hook::RemoveHook(hook);
    device_hooks.clear();
    SetRenderState_Ret = nullptr;
}

void Skybox::Terminate()
{
    SignalTerminate();
    if (sky_vs) { sky_vs->Release(); sky_vs = nullptr; }
    if (composite_ps) { composite_ps->Release(); composite_ps = nullptr; }
    if (world_ambient_ps_object) {
        world_ambient_ps_object->Release();
        world_ambient_ps_object = nullptr;
    }
    if (world_water_ps_object) {
        world_water_ps_object->Release();
        world_water_ps_object = nullptr;
    }
    if (world_fog_ps_object) {
        world_fog_ps_object->Release();
        world_fog_ps_object = nullptr;
    }
    if (sky_decl) { sky_decl->Release(); sky_decl = nullptr; }
    InvalidateDeviceResources();
    resources_failed = false;
}
