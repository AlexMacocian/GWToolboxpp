#include "stdafx.h"

#include "Modules/Weather/Effects/AshEffect.h"

WeatherEffects::Effect WeatherEffects::MakeAshEffect()
{
    Effect effect;
    effect.name = "Ash";
    effect.kind = Kind::Particles;
    effect.overcast = 0.15f;

    effect.particles.type = Particle_Snow;
    effect.particles.density = 20;
    effect.particles.drop_size = 9.f;
    effect.particles.fall_speed = 350.f;
    effect.particles.spread_radius = 2500.f;

    effect.particles.wind_response = 1.3f;
    effect.particles.splash_chance = 0.f;
    effect.particles.tint = 0xFF42464Au;
    effect.particles.floor_decal = Decal_None;
    return effect;
}
