#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

using DWORD = uint32_t;
using Fn = void(*)();
using SetRenderTarget_pt = Fn;
using SetDepthStencilSurface_pt = Fn;
using SetRenderState_pt = Fn;
using SetViewport_pt = Fn;
using DrawPrimitive_pt = Fn;
using DrawIndexedPrimitive_pt = Fn;
constexpr int D3DRS_FOGENABLE = 1, D3DRS_FOGCOLOR = 2;
struct IDirect3DDevice9 {
    uintptr_t* vtable;
    unsigned reads = 0;
    void GetRenderState(int state, DWORD* result)
    {
        ++reads;
        *result = state == D3DRS_FOGENABLE ? 1 : 0x123456;
    }
};
Fn SetRenderTarget_Func = nullptr, SetRenderTarget_Ret = nullptr;
Fn SetDepthStencilSurface_Func = nullptr, SetDepthStencilSurface_Ret = nullptr;
Fn SetRenderState_Func = nullptr, SetRenderState_Ret = nullptr;
Fn SetViewport_Func = nullptr, SetViewport_Ret = nullptr;
Fn DrawPrimitive_Func = nullptr, DrawPrimitive_Ret = nullptr;
Fn DrawIndexedPrimitive_Func = nullptr, DrawIndexedPrimitive_Ret = nullptr;
void OnSetRenderTarget() {}
void OnSetDepthStencilSurface() {}
void OnSetRenderState() {}
void OnSetViewport() {}
void OnDrawPrimitive() {}
void OnDrawIndexedPrimitive() {}
bool hooks_ready = true, hooks_failed = false, replay_needed = false;
DWORD gw_requested_fog_enable = 0, gw_requested_fog_colour = 0;
unsigned attempts = 0, fail_at = 0;
std::vector<void*> device_hooks, enabled, removed;
std::vector<std::string> events;
bool ShadowMapPassEnabled() { return replay_needed; }
bool CreateOwnedHook(Fn& target, Fn, Fn& original, std::vector<void*>& hooks, const char*)
{
    if (++attempts == fail_at) return false;
    original = target;
    hooks.push_back(reinterpret_cast<void*>(target));
    return true;
}
namespace GW::Hook {
    void EnableHooks(void* hook) { enabled.push_back(hook); }
    void RemoveHook(void* hook) { removed.push_back(hook); events.emplace_back("remove"); }
}
void SyncGwFogState(IDirect3DDevice9*)
{
    assert(!hooks_ready && SetRenderState_Ret);
    events.emplace_back("restore");
}

REPLAY_FUNCTIONS

int main()
{
    std::array<uintptr_t, 119> vtable{};
    for (size_t i = 0; i < vtable.size(); ++i) vtable[i] = 0x1000 + i * 16;
    IDirect3DDevice9 device{vtable.data()};
    assert(EnsureD3DReplayHooks(&device));
    assert(device_hooks.size() == 2 && enabled.size() == 2 && attempts == 2 && device.reads == 2);
    assert(device_hooks[0] == reinterpret_cast<void*>(vtable[57]));
    assert(device_hooks[1] == reinterpret_cast<void*>(vtable[82]));
    assert(gw_requested_fog_enable == 1 && gw_requested_fog_colour == 0x123456);
    assert(EnsureD3DReplayHooks(&device) && attempts == 2);
    replay_needed = true;
    assert(EnsureD3DReplayHooks(&device));
    assert(device_hooks.size() == 6 && enabled.size() == 6 && attempts == 6 && device.reads == 2);
    assert(EnsureD3DReplayHooks(&device) && attempts == 6);
    replay_needed = false;
    assert(EnsureD3DReplayHooks(&device) && device_hooks.size() == 6);

    device_hooks.clear();
    enabled.clear();
    attempts = 0;
    assert(EnsureD3DReplayHooks(&device) && device_hooks.size() == 2);
    replay_needed = true;
    fail_at = attempts + 3;
    assert(!EnsureD3DReplayHooks(&device));
    assert(!hooks_ready && hooks_failed && device_hooks.empty());
    assert(!SetRenderState_Ret && !DrawIndexedPrimitive_Ret);
    assert(events.front() == "restore" && removed.size() == 4);
    assert(std::find(removed.begin(), removed.end(), reinterpret_cast<void*>(vtable[47])) == removed.end());
    const auto failed_attempts = attempts;
    assert(!EnsureD3DReplayHooks(&device) && attempts == failed_attempts);

    hooks_ready = true;
    hooks_failed = false;
    attempts = fail_at = 0;
    enabled.clear();
    assert(EnsureD3DReplayHooks(&device));
    assert(device_hooks.size() == 6 && enabled.size() == 6 && attempts == 6);
}
