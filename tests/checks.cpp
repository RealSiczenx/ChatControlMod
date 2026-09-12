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
    if (!failures) std::cout << "PASS: 10 status decision cases\n";
    return failures ? 1 : 0;
}
