#include "task-model.hpp"
#include <iostream>
#include <limits>
int main()
{
    using namespace todo;
    int failures = 0;
    auto check = [&](bool ok, const char *name) { if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; } };
    const QByteArray old = R"({"version":1,"tasks":[{"name":"My mic","manual":false,"done":false,"rule":"unmuted","target":"Mic/Aux"},{"name":"Water","manual":true,"done":true,"rule":"","target":""}]})";
    std::vector<Task> tasks; Appearance appearance;
    check(decode(old, tasks, appearance), "version 1 loads");
    check(tasks.size() == 2 && tasks[0].expected && tasks[1].done, "legacy expected true and manual progress retained");
    tasks[0].expected = false; appearance = {"#8877ee", 14, 50, 3};
    auto bytes = encode(tasks, appearance);
    std::vector<Task> restored; Appearance ui;
    check(decode(bytes, restored, ui), "version 2 loads");
    check(restored.size() == 2 && !restored[0].expected && restored[1].done && ui.fontSize == 14 && ui.accent == "#8877ee" && ui.rowHeight == 50 && ui.refreshSeconds == 3, "state and appearance round trip");
    std::swap(tasks[0], tasks[1]); check(decode(encode(tasks, appearance), restored, ui) && restored[0].name == "Water", "task ordering persists");
    for (const auto &rule : rules) {
        Task t{"Example", rule.id, rule.target[0] ? "Example target" : "", false, false};
        if (rule.kind == Kind::Filter) t.extra = "Noise suppression";
        if (rule.kind == Kind::Item) t.extra = "12";
        if (rule.kind == Kind::Monitor) t.extra = "2";
        if (rule.kind == Kind::Track) t.extra = "6";
        check(decode(encode({t}, appearance), restored, ui), rule.id);
        t.expected = false;
        check(decode(encode({t}, appearance), restored, ui) && !restored[0].expected, "OFF condition round trip");
    }
    Task invalid{"Bad", "track", "Mic", false, false}; invalid.extra = "0";
    check(!decode(encode({invalid}, appearance), restored, ui), "invalid track rejected before bit shift");
    invalid.rule = "filter"; invalid.extra.clear(); check(!decode(encode({invalid}, appearance), restored, ui), "missing filter rejected");
    invalid.rule = "visible"; invalid.extra = "invalid"; check(!decode(encode({invalid}, appearance), restored, ui), "invalid item ID rejected");
    invalid.rule = "not-a-rule"; check(!decode(encode({invalid}, appearance), restored, ui), "unsupported rule rejected");
    invalid.rule = "volume"; invalid.comparison = 5; check(!decode(encode({invalid}, appearance), restored, ui), "invalid comparison rejected");
    auto bad = appearance; bad.accent = "bad-color"; check(!decode(encode(tasks, bad), restored, ui), "invalid color rejected");
    bad = appearance; bad.refreshSeconds = 0; check(!decode(encode(tasks, bad), restored, ui), "invalid refresh rejected");
    const auto before = restored.size(); check(!decode("not json", restored, ui) && restored.size() == before, "failed decode leaves existing state intact");
    if (!failures) std::cout << "PASS: legacy migration, 31 rule round trips, OFF conditions, appearance, ordering, invalid-data protection\n";
    return failures ? 1 : 0;
}
