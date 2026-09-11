#pragma once

#include <ToolboxModule.h>

class WeatherModule : public ToolboxModule {
    WeatherModule() = default;
    ~WeatherModule() override = default;

public:
    static WeatherModule& Instance()
    {
        static WeatherModule instance;
        return instance;
    }

    [[nodiscard]] const char* Name() const override { return "Weather"; }
    [[nodiscard]] const char* Description() const override
    {
        return "Composable weather, shared wind and optional atmospheric sky and lighting, drawn beneath the game UI.";
    }
    [[nodiscard]] const char* Icon() const override { return ICON_FA_TINT; }

    void Initialize() override;
    void Draw(IDirect3DDevice9* device) override;
    void SignalTerminate() override;
    void Terminate() override;
    void LoadSettings(SettingsDoc& doc, ToolboxIni* legacy) override;
    void SaveSettings(SettingsDoc& doc) override;
    void DrawSettingsInternal() override;

    void Reset();
    static bool IsAtmosphereEnabled();
    static void InvalidateDeviceResources();

private:
    static void RegisterSettings(ToolboxModule* module);
    static void OnSettingsLoaded();
    static void DrawSettings();

};
