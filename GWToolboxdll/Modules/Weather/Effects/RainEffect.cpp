#include "stdafx.h"

#include "Modules/Weather/Effects/RainEffect.h"

WeatherEffects::Effect WeatherEffects::MakeRainEffect()
{
    Effect effect;
    effect.name = "Rain";
    effect.kind = Kind::Particles;

    effect.overcast = 0.15f;

    effect.particles.type = Particle_Rain;
    effect.particles.density = 100;
    effect.particles.drop_size = 10.f;
    effect.particles.fall_speed = 1200.f;
    effect.particles.spread_radius = 2500.f;

    effect.particles.wind_response = 2.f;

    effect.particles.splash_chance = 0.30f;
    return effect;
}
