#pragma once

namespace Wind {

    void Update(float delta_seconds);

    [[nodiscard]] float Direction();
    void SetDirection(float degrees);

    [[nodiscard]] float BaseSpeed();
    void SetBaseSpeed(float gwinch_per_second);

    [[nodiscard]] float Speed();

    [[nodiscard]] float Gustiness();
    void SetGustiness(float gustiness);

    void DirectionVector(float out[2]);

    void Velocity(float out[2]);

    [[nodiscard]] float Strength();

    [[nodiscard]] float BaseStrength();

    void DrawSettings();
}
