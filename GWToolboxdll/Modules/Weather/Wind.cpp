#include "stdafx.h"

#include <algorithm>
#include <cmath>

#include "Modules/Weather/Wind.h"

#include <imgui.h>

namespace {
    constexpr float kPi = 3.14159265f;
    constexpr float kDeg2Rad = kPi / 180.f;

    constexpr float kStrongWind = 600.f;

    float direction_deg = 250.f;
    float base_speed = 150.f;
    float gustiness = 0.25f;

    float gust_phase_a = 0.f;
    float gust_phase_b = 1.7f;
    float gust = 0.f;
}

void Wind::Update(const float delta_seconds)
{
    const auto dt = std::isfinite(delta_seconds) ? std::clamp(delta_seconds, 0.f, 0.25f) : 0.f;
    gust_phase_a += dt * 0.37f;
    gust_phase_b += dt * 0.11f;
    if (gust_phase_a > 2.f * kPi) gust_phase_a -= 2.f * kPi;
    if (gust_phase_b > 2.f * kPi) gust_phase_b -= 2.f * kPi;

    gust = std::sin(gust_phase_a) * 0.35f + std::sin(gust_phase_b) * 0.65f;
}

float Wind::Direction()
{
    return direction_deg;
}

void Wind::SetDirection(const float degrees)
{
    direction_deg = std::isfinite(degrees) ? std::fmod(degrees, 360.f) : 250.f;
    if (direction_deg < 0.f) direction_deg += 360.f;
}

float Wind::BaseSpeed()
{
    return base_speed;
}

void Wind::SetBaseSpeed(const float gwinch_per_second)
{
    base_speed = std::isfinite(gwinch_per_second) ? std::clamp(gwinch_per_second, 0.f, 1200.f) : 150.f;
}

float Wind::Speed()
{
    return std::max(base_speed * (1.f + gust * gustiness), 0.f);
}

float Wind::Gustiness()
{
    return gustiness;
}

void Wind::SetGustiness(const float value)
{
    gustiness = std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.25f;
}

void Wind::DirectionVector(float out[2])
{
    const float radians = direction_deg * kDeg2Rad;
    out[0] = std::cos(radians);
    out[1] = std::sin(radians);
}

void Wind::Velocity(float out[2])
{
    DirectionVector(out);
    const float speed = Speed();
    out[0] *= speed;
    out[1] *= speed;
}

float Wind::Strength()
{
    return std::clamp(Speed() / kStrongWind, 0.f, 1.f);
}

float Wind::BaseStrength()
{
    return std::clamp(base_speed / kStrongWind, 0.f, 1.f);
}

void Wind::DrawSettings()
{
    if (!ImGui::CollapsingHeader("Wind")) return;
    ImGui::TextDisabled(
        "One air mass drives everything: the sky's cloud layer scrolls along it, particles are\n"
        "blown off vertical by it, and the fog and sandstorm bands drift with it. Speed is in\n"
        "gwinch/sec, the same units as a particle's fall speed - which is what lets a slow\n"
        "snowflake be blown far further than fast rain by the very same wind.");
    ImGui::SliderFloat("Direction", &direction_deg, 0.f, 360.f, "%.0f deg");
    ImGui::SliderFloat("Speed", &base_speed, 0.f, 1200.f, "%.0f");
    ImGui::SliderFloat("Gustiness", &gustiness, 0.f, 1.f, "%.2f");
    ImGui::Text("Now: %.0f gwinch/s (strength %.2f)", Speed(), Strength());
}
