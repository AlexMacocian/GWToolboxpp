#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace WeatherEffects {
    enum ParticleType : int {
        Particle_Rain = 0,
        Particle_Snow = 1,
    };

    enum Decal : int {
        Decal_None = 0,
        Decal_Splash = 1,
        Decal_Settle = 2,
        Decal_Auto = -1,
    };

    inline constexpr float kDriftAuto = -1.f;

    struct ParticleLayer {
        int type = Particle_Rain;
        int density = 30;
        float drop_size = 8.f;

        float fall_speed = 2000.f;
        float spread_radius = 2500.f;

        float wind_response = 1.f;
        float splash_chance = 1.f;
        uint32_t tint = 0xFFFFFFFFu;
        int floor_decal = Decal_Auto;
        float drift = kDriftAuto;
        float column_height = 2500.f;
        bool wind_camera_relative = false;
        bool center_on_camera = false;

        bool emissive = false;
    };

    struct CloudLayer {
        float base = 0.f;
        float top = 0.f;
        uint32_t tint = 0x80808080u;
        int density = 20;
        float size = 600.f;

        float wind_response = 1.f;
        float radius = 2500.f;
    };

    struct LightningLayer {

        float strikes_per_minute_at_full = 0.f;
        float brightness = 1.f;
        float world_flash = 0.35f;
        float azimuth_deg = 155.f;
        float elevation_deg = 16.f;
        float spread_deg = 30.f;

        float flash_reach = 0.55f;
        float bolt_chance = 0.6f;
    };

    enum class Kind {
        Particles,
        Cloud,
        Lightning,
    };

    struct Effect {
        std::string name = "Rain";
        Kind kind = Kind::Particles;

        float intensity = 0.f;

        float overcast = 0.f;
        uint32_t overcast_tint = 0xFFFFFFFFu;
        std::vector<uint32_t> sounds;
        float sound_min_interval = 8.f;
        float sound_max_interval = 25.f;
        bool sound_3d = false;
        ParticleLayer particles;
        CloudLayer cloud;
        LightningLayer lightning;
    };
}
