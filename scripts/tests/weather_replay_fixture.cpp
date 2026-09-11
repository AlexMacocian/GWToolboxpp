#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace GW {
    struct Vec3f {
        float x, y, z;
        bool operator==(const Vec3f&) const = default;
    };
    struct MapContext {
        void* props = nullptr;
        GW::Vec3f view_eye{};
        GW::Vec3f view_target{};
        GW::Vec3f view_up{};
        uint32_t view_flags = 0;
        uintptr_t h00E8 = 0;
        uint32_t flags = 0;
        void* h010C = nullptr;
        uint32_t h0110 = 0;
        uint32_t h0114 = 0;
    };
    MapContext* current_map = nullptr;
    MapContext* GetMapContext() { return current_map; }
}

namespace Log {
    unsigned errors = 0;
    void Log(const char*) { ++errors; }
}

struct SceneProgramList {
    void* data;
    uint32_t capacity;
    uint32_t size;
};
enum class Phase { WorldUpdateView, PropViewUpdate };
struct ScopedPhase {
    explicit ScopedPhase(Phase) {}
};

bool shadow_explicit_camera_render_active = true;
bool shadow_skip_view_update = true;
bool shadow_props_follow_light = true;
bool shadow_scene_rebuild_active = true;
bool shadow_cell_visibility_override_active = true;
bool shadow_cast_agent_shadows = true;
unsigned shadow_prop_updates_run = 0;
unsigned shadow_view_updates_skipped = 0;
uint32_t shadow_primary_program_count = 0;
float shadow_scene_eye[3]{}, shadow_scene_target[3]{}, shadow_scene_up[3]{};
std::vector<std::string> events;
std::vector<GW::Vec3f> prop_eyes;
std::vector<GW::Vec3f> prop_targets;
int camera_scope = 0, scene_scope = 0, primary_scope = 0;
float environment_lod = 0.75f;

auto GetPropLodScale_Func = +[](void* environment) {
    assert(environment == &environment_lod);
    return environment_lod;
};
auto GmPropsUpdateView_Func = +[](void* manager, float* eye, float* target, float lod) {
    assert(manager == GW::current_map->props && lod == environment_lod);
    events.emplace_back("props");
    prop_eyes.push_back({eye[0], eye[1], eye[2]});
    prop_targets.push_back({target[0], target[1], target[2]});
};
auto GmWorldUpdateView_Func = +[](float delta, float* eye, float* target, float* up, float* direction) {
    assert(delta == 0.0f);
    events.emplace_back("native-view");
    auto& map = *GW::current_map;
    map.view_eye = {eye[0], eye[1], eye[2]};
    map.view_target = {target[0], target[1], target[2]};
    map.view_up = {up[0], up[1], up[2]};
    map.view_flags |= 1;
    direction[0] = target[0] - eye[0];
    direction[1] = target[1] - eye[1];
    direction[2] = target[2] - eye[2];
};
auto CameraRenderScope_Func = +[](int enabled) {
    camera_scope += enabled ? 1 : -1;
    events.emplace_back(enabled ? "camera+" : "camera-");
};
auto SceneRenderScope_Func = +[](int enabled) {
    scene_scope += enabled ? 1 : -1;
    events.emplace_back(enabled ? "scene+" : "scene-");
};
auto GrSetCameraTransform_Func = +[](float x, float y, float z, float dx, float dy, float dz) {
    assert(x == 100 && y == 200 && z == 300);
    assert(std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz));
    assert(camera_scope == 1 && scene_scope == 1);
    events.emplace_back("camera");
};
auto GmWorldSetPrimarySceneBuildEnabled_Func = +[](int enabled) {
    primary_scope += enabled ? 1 : -1;
    events.emplace_back(enabled ? "primary+" : "primary-");
};
auto GmWorldBuildPrimaryScenePrograms_Func = +[](float delta, void* output, int* first, int* count) {
    assert(delta == 0 && primary_scope == 1);
    assert((GW::current_map->flags & 1) == 0);
    events.emplace_back("agents");
    auto& programs = *static_cast<SceneProgramList*>(output);
    assert(programs.size == 0);
    *first = 0;
    *count = 3;
    programs.size += 3;
};
auto GmWorldBuildRemainingScenePrograms_Func = +[](float delta, void* output, void* secondary,
                                                int* has_sky, void* clear_colour) {
    assert(delta == 0 && !secondary && primary_scope == 0);
    assert(camera_scope == 1 && scene_scope == 1);
    assert((GW::current_map->flags & 1) == 0);
    events.emplace_back("remaining");
    static_cast<SceneProgramList*>(output)->size += 5;
    *has_sky = 0;
    *static_cast<uint32_t*>(clear_colour) = 0xff223344;
};
auto GrRenderSceneLists_Ret = +[](void* output, void* secondary, int has_sky, void* clear_colour, int render_target) {
    assert(!secondary && !has_sky && !render_target);
    assert(*static_cast<uint32_t*>(clear_colour) == 0xff223344);
    assert(camera_scope == 0 && scene_scope == 0);
    assert(GW::current_map->flags == 7);
    assert(static_cast<SceneProgramList*>(output)->size == (shadow_cast_agent_shadows ? 8u : 5u));
    events.emplace_back("render");
};

REPLAY_FUNCTIONS

int main()
{
    GW::MapContext map{};
    map.props = &map;
    map.h00E8 = reinterpret_cast<uintptr_t>(&environment_lod);
    map.view_eye = {1, 2, -3};
    map.view_target = {4, 5, -6};
    map.view_up = {0, 0, -1};
    map.view_flags = 6;
    map.flags = 7;
    const auto original = map;
    GW::current_map = &map;
    float eye[3] = {100, 200, 300}, target[3] = {110, 220, 330}, up[3] = {0, 0, 1};

    assert(RenderShadowScene(eye, target, up));
    assert((events == std::vector<std::string>{
        "props", "camera+", "scene+", "camera", "primary+", "agents",
        "primary-", "remaining", "camera-", "scene-", "render", "props"}));
    assert((prop_eyes[0] == GW::Vec3f{110, 220, -330}));
    assert((prop_targets[0] == GW::Vec3f{120, 240, -360}));
    assert(prop_eyes[1] == original.view_eye && prop_targets[1] == original.view_target);
    assert(map.view_eye == original.view_eye && map.view_target == original.view_target);
    assert(map.view_up == original.view_up && map.view_flags == 7 && map.flags == original.flags);
    assert(map.h0114 == 0 && !shadow_cell_visibility_override_active);
    assert(shadow_primary_program_count == 3 && shadow_prop_updates_run == 2);
    assert(shadow_view_updates_skipped == 2 && camera_scope == 0 && scene_scope == 0);

    events.clear();
    shadow_cast_agent_shadows = false;
    assert(RenderShadowScene(eye, target, up));
    assert((events == std::vector<std::string>{
        "props", "camera+", "scene+", "camera", "remaining", "camera-", "scene-", "render", "props"}));

    events.clear();
    shadow_skip_view_update = false;
    assert(RenderShadowScene(eye, target, up));
    assert(events.front() == "native-view" && events.back() == "native-view");
    assert(map.view_eye == original.view_eye && map.view_target == original.view_target);
    assert(map.view_up == original.view_up && map.h0114 == 0);

    events.clear();
    map.h0114 = 1;
    assert(!RenderShadowScene(eye, target, up) && events.empty());
    assert(map.h0114 == 1 && Log::errors == 1);
    GW::current_map = nullptr;
    assert(!RenderShadowScene(eye, target, up) && events.empty());
    assert(Log::errors == 2 && camera_scope == 0 && scene_scope == 0 && primary_scope == 0);
}
