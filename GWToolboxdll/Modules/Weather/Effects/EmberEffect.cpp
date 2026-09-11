#include "stdafx.h"

#include "Modules/Weather/Effects/EmberEffect.h"

WeatherEffects::Effect WeatherEffects::MakeEmberEffect()
{
    Effect effect;
    effect.name = "Embers";
    effect.kind = Kind::Particles;

    effect.overcast = 0.f;

    effect.particles.type = Particle_Snow;
    effect.particles.density = 14;
    effect.particles.drop_size = 5.f;

    effect.particles.fall_speed = -190.f;
    effect.particles.spread_radius = 2200.f;

    effect.particles.wind_response = 2.2f;
    effect.particles.splash_chance = 0.f;

    effect.particles.tint = 0xFF1E63FFu;
    effect.particles.emissive = true;
    effect.particles.floor_decal = Decal_None;

    effect.particles.drift = 55.f;

    effect.particles.column_height = 1400.f;
    return effect;
}
