#pragma once

#include <functional>
#include <d3d9.h>

namespace GameWorldCompositor {
    using DrawCallback = std::function<void(IDirect3DDevice9* device)>;

    inline constexpr float kZNear = 46.875f;
    inline constexpr float kZFar = 48000.f;

    int RegisterDraw(DrawCallback callback, int priority = 0);
    void UnregisterDraw(int token);

    int RegisterPreWorldDraw(DrawCallback callback);
    void UnregisterPreWorldDraw(int token);

    [[nodiscard]] bool IsActive();
    [[nodiscard]] bool HasFailed();

    bool GetWorldPrograms(const uint32_t** programs, uint32_t* count, uint32_t* renderer);
    bool ProgramsBelongToFrCache(const uint32_t* programs, uint32_t count);
    [[nodiscard]] uint64_t FrameId();

    void BeginFrame();

    bool SetupPipeline(IDirect3DDevice9* device, bool occlude, float max_distance, float fog_factor);

    bool SetWorldViewProj(IDirect3DDevice9* device);
    void SetWorldRenderStates(IDirect3DDevice9* device, bool occlude);
    void SetDistanceFog(IDirect3DDevice9* device, float max_distance, float fog_factor);

    [[nodiscard]] IDirect3DVertexShader9* VertexShader();
    [[nodiscard]] IDirect3DPixelShader9* PixelShader();
    [[nodiscard]] IDirect3DVertexDeclaration9* VertexDeclaration();

#ifdef _DEBUG

    void RequestBufferDump();
#endif

    void Terminate();
}
