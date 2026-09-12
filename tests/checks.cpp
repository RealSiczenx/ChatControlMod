#include "checks.hpp"
#include <iostream>
using checklist::State;
int main()
{
    int failures = 0;
    const auto check = [&](State got, State want, const char *name) {
        if (got != want) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
    };
    check(checklist::boolean(std::nullopt), State::Unknown, "missing source cannot pass");
    check(checklist::boolean(false), State::NotDone, "inactive output");
    check(checklist::boolean(true), State::Done, "active output");
    check(checklist::boolean(false, false), State::Done, "unmuted input");
    check(checklist::boolean(true, false), State::NotDone, "muted input fails unmuted check");
    check(checklist::boolean(std::nullopt, false), State::Unknown, "missing audio cannot pass unmuted check");
    check(checklist::recording(false, false), State::NotDone, "stopped recording");
    check(checklist::recording(true, true), State::NotDone, "paused recording");
    check(checklist::recording(true, false), State::Done, "running recording");
    check(checklist::recording(false, true), State::NotDone, "inactive paused recording");
    check(checklist::invert(State::Unknown, false), State::Unknown, "OFF never turns Unknown into Done");
    check(checklist::invert(State::Done, false), State::NotDone, "OFF reverses true");
    check(checklist::invert(State::NotDone, false), State::Done, "OFF passes false");
    check(checklist::numeric(50, 50, 0), State::Done, "minimum inclusive");
    check(checklist::numeric(49, 50, 0), State::NotDone, "below minimum");
    check(checklist::numeric(50, 50, 1), State::Done, "maximum inclusive");
    check(checklist::numeric(51, 50, 1), State::NotDone, "above maximum");
    check(checklist::numeric(59.94, 59.94, 2), State::Done, "fractional FPS exact");
    check(checklist::numeric(59.94, 60, 2), State::NotDone, "fractional FPS mismatch");
    check(checklist::numeric(-150, -100, 1), State::Done, "negative audio sync");
    check(checklist::numeric(NAN, 50, 0), State::Unknown, "invalid numeric value");
    if (!failures) std::cout << "PASS: 21 status and threshold decision cases\n";
    return failures ? 1 : 0;
}
