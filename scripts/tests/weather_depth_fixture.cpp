#include <cassert>
#include <cstdint>

using DWORD = uint32_t;
constexpr DWORD FALSE = 0;
constexpr DWORD D3DCMP_NEVER = 1;
constexpr DWORD D3DCMP_LESSEQUAL = 4;
constexpr DWORD D3DCMP_ALWAYS = 8;
enum RenderState { D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ZENABLE };

struct IDirect3DDevice9 {
    DWORD depth_write;
    DWORD depth_function;
    DWORD depth_enabled;
    unsigned queries = 0;
    unsigned writes = 0;

    void GetRenderState(RenderState state, DWORD* value)
    {
        ++queries;
        switch (state) {
        case D3DRS_ZWRITEENABLE: *value = depth_write; break;
        case D3DRS_ZFUNC: *value = depth_function; break;
        case D3DRS_ZENABLE: *value = depth_enabled; break;
        }
    }
    void SetRenderState(RenderState state, DWORD value)
    {
        assert(state == D3DRS_ZWRITEENABLE);
        ++writes;
        depth_write = value;
    }
};
struct ShadowDrawState {
    DWORD depth_write = FALSE;
    bool guarded = false;
};
bool shadow_replay_active = false;
bool shadow_skip_non_depth_draws = true;
unsigned shadow_replay_draws_skipped = 0;
unsigned shadow_replay_depth_wipe_blocked = 0;

REPLAY_FUNCTIONS

int main()
{
    for (int active = 0; active < 2; ++active)
    for (int skip = 0; skip < 2; ++skip)
    for (DWORD write = 0; write < 2; ++write)
    for (DWORD function = 1; function <= 8; ++function)
    for (DWORD enabled = 0; enabled < 3; ++enabled) {
        shadow_replay_active = active;
        shadow_skip_non_depth_draws = skip;
        shadow_replay_draws_skipped = shadow_replay_depth_wipe_blocked = 0;
        IDirect3DDevice9 device{write, function, enabled};
        ShadowDrawState state{};
        const auto draw = PrepareShadowDraw(&device, state);
        const auto expected_draw = !(active && skip && !write);
        const auto expected_guard = active && expected_draw && write
            && (function == D3DCMP_NEVER || function == D3DCMP_ALWAYS);
        assert(draw == expected_draw);
        assert(state.guarded == expected_guard);
        assert(device.depth_write == (expected_guard ? 0u : write));
        assert(device.depth_function == function && device.depth_enabled == enabled);
        assert(device.queries == (active ? (write ? 2u : 1u) : 0u));
        assert(shadow_replay_draws_skipped == (expected_draw ? 0u : 1u));
        assert(shadow_replay_depth_wipe_blocked == (expected_guard ? 1u : 0u));
        RestoreReplayDepthWrites(&device, state);
        assert(device.depth_write == write);
        assert(device.writes == (expected_guard ? 2u : 0u));
    }

    shadow_replay_active = true;
    shadow_skip_non_depth_draws = false;
    IDirect3DDevice9 device{1, D3DCMP_ALWAYS, 1};
    ShadowDrawState outer{}, inner{};
    assert(PrepareShadowDraw(&device, outer) && device.depth_write == 0);
    assert(PrepareShadowDraw(&device, inner) && !inner.guarded);
    RestoreReplayDepthWrites(&device, inner);
    assert(device.depth_write == 0);
    RestoreReplayDepthWrites(&device, outer);
    assert(device.depth_write == 1);
}
