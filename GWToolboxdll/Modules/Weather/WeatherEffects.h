#pragma once

#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Modules/Weather/Effects/Effect.h"

struct IDirect3DDevice9;

namespace WeatherEffects {
    void Initialize();
    void SignalTerminate();
    void Terminate();
    void InvalidateDeviceResources();

    std::vector<Effect>& Effects();
    std::vector<Effect>& ControlledEffects();
    void ValidateSettings();

    void SetIntensity(int index, float intensity);
    bool SetIntensityByName(const std::string& name, float intensity);
    [[nodiscard]] float TargetIntensity(int index);

    [[nodiscard]] float LiveIntensity(int index);

    void ClearAll();

    [[nodiscard]] float AmbientStrength();
    [[nodiscard]] uint32_t AmbientTint();

    struct LightningFrame {
        float flash = 0.f;
        float bolt = 0.f;
        float world_flash = 0.f;
        float flash_reach = 0.26f;
        float seed = 0.f;
        DirectX::XMFLOAT3 direction_world = {1.f, 0.f, -0.2f};
    };
    [[nodiscard]] LightningFrame CurrentLightning();

    void Reset();

    void ResetEffectsToDefaults();

    void DrawSettings();
}
