#include "stdafx.h"

#include "Modules/Weather/Effects/SandEffect.h"

WeatherEffects::Effect WeatherEffects::MakeSandEffect()
{
    Effect effect;
    effect.name = "Sand";
    effect.kind = Kind::Particles;
    effect.overcast = 0.1f;

    effect.particles.type = Particle_Rain;
    effect.particles.density = 25;
    effect.particles.drop_size = 4.f;

    effect.particles.fall_speed = 60.f;
    effect.particles.wind_response = 4.f;
    effect.particles.spread_radius = 1000.f;
    effect.particles.splash_chance = 0.f;
    effect.particles.tint = 0xFFC8B080u;
    effect.particles.floor_decal = Decal_None;

    effect.particles.column_height = 60.f;

    effect.particles.center_on_camera = true;
    return effect;
}
