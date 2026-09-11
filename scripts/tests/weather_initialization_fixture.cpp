#include <cassert>
#include <cstdint>
#include <vector>

bool initialized = false;
bool atmosphere_enabled = false;
int last_wind_tick = 0;
int chat_hook_entry = 0;
unsigned sky_initializations = 0, weather_initializations = 0, commands = 0, resets = 0;
std::vector<int> conditions, climate_profiles;
#define TIMER_INIT() 10
void CmdWeather() {}
void CmdClimate() {}
struct ToolboxIni {};
struct SettingsDoc {
    bool atmosphere = false;
    template<typename T>
    void Get(const char*, const char*, T&) {}
};
struct ToolboxModule {
    void Initialize() {}
    void LoadSettings(SettingsDoc& doc, ToolboxIni*) { atmosphere_enabled = doc.atmosphere; }
};
struct WeatherModule : ToolboxModule {
    void Initialize();
    void LoadSettings(SettingsDoc&, ToolboxIni*);
    static void RegisterSettings(ToolboxModule*) {}
    static void OnSettingsLoaded() {}
    void Reset() { ++resets; }
    const char* Name() const { return "Weather"; }
};
namespace Skybox {
    void Initialize() { assert(initialized); ++sky_initializations; }
}
namespace WeatherEffects {
    void Initialize() { ++weather_initializations; }
    std::vector<int>& Effects() { static std::vector<int> value; return value; }
    void ValidateSettings() {}
}
namespace Wind {
    float Direction() { return 0.f; }
    float BaseSpeed() { return 1.f; }
    float Gustiness() { return 0.f; }
    void SetDirection(float) {}
    void SetBaseSpeed(float) {}
    void SetGustiness(float) {}
}
namespace GW::Chat {
    void CreateCommand(int*, const wchar_t*, void(*)()) { ++commands; }
}

REPLAY_FUNCTIONS

int main()
{
    WeatherModule module;
    module.Initialize();
    assert(initialized && !sky_initializations && weather_initializations == 1 && commands == 2);
    SettingsDoc settings;
    module.LoadSettings(settings, nullptr);
    assert(!sky_initializations && resets == 1);
    settings.atmosphere = true;
    module.LoadSettings(settings, nullptr);
    assert(sky_initializations == 1 && resets == 2);
    initialized = false;
    module.Initialize();
    assert(sky_initializations == 2 && weather_initializations == 2);
}
