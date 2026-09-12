#pragma once
#include <optional>
#include <cmath>
namespace checklist {
enum class State { Unknown, NotDone, Done };
inline State boolean(std::optional<bool> actual, bool expected = true)
{
    if (!actual.has_value()) return State::Unknown;
    return *actual == expected ? State::Done : State::NotDone;
}
inline State recording(bool active, bool paused) { return boolean(active && !paused); }
inline State invert(State state, bool expected)
{
    if (state == State::Unknown || expected) return state;
    return state == State::Done ? State::NotDone : State::Done;
}
inline State numeric(double actual, double threshold, int comparison)
{
    if (!std::isfinite(actual) || !std::isfinite(threshold)) return State::Unknown;
    if (comparison == 0) return boolean(actual >= threshold);
    if (comparison == 1) return boolean(actual <= threshold);
    if (comparison == 2) return boolean(std::abs(actual - threshold) <= 0.005);
    return State::Unknown;
}
}
