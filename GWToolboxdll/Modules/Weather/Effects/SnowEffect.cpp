#include "stdafx.h"

#include "Modules/Weather/Effects/SnowEffect.h"

WeatherEffects::Effect WeatherEffects::MakeSnowEffect()
{
    Effect effect;
    effect.name = "Snow";
    effect.kind = Kind::Particles;
    effect.overcast = 0.15f;

    effect.particles.type = Particle_Snow;
    effect.particles.density = 40;
    effect.particles.drop_size = 8.f;

    effect.particles.fall_speed = 200.f;
    effect.particles.spread_radius = 1500.f;
    effect.particles.wind_response = 3.f;

    effect.particles.splash_chance = 0.15f;
    effect.particles.column_height = 1500.f;
    return effect;
}
