#include "stdafx.h"

#include "Modules/Weather/Effects/LightningEffect.h"

WeatherEffects::Effect WeatherEffects::MakeLightningEffect()
{
    Effect effect;
    effect.name = "Lightning";
    effect.kind = Kind::Lightning;

    effect.overcast = 0.f;

    effect.lightning.strikes_per_minute_at_full = 60.f;
    effect.lightning.brightness = 1.f;
    effect.lightning.world_flash = 0.75f;

    effect.lightning.azimuth_deg = 155.f;
    effect.lightning.elevation_deg = 16.f;
    effect.lightning.spread_deg = 30.f;

    effect.lightning.bolt_chance = 0.6f;
    return effect;
}
