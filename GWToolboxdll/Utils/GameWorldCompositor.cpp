#include "stdafx.h"

#include <DirectXMath.h>

#include <GWCA/Constants/Constants.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/GameEntities/Camera.h>
#include <GWCA/Managers/CameraMgr.h>
#include <GWCA/Managers/RenderMgr.h>
#include <GWCA/Managers/WorldRenderMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/GameContainers/Array.h>
#include <GWCA/Utilities/Hooker.h>

#include <GWToolbox.h>
#include <Timer.h>
#include <Utils/GameWorldCompositor.h>

#include "Widgets/Minimap/Shaders/game_world_renderer_vs.h"
#include "Widgets/Minimap/Shaders/game_world_renderer_dotted_ps.h"

namespace {

    enum FrCacheEntryType : uint32_t {
        FRCACHE_GPU_RENDER = 0,
        FRCACHE_FRAME_CALLBACK = 1,
        FRCACHE_CLIENT_VIEWPORT = 2,
        FRCACHE_FRAME_VIEWPORT = 3,
    };
    using FrCacheBufferEntry = GW::Render::RenderBufferEntry;
    using FrCacheRenderFn = decltype(GW::Render::FrameRenderBindings::FrCacheRenderAll);
    FrCacheRenderFn FrCacheRenderAll_Func = nullptr;
    FrCacheRenderFn FrCacheRenderAll_Ret = nullptr;
    GW::Array<FrCacheBufferEntry>* render_buffer = nullptr;
    GW::Array<void*>* frame_array = nullptr;
    GW::Array<uint32_t>* render_programs = nullptr;
    const uint32_t* active_world_programs = nullptr;
    uint32_t active_world_program_count = 0;
    uint32_t active_world_renderer = 0;
    bool compositor_scanned = false;
    bool compositor_failed = false;
    bool compositor_hooked = false;
    bool drawn_this_frame = false;
    uint64_t frame_id = 0;
#ifdef _DEBUG
    int dump_calls_remaining = 0;
#endif

    struct DrawRegistration {
        int token;
        int priority;
        GameWorldCompositor::DrawCallback callback;
    };
    std::vector<DrawRegistration> callbacks;
    std::vector<std::pair<int, GameWorldCompositor::DrawCallback>> pre_world_callbacks;
    int next_token = 1;

    IDirect3DVertexShader9* vshader = nullptr;
    IDirect3DPixelShader9* pshader = nullptr;
    IDirect3DVertexDeclaration9* vertex_declaration = nullptr;
    bool need_configure_pipeline = true;

    void RunCallbacks(IDirect3DDevice9* device)
    {
        const auto profiling = GWToolbox::IsProfilingEnabled();
        for (auto& entry : callbacks) {
            if (!entry.callback) continue;
            const auto cb_timer = profiling ? TIMER_INIT() : 0;
            entry.callback(device);
            if (profiling) {
                const auto cb_ms = TIMER_DIFF(cb_timer);
                if (cb_ms > 60) Log::Log("[hitch] compositor callback token=%d took %ld ms", entry.token, static_cast<long>(cb_ms));
            }
        }
    }

    void RunPreWorldCallbacks(IDirect3DDevice9* device)
    {
        for (auto& entry : pre_world_callbacks) {
            if (entry.second) entry.second(device);
        }
    }

    void SetActiveWorldPrograms(
        const uint32_t first_gpu_index, const uint32_t world_program_count, const uint32_t renderer)
    {
        active_world_programs = nullptr;
        active_world_program_count = 0;
        active_world_renderer = renderer;
        if (!render_programs || first_gpu_index > render_programs->size()
            || world_program_count > render_programs->size() - first_gpu_index) {
            return;
        }
        active_world_programs = render_programs->begin() + first_gpu_index;
        active_world_program_count = world_program_count;
    }

    bool ScanCompositor()
    {
        if (compositor_scanned) return !compositor_failed;
        compositor_scanned = true;

        const auto bindings = GW::Render::GetFrameRenderBindings();
        if (!bindings) {
            compositor_failed = true;
            Log::Error("In-world renderer: FrCache context was not found; overlays are disabled.");
            return false;
        }
        const auto context = bindings->frame_context;
        frame_array = &context->frame_array;
        render_programs = &context->render_programs;
        render_buffer = &context->render_buffer;
        FrCacheRenderAll_Func = bindings->FrCacheRenderAll;
        return true;
    }

    void __cdecl OnFrCacheRenderAll(uint32_t render_target, float delta_time)
    {
        GW::Hook::EnterHook();
        IDirect3DDevice9* device = GW::Render::GetDevice();

#ifdef _DEBUG
        if (dump_calls_remaining > 0 && render_buffer) {
            dump_calls_remaining--;
            auto& buf = *render_buffer;
            Log::Log("[frcache] call size=%u drawn_this_frame=%d", buf.size(), drawn_this_frame ? 1 : 0);
            for (uint32_t i = 0; i < buf.size(); i++) {
                const auto& e = buf[i];
                const auto f = (e.type != FRCACHE_GPU_RENDER && frame_array && e.index < frame_array->size())
                    ? static_cast<const GW::UI::Frame*>((*frame_array)[e.index]) : nullptr;
                Log::Log("[frcache]   i=%u type=%u index=%u param=0x%x frame_id=%d visible=%d",
                         i, (uint32_t)e.type, e.index, e.param, f ? (int)f->frame_id : -1, f && f->IsVisible() ? 1 : 0);
            }
        }
#endif

        if ((callbacks.empty() && pre_world_callbacks.empty()) || !render_buffer || !device) {
            FrCacheRenderAll_Ret(render_target, delta_time);
            GW::Hook::LeaveHook();
            return;
        }

        if (!GW::Map::GetIsMapLoaded()
            || GW::Map::GetInstanceType() == GW::Constants::InstanceType::Loading) {
            FrCacheRenderAll_Ret(render_target, delta_time);
            GW::Hook::LeaveHook();
            return;
        }

        auto& buffer = *render_buffer;

        uint32_t boundary = 0;
        uint32_t first_gpu_index = 0;
        uint32_t world_program_count = 0;
        bool stream_restart = false;
        uint32_t stream_expect = 0;
        for (uint32_t i = 0; i < buffer.size(); i++) {
            const auto& e = buffer[i];
            if (e.type != FRCACHE_GPU_RENDER) continue;
            if (boundary == 0) {
                boundary = i + 1;
                first_gpu_index = e.index;
            }
            else if (e.index != stream_expect) {
                stream_restart = true;
                break;
            }
            stream_expect = e.index + e.param;
            world_program_count = stream_expect - first_gpu_index;
        }

        if (boundary == 0 || boundary >= buffer.size() || drawn_this_frame) {
            FrCacheRenderAll_Ret(render_target, delta_time);
            GW::Hook::LeaveHook();
            return;
        }

        SetActiveWorldPrograms(first_gpu_index, world_program_count, 0);
        RunPreWorldCallbacks(device);
        active_world_programs = nullptr;
        active_world_program_count = 0;

        if (stream_restart) {
            FrCacheRenderAll_Ret(render_target, delta_time);
            drawn_this_frame = true;
            GW::Hook::LeaveHook();
            return;
        }

        auto* const orig_buffer = buffer.m_buffer;
        const auto orig_size = buffer.m_size;

        buffer.m_buffer = orig_buffer;
        buffer.m_size = boundary;
        FrCacheRenderAll_Ret(render_target, delta_time);

        buffer.m_buffer = orig_buffer;
        buffer.m_size = orig_size;
        GW::Render::FlushCommandQueue();
        SetActiveWorldPrograms(first_gpu_index, world_program_count, 0);
        RunCallbacks(device);
        active_world_programs = nullptr;
        active_world_program_count = 0;
        drawn_this_frame = true;

        GW::Render::FlushCommandQueue();
        buffer.m_buffer = orig_buffer + boundary;
        buffer.m_size = orig_size - boundary;
        FrCacheRenderAll_Ret(render_target, delta_time);

        buffer.m_buffer = orig_buffer;
        buffer.m_size = orig_size;

        GW::Hook::LeaveHook();
    }

    void EnsureHook()
    {
        if (compositor_hooked || compositor_failed) return;
        if (!ScanCompositor()) return;

        if (const int result = GW::Hook::CreateHook(
                reinterpret_cast<void**>(&FrCacheRenderAll_Func), OnFrCacheRenderAll,
                reinterpret_cast<void**>(&FrCacheRenderAll_Ret));
            result != 0) {
            Log::Log(
                "[compositor] failed to hook FrCache_RenderAll (MinHook status %d). "
                "Unload standalone Rebirth before enabling Toolbox's in-world renderer.",
                result);
            compositor_failed = true;
            return;
        }
        GW::Hook::EnableHooks(FrCacheRenderAll_Func);
        compositor_hooked = true;
    }

    void RemoveHook()
    {
        if (compositor_hooked && FrCacheRenderAll_Func) {
            GW::Hook::RemoveHook(FrCacheRenderAll_Func);
            compositor_hooked = false;
        }
        compositor_failed = false;
        compositor_scanned = false;
    }

    bool ConfigureProgrammablePipeline(IDirect3DDevice9* device)
    {
        constexpr D3DVERTEXELEMENT9 decl[] = {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
            D3DDECL_END()};
        if (device->CreateVertexDeclaration(decl, &vertex_declaration) != D3D_OK) {
            return false;
        }
        if (device->CreateVertexShader(reinterpret_cast<const DWORD*>(&game_world_renderer_vs), &vshader) != D3D_OK) {
            return false;
        }
        if (device->CreatePixelShader(reinterpret_cast<const DWORD*>(&game_world_renderer_dotted_ps), &pshader) != D3D_OK) {
            return false;
        }
        need_configure_pipeline = false;
        return true;
    }

    bool SetWorldTransform(IDirect3DDevice9* device)
    {

        constexpr auto vertex_shader_view_matrix_offset = 0u;
        constexpr auto vertex_shader_proj_matrix_offset = 4u;

        const auto cam = GW::CameraMgr::GetCamera();
        if (!cam) {
            return false;
        }

        DirectX::XMFLOAT4X4A mat_view{};
        const DirectX::XMFLOAT3 eye_pos = {cam->position.x, cam->position.y, cam->position.z};
        const DirectX::XMFLOAT3 player_pos = {cam->look_at_target.x, cam->look_at_target.y, cam->look_at_target.z};
        constexpr DirectX::XMFLOAT3 up = {0.0f, 0.0f, -1.0f};
        XMStoreFloat4x4A(
            &mat_view,
            XMMatrixTranspose(
                DirectX::XMMatrixLookAtLH(XMLoadFloat3(&eye_pos), XMLoadFloat3(&player_pos), XMLoadFloat3(&up))
            )
        );
        if (device->SetVertexShaderConstantF(vertex_shader_view_matrix_offset, reinterpret_cast<const float*>(&mat_view), 4) != D3D_OK) {
            return false;
        }

        DirectX::XMFLOAT4X4A mat_proj{};
        const auto fov = GW::Render::GetFieldOfView();
        const auto aspect_ratio = static_cast<float>(GW::Render::GetViewportWidth()) / static_cast<float>(GW::Render::GetViewportHeight());
        XMStoreFloat4x4A(
            &mat_proj,
            XMMatrixTranspose(
                DirectX::XMMatrixPerspectiveFovLH(fov, aspect_ratio, GameWorldCompositor::kZNear, GameWorldCompositor::kZFar)
            )
        );
        if (device->SetVertexShaderConstantF(vertex_shader_proj_matrix_offset, reinterpret_cast<const float*>(&mat_proj), 4) != D3D_OK) {
            return false;
        }
        return true;
    }
}

int GameWorldCompositor::RegisterDraw(DrawCallback callback, const int priority)
{
    if (!callback) {
        return 0;
    }

    const int token = next_token++;
    callbacks.push_back({token, priority, std::move(callback)});
    std::stable_sort(callbacks.begin(), callbacks.end(), [](const auto& a, const auto& b) {
        return a.priority < b.priority;
    });
    return token;
}

void GameWorldCompositor::UnregisterDraw(const int token)
{
    if (token <= 0) {
        return;
    }
    std::erase_if(callbacks, [token](const auto& entry) { return entry.token == token; });
    if (callbacks.empty() && pre_world_callbacks.empty()) {
        RemoveHook();
    }
}

int GameWorldCompositor::RegisterPreWorldDraw(DrawCallback callback)
{
    if (!callback) return 0;
    const int token = next_token++;
    pre_world_callbacks.emplace_back(token, std::move(callback));
    return token;
}

void GameWorldCompositor::UnregisterPreWorldDraw(const int token)
{
    if (token <= 0) return;
    std::erase_if(pre_world_callbacks, [token](const auto& entry) { return entry.first == token; });
    if (callbacks.empty() && pre_world_callbacks.empty()) {
        RemoveHook();
    }
}

#ifdef _DEBUG
void GameWorldCompositor::RequestBufferDump()
{
    dump_calls_remaining = 50;
}
#endif

bool GameWorldCompositor::IsActive()
{
    return compositor_hooked && !compositor_failed;
}

bool GameWorldCompositor::HasFailed()
{
    return compositor_failed;
}

bool GameWorldCompositor::GetWorldPrograms(
    const uint32_t** programs, uint32_t* count, uint32_t* renderer)
{
    if (!programs || !count || !renderer || !active_world_programs || active_world_program_count == 0) {
        return false;
    }
    *programs = active_world_programs;
    *count = active_world_program_count;
    *renderer = active_world_renderer;
    return true;
}

bool GameWorldCompositor::ProgramsBelongToFrCache(
    const uint32_t* programs, const uint32_t count)
{
    if (!programs || !render_programs || !render_programs->begin()) return false;
    const auto first = reinterpret_cast<uintptr_t>(render_programs->begin());
    const auto last = first + render_programs->size() * sizeof(uint32_t);
    const auto candidate = reinterpret_cast<uintptr_t>(programs);
    return candidate >= first && candidate <= last
        && count <= (last - candidate) / sizeof(uint32_t);
}

uint64_t GameWorldCompositor::FrameId()
{
    return frame_id;
}

void GameWorldCompositor::BeginFrame()
{

    if (!callbacks.empty() || !pre_world_callbacks.empty()) {
        EnsureHook();
    }
    drawn_this_frame = false;
    ++frame_id;
}

bool GameWorldCompositor::SetWorldViewProj(IDirect3DDevice9* device)
{
    return device && SetWorldTransform(device);
}

void GameWorldCompositor::SetWorldRenderStates(IDirect3DDevice9* device, const bool occlude)
{
    if (!device) {
        return;
    }

    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,
                           D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    device->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, TRUE);

    if (occlude) {

        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    }
    else {
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    }
}

void GameWorldCompositor::SetDistanceFog(IDirect3DDevice9* device, const float max_distance, const float fog_factor)
{
    if (!device) {
        return;
    }
    const GW::Camera* cam = GW::CameraMgr::GetCamera();

    const float cur_pos_constant[4] = {cam ? cam->look_at_target.x : 0.f, cam ? cam->look_at_target.y : 0.f, cam ? cam->look_at_target.z : 0.f, 0.0f};
    device->SetPixelShaderConstantF(0, cur_pos_constant, 1);
    const float max_dist_constant[4] = {max_distance, 0.0f, 0.0f, 0.0f};
    device->SetPixelShaderConstantF(1, max_dist_constant, 1);
    const float fog_starts_at_constant[4] = {max_distance - max_distance * fog_factor, 0.0f, 0.0f, 0.0f};
    device->SetPixelShaderConstantF(2, fog_starts_at_constant, 1);
}

bool GameWorldCompositor::SetupPipeline(IDirect3DDevice9* device, const bool occlude, const float max_distance, const float fog_factor)
{
    if (!device) {
        return false;
    }
    if (need_configure_pipeline && !ConfigureProgrammablePipeline(device)) {
        return false;
    }
    if (device->SetVertexShader(vshader) != D3D_OK
        || device->SetPixelShader(pshader) != D3D_OK
        || device->SetVertexDeclaration(vertex_declaration) != D3D_OK
        || !SetWorldTransform(device)) {
        return false;
    }
    SetWorldRenderStates(device, occlude);
    SetDistanceFog(device, max_distance, fog_factor);
    return true;
}

IDirect3DVertexShader9* GameWorldCompositor::VertexShader() { return vshader; }
IDirect3DPixelShader9* GameWorldCompositor::PixelShader() { return pshader; }
IDirect3DVertexDeclaration9* GameWorldCompositor::VertexDeclaration() { return vertex_declaration; }

void GameWorldCompositor::Terminate()
{
    RemoveHook();
    callbacks.clear();
    pre_world_callbacks.clear();
    active_world_programs = nullptr;
    active_world_program_count = 0;
    compositor_scanned = compositor_failed = drawn_this_frame = false;
    if (vshader) {
        vshader->Release();
        vshader = nullptr;
    }
    if (pshader) {
        pshader->Release();
        pshader = nullptr;
    }
    if (vertex_declaration) {
        vertex_declaration->Release();
        vertex_declaration = nullptr;
    }
    need_configure_pipeline = true;
}
