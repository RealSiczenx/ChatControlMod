// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <cmath>
#include <vector>
#include "checks.hpp"
namespace todo {
enum class Kind { Toggle, Text, Number, Filter, Item, Monitor, Track };
struct Rule { const char *id; const char *label; const char *target; Kind kind; const char *help; };
inline const Rule rules[] = {
 {"unmuted", "Audio / Unmuted", "audio", Kind::Toggle, "Checks the OBS mute switch, not whether sound is present."},
 {"muted", "Audio / Muted", "audio", Kind::Toggle, "Checks the OBS mute switch, not audio routing."},
 {"volume", "Audio / Volume level", "audio", Kind::Number, "OBS volume fader percentage: 100% is unity gain. This is not a live sound meter."},
 {"sync", "Audio / Sync offset", "audio", Kind::Number, "Configured audio sync offset in milliseconds."},
 {"monitor", "Audio / Monitoring mode matches", "audio", Kind::Monitor, "Compares the source monitoring setting; it cannot verify your speakers."},
 {"track", "Audio / Assigned to track", "audio", Kind::Track, "Checks the source track assignment, not which tracks the output records."},
 {"scene", "Scenes / Program scene matches", "scene", Kind::Text, "Checks the live Program scene, not Studio Mode preview."},
 {"preview_scene", "Scenes / Preview scene matches", "scene", Kind::Text, "Studio Mode must be enabled. Otherwise this check is Unknown."},
 {"visible", "Scenes / Item visibility enabled", "scene", Kind::Item, "Checks a direct item in the selected scene. It does not prove the item is on the live output. Items inside groups must be checked separately using source activity."},
 {"locked", "Scenes / Item locked", "scene", Kind::Item, "Checks the lock on a direct item in the selected scene."},
 {"active", "Sources / Active in an output", "source", Kind::Toggle, "OBS uses this source in an output. This does not prove the picture is correct."},
 {"showing", "Sources / Showing in any view", "source", Kind::Toggle, "Includes OBS preview displays; use Active for output activity."},
 {"enabled", "Sources / Enabled", "source", Kind::Toggle, "Checks the source enabled flag. Use Item visibility for the scene eye switch."},
 {"source_exists", "Sources / Exists", "source", Kind::Toggle, "Choose No / OFF to require that this named source does not exist."},
 {"filter", "Filters / Enabled", "source", Kind::Filter, "Checks an existing named filter on this source. A missing filter is Unknown."},
 {"media_playing", "Media / Playing", "media", Kind::Toggle, "Checks playback state for sources with controllable media."},
 {"media_paused", "Media / Paused", "media", Kind::Toggle, "Checks playback state for sources with controllable media."},
 {"media_ended", "Media / Ended", "media", Kind::Toggle, "Checks whether controllable media has reached the end."},
 {"stream", "Outputs / Streaming active", "", Kind::Toggle, "Checks OBS streaming state, not whether viewers can receive it."},
 {"record", "Outputs / Recording running, not paused", "", Kind::Toggle, "No / OFF passes when recording is stopped or paused."},
 {"record_active", "Outputs / Recording active", "", Kind::Toggle, "Active includes a recording that is currently paused."},
 {"record_paused", "Outputs / Recording paused", "", Kind::Toggle, "No / OFF means not paused; recording may also be stopped."},
 {"replay", "Outputs / Replay buffer running", "", Kind::Toggle, "Checks the running state, not just whether replay buffer is configured."},
 {"camera", "Outputs / Virtual camera running", "", Kind::Toggle, "Checks the virtual camera output state."},
 {"studio", "Workspace / Studio Mode enabled", "", Kind::Toggle, "Checks whether Studio Mode is enabled."},
 {"preview_enabled", "Workspace / Preview enabled", "", Kind::Toggle, "Checks the main preview display setting outside Studio Mode. In Studio Mode this check is Unknown."},
 {"profile", "Workspace / Profile matches", "profile", Kind::Text, "Compares the current OBS profile name."},
 {"collection", "Workspace / Scene collection matches", "collection", Kind::Text, "Compares the current OBS scene collection name."},
 {"fps", "Video / Configured FPS", "", Kind::Number, "Checks configured FPS, not measured performance or dropped frames."},
 {"width", "Video / Output width", "", Kind::Number, "Checks configured output width in pixels."},
 {"height", "Video / Output height", "", Kind::Number, "Checks configured output height in pixels."}
};
inline const Rule *findRule(const QString &id)
{ for (const auto &r : rules) if (id == QLatin1String(r.id)) return &r; return nullptr; }
struct Task {
 QString name, rule, target;
 bool manual = true, done = false;
 QString extra;
 bool expected = true;
 double threshold = 50.0;
 int comparison = 0; // 0 >=, 1 <=, 2 approximately equal
};
struct Appearance {
 QString accent; // empty follows OBS theme
 int fontSize = 11, rowHeight = 42, refreshSeconds = 1;
};
inline QJsonObject encodeTask(const Task &t)
{
 return {{"name", t.name}, {"rule", t.rule}, {"target", t.target}, {"manual", t.manual}, {"done", t.done},
         {"extra", t.extra}, {"expected", t.expected}, {"threshold", t.threshold}, {"comparison", t.comparison}};
}
inline QByteArray encode(const std::vector<Task> &tasks, const Appearance &ui)
{
 QJsonArray a; for (const auto &t : tasks) a.append(encodeTask(t));
 return QJsonDocument(QJsonObject{{"version", 2}, {"tasks", a}, {"appearance", QJsonObject{
  {"accent", ui.accent}, {"fontSize", ui.fontSize}, {"rowHeight", ui.rowHeight}, {"refreshSeconds", ui.refreshSeconds}}}}).toJson();
}
inline bool decode(const QByteArray &bytes, std::vector<Task> &out, Appearance &ui)
{
 QJsonParseError err; const auto doc = QJsonDocument::fromJson(bytes, &err);
 if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
 const auto root = doc.object(); const int version = root.value("version").toInt();
 if ((version != 1 && version != 2) || !root.value("tasks").isArray()) return false;
 const auto a = root.value("tasks").toArray(); if (a.size() > 200) return false;
 std::vector<Task> candidate;
 for (const auto v : a) {
  if (!v.isObject()) return false;
  const auto o = v.toObject();
  if (!o.value("name").isString() || !o.value("manual").isBool() || !o.value("done").isBool()) return false;
  Task t{o.value("name").toString().trimmed(), o.value("rule").toString(), o.value("target").toString().trimmed(), o.value("manual").toBool(), o.value("done").toBool()};
  if (version == 2) {
   if (!o.value("expected").isBool() || !o.value("extra").isString() || !o.value("threshold").isDouble() || !o.value("comparison").isDouble()) return false;
   t.extra = o.value("extra").toString().trimmed(); t.expected = o.value("expected").toBool();
   t.threshold = o.value("threshold").toDouble(); t.comparison = o.value("comparison").toInt(-1);
   if (!std::isfinite(t.threshold) || t.threshold < -1000000 || t.threshold > 1000000 || t.comparison < 0 || t.comparison > 2 || o.value("comparison").toDouble() != t.comparison) return false;
  }
  if (t.name.isEmpty() || t.name.size() > 180 || t.target.size() > 200 || t.extra.size() > 200) return false;
  if (!t.manual) {
   const auto *r = findRule(t.rule);
   if (!r || (r->target[0] && t.target.isEmpty())) return false;
   if (r->kind == Kind::Filter && t.extra.isEmpty()) return false;
   if (r->kind == Kind::Item) { bool ok; t.extra.toLongLong(&ok); if (!ok || t.extra.toLongLong() < 0) return false; }
   if (r->kind == Kind::Monitor && t.extra != "0" && t.extra != "1" && t.extra != "2") return false;
   if (r->kind == Kind::Track && (t.extra.size() != 1 || t.extra.toInt() < 1 || t.extra.toInt() > 6)) return false;
  }
  candidate.push_back(t);
 }
 Appearance appearance;
 if (root.contains("appearance")) {
  if (!root.value("appearance").isObject()) return false;
  const auto o = root.value("appearance").toObject();
  appearance.accent = o.value("accent").toString();
  appearance.fontSize = o.value("fontSize").toInt(11); appearance.rowHeight = o.value("rowHeight").toInt(42); appearance.refreshSeconds = o.value("refreshSeconds").toInt(1);
  if ((!appearance.accent.isEmpty() && !QColor(appearance.accent).isValid()) || appearance.fontSize < 9 || appearance.fontSize > 18 || appearance.rowHeight < 34 || appearance.rowHeight > 60 || appearance.refreshSeconds < 1 || appearance.refreshSeconds > 10) return false;
  if (!appearance.accent.isEmpty()) appearance.accent = QColor(appearance.accent).name();
 }
 out = std::move(candidate); ui = appearance; return true;
}
}
