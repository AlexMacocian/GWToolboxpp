#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

using DWORD = uint32_t;
using HRESULT = int32_t;
constexpr HRESULT D3D_OK = 0;
constexpr HRESULT D3DERR_NOTAVAILABLE = -1;
#define SUCCEEDED(value) ((value) >= 0)
#define FAILED(value) ((value) < 0)
#define MAKEFOURCC(a, b, c, d) (uint32_t(a) | uint32_t(b) << 8 | uint32_t(c) << 16 | uint32_t(d) << 24)
constexpr unsigned D3DUSAGE_RENDERTARGET = 1;
constexpr unsigned D3DUSAGE_DEPTHSTENCIL = 2;
constexpr unsigned D3DPOOL_DEFAULT = 0;
constexpr unsigned D3DMULTISAMPLE_NONE = 0;
constexpr bool TRUE = true;
enum D3DFORMAT : uint32_t {
    D3DFMT_UNKNOWN = 0, D3DFMT_A8R8G8B8 = 21, D3DFMT_A16B16G16R16F = 113,
    D3DFMT_R32F = 114, D3DFMT_D24S8 = 75
};
unsigned live_resources = 0;
unsigned errors = 0;
struct Resource {
    unsigned refs = 1;
    Resource() { ++live_resources; }
    ~Resource() { --live_resources; }
    void Release() { if (--refs == 0) delete this; }
    HRESULT GetSurfaceLevel(unsigned level, Resource** surface)
    {
        assert(level == 0);
        ++refs;
        *surface = this;
        return D3D_OK;
    }
};
using IDirect3DTexture9 = Resource;
using IDirect3DSurface9 = Resource;
using IDirect3DVertexShader9 = Resource;
using IDirect3DPixelShader9 = Resource;
namespace Log {
    void Log(const char*, ...) { ++errors; }
    void Error(const char*, ...) { ++errors; }
}
struct IDirect3DDevice9 {
    struct TextureCall { unsigned width; unsigned height; unsigned usage; D3DFORMAT format; };
    std::vector<TextureCall> textures;
    unsigned vertex_shaders = 0, pixel_shaders = 0, fallback_depths = 0;
    bool reject_colour = false, reject_intz = false, reject_pixel_shader = false;

    HRESULT CreateTexture(unsigned width, unsigned height, unsigned levels, unsigned usage,
                          D3DFORMAT format, unsigned pool, Resource** result, void*)
    {
        assert(levels == 1 && pool == D3DPOOL_DEFAULT);
        textures.push_back({width, height, usage, format});
        if ((reject_colour && usage == D3DUSAGE_RENDERTARGET)
            || (reject_intz && usage == D3DUSAGE_DEPTHSTENCIL)) return D3DERR_NOTAVAILABLE;
        *result = new Resource;
        return D3D_OK;
    }
    HRESULT CreateDepthStencilSurface(unsigned, unsigned, D3DFORMAT format,
                                      unsigned msaa, unsigned quality, bool discard, Resource** result, void*)
    {
        assert(format == D3DFMT_D24S8 && msaa == D3DMULTISAMPLE_NONE && quality == 0 && discard);
        ++fallback_depths;
        *result = new Resource;
        return D3D_OK;
    }
    HRESULT CreateVertexShader(const DWORD*, Resource** result)
    {
        ++vertex_shaders;
        *result = new Resource;
        return D3D_OK;
    }
    HRESULT CreatePixelShader(const DWORD*, Resource** result)
    {
        ++pixel_shaders;
        if (reject_pixel_shader) return D3DERR_NOTAVAILABLE;
        *result = new Resource;
        return D3D_OK;
    }
};

constexpr std::array<uint32_t, 5> kShadowMapSizes{256, 512, 1024, 2048, 4096};
int shadow_map_size_index = 4;
uint32_t shadow_map_size = 0, shadow_map_size_active = 0;
bool gpu_preview = false;
bool UseGpuTerrainReplay() { return gpu_preview; }
bool shadow_preview_failed = false, shadow_preview_gpu_mode = false, shadow_replay_shaders_failed = false;
bool shadow_map_valid = true, shadow_native_light_wvp_ready = true, shadow_gpu_light_matrices_ready = true;
D3DFORMAT shadow_preview_format = D3DFMT_UNKNOWN;
uint32_t shadow_preview_depth_precision = 8, shadow_camera_depth_width = 0, shadow_camera_depth_height = 0;
IDirect3DTexture9* shadow_preview_texture = nullptr;
IDirect3DTexture9* shadow_preview_depth_texture = nullptr;
IDirect3DTexture9* shadow_camera_depth_texture = nullptr;
IDirect3DSurface9* shadow_preview_surface = nullptr;
IDirect3DSurface9* shadow_preview_depth = nullptr;
IDirect3DVertexShader9* shadow_terrain_replay_vs_object = nullptr;
IDirect3DPixelShader9* shadow_terrain_replay_ps_object = nullptr;
IDirect3DPixelShader9* shadow_screenspace_ps_object = nullptr;
constexpr DWORD shadow_screenspace_ps = 1, shadow_terrain_replay_vs = 2, shadow_terrain_replay_ps = 3;

REPLAY_FUNCTIONS

int main()
{
    IDirect3DDevice9 device;
    assert(EnsureShadowPreviewResources(&device));
    assert(device.textures.size() == 2);
    assert(device.textures[0].format == D3DFMT_A8R8G8B8);
    assert(device.textures[1].format == static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z')));
    assert(device.textures[0].width == 4096 && device.textures[1].width == 4096);
    assert(device.pixel_shaders == 1 && device.vertex_shaders == 0);
    assert(!shadow_terrain_replay_vs_object && !shadow_terrain_replay_ps_object);
    assert(live_resources == 3 && errors == 0);
    assert(!shadow_map_valid && !shadow_native_light_wvp_ready && !shadow_gpu_light_matrices_ready);
    assert(EnsureShadowPreviewResources(&device) && device.textures.size() == 2);

    gpu_preview = true;
    shadow_map_valid = shadow_native_light_wvp_ready = true;
    assert(EnsureShadowPreviewResources(&device));
    assert(shadow_preview_format == D3DFMT_A16B16G16R16F && shadow_preview_depth_precision == 10);
    assert(device.textures.size() == 4 && device.vertex_shaders == 0 && live_resources == 3);
    assert(!shadow_map_valid && !shadow_native_light_wvp_ready);
    assert(EnsureTerrainReplayShaders(&device));
    assert(device.vertex_shaders == 1 && device.pixel_shaders == 3 && live_resources == 5);
    assert(EnsureTerrainReplayShaders(&device) && device.vertex_shaders == 1);

    gpu_preview = false;
    assert(EnsureShadowPreviewResources(&device));
    assert(shadow_preview_format == D3DFMT_A8R8G8B8 && live_resources == 3);
    assert(!shadow_terrain_replay_vs_object && !shadow_terrain_replay_ps_object);
    const uint64_t native_target_bytes = uint64_t(shadow_map_size_active) * shadow_map_size_active * 8;
    const uint64_t previous_target_bytes = uint64_t(shadow_map_size_active) * shadow_map_size_active * 12;
    assert(previous_target_bytes - native_target_bytes == 64ull * 1024 * 1024);

    device.reject_pixel_shader = true;
    assert(!EnsureTerrainReplayShaders(&device));
    assert(shadow_replay_shaders_failed && live_resources == 3 && errors == 1);
    const auto shader_attempts = device.pixel_shaders;
    assert(!EnsureTerrainReplayShaders(&device) && device.pixel_shaders == shader_attempts && errors == 1);
    device.reject_pixel_shader = false;
    ReleaseShadowPreviewResources();
    assert(live_resources == 0 && !shadow_replay_shaders_failed);

    device.reject_colour = true;
    assert(!EnsureShadowPreviewResources(&device) && live_resources == 0 && errors == 2);
    const auto texture_attempts = device.textures.size();
    assert(!EnsureShadowPreviewResources(&device) && device.textures.size() == texture_attempts && errors == 2);
    shadow_preview_failed = false;
    device.reject_colour = false;
    device.reject_intz = true;
    assert(EnsureShadowPreviewResources(&device) && device.fallback_depths == 1);
    assert(!shadow_preview_depth_texture && shadow_preview_depth);
    ReleaseShadowPreviewResources();
    assert(live_resources == 0);
}
