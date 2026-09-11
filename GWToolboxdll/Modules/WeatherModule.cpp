#include "stdafx.h"

#include <atomic>

#include <GWCA/Constants/Constants.h>
#include <GWCA/Constants/Maps.h>
#include <GWCA/GameEntities/Map.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/MapMgr.h>

#include <Color.h>
#include <Defines.h>
#include <GWToolbox.h>
#include <ImGuiAddons.h>
#include <Logger.h>
#include <Modules/AudioSettings.h>
#include <Modules/Resources.h>
#include <Modules/WeatherModule.h>
#include <Modules/Weather/Skybox.h>
#include <Modules/Weather/WeatherEffects.h>
#include <Modules/Weather/Wind.h>
#include <Timer.h>
#include <Utils/ArenaNetFileParser.h>
#include <Utils/GameWorldCompositor.h>
#include <Utils/GuiUtils.h>
#include <Utils/SettingsDoc.h>
#include <Utils/SettingsRegistry.h>
#include <Utils/TextUtils.h>
#include <Utils/ToolboxUtils.h>

namespace weather_module {
    constexpr int kTypeRain = 0;
    constexpr int kTypeSnow = 1;
    constexpr int kTypeCount = 2;

    constexpr int kDecalNone = 0;
    constexpr int kDecalSplash = 1;
    constexpr int kDecalSettle = 2;
    constexpr int kDecalCount = 3;
    constexpr int kDecalAuto = -1;
    constexpr float kDriftAuto = -1.f;

    struct CloudCover {
        float base = 0.f;
        float top = 0.f;
        unsigned int tint = 0x80808080u;
        int density = 20;
        float size = 600.f;
        float speed = 0.f;
        float radius = 2500.f;
    };

    struct WeatherCondition {
        std::string name = "Rain";
        int type = kTypeRain;
        bool active = false;
        int density = 30;
        float drop_size = 8.f;
        float fall_speed = 2000.f;
        float spread_radius = 2500.f;
        float wind_dir_min = 0.f;
        float wind_dir_max = 0.f;
        float wind_tilt = 0.f;
        float splash_chance = 1.f;
        std::vector<uint32_t> sounds;
        float sound_min_interval = 8.f;
        float sound_max_interval = 25.f;
        bool sound_3d = false;
        float ambient = 0.f;
        unsigned int tint = 0xFFFFFFFFu;
        unsigned int overcast_tint = 0xFFA09078u;
        int floor_decal = kDecalAuto;
        float drift = kDriftAuto;
        bool wind_camera_relative = false;
        bool center_on_camera = false;
        float column_height = 2500.f;
        CloudCover cloud;
        bool enabled = true;
    };

    enum class Climate {
        Temperate,
        Arid,
        Tropical,
        Mountainous,
        Volcanic,
        Desertous,
        None
    };

    struct ClimateWeather {
        std::string condition;
        float weight = 0.3f;
    };
    struct ClimateProfile {
        Climate climate = Climate::Temperate;
        std::vector<ClimateWeather> entries;
    };
}

namespace {
    using namespace weather_module;

    constexpr auto kMaxRadius = GW::Constants::Range::Spirit;
    constexpr auto particle_area_full = 500.f;
    constexpr auto max_particles = 30000;
    constexpr auto column_height_max = kMaxRadius;
    constexpr auto snow_sway_amp = 120.f;
    constexpr auto kConditionWindReference = 150.f;
    std::atomic_bool initialized = false;
    bool atmosphere_enabled = false;
    clock_t last_wind_tick = 0;
    uint32_t rng = 0x1234567u;
    GW::HookEntry chat_hook_entry;

    float frand(const float lo, const float hi)
    {
        rng = rng * 1664525u + 1013904223u;
        return lo + static_cast<float>(rng >> 8) / static_cast<float>(0xFFFFFFu) * (hi - lo);
    }

    constexpr int EffectiveDecal(const WeatherCondition& c)
    {
        return c.floor_decal != kDecalAuto ? c.floor_decal : (c.type == kTypeSnow ? kDecalSettle : kDecalSplash);
    }

    float EffectiveDrift(const WeatherCondition& c)
    {
        return c.drift >= 0.f ? c.drift : (c.type == kTypeSnow ? snow_sway_amp : 0.f);
    }

    float ColumnHeight(const WeatherCondition& c)
    {
        return std::clamp(c.column_height, 1.f, column_height_max);
    }

    int DropCount(const WeatherCondition& c)
    {
        if (c.density <= 0) return 0;
        const float disk = 3.14159265f * c.spread_radius * c.spread_radius;
        const float per_particle = particle_area_full * 100.f / static_cast<float>(std::clamp(c.density, 1, 100));

        const float height_fraction = ColumnHeight(c) / c.spread_radius;
        return std::min(max_particles, static_cast<int>(disk / per_particle * height_fraction));
    }

    int CloudCount(const WeatherCondition& c)
    {
        if (c.cloud.top <= c.cloud.base || c.cloud.density <= 0) return 0;
        const float disk = 3.14159265f * c.cloud.radius * c.cloud.radius;
        const float per_particle = particle_area_full * 100.f / static_cast<float>(std::clamp(c.cloud.density, 1, 100));
        return std::min(max_particles, static_cast<int>(disk / per_particle));
    }

    std::vector<WeatherCondition> DefaultConditions()
    {

        std::vector<WeatherCondition> v = {
            {"Heavy Rain", kTypeRain, false, 70, 10.f, 500.f, 2500.f, 0.f, 25.f, 0.f, 0.30f, {0x20ed0, 0x20ed1}, 6.f, 60.f, false, 1.0f},
            {"Light Rain", kTypeRain, false, 5, 10.f, 500.f, 1500.f, 0.f, 25.f, 10.f, 1.0f, {0x20ed0, 0x20ed1}, 6.f, 60.f, false, 0.40f},
            {"Snow", kTypeSnow, false, 20, 8.f, 100.f, 1500.f, 30.f, 55.f, 10.f, 0.15f, {}, 10.f, 30.f, false, 0.40f, 0xFFFFFFFFu, 0xFF98E4FFu},

            {"Ashfall", kTypeSnow, false, 10, 9.f, 350.f, 2500.f, 30.f, 55.f, 8.f, 0.f, {}, 12.f, 35.f, false, 0.45f, 0xFF42464Au, 0xFFA09078u, kDecalNone},

            {"Fog", kTypeRain, false, 2, 10.f, 300.f, 1500.f, 0.f, 25.f, 0.f, 1.0f, {}, 10.f, 30.f, false, 0.0f, 0xFFFFFFFFu, 0xFFFFFFFFu},
            {"Sandstorm", kTypeRain, false, 10, 4.f, 500.f, 1000.f, 90.f, 90.f, 85.f, 0.f, {}, 10.f, 30.f, false, 0.0f, 0xFFC8B080u, 0xFFA09078u, kDecalNone},

            {"Blizzard", kTypeSnow, false, 85, 6.f, 700.f, 1500.f, 35.f, 60.f, 39.f, 0.f, {}, 10.f, 30.f, false, 0.55f, 0xFFFFFFFFu, 0xFFA09078u, kDecalAuto, 23.f},
        };
        v[1].column_height = 1500.f;
        v[2].column_height = 1500.f;
        v[4].column_height = 1500.f;
        v[5].column_height = 60.f;
        v[6].column_height = 1000.f;
        v[0].cloud = {1000.f, 1500.f, 0xB0303840u, 25, 700.f, 60.f};
        v[1].cloud = {1000.f, 1500.f, 0x70404850u, 15, 700.f, 20.f};
        v[2].cloud = {1000.f, 1500.f, 0xFDC6C6C6u, 20, 500.f, 20.f};
        v[4].cloud = {-500.f, 100.f, 0x11C8C8D0u, 10, 800.f, 10.f, 1500.f};
        v[5].cloud = {-400.f, 60.f, 0x38C8B080u, 10, 500.f, 700.f, 600.f};
        v[6].cloud = {0.f, 1000.f, 0x2ED0D8E0u, 3, 800.f, 0.f, 1500.f};
        v[4].enabled = false;
        v[5].enabled = false;
        return v;
    }
    std::vector<WeatherCondition> conditions = DefaultConditions();

    std::vector<ClimateProfile> DefaultClimateProfiles()
    {
        const auto p = [](const Climate c, std::vector<ClimateWeather> e) { return ClimateProfile{c, std::move(e)}; };
        return {
            p(Climate::Temperate, {{"Light Rain", 0.01f}, {"Heavy Rain", 0.005f}, {"Fog", 0.002f}}),
            p(Climate::Tropical, {{"Heavy Rain", 0.02f}, {"Light Rain", 0.01f}, {"Fog", 0.005f}}),
            p(Climate::Arid, {{"Light Rain", 0.01f}}),
            p(Climate::Desertous, {{"Light Rain", 0.005f}, {"Sandstorm", 0.015f}}),
            p(Climate::Mountainous, {{"Snow", 0.04f}, {"Blizzard", 0.015f}}),
            p(Climate::Volcanic, {{"Ashfall", 0.04f}, {"Fog", 0.005f}}),
        };
    }
    std::vector<ClimateProfile> climate_profiles = DefaultClimateProfiles();

    bool auto_weather = false;
    float auto_change_min = 2.f;
    float auto_change_max = 5.f;
    Climate auto_climate = Climate::Temperate;
    float auto_timer = -1.f;
    Climate auto_climate_override = Climate::None;

    constexpr struct {
        Climate climate;
        const char* name;
    } kClimates[] = {
        {Climate::Temperate, "Temperate"}, {Climate::Arid, "Arid"}, {Climate::Desertous, "Desertous"}, {Climate::Tropical, "Tropical"}, {Climate::Mountainous, "Mountainous"}, {Climate::Volcanic, "Volcanic"},
    };

    const char* ClimateName(const Climate climate)
    {
        for (const auto& k : kClimates)
            if (k.climate == climate) return k.name;
        return "(climate)";
    }

    bool ClimateByName(const std::string& name, Climate& out)
    {
        const std::string want = TextUtils::ToLower(name);
        for (const auto& k : kClimates)
            if (TextUtils::ToLower(k.name) == want) { out = k.climate; return true; }
        return false;
    }

    Climate ClimateForMap(const GW::Constants::MapID map_id)
    {

        static const std::unordered_map<GW::Constants::MapID, Climate> overrides = {
            {GW::Constants::MapID::Dry_Top, Climate::Arid},
            {GW::Constants::MapID::Ettins_Back, Climate::Arid},
            {GW::Constants::MapID::Ventaris_Refuge_outpost, Climate::Arid},
            {GW::Constants::MapID::Druids_Overlook_outpost, Climate::Arid},
            {GW::Constants::MapID::Sage_Lands, Climate::Arid},
            {GW::Constants::MapID::The_Deep, Climate::None},
            {GW::Constants::MapID::Urgozs_Warren, Climate::None}
        };
        if (const auto it = overrides.find(map_id); it != overrides.end()) return it->second;
        const auto info = GW::Map::GetMapInfo(map_id);
        if (!info || (!GW::Map::HasMapDisplayInfo(info) && !info->GetIsOnWorldMap()))
            return Climate::None;

        if (info && info->type == GW::RegionType::Dungeon)
            return Climate::None;
        switch (info ? info->region : GW::Region_DevRegion) {
            case GW::Region_NorthernShiverpeaks:
            case GW::Region_FarShiverpeaks:
                return Climate::Mountainous;
            case GW::Region_DepthsOfTyria:
                return Climate::None;
            case GW::Region_CrystalDesert:
            case GW::Region_Desolation:
            case GW::Region_Istan:
                return Climate::Desertous;
            case GW::Region_Kourna:
            case GW::Region_Vaabi:
                return Climate::Arid;
            case GW::Region_FissureOfWoe:
                return Climate::Volcanic;
            case GW::Region_Maguuma:
            case GW::Region_Kurzick:
            case GW::Region_TarnishedCoast:
                return Climate::Tropical;
            case GW::Region_DomainOfAnguish:
                return Climate::None;
        }
        return Climate::Temperate;
    }

    bool IsValidSound(const uint32_t id)
    {
        static std::map<uint32_t, bool> cache;
        if (!id) return false;
        if (const auto it = cache.find(id); it != cache.end()) return it->second;
        ArenaNetFileParser::GameAssetFile f;
        f.readFromDat(id);
        const char* ft = f.fileType();
        const bool ok = f.file_id == id && ft && static_cast<unsigned char>(ft[0]) == 0xFF && (static_cast<unsigned char>(ft[1]) & 0xE6) == 0xE2;
        cache[id] = ok;
        return ok;
    }

    void RerollAutoWeather(const Climate climate)
    {
        const ClimateProfile* prof = nullptr;
        for (const auto& cp : climate_profiles)
            if (cp.climate == climate) { prof = &cp; break; }

        int chosen = -1;
        if (prof) {
            float sum = 0.f;
            for (const auto& e : prof->entries) sum += std::max(0.f, e.weight);
            float r = frand(0.f, std::max(sum, 1.f));
            for (const auto& e : prof->entries) {
                const float w = std::max(0.f, e.weight);
                if (w <= 0.f) continue;
                if (r < w) {
                    for (int i = 0; i < static_cast<int>(conditions.size()); i++)
                        if (conditions[i].name == e.condition && conditions[i].enabled) { chosen = i; break; }
                    break;
                }
                r -= w;
            }
        }
        for (int i = 0; i < static_cast<int>(conditions.size()); i++)
            conditions[i].active = i == chosen;
    }

    void UpdateAutoWeather(const float dt)
    {
        if (!auto_weather) return;
        if (!GW::Map::GetCurrentMapInfo()) return;

        const Climate climate = auto_climate_override != Climate::None ? auto_climate_override : ClimateForMap(GW::Map::GetMapID());
        if (climate != auto_climate || auto_timer < 0.f) {
            auto_climate = climate;
            RerollAutoWeather(climate);
            auto_timer = frand(auto_change_min, auto_change_max);
            return;
        }
        if ((auto_timer -= dt / 60.f) <= 0.f) {
            RerollAutoWeather(climate);
            auto_timer = frand(auto_change_min, auto_change_max);
        }
    }

    void EnableAutoWeatherFollowMap()
    {
        auto_climate_override = Climate::None;
        auto_weather = true;
        auto_timer = -1.f;
        Log::Info("Automatic weather on (by map): %s", ClimateName(ClimateForMap(GW::Map::GetMapID())));
    }

    void StopWeather()
    {
        auto_weather = false;
        for (auto& c : conditions)
            c.active = false;
        WeatherEffects::ClearAll();
        Log::Info("Weather off");
    }

    void ClearWeather()
    {
        for (auto& c : conditions)
            c.active = false;
        WeatherEffects::ClearAll();
        Log::Info("Weather cleared");
    }

    void CHAT_CMD_FUNC(CmdWeather)
    {
        if (argc < 2) {
            Log::Info("Weather conditions:");
            for (const auto& c : conditions)
                Log::Info("  %s: %s", c.name.c_str(), c.active ? "on" : "off");
            Log::Info("Usage: /weather <condition> [on|off|toggle] | /weather [auto|off|clear]");
            return;
        }
        if (argc == 2) {
            const std::string only = TextUtils::ToLower(TextUtils::WStringToString(argv[1]));
            if (only == "auto") return EnableAutoWeatherFollowMap();
            if (only == "off") return StopWeather();
            if (only == "clear") return ClearWeather();
        }

        const std::string last = TextUtils::ToLower(TextUtils::WStringToString(argv[argc - 1]));
        int state = -1;
        if (last == "on" || last == "1")
            state = 1;
        else if (last == "off" || last == "0")
            state = 0;
        else if (last == "toggle")
            state = 2;
        const int name_end = state < 0 ? argc : argc - 1;
        std::string name;
        for (int i = 1; i < name_end; i++) {
            if (i > 1) name += ' ';
            name += TextUtils::WStringToString(argv[i]);
        }
        if (name.empty()) return Log::Error("Usage: /weather <condition> [on|off|toggle]");
        if (state < 0) state = 2;

        const std::string want = TextUtils::ToLower(name);
        for (size_t i = 0; i < conditions.size(); i++) {
            if (TextUtils::ToLower(conditions[i].name) != want) continue;
            const bool on = state == 2 ? !conditions[i].active : state == 1;
            if (on && !conditions[i].enabled) return Log::Error("%s is disabled; enable it in the weather settings first.", conditions[i].name.c_str());

            for (auto& o : conditions)
                o.active = false;
            conditions[i].active = on;
            Log::Info("%s: %s", conditions[i].name.c_str(), on ? "on" : "off");
            return;
        }
        Log::Error("No weather condition named '%s'", name.c_str());
    }

    void CHAT_CMD_FUNC(CmdClimate)
    {
        if (argc < 2) {
            const Climate effective = auto_climate_override != Climate::None ? auto_climate_override : ClimateForMap(GW::Map::GetMapID());
            Log::Info("Automatic weather: %s", !auto_weather ? "off" : auto_climate_override != Climate::None ? "on (forced climate)" : "on (by map)");
            Log::Info("Current climate: %s", ClimateName(effective));
            std::string names;
            for (const auto& k : kClimates) names += (names.empty() ? "" : ", ") + std::string(k.name);
            Log::Info("Usage: /climate [auto|off|<climate>] - climates: %s", names.c_str());
            return;
        }

        std::string arg;
        for (int i = 1; i < argc; i++) {
            if (i > 1) arg += ' ';
            arg += TextUtils::WStringToString(argv[i]);
        }
        const std::string key = TextUtils::ToLower(arg);

        if (key == "off") return StopWeather();
        if (key == "auto") return EnableAutoWeatherFollowMap();

        Climate climate;
        if (!ClimateByName(arg, climate)) {
            Log::Error("Unknown climate '%s'. Use 'auto', 'off', or a climate name.", arg.c_str());
            return;
        }
        auto_climate_override = climate;
        auto_weather = true;
        auto_timer = -1.f;
        Log::Info("Climate forced to %s", ClimateName(climate));
    }
}

void WeatherModule::RegisterSettings(ToolboxModule* module)
{
    SettingsRegistry::RegisterField(module, "atmosphere_enabled", &atmosphere_enabled);
    SettingsRegistry::RegisterField(module, "auto_weather", &auto_weather);
    SettingsRegistry::RegisterField(module, "auto_change_min", &auto_change_min);
    SettingsRegistry::RegisterField(module, "auto_change_max", &auto_change_max);
}

void WeatherModule::OnSettingsLoaded()
{
    bool seen_active = false;
    for (auto& c : conditions) {
        if (!c.enabled) c.active = false;
        if (c.active && std::exchange(seen_active, true)) c.active = false;
        c.type = std::clamp(c.type, 0, kTypeCount - 1);
        c.floor_decal = std::clamp(c.floor_decal, kDecalAuto, kDecalCount - 1);
        c.drift = std::max(c.drift, kDriftAuto);
        c.wind_dir_max = std::max(c.wind_dir_max, c.wind_dir_min);
        c.wind_tilt = std::clamp(c.wind_tilt, 0.f, 90.f);
        c.density = std::clamp(c.density, 0, 100);
        c.spread_radius = std::clamp(c.spread_radius, 250.f, kMaxRadius);
        c.column_height = std::clamp(c.column_height, 1.f, column_height_max);
        c.drop_size = std::clamp(c.drop_size, 1.f, 500.f);
        c.fall_speed = std::clamp(c.fall_speed, 0.f, 30000.f);
        c.splash_chance = std::clamp(c.splash_chance, 0.f, 1.f);
        c.sound_min_interval = std::max(c.sound_min_interval, 0.f);
        c.sound_max_interval = std::max(c.sound_max_interval, c.sound_min_interval);
        c.ambient = std::clamp(c.ambient, 0.f, 1.f);
        c.cloud.radius = std::clamp(c.cloud.radius, 250.f, kMaxRadius);
        c.cloud.density = std::clamp(c.cloud.density, 0, 100);
        c.cloud.size = std::clamp(c.cloud.size, 1.f, 3000.f);
        c.cloud.speed = std::clamp(c.cloud.speed, 0.f, 5000.f);
        c.cloud.base = std::clamp(c.cloud.base, -2000.f, 2500.f);
        c.cloud.top = std::clamp(c.cloud.top, -2000.f, 2500.f);
    }
    auto_change_min = std::max(auto_change_min, 0.1f);
    auto_change_max = std::max(auto_change_max, auto_change_min);
    for (auto& cp : climate_profiles)
        for (auto& e : cp.entries)
            e.weight = std::clamp(e.weight, 0.f, 1.f);
}

void WeatherModule::LoadSettings(SettingsDoc& doc, ToolboxIni* legacy)
{
    ToolboxModule::LoadSettings(doc, legacy);
    doc.Get(Name(), "conditions", conditions);
    doc.Get(Name(), "climate_profiles", climate_profiles);
    doc.Get(Name(), "effects", WeatherEffects::Effects());
    auto wind_direction = Wind::Direction();
    auto wind_speed = Wind::BaseSpeed();
    auto wind_gustiness = Wind::Gustiness();
    doc.Get(Name(), "wind_direction", wind_direction);
    doc.Get(Name(), "wind_speed", wind_speed);
    doc.Get(Name(), "wind_gustiness", wind_gustiness);
    Wind::SetDirection(wind_direction);
    Wind::SetBaseSpeed(wind_speed);
    Wind::SetGustiness(wind_gustiness);
    OnSettingsLoaded();
    WeatherEffects::ValidateSettings();
    if (initialized && atmosphere_enabled) Skybox::Initialize();
    Reset();
}

void WeatherModule::SaveSettings(SettingsDoc& doc)
{
    ToolboxModule::SaveSettings(doc);
    doc.Set(Name(), "conditions", conditions);
    doc.Set(Name(), "climate_profiles", climate_profiles);
    doc.Set(Name(), "effects", WeatherEffects::Effects());
    doc.Set(Name(), "wind_direction", Wind::Direction());
    doc.Set(Name(), "wind_speed", Wind::BaseSpeed());
    doc.Set(Name(), "wind_gustiness", Wind::Gustiness());
}

void WeatherModule::DrawSettings()
{
    ImGui::Checkbox("Atmospheric sky and lighting", &atmosphere_enabled);
    ImGui::ShowHelp("Opt in to the replacement sky, day/night lighting, shadows and lightning.\nWeather particles and existing /weather and /climate controls work with this off.");
    Wind::DrawSettings();
    Skybox::DrawSettings();
    WeatherEffects::DrawSettings();
    ImGui::Separator();
    const auto red = ImGui::ColorConvertU32ToFloat4(Colors::Red());
    const auto green = ImGui::ColorConvertU32ToFloat4(Colors::Green());
    if (GameWorldCompositor::HasFailed())
        ImGui::TextColored(red, "  in-world compositor FAILED to install.");
    else if (GameWorldCompositor::IsActive())
        ImGui::TextColored(green, "  in-world compositor active.");
    else
        ImGui::TextDisabled("  in-world compositor: not installed yet.");

    if (const GW::AreaInfo* info = GW::Map::GetCurrentMapInfo())
        ImGui::Text("Current map: %s (climate: %s)", Resources::GetRegionName(info->region)->string().c_str(), ClimateName(ClimateForMap(GW::Map::GetMapID())));
    if (auto_climate_override != Climate::None)
        ImGui::Text("Forced climate: %s (untick it below, or tick 'Automatic weather (follow map climate)')", ClimateName(auto_climate_override));
    std::string showing;
    for (const auto& c : conditions)
        if (c.active) showing += (showing.empty() ? "" : ", ") + c.name;
    ImGui::Text("Showing: %s", showing.empty() ? "Clear" : showing.c_str());

    ImGui::SeparatorText("Weather conditions");
    int to_remove = -1, to_duplicate = -1;
    for (int i = 0; i < static_cast<int>(conditions.size()); i++) {
        auto& c = conditions[i];
        ImGui::PushID(i);
        ImGui::BeginDisabled(auto_weather || !c.enabled);
        if (ImGui::Checkbox("##active", &c.active) && c.active)
            for (int j = 0; j < static_cast<int>(conditions.size()); j++)
                if (j != i) conditions[j].active = false;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!c.enabled) ImGui::SetTooltip("This condition is disabled.\nTick \"Enabled\" in its settings below to allow it to be used.");
            else if (auto_weather) ImGui::SetTooltip("Automatic weather is on and controls which condition is active.\nTurn off \"Automatic weather\" below to switch conditions manually.");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        char header[80];
        snprintf(header, sizeof(header), "%s%s###cond", c.name.empty() ? "(unnamed)" : c.name.c_str(), c.enabled ? "" : " [Disabled]");
        if (ImGui::CollapsingHeader(header)) {
            if (ImGui::Checkbox("Enabled", &c.enabled) && !c.enabled) c.active = false;
            ImGui::ShowHelp("Untick to exclude this condition from manual selection and automatic weather rolls.");
            ImGui::InputText("Name", c.name, 32);
            ImGui::TextDisabled("Falling particles (set Density 0 for a cloud-cover-only condition like fog).");
            const char* type_names[kTypeCount] = {"Rain", "Snow"};
            ImGui::Combo("Type", &c.type, type_names, kTypeCount);
            ImGui::DragInt("Density", &c.density, 1.f, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
            ImGui::ShowHelp("How densely the volume is filled, 0-100%. 0 = no falling particles (cloud-cover-only). The count\nis derived from this and the spread area, so it stays consistent if the radius changes. Higher = heavier on FPS.");
            ImGui::Text("  ~%d particles", DropCount(c));
            ImGui::DragFloat("Range", &c.spread_radius, 25.f, 250.f, kMaxRadius, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::ShowHelp("Radius of the weather volume around the focus (also the horizontal extent of the cloud-cover layer).");
            ImGui::DragFloat("Height", &c.column_height, 25.f, 50.f, column_height_max, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::ShowHelp("Height of the falling-particle column above you.");
            ImGui::DragFloat("Drop size", &c.drop_size, 1.f, 1.f, 500.f, "%.0f");
            ImGui::DragFloat("Fall speed", &c.fall_speed, 50.f, 0.f, 30000.f, "%.0f");
            ImGui::ShowHelp("Particle speed at the reference wind speed of 150 gwinch/s. Wind tilt splits this into vertical fall and shared-wind response.");

            ImGui::DragFloat("Wind tilt", &c.wind_tilt, 1.f, 0.f, 90.f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
            ImGui::ShowHelp("Tilt at the reference wind speed of 150 gwinch/s: 0 = vertical, 90 = sideways.\nThe shared wind direction and gusts also drive the sky and cloud layers.");
            ImGui::Checkbox("Wind relative to camera", &c.wind_camera_relative);
            ImGui::ShowHelp("Measure the wind direction from the camera instead of the world, so the storm keeps the same\non-screen direction as you rotate the camera (e.g. always blowing across the view).");
            ImGui::Checkbox("Centre on camera", &c.center_on_camera);
            ImGui::ShowHelp("Centre the volume on the camera itself instead of its target, so the effect wraps tightly\naround the viewer and fills the near view. Best paired with a small range (e.g. a sandstorm).");
            if (c.type == kTypeSnow) {
                float drift = EffectiveDrift(c);
                if (ImGui::DragFloat("Drift", &drift, 1.f, 0.f, 1000.f, "%.0f", ImGuiSliderFlags_AlwaysClamp)) c.drift = drift;
                ImGui::ShowHelp("How much flakes wander sideways as they float down. 0 = fall straight.");
            }
            const char* decal_names[kDecalCount] = {"None", "Splash", "Settle"};
            int decal = EffectiveDecal(c);
            if (ImGui::Combo("Floor decal", &decal, decal_names, kDecalCount)) c.floor_decal = decal;
            ImGui::ShowHelp("What each impact leaves on the ground: nothing, a water splash, or a settled flake/patch.");
            if (decal != kDecalNone) {
                ImGui::DragFloat("Floor decal chance", &c.splash_chance, 0.01f, 0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::ShowHelp("Chance (0..1) that a drop leaves its floor decal - a splash or a settled patch - when it lands.");
            }
            auto ct = ImGui::ColorConvertU32ToFloat4(c.tint);
            if (ImGui::ColorEdit4("Tint", &ct.x, ImGuiColorEditFlags_AlphaBar)) c.tint = ImGui::ColorConvertFloat4ToU32(ct);
            ImGui::ShowHelp("This condition's particle colour - e.g. a dark grey for ashfall.");
            ImGui::DragFloat("Overcast", &c.ambient, 0.01f, 0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::ShowHelp("How strongly this condition dims the scene toward its overcast tint while active.");
            auto oc = ImGui::ColorConvertU32ToFloat4(c.overcast_tint);
            if (ImGui::ColorEdit3("Overcast tint", &oc.x)) c.overcast_tint = ImGui::ColorConvertFloat4ToU32(oc);
            ImGui::ShowHelp("The colour the scene is dimmed toward while this condition drives the overcast.");

            ImGui::Separator();
            bool cloud_on = c.cloud.top > c.cloud.base;
            if (ImGui::Checkbox("Cloud cover", &cloud_on)) {
                if (cloud_on && c.cloud.top <= c.cloud.base) { c.cloud.base = 0.f; c.cloud.top = 1000.f; }
                else if (!cloud_on) c.cloud.top = c.cloud.base;
            }
            ImGui::ShowHelp("Add a soft cloud/fog layer in a height band above you, ON TOP OF the falling particles above.\nThis is how you combine effects - e.g. tick this on Snow for a blizzard (snow + fog).");
            if (c.cloud.top > c.cloud.base) {
                ImGui::DragFloatRange2("Cloud band (above you)", &c.cloud.base, &c.cloud.top, 10.f, -500.f, 2500.f, "%.0f", "%.0f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::ShowHelp("Bottom and top of the band, gwinch above the player. Rain clouds ~1000-1500, fog ~0-1000, a ground sandstorm ~0-200.");
                ImGui::DragFloat("Cloud radius", &c.cloud.radius, 25.f, 250.f, kMaxRadius, "%.0f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::ShowHelp("Horizontal radius of the cloud-cover bubble, independent of the particle Range above (e.g. wide overhead cloud cover over a small rain volume).");
                ImGui::DragInt("Cloud density", &c.cloud.density, 1.f, 1, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
                ImGui::Text("  ~%d puffs", CloudCount(c));
                ImGui::DragFloat("Cloud puff size", &c.cloud.size, 5.f, 50.f, 3000.f, "%.0f");
                ImGui::DragFloat("Cloud drift speed", &c.cloud.speed, 5.f, 0.f, 5000.f, "%.0f");
                ImGui::ShowHelp("Horizontal drift at the reference wind speed of 150 gwinch/s; direction and gusts follow the shared wind.");
                auto cct = ImGui::ColorConvertU32ToFloat4(c.cloud.tint);
                if (ImGui::ColorEdit4("Cloud tint", &cct.x, ImGuiColorEditFlags_AlphaBar)) c.cloud.tint = ImGui::ColorConvertFloat4ToU32(cct);
                ImGui::ShowHelp("Cloud colour and opacity (alpha) - dark grey rain clouds, white fog, tan sand.");
            }

            ImGui::TextUnformatted("Sounds (file id, played at random while active)");
            int snd_remove = -1;
            for (int s = 0; s < static_cast<int>(c.sounds.size()); s++) {
                ImGui::PushID(s);
                ImGui::SetNextItemWidth(120.f);
                ImGui::InputScalar("##sid", ImGuiDataType_U32, &c.sounds[s], nullptr, nullptr, "%X", ImGuiInputTextFlags_CharsHexadecimal);
                const bool ok = IsValidSound(c.sounds[s]);
                ImGui::SameLine();
                ImGui::TextColored(ok ? green : red, ok ? ICON_FA_CHECK : ICON_FA_TIMES);
                ImGui::SameLine();
                if (ImGui::SmallButton("Test")) AudioSettings::PlaySoundFileId(c.sounds[s]);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove##snd")) snd_remove = s;
                ImGui::PopID();
            }
            if (snd_remove >= 0) c.sounds.erase(c.sounds.begin() + snd_remove);
            if (ImGui::SmallButton("Add sound")) c.sounds.push_back(0);
            ImGui::DragFloat2("Sound interval (s)", &c.sound_min_interval, 0.5f, 0.f, 600.f, "%.0f");
            ImGui::Checkbox("Play sounds in 3D", &c.sound_3d);
            ImGui::ShowHelp("Play each sound from a random nearby position so the game's 3D audio varies its\nvolume and stereo panning - reduces repetition when there's only one sound.\nNo effect if the sound isn't a positional one.");

            if (ImGui::Button("Duplicate")) to_duplicate = i;
            ImGui::SameLine();
            if (ImGui::Button("Remove condition")) to_remove = i;
        }
        ImGui::PopID();
    }
    if (to_remove >= 0) {
        conditions.erase(conditions.begin() + to_remove);
        WeatherEffects::Reset();
    }
    if (to_duplicate >= 0 && to_remove < 0) {
        WeatherCondition copy = conditions[to_duplicate];
        copy.name += " copy";
        copy.active = false;
        conditions.insert(conditions.begin() + to_duplicate + 1, std::move(copy));
    }

    if (ImGui::Button("Add condition")) conditions.push_back({});
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) ImGui::OpenPopup("Reset weather conditions?");
    if (ImGui::BeginPopupModal("Reset weather conditions?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Replace all conditions with the %d defaults?", static_cast<int>(DefaultConditions().size()));
        if (ImGui::Button("Reset")) {
            conditions = DefaultConditions();
            WeatherEffects::Reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SeparatorText("Climates");

    bool follow_map = auto_weather && auto_climate_override == Climate::None;
    if (ImGui::Checkbox("Automatic weather (follow map climate)", &follow_map)) {
        auto_weather = follow_map;
        auto_climate_override = Climate::None;
        if (follow_map) auto_timer = -1.f;
    }
    ImGui::ShowHelp("Roll the active weather automatically from the current map's climate, re-rolling every few minutes.\nWhile on, the manual condition toggles AND the per-climate tickboxes below are disabled.\n\nThe /climate chat command mirrors this: /climate auto (follow map), /climate <name> (force a climate), /climate off.");
    if (auto_weather)
        ImGui::DragFloatRange2("Change interval (min)", &auto_change_min, &auto_change_max, 0.25f, 0.1f, 240.f, "%.1f", "%.1f", ImGuiSliderFlags_AlwaysClamp);

    ImGui::PushID("climates");
    int climate_remove = -1;
    for (int i = 0; i < static_cast<int>(climate_profiles.size()); i++) {
        auto& cp = climate_profiles[i];
        ImGui::PushID(i);

        bool forced = auto_weather && auto_climate_override == cp.climate;
        ImGui::BeginDisabled(follow_map);
        if (ImGui::Checkbox("##climate_active", &forced)) {
            if (forced) { auto_climate_override = cp.climate; auto_weather = true; auto_timer = -1.f; }
            else { auto_climate_override = Climate::None; auto_weather = false; }
        }
        if (follow_map && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Automatic weather is following the map's climate.\nTurn off \"Automatic weather (follow map climate)\" above to force a specific climate.");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::CollapsingHeader(ClimateName(cp.climate))) {
            int ent_remove = -1;
            for (int e = 0; e < static_cast<int>(cp.entries.size()); e++) {
                ImGui::PushID(e);
                ImGui::SetNextItemWidth(160.f);
                if (ImGui::BeginCombo("##cond", cp.entries[e].condition.c_str())) {
                    for (const auto& c : conditions)
                        if (ImGui::Selectable(c.name.c_str(), c.name == cp.entries[e].condition)) cp.entries[e].condition = c.name;
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110.f);

                float weight_pct = cp.entries[e].weight * 100.f;
                if (ImGui::DragFloat("##weight", &weight_pct, 0.1f, 0.f, 100.f, "%.1f%%", ImGuiSliderFlags_AlwaysClamp))
                    cp.entries[e].weight = std::clamp(weight_pct / 100.f, 0.f, 1.f);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Chance this condition is picked for the climate on each weather roll.");
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove##ent")) ent_remove = e;
                ImGui::PopID();
            }
            if (ent_remove >= 0) cp.entries.erase(cp.entries.begin() + ent_remove);
            if (ImGui::SmallButton("Add condition##ent")) cp.entries.push_back({conditions.empty() ? "" : conditions.front().name, 0.3f});
            float sum = 0.f;
            for (const auto& e : cp.entries) sum += std::max(0.f, e.weight);
            ImGui::Text("Clear weather: %.1f%%", std::max(0.f, 1.f - sum) * 100.f);
            if (ImGui::Button("Remove climate")) climate_remove = i;
        }
        ImGui::PopID();
    }
    if (climate_remove >= 0) climate_profiles.erase(climate_profiles.begin() + climate_remove);

    if (ImGui::Button("Add climate")) ImGui::OpenPopup("add_climate");
    if (ImGui::BeginPopup("add_climate")) {
        for (const auto& k : kClimates) {
            if (std::any_of(climate_profiles.begin(), climate_profiles.end(), [&](const ClimateProfile& cp) { return cp.climate == k.climate; })) continue;
            if (ImGui::Selectable(k.name)) climate_profiles.push_back({k.climate, {}});
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset climate table")) ImGui::OpenPopup("Reset climate table?");
    if (ImGui::BeginPopupModal("Reset climate table?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Replace the climate->weather table with the %d defaults?", static_cast<int>(DefaultClimateProfiles().size()));
        if (ImGui::Button("Reset")) {
            climate_profiles = DefaultClimateProfiles();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void WeatherModule::DrawSettingsInternal()
{
    DrawSettings();
}

void WeatherModule::Draw(IDirect3DDevice9* device)
{
    if (!initialized || !device) return;
    if (atmosphere_enabled) Skybox::Initialize();
    if (GWToolbox::ShouldDisableToolbox() || !GW::Map::GetIsMapLoaded()
        || GW::Map::GetInstanceType() == GW::Constants::InstanceType::Loading
        || GW::Map::GetIsInCinematic()) {
        last_wind_tick = TIMER_INIT();
        WeatherEffects::Reset();
        return;
    }
    const auto now = TIMER_INIT();
    const auto dt = static_cast<float>(now - last_wind_tick) * 0.001f;
    Wind::Update(dt);
    last_wind_tick = now;
    UpdateAutoWeather(std::clamp(dt, 0.f, 0.25f));

    auto& effects = WeatherEffects::ControlledEffects();
    if (effects.size() != conditions.size() * 2) {
        effects.resize(conditions.size() * 2);
        WeatherEffects::Reset();
    }
    for (size_t i = 0; i < conditions.size(); ++i) {
        const auto& c = conditions[i];
        auto& precipitation = effects[i * 2];
        precipitation.name = c.name;
        precipitation.kind = WeatherEffects::Kind::Particles;
        precipitation.intensity = c.active && c.enabled ? 1.f : 0.f;
        precipitation.overcast = c.ambient;
        precipitation.overcast_tint = c.overcast_tint;
        if (precipitation.sounds != c.sounds) precipitation.sounds = c.sounds;
        precipitation.sound_min_interval = c.sound_min_interval;
        precipitation.sound_max_interval = c.sound_max_interval;
        precipitation.sound_3d = c.sound_3d;
        auto& p = precipitation.particles;
        p.type = c.type;
        p.density = std::clamp(c.density, 0, 100);
        p.drop_size = std::max(c.drop_size, 1.f);
        const auto tilt = std::clamp(c.wind_tilt, 0.f, 90.f) * 0.0174532925f;
        const auto speed = std::max(c.fall_speed, 0.f);
        p.fall_speed = speed * std::max(std::cos(tilt), 0.f);
        p.wind_response = speed * std::sin(tilt) / kConditionWindReference;
        p.spread_radius = std::clamp(c.spread_radius, 250.f, kMaxRadius);
        p.splash_chance = std::clamp(c.splash_chance, 0.f, 1.f);
        p.tint = c.tint;
        p.floor_decal = EffectiveDecal(c);
        p.drift = EffectiveDrift(c);
        p.column_height = ColumnHeight(c);
        p.wind_camera_relative = c.wind_camera_relative;
        p.center_on_camera = c.center_on_camera;

        auto& cloud = effects[i * 2 + 1];
        cloud.name = c.name + " cloud cover";
        cloud.kind = WeatherEffects::Kind::Cloud;
        cloud.intensity = precipitation.intensity;
        cloud.overcast = 0.f;
        cloud.particles.center_on_camera = c.center_on_camera;
        cloud.particles.wind_camera_relative = c.wind_camera_relative;
        cloud.cloud.base = c.cloud.base;
        cloud.cloud.top = c.cloud.top;
        cloud.cloud.tint = c.cloud.tint;
        cloud.cloud.density = std::clamp(c.cloud.density, 0, 100);
        cloud.cloud.size = std::max(c.cloud.size, 1.f);
        cloud.cloud.wind_response = std::max(c.cloud.speed, 0.f) / kConditionWindReference;
        cloud.cloud.radius = std::clamp(c.cloud.radius, 250.f, kMaxRadius);
    }
}

bool WeatherModule::IsAtmosphereEnabled()
{
    // Baked shadow tiles must also be cleared while the next map is streaming.
    return initialized && atmosphere_enabled && !GWToolbox::ShouldDisableToolbox()
        && !GW::Map::GetIsInCinematic();
}

void WeatherModule::Initialize()
{
    ToolboxModule::Initialize();
    RegisterSettings(this);
    initialized = true;
    last_wind_tick = TIMER_INIT();
    if (atmosphere_enabled) Skybox::Initialize();
    WeatherEffects::Initialize();
    GW::Chat::CreateCommand(&chat_hook_entry, L"weather", CmdWeather);
    GW::Chat::CreateCommand(&chat_hook_entry, L"climate", CmdClimate);
}

void WeatherModule::Reset()
{
    WeatherEffects::Reset();
}

void WeatherModule::InvalidateDeviceResources()
{
    Skybox::InvalidateDeviceResources();
    WeatherEffects::InvalidateDeviceResources();
}

void WeatherModule::SignalTerminate()
{
    if (!initialized.exchange(false)) return;
    Skybox::SignalTerminate();
    WeatherEffects::SignalTerminate();
    GW::Chat::DeleteCommand(&chat_hook_entry);
}

void WeatherModule::Terminate()
{
    SignalTerminate();
    WeatherEffects::Terminate();
    Skybox::Terminate();
    last_wind_tick = 0;
    auto_climate = Climate::Temperate;
    auto_timer = -1.f;
    ToolboxModule::Terminate();
}
