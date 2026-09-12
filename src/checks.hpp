#pragma once
#include <optional>
namespace checklist {
enum class State { Unknown, NotDone, Done };
inline State boolean(std::optional<bool> actual, bool expected = true)
{
    if (!actual.has_value()) return State::Unknown;
    return *actual == expected ? State::Done : State::NotDone;
}
inline State recording(bool active, bool paused)
{
    return boolean(active && !paused);
}
}
