#include "stdafx.h"

#include "Modules/Weather/Effects/HighFogEffect.h"

WeatherEffects::Effect WeatherEffects::MakeHighFogEffect()
{
    Effect effect;
    effect.name = "High fog";
    effect.kind = Kind::Cloud;

    effect.overcast = 0.7f;

    effect.cloud.base = 1000.f;
    effect.cloud.top = 1500.f;
    effect.cloud.tint = 0xB0505A64u;
    effect.cloud.density = 25;
    effect.cloud.size = 700.f;
    effect.cloud.radius = 2500.f;

    effect.cloud.wind_response = 1.f;
    return effect;
}
