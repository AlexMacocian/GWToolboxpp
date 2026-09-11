#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct IDirect3DDevice9 {};
enum FrCacheEntryType : uint32_t {
    FRCACHE_GPU_RENDER,
    FRCACHE_FRAME_CALLBACK,
    FRCACHE_CLIENT_VIEWPORT,
    FRCACHE_FRAME_VIEWPORT,
};
struct FrCacheBufferEntry {
    FrCacheEntryType type;
    uint32_t index;
    uint32_t param;
};
template <typename T>
struct Array {
    T* m_buffer;
    uint32_t m_size;
    uint32_t size() const { return m_size; }
    T* begin() const { return m_buffer; }
    T& operator[](uint32_t index) const { return m_buffer[index]; }
};

std::vector<std::string> events;
IDirect3DDevice9 device;
bool map_loaded = true;
bool loading = false;
bool drawn_this_frame = false;
int in_hook = 0;
int remove_hook_count = 0;
int next_token = 1;
uint32_t world_programs[64]{};
Array<uint32_t> programs{world_programs, 64};
Array<uint32_t>* render_programs = &programs;
Array<FrCacheBufferEntry>* render_buffer = nullptr;
const uint32_t* active_world_programs = nullptr;
uint32_t active_world_program_count = 0, active_world_renderer = 0;

namespace GW {
    namespace Constants {
        enum class InstanceType { Loading, Outpost };
    }
    namespace Map {
        bool GetIsMapLoaded() { return map_loaded; }
        Constants::InstanceType GetInstanceType() {
            return loading ? Constants::InstanceType::Loading : Constants::InstanceType::Outpost;
        }
    }
    namespace Render {
        IDirect3DDevice9* GetDevice() { return &device; }
        void FlushCommandQueue() { events.emplace_back("flush"); }
    }
    namespace Hook {
        void EnterHook() { ++in_hook; }
        void LeaveHook() { --in_hook; }
    }
}
namespace GWToolbox {
    bool IsProfilingEnabled() { return false; }
}
namespace Log {
    template <typename... Args>
    void Log(const char*, Args...) {}
}
#define TIMER_INIT() 0
#define TIMER_DIFF(value) (value)

namespace GameWorldCompositor {
    using DrawCallback = std::function<void(IDirect3DDevice9*)>;
    int RegisterDraw(DrawCallback, int);
    void UnregisterDraw(int);
    int RegisterPreWorldDraw(DrawCallback);
    void UnregisterPreWorldDraw(int);
}
struct DrawRegistration {
    int token;
    int priority;
    GameWorldCompositor::DrawCallback callback;
};
std::vector<DrawRegistration> callbacks;
std::vector<std::pair<int, GameWorldCompositor::DrawCallback>> pre_world_callbacks;
void RemoveHook() { ++remove_hook_count; }
constexpr uint32_t expected_render_target = 0x1234abcd;
constexpr float expected_delta_time = 0.03125f;
auto FrCacheRenderAll_Ret = +[](uint32_t render_target, float delta_time) {
    assert(render_target == expected_render_target);
    assert(delta_time == expected_delta_time);
    if (render_buffer->size() == 1) {
        events.emplace_back((*render_buffer)[0].type == FRCACHE_GPU_RENDER ? "world" : "hud");
    }
    else {
        events.emplace_back("whole");
    }
};

REPLAY_FUNCTIONS

int main()
{
    const auto overlay1 = GameWorldCompositor::RegisterDraw(
        [](auto*) { events.emplace_back("overlay1"); }, 0);
    const auto overlay2 = GameWorldCompositor::RegisterDraw(
        [](auto*) { events.emplace_back("overlay2"); }, 0);
    const auto sky = GameWorldCompositor::RegisterDraw(
        [](auto*) {
            assert(active_world_programs == world_programs + 3 && active_world_program_count == 5);
            events.emplace_back("sky");
        }, -100);
    const auto weather = GameWorldCompositor::RegisterDraw(
        [](auto*) { events.emplace_back("weather"); }, -50);
    const auto pre = GameWorldCompositor::RegisterPreWorldDraw(
        [](auto*) { events.emplace_back("pre"); });
    assert(GameWorldCompositor::RegisterDraw(nullptr, 0) == 0);
    assert(GameWorldCompositor::RegisterPreWorldDraw(nullptr) == 0);
    FrCacheBufferEntry entries[] = {
        {FRCACHE_GPU_RENDER, 3, 5}, {FRCACHE_FRAME_CALLBACK, 0, 0}};
    Array<FrCacheBufferEntry> buffer{entries, 2};
    render_buffer = &buffer;

    OnFrCacheRenderAll(expected_render_target, expected_delta_time);
    assert((events == std::vector<std::string>{
        "pre", "world", "flush", "sky", "weather", "overlay1", "overlay2", "flush", "hud"}));
    assert(buffer.m_buffer == entries && buffer.m_size == 2 && in_hook == 0);
    assert(!active_world_programs && !active_world_program_count);
    events.clear();
    OnFrCacheRenderAll(expected_render_target, expected_delta_time);
    assert((events == std::vector<std::string>{"whole"}));

    FrCacheBufferEntry restart[] = {
        {FRCACHE_GPU_RENDER, 0, 2}, {FRCACHE_FRAME_CALLBACK, 0, 0}, {FRCACHE_GPU_RENDER, 0, 2}};
    buffer = {restart, 3};
    drawn_this_frame = false;
    events.clear();
    OnFrCacheRenderAll(expected_render_target, expected_delta_time);
    assert((events == std::vector<std::string>{"pre", "whole"}));
    assert(buffer.m_buffer == restart && buffer.m_size == 3 && in_hook == 0);

    buffer = {entries, 2};
    for (int mode = 0; mode < 2; ++mode) {
        drawn_this_frame = false;
        events.clear();
        map_loaded = mode != 0;
        loading = mode == 1;
        OnFrCacheRenderAll(expected_render_target, expected_delta_time);
        assert((events == std::vector<std::string>{"whole"}));
        assert(buffer.m_buffer == entries && buffer.m_size == 2 && in_hook == 0);
    }
    map_loaded = true;
    loading = false;
    GameWorldCompositor::UnregisterDraw(overlay1);
    GameWorldCompositor::UnregisterDraw(overlay2);
    GameWorldCompositor::UnregisterDraw(sky);
    GameWorldCompositor::UnregisterDraw(weather);
    assert(remove_hook_count == 0);
    GameWorldCompositor::UnregisterPreWorldDraw(pre);
    assert(remove_hook_count == 1 && callbacks.empty() && pre_world_callbacks.empty());
    events.clear();
    OnFrCacheRenderAll(expected_render_target, expected_delta_time);
    assert((events == std::vector<std::string>{"whole"}));
    assert(in_hook == 0);
}
