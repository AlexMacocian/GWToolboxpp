#include "stdafx.h"

#include "Modules/Weather/Effects/GroundFogEffect.h"

WeatherEffects::Effect WeatherEffects::MakeGroundFogEffect()
{
    Effect effect;
    effect.name = "Ground fog";
    effect.kind = Kind::Cloud;

    effect.overcast = 0.2f;

    effect.cloud.base = -500.f;
    effect.cloud.top = 100.f;
    effect.cloud.tint = 0x40C8C8D0u;
    effect.cloud.density = 14;
    effect.cloud.size = 800.f;
    effect.cloud.radius = 1500.f;

    effect.cloud.wind_response = 0.15f;
    return effect;
}
