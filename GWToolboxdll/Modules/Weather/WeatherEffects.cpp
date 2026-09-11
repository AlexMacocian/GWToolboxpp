#include "stdafx.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <unordered_map>

#include <GWCA/Constants/Constants.h>
#include <GWCA/Constants/Maps.h>
#include <GWCA/GameContainers/GamePos.h>
#include <GWCA/GameEntities/Camera.h>
#include <GWCA/Managers/CameraMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/RenderMgr.h>

#include "Logger.h"
#include "GWToolbox.h"
#include "Timer.h"
#include "Modules/AudioSettings.h"
#include "Modules/WeatherModule.h"
#include "Utils/GameWorldCompositor.h"
#include "Utils/TerrainDrape.h"
#include "Widgets/Minimap/GameWorldRenderer.h"
#include "Modules/Weather/WeatherEffects.h"
#include "Modules/Weather/Wind.h"

#include "Modules/Weather/Effects/AshEffect.h"
#include "Modules/Weather/Effects/EmberEffect.h"
#include "Modules/Weather/Effects/GroundFogEffect.h"
#include "Modules/Weather/Effects/HighFogEffect.h"
#include "Modules/Weather/Effects/LightningEffect.h"
#include "Modules/Weather/Effects/RainEffect.h"
#include "Modules/Weather/Effects/SandEffect.h"
#include "Modules/Weather/Effects/SnowEffect.h"

#include "Widgets/Minimap/Shaders/weather_billboard_ps.h"

#include "Modules/Weather/Skybox.h"
#include "Widgets/Minimap/Shaders/weather_billboard_vs.h"
#include "Widgets/Minimap/Shaders/weather_instanced_vs.h"

#include <imgui.h>

namespace {
    using namespace WeatherEffects;

    constexpr float kMaxRadius = 2500.f;
    constexpr float kPi = 3.14159265f;
    constexpr float kDeg2Rad = kPi / 180.f;

    constexpr float kParticleAreaFull = 500.f;

    constexpr int kMaxParticlesTotal = 30000;
    constexpr float kColumnHeightMax = kMaxRadius;
    constexpr float kFogFactor = 1.0f;
    constexpr float kSplashSize = 8.f;
    constexpr float kSplashDuration = 0.5f;
    constexpr float kRecycleBelow = 600.f;
    constexpr float kSplashLift = 5.f;
    constexpr int kMaxSplashes = 4000;
    constexpr int kMaxSettled = 4000;
    constexpr float kSnowSwaySpeed = 1.5f;
    constexpr float kSnowSwayAmp = 120.f;
    constexpr float kSnowSettleSize = 14.f;
    constexpr float kSnowSettleDuration = 4.f;
    constexpr float kSnowSettleFade = 0.4f;
    constexpr int kSplashCols = 4, kSplashRows = 4;
    constexpr int kSplashFrames = kSplashCols * kSplashRows;

    constexpr float kIntensityEpsilon = 0.004f;

    std::vector<Effect> effects;
    std::vector<Effect> controlled_effects;
    std::atomic_bool reset_requested = false;
    bool initialized = false;
    bool terminating = false;
    GW::Constants::MapID last_map_id = GW::Constants::MapID::None;
    float combined_overcast = 0.f;
    uint32_t combined_overcast_tint = 0xFFFFFFFFu;
    LightningFrame lightning_frame;

    uint32_t rng = 0x1234567u;

    float frand(const float lo, const float hi)
    {
        rng = rng * 1664525u + 1013904223u;
        return lo + static_cast<float>(rng >> 8) / static_cast<float>(0xFFFFFFu) * (hi - lo);
    }

    void cross3(const float a[3], const float b[3], float out[3])
    {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }

    void normalize3(float v[3])
    {
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (length <= 1e-6f) return;
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    }

    DirectX::XMFLOAT3 DirFromElevAzim(const float elevation_deg, const float azimuth_deg)
    {
        const float elevation = elevation_deg * kDeg2Rad;
        const float azimuth = azimuth_deg * kDeg2Rad;
        const float horizontal = std::cos(elevation);

        return {
            horizontal * std::cos(azimuth), horizontal * std::sin(azimuth),
            -std::sin(elevation)};
    }

    void ParticleVelocity(
        const float heading_deg, const float fall_speed, const float wind_response,
        float out[3])
    {
        const float horizontal = Wind::Speed() * std::max(wind_response, 0.f);
        const float radians = heading_deg * kDeg2Rad;
        out[0] = horizontal * std::cos(radians);
        out[1] = horizontal * std::sin(radians);

        out[2] = fall_speed;
    }

    constexpr int EffectiveDecal(const ParticleLayer& p)
    {
        if (p.floor_decal != Decal_Auto) return p.floor_decal;
        return p.type == Particle_Snow ? Decal_Settle : Decal_Splash;
    }

    float EffectiveDrift(const ParticleLayer& p)
    {
        return p.drift >= 0.f ? p.drift : (p.type == Particle_Snow ? kSnowSwayAmp : 0.f);
    }

    float ColumnHeight(const ParticleLayer& p)
    {
        return std::clamp(p.column_height, 1.f, kColumnHeightMax);
    }

    int DesiredDropCount(const ParticleLayer& p, const float intensity)
    {
        if (p.density <= 0 || intensity <= kIntensityEpsilon) return 0;
        const float disk = kPi * p.spread_radius * p.spread_radius;
        const float per_particle =
            kParticleAreaFull * 100.f / static_cast<float>(std::clamp(p.density, 1, 100));
        const float height_fraction = ColumnHeight(p) / p.spread_radius;
        return static_cast<int>(disk / per_particle * height_fraction * intensity);
    }

    int DesiredCloudCount(const CloudLayer& c, const float intensity)
    {
        if (c.top <= c.base || c.density <= 0 || intensity <= kIntensityEpsilon) return 0;
        const float disk = kPi * c.radius * c.radius;
        const float per_particle =
            kParticleAreaFull * 100.f / static_cast<float>(std::clamp(c.density, 1, 100));
        return static_cast<int>(disk / per_particle * intensity);
    }

    struct Raindrop {

        float x, y, z, ground_z, sway_sin, sway_cos;
    };
    struct Splash {
        float x, y, z, age;
    };
    struct Settle {
        float x, y, z, age;
    };
    struct CloudPuff {
        float x, y, h;
    };

    struct LightningState {
        clock_t next_strike = 0;
        clock_t strike_start = 0;
        std::array<float, 3> pulse_offsets{};
        std::array<float, 3> pulse_amplitudes{};
        DirectX::XMFLOAT3 direction_world = {1.f, 0.f, -0.2f};
        float seed = 0.f;
        bool bolt_visible = false;
        bool active = false;
    };

    struct EffectRuntime {
        float live_intensity = 0.f;
        float center_z = 0.f;
        bool seeded = false;
        std::vector<Raindrop> raindrops;
        std::vector<Splash> splashes;
        std::vector<Settle> settled;
        std::vector<CloudPuff> clouds;
        float sound_timer = -1.f;
        LightningState lightning;
    };
    std::vector<EffectRuntime> runtimes;

    constexpr float kNoGround = std::numeric_limits<float>::max();
    constexpr float kGroundCell = 64.f;
    constexpr size_t kGroundCacheMax = 1u << 16;
    std::unordered_map<uint64_t, float> ground_cache;

    float RawGroundZAt(const float x, const float y)
    {
        const GW::PathingMapArray* pathing = GW::Map::GetPathingMap();
        const uint32_t planes = pathing ? static_cast<uint32_t>(pathing->size()) : 0;
        float best = kNoGround;
        for (uint32_t plane = 0; plane < planes; ++plane) {
            const float altitude = TerrainDrape::QueryAltAt(x, y, plane);
            if (altitude != 0.f && altitude < best) best = altitude;
        }
        return best;
    }

    float GroundZAt(const float x, const float y, const float fallback)
    {
        const auto gx = static_cast<int>(std::floor(x / kGroundCell));
        const auto gy = static_cast<int>(std::floor(y / kGroundCell));
        const uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(gx)) << 32
            | static_cast<uint32_t>(gy);
        if (const auto it = ground_cache.find(key); it != ground_cache.end()) {
            return it->second == kNoGround ? fallback : it->second;
        }
        const float z = RawGroundZAt(x, y);

        if (z != kNoGround) {
            if (ground_cache.size() >= kGroundCacheMax) ground_cache.clear();
            ground_cache.emplace(key, z);
        }
        return z == kNoGround ? fallback : z;
    }

    float LandingGroundZ(
        const ParticleLayer& layer, const float x, const float y, const float top,
        const float vx, const float vy, const float vz, const float cz)
    {
        const float fallback = cz + kRecycleBelow;
        const float local = GroundZAt(x, y, fallback);

        if (vz < 1.f) return local;
        const float drop = std::max(0.f, local - top);
        float hx = vx / vz * drop;
        float hy = vy / vz * drop;
        if (const float h = std::sqrt(hx * hx + hy * hy); h > layer.spread_radius) {
            const float scale = layer.spread_radius / h;
            hx *= scale;
            hy *= scale;
        }
        return GroundZAt(x + hx, y + hy, fallback);
    }

    void QuasirandomUnitSquare(const int index, float& u, float& v)
    {

        constexpr float kA1 = 0.7548776662466927f;
        constexpr float kA2 = 0.5698402909980532f;
        const auto fractional = [](const float value) { return value - std::floor(value); };
        const auto i = static_cast<float>(index);
        u = fractional(0.5f + kA1 * i);
        v = fractional(0.5f + kA2 * i);
    }

    void SeedDrop(
        Raindrop& drop, const ParticleLayer& layer, const float cx, const float cy,
        const float cz, const int index, const float vx, const float vy, const float vz)
    {
        const float top_z = cz - ColumnHeight(layer);
        float u = 0.f, v = 0.f;
        QuasirandomUnitSquare(index, u, v);
        const float span = 2.f * layer.spread_radius;
        const float jitter = span * 0.03f;
        drop.x = cx - layer.spread_radius + u * span + frand(-jitter, jitter);
        drop.y = cy - layer.spread_radius + v * span + frand(-jitter, jitter);
        drop.ground_z = LandingGroundZ(layer, drop.x, drop.y, top_z, vx, vy, vz, cz);

        drop.z = top_z + frand(0.f, std::max(0.f, drop.ground_z - top_z));
        const float angle = frand(0.f, 2.f * kPi);
        drop.sway_sin = std::sin(angle);
        drop.sway_cos = std::cos(angle);
    }

    void SeedCloud(
        CloudPuff& puff, const CloudLayer& layer, const float cx, const float cy,
        const int index)
    {
        float u = 0.f, v = 0.f;
        QuasirandomUnitSquare(index, u, v);
        const float span = 2.f * layer.radius;

        const float jitter = span * 0.03f;
        puff.x = cx - layer.radius + u * span + frand(-jitter, jitter);
        puff.y = cy - layer.radius + v * span + frand(-jitter, jitter);
        puff.h = frand(layer.base, layer.top);
    }

    void UpdateParticleLayer(
        const ParticleLayer& layer, EffectRuntime& rt, const int count, const float dt,
        const float cx, const float cy, const float cz, const float wind_dir,
        const float center_dz)
    {

        float velocity[3];
        ParticleVelocity(wind_dir, layer.fall_speed, layer.wind_response, velocity);
        const float vx = velocity[0];
        const float vy = velocity[1];
        const float vz = velocity[2];

        if (static_cast<int>(rt.raindrops.size()) != count) {
            const int old_count = static_cast<int>(rt.raindrops.size());
            rt.raindrops.resize(std::max(0, count));

            for (int i = old_count; i < count; ++i) {
                SeedDrop(rt.raindrops[i], layer, cx, cy, cz, i, vx, vy, vz);
            }
        }
        if (rt.raindrops.empty()) return;

        const int decal = EffectiveDecal(layer);
        const bool splashes = decal == Decal_Splash;
        const bool settles = decal == Decal_Settle;
        const float drift = EffectiveDrift(layer);
        const float top_z = cz - ColumnHeight(layer);
        const float diameter = 2.f * layer.spread_radius;

        const bool has_sway = drift > 0.f;
        const float rot_sin = std::sin(kSnowSwaySpeed * dt);
        const float rot_cos = std::cos(kSnowSwaySpeed * dt);

        for (auto& drop : rt.raindrops) {

            drop.z += vz * dt + center_dz;
            float sway_x = 0.f, sway_y = 0.f;
            if (has_sway) {
                sway_x = drift * drop.sway_sin;
                sway_y = drift * drop.sway_cos;
                const float next_sin = drop.sway_sin * rot_cos + drop.sway_cos * rot_sin;
                drop.sway_cos = drop.sway_cos * rot_cos - drop.sway_sin * rot_sin;
                drop.sway_sin = next_sin;
            }
            drop.x += (vx + sway_x) * dt;
            drop.y += (vy + sway_y) * dt;

            if (const float rx = drop.x - cx; rx > layer.spread_radius) drop.x -= diameter;
            else if (rx < -layer.spread_radius) drop.x += diameter;
            if (const float ry = drop.y - cy; ry > layer.spread_radius) drop.y -= diameter;
            else if (ry < -layer.spread_radius) drop.y += diameter;

            if (vz < 0.f) {
                if (drop.z > top_z) continue;
                drop.x = cx - layer.spread_radius + frand(0.f, diameter);
                drop.y = cy - layer.spread_radius + frand(0.f, diameter);
                drop.ground_z = GroundZAt(drop.x, drop.y, cz);

                drop.z = drop.ground_z - frand(0.f, std::max(1.f, -vz * dt));
                continue;
            }

            if (drop.z < drop.ground_z) continue;

            if (splashes && frand(0.f, 1.f) < layer.splash_chance
                && static_cast<int>(rt.splashes.size()) < kMaxSplashes) {
                rt.splashes.push_back(
                    {drop.x, drop.y, GroundZAt(drop.x, drop.y, drop.ground_z), 0.f});
            }
            if (settles && frand(0.f, 1.f) < layer.splash_chance
                && static_cast<int>(rt.settled.size()) < kMaxSettled) {
                rt.settled.push_back(
                    {drop.x, drop.y, GroundZAt(drop.x, drop.y, drop.ground_z), 0.f});
            }

            drop.x = cx - layer.spread_radius + frand(0.f, diameter);
            drop.y = cy - layer.spread_radius + frand(0.f, diameter);
            drop.z = top_z + frand(0.f, std::max(1.f, vz * dt));
            drop.ground_z = LandingGroundZ(layer, drop.x, drop.y, top_z, vx, vy, vz, cz);
        }
    }

    void AgeDecals(EffectRuntime& rt, const float dt)
    {
        for (size_t i = 0; i < rt.splashes.size();) {
            if ((rt.splashes[i].age += dt) >= kSplashDuration) {
                rt.splashes[i] = rt.splashes.back();
                rt.splashes.pop_back();
            }
            else ++i;
        }
        for (size_t i = 0; i < rt.settled.size();) {
            if ((rt.settled[i].age += dt) >= kSnowSettleDuration) {
                rt.settled[i] = rt.settled.back();
                rt.settled.pop_back();
            }
            else ++i;
        }
    }

    void UpdateCloudLayer(
        const CloudLayer& layer, EffectRuntime& rt, const int count, const float dt,
        const float cx, const float cy, const float wind_dir)
    {
        if (static_cast<int>(rt.clouds.size()) != count) {
            const int old_count = static_cast<int>(rt.clouds.size());
            rt.clouds.resize(std::max(0, count));
            for (int i = old_count; i < count; ++i) {
                SeedCloud(rt.clouds[i], layer, cx, cy, i);
            }
        }
        if (rt.clouds.empty()) return;

        const float carried = Wind::Speed() * std::max(layer.wind_response, 0.f);
        const float vx = std::cos(wind_dir * kDeg2Rad) * carried;
        const float vy = std::sin(wind_dir * kDeg2Rad) * carried;
        const float diameter = 2.f * layer.radius;
        for (auto& puff : rt.clouds) {
            puff.x += vx * dt;
            puff.y += vy * dt;
            if (const float rx = puff.x - cx; rx > layer.radius) puff.x -= diameter;
            else if (rx < -layer.radius) puff.x += diameter;
            if (const float ry = puff.y - cy; ry > layer.radius) puff.y -= diameter;
            else if (ry < -layer.radius) puff.y += diameter;
        }
    }

    LightningFrame UpdateLightning(
        const LightningLayer& layer, LightningState& state, const float intensity)
    {
        LightningFrame out;
        if (layer.strikes_per_minute_at_full <= 0.f || intensity <= kIntensityEpsilon) {
            state.active = false;
            return out;
        }
        const clock_t now = TIMER_INIT();

        const float rate = std::max(layer.strikes_per_minute_at_full * intensity, 0.1f);
        const float mean_gap = 60000.f / rate;

        const auto longest_wait = now + static_cast<clock_t>(mean_gap * 1.85f);
        if (state.next_strike == 0 || state.next_strike > longest_wait) {
            state.next_strike = longest_wait;
        }

        if (now >= state.next_strike) {
            state.strike_start = now;
            state.active = true;

            const auto scatter = [](const float spread) {
                return frand(-1.f, 1.f) * spread;
            };
            const float azimuth = layer.azimuth_deg + scatter(layer.spread_deg);
            const float elevation = std::clamp(
                layer.elevation_deg + scatter(layer.spread_deg * 0.5f), 2.f, 70.f);
            state.direction_world = DirFromElevAzim(elevation, azimuth);
            state.seed = frand(0.f, 120.f);

            state.bolt_visible = frand(0.f, 1.f) < layer.bolt_chance;
            state.pulse_offsets[0] = 0.f;
            state.pulse_amplitudes[0] = 0.75f + frand(0.f, 0.5f);
            state.pulse_offsets[1] = 0.05f + frand(0.f, 0.09f);
            state.pulse_amplitudes[1] =
                frand(0.f, 1.f) < 0.65f ? 0.4f + frand(0.f, 0.5f) : 0.f;
            state.pulse_offsets[2] = 0.16f + frand(0.f, 0.14f);
            state.pulse_amplitudes[2] =
                frand(0.f, 1.f) < 0.35f ? 0.3f + frand(0.f, 0.4f) : 0.f;
            state.next_strike =
                now + static_cast<clock_t>(mean_gap * (0.45f + frand(0.f, 1.4f)));
        }
        if (!state.active) return out;

        const float elapsed = static_cast<float>(now - state.strike_start) * 0.001f;
        if (elapsed > 1.2f) {
            state.active = false;
            return out;
        }
        float flash = 0.f;
        float bolt = 0.f;
        for (size_t i = 0; i < state.pulse_offsets.size(); ++i) {
            const float since = elapsed - state.pulse_offsets[i];
            if (since < 0.f || state.pulse_amplitudes[i] <= 0.f) continue;

            const float attack = std::min(since / 0.012f, 1.f);
            flash += state.pulse_amplitudes[i] * attack * std::exp(-since * 9.f);

            bolt += state.pulse_amplitudes[i] * attack * std::exp(-since * 34.f);
        }

        const float scale = layer.brightness;
        out.flash = flash * scale;
        out.bolt = state.bolt_visible ? std::min(bolt, 1.5f) * scale : 0.f;
        out.world_flash = layer.world_flash;
        out.flash_reach = layer.flash_reach;
        out.seed = state.seed;
        out.direction_world = state.direction_world;
        return out;
    }

    void UpdateSounds(
        const Effect& effect, EffectRuntime& rt, const float dt, const float cx, const float cy,
        const float cz)
    {
        if (effect.sounds.empty() || effect.sound_max_interval <= 0.f) return;
        if (rt.sound_timer < 0.f) {
            rt.sound_timer = frand(effect.sound_min_interval, effect.sound_max_interval);
            return;
        }
        if ((rt.sound_timer -= dt) > 0.f) return;
        const size_t index = std::min(
            effect.sounds.size() - 1,
            static_cast<size_t>(frand(0.f, static_cast<float>(effect.sounds.size()))));
        if (effect.sounds[index]) {
            if (effect.sound_3d) {
                const GW::Vec3f position{
                    cx + frand(-effect.particles.spread_radius, effect.particles.spread_radius),
                    cy + frand(-effect.particles.spread_radius, effect.particles.spread_radius),
                    cz};
                AudioSettings::PlaySoundFileId(effect.sounds[index], &position);
            }
            else {
                AudioSettings::PlaySoundFileId(effect.sounds[index]);
            }
        }

        const float spacing = std::max(rt.live_intensity, 0.15f);
        rt.sound_timer =
            frand(effect.sound_min_interval, effect.sound_max_interval) / spacing;
    }

    std::vector<Effect> DefaultEffects()
    {
        return {
            MakeRainEffect(), MakeSnowEffect(), MakeAshEffect(), MakeEmberEffect(),
            MakeSandEffect(), MakeGroundFogEffect(), MakeHighFogEffect(),
            MakeLightningEffect()};
    }

    struct Instance {
        float cx, cy, cz;
        float alpha;
    };
    struct Vertex {
        float x, y, z;
        DWORD color;
        float u, v;
    };
    struct GeomVert {
        float sx, sy, u, v;
    };
    struct Axes {
        float x[3], y[3];
    };

    enum class Sprite { Drop, Flake, Cloud };
    struct Batch {
        size_t first = 0;
        size_t count = 0;
        Sprite sprite = Sprite::Drop;
        uint32_t tint = 0xFFFFFFFFu;
        Axes axes{};
        bool emissive = false;
    };

    std::vector<Instance> instances;
    std::vector<Batch> batches;
    std::vector<Vertex> splash_vertices;

    IDirect3DVertexBuffer9* instance_vb = nullptr;
    IDirect3DVertexBuffer9* splash_vb = nullptr;
    IDirect3DVertexBuffer9* quad_geom_vb = nullptr;
    IDirect3DIndexBuffer9* quad_ib = nullptr;
    size_t instance_cap = 0, splash_cap = 0, quad_ib_quads = 0;

    IDirect3DVertexShader9* weather_vs = nullptr;
    IDirect3DVertexShader9* weather_inst_vs = nullptr;
    IDirect3DPixelShader9* weather_ps = nullptr;
    IDirect3DVertexDeclaration9* weather_decl = nullptr;
    IDirect3DVertexDeclaration9* weather_inst_decl = nullptr;

    IDirect3DTexture9* drop_tex = nullptr;
    IDirect3DTexture9* flake_tex = nullptr;
    IDirect3DTexture9* splash_tex = nullptr;
    IDirect3DTexture9* cloud_tex = nullptr;

    bool instances_ready = false, splash_ready = false;
    int compositor_token = 0;
    clock_t last_update = 0;
    constexpr clock_t kUpdateIntervalMs = 16;

    DWORD ToD3DColor(const uint32_t packed)
    {
        return (packed & 0xFF00FF00u) | ((packed & 0xFFu) << 16) | ((packed >> 16) & 0xFFu);
    }

    void ReleaseTexture(IDirect3DTexture9*& texture)
    {
        if (!texture) return;
        texture->Release();
        texture = nullptr;
    }

    IDirect3DTexture9* MakeRadialTexture(
        IDirect3DDevice9* device, const UINT size, const float power)
    {
        IDirect3DTexture9* texture = nullptr;
        if (FAILED(device->CreateTexture(
                size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))) {
            return nullptr;
        }
        D3DLOCKED_RECT rect;
        if (FAILED(texture->LockRect(0, &rect, nullptr, 0))) {
            texture->Release();
            return nullptr;
        }
        const float half = static_cast<float>(size) * 0.5f;
        for (UINT y = 0; y < size; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(rect.pBits) + y * rect.Pitch);
            for (UINT x = 0; x < size; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f - half) / half;
                const float dy = (static_cast<float>(y) + 0.5f - half) / half;
                const float r = std::sqrt(dx * dx + dy * dy);
                const float falloff = std::pow(std::clamp(1.f - r, 0.f, 1.f), power);
                const auto alpha = static_cast<uint32_t>(falloff * 255.f + 0.5f);
                row[x] = alpha << 24 | 0x00FFFFFFu;
            }
        }
        texture->UnlockRect(0);
        return texture;
    }

    IDirect3DTexture9* MakeSplashTexture(IDirect3DDevice9* device)
    {
        constexpr UINT kCell = 32;
        constexpr UINT kSize = kCell * kSplashCols;
        IDirect3DTexture9* texture = nullptr;
        if (FAILED(device->CreateTexture(
                kSize, kSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))) {
            return nullptr;
        }
        D3DLOCKED_RECT rect;
        if (FAILED(texture->LockRect(0, &rect, nullptr, 0))) {
            texture->Release();
            return nullptr;
        }
        for (UINT y = 0; y < kSize; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(rect.pBits) + y * rect.Pitch);
            for (UINT x = 0; x < kSize; ++x) {
                const UINT frame = y / kCell * kSplashCols + x / kCell;
                const float t = static_cast<float>(frame) / static_cast<float>(kSplashFrames - 1);
                const float cx = static_cast<float>(x % kCell) + 0.5f - kCell * 0.5f;
                const float cy = static_cast<float>(y % kCell) + 0.5f - kCell * 0.5f;
                const float r = std::sqrt(cx * cx + cy * cy) / (kCell * 0.5f);

                const float radius = 0.15f + t * 0.85f;
                const float thickness = 0.35f * (1.f - t) + 0.05f;
                const float ring = std::exp(
                    -(r - radius) * (r - radius) / (thickness * thickness));
                const float alpha = ring * (1.f - t) * (r <= 1.f ? 1.f : 0.f);
                row[x] = static_cast<uint32_t>(std::clamp(alpha, 0.f, 1.f) * 255.f + 0.5f) << 24
                    | 0x00FFFFFFu;
            }
        }
        texture->UnlockRect(0);
        return texture;
    }

    bool EnsureTextures(IDirect3DDevice9* device)
    {
        if (!drop_tex) drop_tex = MakeRadialTexture(device, 32, 1.6f);
        if (!flake_tex) flake_tex = MakeRadialTexture(device, 32, 1.0f);
        if (!cloud_tex) cloud_tex = MakeRadialTexture(device, 64, 2.4f);
        if (!splash_tex) splash_tex = MakeSplashTexture(device);
        return drop_tex && flake_tex && cloud_tex && splash_tex;
    }

    IDirect3DTexture9* TextureFor(const Sprite sprite)
    {
        switch (sprite) {
            case Sprite::Flake: return flake_tex;
            case Sprite::Cloud: return cloud_tex;
            case Sprite::Drop:
            default: return drop_tex;
        }
    }

    bool UploadVB(
        IDirect3DDevice9* device, IDirect3DVertexBuffer9*& vb, size_t& capacity,
        const void* data, const size_t bytes)
    {
        if (bytes == 0) return false;
        if (!vb || capacity < bytes) {
            if (vb) {
                vb->Release();
                vb = nullptr;
            }
            capacity = bytes + bytes / 2;
            if (FAILED(device->CreateVertexBuffer(
                    static_cast<UINT>(capacity), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                    D3DPOOL_DEFAULT, &vb, nullptr))) {
                capacity = 0;
                return false;
            }
        }
        void* dest = nullptr;
        if (FAILED(vb->Lock(0, static_cast<UINT>(bytes), &dest, D3DLOCK_DISCARD))) return false;
        std::memcpy(dest, data, bytes);
        vb->Unlock();
        return true;
    }

    bool EnsureQuadIndices(IDirect3DDevice9* device, const size_t quads)
    {
        if (quad_ib && quad_ib_quads >= quads) return true;
        if (quad_ib) {
            quad_ib->Release();
            quad_ib = nullptr;
        }

        quad_ib_quads = std::clamp<size_t>(quads + quads / 2, 256, 0xFFFFu / 4);
        if (FAILED(device->CreateIndexBuffer(
                static_cast<UINT>(quad_ib_quads * 6 * sizeof(uint16_t)), D3DUSAGE_WRITEONLY,
                D3DFMT_INDEX16, D3DPOOL_MANAGED, &quad_ib, nullptr))) {
            quad_ib_quads = 0;
            return false;
        }
        void* dest = nullptr;
        if (FAILED(quad_ib->Lock(0, 0, &dest, 0))) {
            quad_ib->Release();
            quad_ib = nullptr;
            quad_ib_quads = 0;
            return false;
        }
        auto* indices = static_cast<uint16_t*>(dest);
        for (size_t q = 0; q < quad_ib_quads; ++q) {
            const auto base = static_cast<uint16_t>(q * 4);
            *indices++ = base;
            *indices++ = static_cast<uint16_t>(base + 1);
            *indices++ = static_cast<uint16_t>(base + 2);
            *indices++ = base;
            *indices++ = static_cast<uint16_t>(base + 2);
            *indices++ = static_cast<uint16_t>(base + 3);
        }
        quad_ib->Unlock();
        return true;
    }

    bool EnsureQuadGeometry(IDirect3DDevice9* device)
    {
        if (quad_geom_vb) return true;
        constexpr GeomVert quad[4] = {
            {-1.f, -1.f, 0.f, 1.f}, {-1.f, 1.f, 0.f, 0.f},
            {1.f, 1.f, 1.f, 0.f}, {1.f, -1.f, 1.f, 1.f}};
        if (FAILED(device->CreateVertexBuffer(
                sizeof(quad), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &quad_geom_vb, nullptr))) {
            return false;
        }
        void* dest = nullptr;
        if (FAILED(quad_geom_vb->Lock(0, sizeof(quad), &dest, 0))) {
            quad_geom_vb->Release();
            quad_geom_vb = nullptr;
            return false;
        }
        std::memcpy(dest, quad, sizeof(quad));
        quad_geom_vb->Unlock();
        return true;
    }

    bool EnsureShaders(IDirect3DDevice9* device)
    {
        if (!weather_vs
            && FAILED(device->CreateVertexShader(
                reinterpret_cast<const DWORD*>(&weather_billboard_vs), &weather_vs))) {
            return false;
        }
        if (!weather_inst_vs
            && FAILED(device->CreateVertexShader(
                reinterpret_cast<const DWORD*>(&weather_instanced_vs), &weather_inst_vs))) {
            return false;
        }
        if (!weather_ps
            && FAILED(device->CreatePixelShader(
                reinterpret_cast<const DWORD*>(&weather_billboard_ps), &weather_ps))) {
            return false;
        }
        if (!weather_decl) {
            constexpr D3DVERTEXELEMENT9 elements[] = {
                {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
                {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                D3DDECL_END()};
            if (FAILED(device->CreateVertexDeclaration(elements, &weather_decl))) return false;
        }
        if (!weather_inst_decl) {
            constexpr D3DVERTEXELEMENT9 elements[] = {
                {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                {0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                {1, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
                {1, 12, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
                D3DDECL_END()};
            if (FAILED(device->CreateVertexDeclaration(elements, &weather_inst_decl))) {
                return false;
            }
        }
        return true;
    }

    void ReleaseResources()
    {
        const auto release_vb = [](IDirect3DVertexBuffer9*& vb, size_t& capacity) {
            if (vb) vb->Release();
            vb = nullptr;
            capacity = 0;
        };
        release_vb(instance_vb, instance_cap);
        release_vb(splash_vb, splash_cap);
        if (quad_geom_vb) quad_geom_vb->Release();
        quad_geom_vb = nullptr;
        if (quad_ib) quad_ib->Release();
        quad_ib = nullptr;
        quad_ib_quads = 0;
        if (weather_vs) weather_vs->Release();
        weather_vs = nullptr;
        if (weather_inst_vs) weather_inst_vs->Release();
        weather_inst_vs = nullptr;
        if (weather_ps) weather_ps->Release();
        weather_ps = nullptr;
        if (weather_decl) weather_decl->Release();
        weather_decl = nullptr;
        if (weather_inst_decl) weather_inst_decl->Release();
        weather_inst_decl = nullptr;
        ReleaseTexture(drop_tex);
        ReleaseTexture(flake_tex);
        ReleaseTexture(splash_tex);
        ReleaseTexture(cloud_tex);
    }

    bool InViewCone(
        const float x, const float y, const float z, const float eye[3], const float fwd[3],
        const float cone_tan_sq)
    {
        const float dx = x - eye[0], dy = y - eye[1], dz = z - eye[2];
        const float along = dx * fwd[0] + dy * fwd[1] + dz * fwd[2];
        if (along <= 0.f) return false;
        const float perp_sq = dx * dx + dy * dy + dz * dz - along * along;
        return perp_sq <= cone_tan_sq * along * along;
    }

    uint32_t ScaleAlpha(const uint32_t packed, const float scale)
    {
        const auto alpha = static_cast<uint32_t>(
            static_cast<float>(packed >> 24 & 0xFFu) * std::clamp(scale, 0.f, 1.f) + 0.5f);
        return (packed & 0x00FFFFFFu) | alpha << 24;
    }

    void AppendRainBatch(
        const ParticleLayer& layer, const EffectRuntime& rt, const float fwd[3],
        const float wind_dir, const float alpha_scale)
    {
        if (rt.raindrops.empty()) return;

        float velocity[3];
        ParticleVelocity(wind_dir, layer.fall_speed, layer.wind_response, velocity);
        const float along = velocity[0] * fwd[0] + velocity[1] * fwd[1] + velocity[2] * fwd[2];
        float axis_y[3] = {
            velocity[0] - fwd[0] * along, velocity[1] - fwd[1] * along,
            velocity[2] - fwd[2] * along};
        normalize3(axis_y);
        float axis_x[3];
        cross3(axis_y, fwd, axis_x);
        normalize3(axis_x);
        const float half_width = layer.drop_size * 0.15f;

        const float half_length =
            layer.drop_size * 0.5f + std::abs(layer.fall_speed) * 0.0005f * layer.drop_size;

        Batch batch;
        batch.first = instances.size();
        batch.count = rt.raindrops.size();
        batch.sprite = Sprite::Drop;
        batch.tint = ScaleAlpha(layer.tint, alpha_scale);
        batch.emissive = layer.emissive;
        for (int i = 0; i < 3; ++i) {
            batch.axes.x[i] = axis_x[i] * half_width;
            batch.axes.y[i] = axis_y[i] * half_length;
        }
        instances.reserve(instances.size() + rt.raindrops.size());
        for (const auto& drop : rt.raindrops) instances.push_back({drop.x, drop.y, drop.z, 1.f});
        batches.push_back(batch);
    }

    void AppendSnowBatch(
        const ParticleLayer& layer, const EffectRuntime& rt, const float right[3],
        const float up[3], const float alpha_scale)
    {
        if (rt.raindrops.empty()) return;
        const float half = layer.drop_size * 0.5f;
        Batch batch;
        batch.first = instances.size();
        batch.count = rt.raindrops.size();
        batch.sprite = Sprite::Flake;
        batch.tint = ScaleAlpha(layer.tint, alpha_scale);
        batch.emissive = layer.emissive;
        for (int i = 0; i < 3; ++i) {
            batch.axes.x[i] = right[i] * half;
            batch.axes.y[i] = up[i] * half;
        }
        instances.reserve(instances.size() + rt.raindrops.size());
        for (const auto& drop : rt.raindrops) instances.push_back({drop.x, drop.y, drop.z, 1.f});
        batches.push_back(batch);
    }

    void AppendSettledBatch(
        const ParticleLayer& layer, const EffectRuntime& rt, const float eye[3],
        const float fwd[3], const float cone_tan_sq, const float alpha_scale)
    {
        if (rt.settled.empty()) return;
        Batch batch;
        batch.first = instances.size();
        batch.sprite = Sprite::Flake;
        batch.tint = ScaleAlpha(layer.tint, alpha_scale);

        const float half = kSnowSettleSize * 0.5f;
        batch.axes.x[0] = half;
        batch.axes.y[1] = half;
        const float fade_from = kSnowSettleDuration * (1.f - kSnowSettleFade);
        for (const auto& flake : rt.settled) {
            const float z = flake.z - kSplashLift;
            if (!InViewCone(flake.x, flake.y, z, eye, fwd, cone_tan_sq)) continue;
            const float alpha = flake.age <= fade_from
                ? 1.f
                : 1.f - (flake.age - fade_from) / std::max(1e-3f, kSnowSettleDuration - fade_from);
            instances.push_back({flake.x, flake.y, z, std::clamp(alpha, 0.f, 1.f)});
        }
        batch.count = instances.size() - batch.first;
        if (batch.count) batches.push_back(batch);
    }

    void AppendCloudBatch(
        const CloudLayer& layer, const EffectRuntime& rt, const float right[3],
        const float up[3], const float cz, const float alpha_scale)
    {
        if (rt.clouds.empty()) return;
        const float half = layer.size * 0.5f;
        Batch batch;
        batch.first = instances.size();
        batch.count = rt.clouds.size();
        batch.sprite = Sprite::Cloud;
        batch.tint = ScaleAlpha(layer.tint, alpha_scale);
        for (int i = 0; i < 3; ++i) {
            batch.axes.x[i] = right[i] * half;
            batch.axes.y[i] = up[i] * half;
        }
        instances.reserve(instances.size() + rt.clouds.size());
        for (const auto& puff : rt.clouds) instances.push_back({puff.x, puff.y, cz - puff.h, 1.f});
        batches.push_back(batch);
    }

    void AppendSplashes(
        const EffectRuntime& rt, const float right[3], const uint32_t tint, const float eye[3],
        const float fwd[3], const float cone_tan_sq)
    {
        if (rt.splashes.empty()) return;
        const DWORD color = ToD3DColor(tint);
        const float half = kSplashSize * 0.5f;

        const float ax[3] = {right[0] * half, right[1] * half, 0.f};
        const float ay[3] = {-right[1] * half, right[0] * half, 0.f};
        for (const auto& splash : rt.splashes) {
            const float z = splash.z - kSplashLift;
            if (!InViewCone(splash.x, splash.y, z, eye, fwd, cone_tan_sq)) continue;
            const int frame = std::clamp(
                static_cast<int>(splash.age / kSplashDuration * kSplashFrames), 0,
                kSplashFrames - 1);
            const float u0 = static_cast<float>(frame % kSplashCols) / kSplashCols;
            const float v0 = static_cast<float>(frame / kSplashCols) / kSplashRows;
            const float u1 = u0 + 1.f / kSplashCols;
            const float v1 = v0 + 1.f / kSplashRows;
            const auto corner = [&](const float sx, const float sy, const float u,
                                    const float v) -> Vertex {
                return {
                    splash.x + ax[0] * sx + ay[0] * sy, splash.y + ax[1] * sx + ay[1] * sy,
                    z + ax[2] * sx + ay[2] * sy, color, u, v};
            };
            splash_vertices.push_back(corner(-1.f, -1.f, u0, v1));
            splash_vertices.push_back(corner(-1.f, 1.f, u0, v0));
            splash_vertices.push_back(corner(1.f, 1.f, u1, v0));
            splash_vertices.push_back(corner(1.f, -1.f, u1, v1));
        }
    }

    void SyncWeather(IDirect3DDevice9* device, const GW::Camera* camera, const float dt)
    {
        instances_ready = splash_ready = false;
        instances.clear();
        batches.clear();
        splash_vertices.clear();
        if (!camera) return;

        std::vector<const Effect*> frame_effects;
        frame_effects.reserve(effects.size() + controlled_effects.size());
        for (const auto& effect : effects) frame_effects.push_back(&effect);
        for (const auto& effect : controlled_effects) frame_effects.push_back(&effect);
        runtimes.resize(frame_effects.size());

        bool reset = reset_requested.exchange(false);
        if (!GW::Map::GetIsMapLoaded()) reset = true;

        if (const auto map_id = GW::Map::GetMapID(); map_id != last_map_id) {
            last_map_id = map_id;
            reset = true;
        }
        if (reset) {

            ground_cache.clear();
            for (auto& rt : runtimes) {
                rt.raindrops.clear();
                rt.splashes.clear();
                rt.settled.clear();
                rt.clouds.clear();
                rt.seeded = false;
                rt.live_intensity = 0.f;
                rt.center_z = 0.f;
                rt.sound_timer = -1.f;

                rt.lightning = {};
            }
        }

        const float eye[3] = {camera->position.x, camera->position.y, camera->position.z};

        float fwd[3] = {
            camera->look_at_target.x - eye[0], camera->look_at_target.y - eye[1],
            camera->look_at_target.z - eye[2]};
        normalize3(fwd);
        constexpr float world_up[3] = {0.f, 0.f, -1.f};
        float right[3];
        cross3(world_up, fwd, right);
        normalize3(right);
        float up[3];
        cross3(fwd, right, up);

        float cone_tan_sq = 1e30f;
        if (const float fov = GW::Render::GetFieldOfView(); fov > 0.1f) {
            const int height = GW::Render::GetViewportHeight();
            const float aspect = height > 0
                ? static_cast<float>(GW::Render::GetViewportWidth())
                    / static_cast<float>(height)
                : 1.7778f;
            const float tan_v = std::tan(fov * 0.5f);
            cone_tan_sq = tan_v * tan_v * (1.f + aspect * aspect) * 1.21f;
        }

        const float ease = std::clamp(dt * 2.f, 0.f, 1.f);
        for (size_t i = 0; i < frame_effects.size(); ++i) {
            auto& rt = runtimes[i];
            const float target = reset ? 0.f : std::clamp(frame_effects[i]->intensity, 0.f, 1.f);
            rt.live_intensity += (target - rt.live_intensity) * ease;
            if (rt.live_intensity < kIntensityEpsilon && target <= 0.f) {
                rt.live_intensity = 0.f;
                if (rt.seeded) {

                    rt.raindrops.clear();
                    rt.raindrops.shrink_to_fit();
                    rt.clouds.clear();
                    rt.clouds.shrink_to_fit();
                    rt.seeded = false;
                    rt.sound_timer = -1.f;
                }
            }
            else if (!rt.seeded) {
                rt.seeded = true;
            }
        }

        float overcast_transmission = 1.f;
        auto strongest_overcast = 0.f;
        combined_overcast_tint = 0xFFFFFFFFu;
        for (size_t i = 0; i < frame_effects.size(); ++i) {
            const auto overcast = std::clamp(frame_effects[i]->overcast, 0.f, 1.f)
                * std::clamp(runtimes[i].live_intensity, 0.f, 1.f);
            overcast_transmission *= 1.f
                - overcast;
            if (overcast > strongest_overcast && frame_effects[i]->overcast_tint != 0xFFFFFFFFu) {
                strongest_overcast = overcast;
                combined_overcast_tint = frame_effects[i]->overcast_tint;
            }
        }
        combined_overcast = std::clamp(1.f - overcast_transmission, 0.f, 1.f);

        auto wanted = int64_t{0};
        for (size_t i = 0; i < frame_effects.size(); ++i) {
            const float live = runtimes[i].live_intensity;
            if (frame_effects[i]->kind == Kind::Particles) {
                wanted += DesiredDropCount(frame_effects[i]->particles, live);
            }
            else if (frame_effects[i]->kind == Kind::Cloud) {
                wanted += DesiredCloudCount(frame_effects[i]->cloud, live);
            }
        }
        const float budget_scale =
            wanted > kMaxParticlesTotal
            ? static_cast<float>(kMaxParticlesTotal) / static_cast<float>(wanted)
            : 1.f;

        LightningFrame strongest;

        for (size_t i = 0; i < frame_effects.size(); ++i) {
            const auto& effect = *frame_effects[i];
            auto& rt = runtimes[i];
            const float intensity = rt.live_intensity;

            if (effect.kind == Kind::Lightning) {
                const LightningFrame strike =
                    UpdateLightning(effect.lightning, rt.lightning, intensity);
                if (strike.flash > strongest.flash) strongest = strike;
            }

            if (intensity <= kIntensityEpsilon) {

                AgeDecals(rt, dt);
                continue;
            }

            const auto& layer = effect.particles;

            const float cx = layer.center_on_camera ? eye[0] : camera->look_at_target.x;
            const float cy = layer.center_on_camera ? eye[1] : camera->look_at_target.y;
            const float cz = layer.center_on_camera ? eye[2] : camera->look_at_target.z;
            if (!rt.center_z) rt.center_z = cz;
            const float center_dz = cz - rt.center_z;
            rt.center_z = cz;

            const float heading = Wind::Direction()
                + (layer.wind_camera_relative ? camera->yaw * (180.f / kPi) : 0.f);

            if (effect.kind == Kind::Particles) {
                const int drops = static_cast<int>(
                    static_cast<float>(DesiredDropCount(layer, intensity)) * budget_scale);
                if (drops > 0 || !rt.raindrops.empty()) {
                    UpdateParticleLayer(layer, rt, drops, dt, cx, cy, cz, heading, center_dz);
                }
                AgeDecals(rt, dt);
                if (!rt.raindrops.empty()) {
                    if (layer.type == Particle_Snow) {
                        AppendSnowBatch(layer, rt, right, up, intensity);
                    }
                    else {
                        AppendRainBatch(layer, rt, fwd, heading, intensity);
                    }
                }
                AppendSettledBatch(layer, rt, eye, fwd, cone_tan_sq, intensity);
                AppendSplashes(
                    rt, right, ScaleAlpha(layer.tint, intensity), eye, fwd, cone_tan_sq);
            }
            else if (effect.kind == Kind::Cloud) {
                const int puffs = static_cast<int>(
                    static_cast<float>(DesiredCloudCount(effect.cloud, intensity))
                    * budget_scale);
                if (puffs > 0 || !rt.clouds.empty()) {
                    UpdateCloudLayer(effect.cloud, rt, puffs, dt, cx, cy, heading);
                }
                if (!rt.clouds.empty()) {
                    AppendCloudBatch(effect.cloud, rt, right, up, cz, intensity);
                }
            }
            UpdateSounds(effect, rt, dt, cx, cy, cz);
        }

        lightning_frame = strongest;

        instances_ready = UploadVB(
            device, instance_vb, instance_cap, instances.data(),
            instances.size() * sizeof(Instance));
        splash_ready = UploadVB(
            device, splash_vb, splash_cap, splash_vertices.data(),
            splash_vertices.size() * sizeof(Vertex));
    }

    void DrawBatch(IDirect3DDevice9* device, const Batch& batch)
    {
        IDirect3DTexture9* texture = TextureFor(batch.sprite);
        if (!batch.count || !texture) return;

        const float rgba[4] = {
            static_cast<float>(batch.tint & 0xFFu) / 255.f,
            static_cast<float>(batch.tint >> 8 & 0xFFu) / 255.f,
            static_cast<float>(batch.tint >> 16 & 0xFFu) / 255.f,
            static_cast<float>(batch.tint >> 24 & 0xFFu) / 255.f};
        device->SetVertexShaderConstantF(8, rgba, 1);
        constexpr float flags[4] = {0.f, 0.f, 0.f, 0.f};
        device->SetVertexShaderConstantF(9, flags, 1);
        const float axis_x[4] = {batch.axes.x[0], batch.axes.x[1], batch.axes.x[2], 0.f};
        const float axis_y[4] = {batch.axes.y[0], batch.axes.y[1], batch.axes.y[2], 0.f};
        device->SetVertexShaderConstantF(10, axis_x, 1);
        device->SetVertexShaderConstantF(11, axis_y, 1);

        const float particle_flags[4] = {batch.emissive ? 1.f : 0.f, 0.f, 0.f, 0.f};
        device->SetPixelShaderConstantF(4, particle_flags, 1);
        device->SetTexture(0, texture);

        device->SetStreamSource(0, quad_geom_vb, 0, sizeof(GeomVert));
        device->SetStreamSourceFreq(
            0, D3DSTREAMSOURCE_INDEXEDDATA | static_cast<UINT>(batch.count));
        device->SetStreamSource(
            1, instance_vb, static_cast<UINT>(batch.first * sizeof(Instance)),
            sizeof(Instance));
        device->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
        device->SetIndices(quad_ib);
        device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    }

    void DrawInWorld(IDirect3DDevice9* device)
    {

        struct ClearLightningOnEarlyOut {
            bool synced = false;
            ~ClearLightningOnEarlyOut()
            {
                if (!synced) lightning_frame = {};
            }
        } lightning_guard;

        if (!device || !initialized || terminating) return;

        if (GWToolbox::ShouldDisableToolbox() || !GW::Map::GetIsMapLoaded()
            || GW::Map::GetInstanceType() == GW::Constants::InstanceType::Loading
            || GW::Map::GetIsInCinematic()) {
            reset_requested = true;
            combined_overcast = 0.f;
            combined_overcast_tint = 0xFFFFFFFFu;
            last_update = 0;
            return;
        }
        const GW::Camera* camera = GW::CameraMgr::GetCamera();
        if (!camera) return;
        if (!EnsureShaders(device) || !EnsureQuadGeometry(device) || !EnsureTextures(device)) {
            return;
        }
        lightning_guard.synced = true;

        const clock_t now = TIMER_INIT();
        if (!last_update) last_update = now;
        if (const clock_t elapsed = TIMER_DIFF(last_update); elapsed >= kUpdateIntervalMs) {
            const float dt = std::min(static_cast<float>(elapsed) * 0.001f, 0.25f);
            last_update = now;
            SyncWeather(device, camera, dt);
        }

        const auto draw_overcast = !WeatherModule::IsAtmosphereEnabled()
            && combined_overcast > 0.003f && combined_overcast_tint != 0xFFFFFFFFu;
        if (!instances_ready && !splash_ready && !draw_overcast) return;

        IDirect3DStateBlock9* state_block = nullptr;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state_block))) return;
        if (FAILED(state_block->Capture())) {
            state_block->Release();
            return;
        }

        if (draw_overcast) {
            D3DVIEWPORT9 viewport{};
            if (SUCCEEDED(device->GetViewport(&viewport))) {
                GameWorldCompositor::SetWorldRenderStates(device, false);
                const auto channel = [](const uint32_t value) {
                    const auto tint = static_cast<float>(value & 0xFFu) / 255.f;
                    return static_cast<DWORD>((1.f - combined_overcast * (1.f - tint)) * 255.f + 0.5f);
                };
                const auto color = D3DCOLOR_ARGB(255, channel(combined_overcast_tint),
                    channel(combined_overcast_tint >> 8), channel(combined_overcast_tint >> 16));
                struct ScreenVertex {
                    float x, y, z, rhw;
                    DWORD color;
                };
                const auto left = static_cast<float>(viewport.X) - 0.5f;
                const auto top = static_cast<float>(viewport.Y) - 0.5f;
                const auto right = left + static_cast<float>(viewport.Width);
                const auto bottom = top + static_cast<float>(viewport.Height);
                const ScreenVertex quad[] = {
                    {left, top, 0.f, 1.f, color}, {right, top, 0.f, 1.f, color},
                    {left, bottom, 0.f, 1.f, color}, {right, bottom, 0.f, 1.f, color}};
                device->SetVertexShader(nullptr);
                device->SetPixelShader(nullptr);
                device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                device->SetStreamSourceFreq(0, 1);
                device->SetStreamSourceFreq(1, 1);
                device->SetTexture(0, nullptr);
                device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
                device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                device->SetRenderState(D3DRS_ZENABLE, FALSE);
                device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                device->SetRenderState(D3DRS_FOGENABLE, FALSE);
                device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
                device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
                device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
                device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
                device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
                device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ScreenVertex));
            }
        }

        device->SetPixelShader(weather_ps);

        Skybox::UploadWorldLighting(device, 3);
        GameWorldCompositor::SetWorldRenderStates(device, GameWorldRenderer::GetOccludeBehindTerrain());

        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        if (GameWorldCompositor::SetWorldViewProj(device)) {
            GameWorldCompositor::SetDistanceFog(device, kMaxRadius, kFogFactor);
            if (instances_ready && EnsureQuadIndices(device, 1)) {
                device->SetVertexShader(weather_inst_vs);
                device->SetVertexDeclaration(weather_inst_decl);
                for (const auto& batch : batches) DrawBatch(device, batch);
                device->SetStreamSourceFreq(0, 1);
                device->SetStreamSourceFreq(1, 1);
            }
            if (splash_ready && EnsureQuadIndices(device, splash_vertices.size() / 4)) {
                constexpr float particle_flags[4]{};
                device->SetPixelShaderConstantF(4, particle_flags, 1);
                device->SetVertexShader(weather_vs);
                device->SetVertexDeclaration(weather_decl);
                device->SetTexture(0, splash_tex);
                device->SetStreamSourceFreq(0, 1);
                device->SetStreamSourceFreq(1, 1);
                device->SetStreamSource(0, splash_vb, 0, sizeof(Vertex));
                device->SetIndices(quad_ib);
                const size_t quads = std::min(splash_vertices.size() / 4, quad_ib_quads);
                if (quads) {
                    device->DrawIndexedPrimitive(
                        D3DPT_TRIANGLELIST, 0, 0, static_cast<UINT>(quads * 4), 0,
                        static_cast<UINT>(quads * 2));
                }
            }
        }

        state_block->Apply();
        state_block->Release();
    }
}

void WeatherEffects::Initialize()
{
    if (initialized) return;
    if (effects.empty()) effects = DefaultEffects();
    runtimes.assign(effects.size(), {});
    terminating = false;
    last_update = 0;
    reset_requested = true;
    compositor_token = GameWorldCompositor::RegisterDraw(DrawInWorld, -50);
    if (!compositor_token) {
        Log::Error("Weather: could not register an in-world draw");
    }
    initialized = true;
}

void WeatherEffects::SignalTerminate()
{
    terminating = true;
    if (compositor_token) {
        GameWorldCompositor::UnregisterDraw(compositor_token);
        compositor_token = 0;
    }
    combined_overcast = 0.f;
    combined_overcast_tint = 0xFFFFFFFFu;
    lightning_frame = {};
}

void WeatherEffects::InvalidateDeviceResources()
{
    ReleaseResources();
    instances_ready = splash_ready = false;
    last_update = 0;
    reset_requested = true;
    combined_overcast = 0.f;
    combined_overcast_tint = 0xFFFFFFFFu;
    lightning_frame = {};
}

void WeatherEffects::Terminate()
{
    SignalTerminate();
    ReleaseResources();
    runtimes.clear();
    controlled_effects.clear();
    instances.clear();
    batches.clear();
    splash_vertices.clear();
    combined_overcast = 0.f;
    combined_overcast_tint = 0xFFFFFFFFu;
    lightning_frame = {};
    instances_ready = splash_ready = false;
    ground_cache.clear();
    last_map_id = GW::Constants::MapID::None;
    last_update = 0;
    reset_requested = false;
    initialized = false;
}

std::vector<Effect>& WeatherEffects::Effects()
{
    return effects;
}

std::vector<Effect>& WeatherEffects::ControlledEffects()
{
    return controlled_effects;
}

void WeatherEffects::ValidateSettings()
{
    const auto finite = [](float& value, const float fallback, const float low, const float high) {
        value = std::clamp(std::isfinite(value) ? value : fallback, low, high);
    };
    for (auto& effect : effects) {
        finite(effect.intensity, 0.f, 0.f, 1.f);
        finite(effect.overcast, 0.f, 0.f, 1.f);
        finite(effect.sound_min_interval, 8.f, 0.f, 3600.f);
        finite(effect.sound_max_interval, 25.f, effect.sound_min_interval, 3600.f);
        auto& p = effect.particles;
        p.type = std::clamp<int>(p.type, Particle_Rain, Particle_Snow);
        p.floor_decal = std::clamp<int>(p.floor_decal, Decal_Auto, Decal_Settle);
        p.density = std::clamp(p.density, 0, 100);
        finite(p.drop_size, 8.f, 1.f, 500.f);
        finite(p.fall_speed, 2000.f, -3000.f, 30000.f);
        finite(p.spread_radius, 2500.f, 200.f, kMaxRadius);
        finite(p.column_height, 2500.f, 1.f, kColumnHeightMax);
        finite(p.wind_response, 1.f, 0.f, 200.f);
        finite(p.splash_chance, 0.f, 0.f, 1.f);
        finite(p.drift, kDriftAuto, kDriftAuto, 1000.f);
        auto& cloud = effect.cloud;
        cloud.density = std::clamp(cloud.density, 0, 100);
        finite(cloud.base, 0.f, -2000.f, 2500.f);
        finite(cloud.top, 0.f, -2000.f, 2500.f);
        finite(cloud.radius, 2500.f, 200.f, kMaxRadius);
        finite(cloud.size, 600.f, 1.f, 3000.f);
        finite(cloud.wind_response, 1.f, 0.f, 40.f);
        auto& lightning = effect.lightning;
        finite(lightning.strikes_per_minute_at_full, 0.f, 0.f, 60.f);
        finite(lightning.brightness, 1.f, 0.f, 2.f);
        finite(lightning.world_flash, 0.35f, 0.f, 1.f);
        finite(lightning.azimuth_deg, 155.f, 0.f, 360.f);
        finite(lightning.elevation_deg, 16.f, 0.f, 70.f);
        finite(lightning.spread_deg, 30.f, 0.f, 90.f);
        finite(lightning.flash_reach, 0.55f, 0.05f, 3.f);
        finite(lightning.bolt_chance, 0.6f, 0.f, 1.f);
    }
    Reset();
}

void WeatherEffects::SetIntensity(const int index, const float intensity)
{
    if (index < 0 || index >= static_cast<int>(effects.size())) return;
    effects[index].intensity = std::clamp(intensity, 0.f, 1.f);
}

bool WeatherEffects::SetIntensityByName(const std::string& name, const float intensity)
{
    for (auto& effect : effects) {
        if (effect.name != name) continue;
        effect.intensity = std::clamp(intensity, 0.f, 1.f);
        return true;
    }
    return false;
}

float WeatherEffects::TargetIntensity(const int index)
{
    if (index < 0 || index >= static_cast<int>(effects.size())) return 0.f;
    return effects[index].intensity;
}

float WeatherEffects::LiveIntensity(const int index)
{
    if (index < 0 || index >= static_cast<int>(runtimes.size())) return 0.f;
    return runtimes[index].live_intensity;
}

void WeatherEffects::ClearAll()
{
    for (auto& effect : effects) effect.intensity = 0.f;
    for (auto& effect : controlled_effects) effect.intensity = 0.f;
}

float WeatherEffects::AmbientStrength()
{
    return initialized && !terminating ? combined_overcast : 0.f;
}

uint32_t WeatherEffects::AmbientTint()
{
    return initialized && !terminating ? combined_overcast_tint : 0xFFFFFFFFu;
}

WeatherEffects::LightningFrame WeatherEffects::CurrentLightning()
{
    return initialized && !terminating ? lightning_frame : LightningFrame{};
}

void WeatherEffects::Reset()
{
    reset_requested = true;
}

void WeatherEffects::ResetEffectsToDefaults()
{
    effects = DefaultEffects();
    runtimes.assign(effects.size(), {});
    Reset();
}

void WeatherEffects::DrawSettings()
{
    if (!ImGui::CollapsingHeader("Additional weather effects")) return;

    ImGui::TextDisabled(
        "These effects combine with the condition selected below or by /weather and /climate.\n"
        "Each runs independently at its own intensity, so a chill morning is\n"
        "rain at 0.2 plus fog at 0.3. Intensity scales particle COUNT as well as opacity, so a\n"
        "light effect is genuinely sparse rather than a dim version of a heavy one.");
    ImGui::Text(
        "Combined overcast %.2f (feeds the sky, not a second screen tint)", combined_overcast);
    if (lightning_frame.flash > 0.001f) {
        ImGui::Text(
            "Lightning: flash %.2f, bolt %.2f", lightning_frame.flash, lightning_frame.bolt);
    }
    size_t total_particles = 0;
    for (const auto& rt : runtimes) {
        total_particles += rt.raindrops.size() + rt.clouds.size();
    }
    ImGui::Text(
        "%d particles across %d effects (budget %d)", static_cast<int>(total_particles),
        static_cast<int>(effects.size()), kMaxParticlesTotal);
    if (!WeatherModule::IsAtmosphereEnabled()) {
        ImGui::TextDisabled("Enable Atmospheric sky and lighting to see lightning.");
    }

    if (ImGui::Button("Clear additional effects")) {
        for (auto& effect : effects) effect.intensity = 0.f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset particles")) Reset();
    ImGui::SameLine();
    if (ImGui::Button("Restore defaults")) ResetEffectsToDefaults();

    ImGui::Separator();
    for (int i = 0; i < static_cast<int>(effects.size()); ++i) {
        auto& effect = effects[i];
        ImGui::PushID(i);
        ImGui::SliderFloat(effect.name.c_str(), &effect.intensity, 0.f, 1.f, "%.2f");
        if (i < static_cast<int>(runtimes.size())) {
            const auto& rt = runtimes[i];
            const auto live_objects = rt.raindrops.size() + rt.clouds.size()
                + rt.splashes.size() + rt.settled.size();

            if (rt.live_intensity > 0.001f || live_objects != 0) {
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "live %.3f | %d drops %d puffs %d decals", rt.live_intensity,
                    static_cast<int>(rt.raindrops.size()),
                    static_cast<int>(rt.clouds.size()),
                    static_cast<int>(rt.splashes.size() + rt.settled.size()));
            }
        }
        ImGui::PopID();
    }

    for (auto& effect : effects) {
        if (effect.kind != Kind::Cloud) continue;
        ImGui::SeparatorText(effect.name.c_str());
        ImGui::PushID(&effect);
        ImGui::SliderFloat("Radius", &effect.cloud.radius, 200.f, kMaxRadius, "%.0f");
        ImGui::SliderFloat("Band base", &effect.cloud.base, -2000.f, 2000.f, "%.0f");
        ImGui::SliderFloat("Band top", &effect.cloud.top, -2000.f, 2500.f, "%.0f");
        ImGui::SliderInt("Puff density at full", &effect.cloud.density, 0, 100, "%d%%");
        ImGui::SliderFloat("Puff size", &effect.cloud.size, 100.f, 3000.f, "%.0f");
        ImGui::SliderFloat("Wind response", &effect.cloud.wind_response, 0.f, 4.f, "%.2fx");
        float tint[4] = {
            static_cast<float>(effect.cloud.tint & 0xFFu) / 255.f,
            static_cast<float>(effect.cloud.tint >> 8 & 0xFFu) / 255.f,
            static_cast<float>(effect.cloud.tint >> 16 & 0xFFu) / 255.f,
            static_cast<float>(effect.cloud.tint >> 24 & 0xFFu) / 255.f};
        if (ImGui::ColorEdit4("Tint", tint, ImGuiColorEditFlags_AlphaBar)) {
            const auto channel = [](const float value) {
                return static_cast<uint32_t>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
            };
            effect.cloud.tint = channel(tint[0]) | channel(tint[1]) << 8
                | channel(tint[2]) << 16 | channel(tint[3]) << 24;
        }
        ImGui::PopID();
        if (effect.cloud.top <= effect.cloud.base) {
            ImGui::TextDisabled("Inactive: top must be above base.");
        }
        ImGui::TextDisabled(
            "Base and top are heights relative to YOU, so a negative base puts the band\n"
            "underfoot and it reads as sitting in the landscape rather than hanging over it.\n"
            "The band is centred on you and wraps at its radius, so it travels with you.");
    }

    for (auto& effect : effects) {
        if (effect.kind != Kind::Lightning) continue;
        ImGui::SeparatorText("Where the lightning strikes");
        ImGui::PushID(&effect);
        ImGui::SliderFloat("Azimuth", &effect.lightning.azimuth_deg, 0.f, 360.f, "%.0f deg");
        ImGui::SliderFloat("Elevation", &effect.lightning.elevation_deg, 0.f, 70.f, "%.0f deg");
        ImGui::SliderFloat("Scatter", &effect.lightning.spread_deg, 0.f, 90.f, "%.0f deg");
        ImGui::SliderFloat("Flash spread", &effect.lightning.flash_reach, 0.05f, 3.f, "%.2f");
        ImGui::SliderFloat("Bolt chance", &effect.lightning.bolt_chance, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat(
            "Strikes/min at full", &effect.lightning.strikes_per_minute_at_full, 0.f, 60.f,
            "%.1f");
        ImGui::SliderFloat("Brightness", &effect.lightning.brightness, 0.f, 2.f, "%.2f");
        ImGui::SliderFloat("Ground flash", &effect.lightning.world_flash, 0.f, 1.f, "%.2f");
        ImGui::PopID();
        ImGui::TextDisabled(
            "Azimuth and elevation aim the storm; scatter is how far individual strikes stray\n"
            "from that centre. Flash spread is how far the glow carries across the sky - low\n"
            "for a distant flare over the horizon, high to light the whole sky. Only a fraction\n"
            "of strikes show a channel; the rest stay buried as glow, which is what most real\n"
            "strikes look like from a distance.");
        break;
    }

    ImGui::Separator();
    ImGui::TextDisabled(
        "The sliders above set what is RUNNING. The editor below changes one block's\n"
        "definition; a block only shows the one layer it actually is.");
    static int edit = 0;
    edit = std::clamp(edit, 0, std::max(0, static_cast<int>(effects.size()) - 1));
    if (effects.empty()) return;
    if (ImGui::BeginCombo("Edit effect", effects[edit].name.c_str())) {
        for (int i = 0; i < static_cast<int>(effects.size()); ++i) {
            if (ImGui::Selectable(effects[i].name.c_str(), edit == i)) edit = i;
        }
        ImGui::EndCombo();
    }
    auto& e = effects[edit];
    ImGui::SliderFloat("Overcast at full", &e.overcast, 0.f, 1.f, "%.2f");
    auto overcast_tint = ImGui::ColorConvertU32ToFloat4(e.overcast_tint);
    if (ImGui::ColorEdit3("Overcast tint", &overcast_tint.x)) {
        e.overcast_tint = ImGui::ColorConvertFloat4ToU32(overcast_tint);
    }
    ImGui::TextUnformatted("Sounds (game file IDs)");
    auto sound_to_remove = -1;
    for (int i = 0; i < static_cast<int>(e.sounds.size()); ++i) {
        ImGui::PushID(i);
        ImGui::InputScalar("File ID", ImGuiDataType_U32, &e.sounds[i], nullptr, nullptr, "%X",
            ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (ImGui::SmallButton("Test")) AudioSettings::PlaySoundFileId(e.sounds[i]);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) sound_to_remove = i;
        ImGui::PopID();
    }
    if (sound_to_remove >= 0) e.sounds.erase(e.sounds.begin() + sound_to_remove);
    if (ImGui::SmallButton("Add effect sound")) e.sounds.push_back(0);
    ImGui::DragFloatRange2("Sound interval (seconds)", &e.sound_min_interval,
        &e.sound_max_interval, 0.5f, 0.f, 3600.f, "%.1f", "%.1f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Checkbox("Positional sound", &e.sound_3d);

    switch (e.kind) {
        case Kind::Particles: {
            ImGui::SeparatorText("Particles");
            ImGui::Combo("Type", &e.particles.type, "Rain\0Snow\0");
            ImGui::SliderInt("Density at full", &e.particles.density, 0, 100, "%d%%");
            ImGui::SliderFloat("Particle size", &e.particles.drop_size, 1.f, 40.f, "%.1f");
            ImGui::SliderFloat("Fall speed", &e.particles.fall_speed, -3000.f, 3000.f, "%.0f");
            ImGui::SliderFloat(
                "Spread radius", &e.particles.spread_radius, 200.f, kMaxRadius, "%.0f");
            ImGui::SliderFloat(
                "Column height", &e.particles.column_height, 50.f, kColumnHeightMax, "%.0f");
            ImGui::SliderFloat("Wind response", &e.particles.wind_response, 0.f, 6.f, "%.2fx");
            ImGui::SliderFloat("Decal chance", &e.particles.splash_chance, 0.f, 1.f, "%.2f");
            auto tint = ImGui::ColorConvertU32ToFloat4(e.particles.tint);
            if (ImGui::ColorEdit4("Particle tint", &tint.x, ImGuiColorEditFlags_AlphaBar)) {
                e.particles.tint = ImGui::ColorConvertFloat4ToU32(tint);
            }
            ImGui::SliderFloat("Drift", &e.particles.drift, kDriftAuto, 1000.f, "%.0f");
            auto decal = EffectiveDecal(e.particles);
            if (ImGui::Combo("Floor decal", &decal, "None\0Splash\0Settle\0")) {
                e.particles.floor_decal = decal;
            }
            ImGui::Checkbox("Self-luminous particles", &e.particles.emissive);
            ImGui::Checkbox("Wind follows the camera", &e.particles.wind_camera_relative);
            ImGui::Checkbox("Centre on the camera", &e.particles.center_on_camera);
            ImGui::TextDisabled(
                "Tilt is not set here: it falls out of wind speed over fall speed, so the same\n"
                "wind blows slow snow far further than fast rain, and nothing tilts in calm.");
            break;
        }
        case Kind::Cloud:
        case Kind::Lightning:

            ImGui::TextDisabled("Settings for this block are in its own section above.");
            break;
    }
}
