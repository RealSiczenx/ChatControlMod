// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QStringList>
#include <utility>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QUuid>
#include <vector>
#include "task-model.hpp"
#include <memory>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QColorDialog>
#include <QPalette>
#include <QToolButton>

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void)
{ return "Offline native pre-stream checklist with automatic OBS checks and manual tasks."; }
MODULE_EXPORT const char *obs_module_name(void) { return "Todo Plugin"; }
MODULE_EXPORT const char *obs_module_author(void) { return "Todo Plugin contributors"; }

namespace {
constexpr auto dockId = "stream-checklist-native";
using checklist::State;
using todo::Task;
using todo::Rule;
using todo::Kind;
using todo::rules;
using todo::findRule;
using Source = std::unique_ptr<obs_source_t, decltype(&obs_source_release)>;
Source getSource(const QString &name) { return Source(obs_get_source_by_name(name.toUtf8().constData()), obs_source_release); }
struct Result { State state; QString detail; };
QString sourceName(obs_source_t *s) { return QString::fromUtf8(obs_source_get_name(s)); }
Result evaluate(const Task &t)
{
    State state = State::Unknown; QString actual;
    if (t.manual) return {checklist::boolean(t.done), t.done ? "Done" : "Not done"};
    const auto *r = findRule(t.rule);
    if (!r) return {State::Unknown, "Unknown: unsupported check"};
    if (t.rule == "record") state = checklist::recording(obs_frontend_recording_active(), obs_frontend_recording_paused());
    else if (t.rule == "record_active") state = checklist::boolean(obs_frontend_recording_active());
    else if (t.rule == "record_paused") state = checklist::boolean(obs_frontend_recording_paused());
    else if (t.rule == "stream") state = checklist::boolean(obs_frontend_streaming_active());
    else if (t.rule == "replay") state = checklist::boolean(obs_frontend_replay_buffer_active());
    else if (t.rule == "camera") state = checklist::boolean(obs_frontend_virtualcam_active());
    else if (t.rule == "studio") state = checklist::boolean(obs_frontend_preview_program_mode_active());
    else if (t.rule == "preview_enabled") {
        if (obs_frontend_preview_program_mode_active()) return {State::Unknown, "Unknown: this check requires Studio Mode off"};
        state = checklist::boolean(obs_frontend_preview_enabled());
    } else if (t.rule == "scene" || t.rule == "preview_scene") {
        Source s(t.rule == "scene" ? obs_frontend_get_current_scene() : obs_frontend_get_current_preview_scene(), obs_source_release);
        if (!s) return {State::Unknown, "Unknown: no scene available (preview requires Studio Mode)"};
        actual = sourceName(s.get()); state = checklist::boolean(actual == t.target);
    } else if (t.rule == "profile" || t.rule == "collection") {
        char *name = t.rule == "profile" ? obs_frontend_get_current_profile() : obs_frontend_get_current_scene_collection();
        if (!name) return {State::Unknown, "Unknown: current workspace unavailable"};
        actual = QString::fromUtf8(name); bfree(name); state = checklist::boolean(actual == t.target);
    } else if (t.rule == "fps" || t.rule == "width" || t.rule == "height") {
        obs_video_info info = {};
        if (!obs_get_video_info(&info) || !info.fps_den) return {State::Unknown, "Unknown: video settings unavailable"};
        const double value = t.rule == "fps" ? double(info.fps_num) / info.fps_den : t.rule == "width" ? info.output_width : info.output_height;
        actual = QString::number(value, 'f', 2); state = checklist::numeric(value, t.threshold, t.comparison);
    } else {
        auto s = getSource(t.target);
        if (t.rule == "source_exists") state = checklist::boolean(bool(s));
        else if (!s) return {State::Unknown, "Unknown: source or scene not found"};
        else if (r->kind == Kind::Item) {
            auto *scene = obs_scene_from_source(s.get());
            if (!scene) return {State::Unknown, "Unknown: selected target is not a scene"};
            struct ItemState { int64_t id; QString name; std::optional<bool> value; bool visible; } itemState{t.extra.toLongLong(), {}, std::nullopt, t.rule == "visible"};
            obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
                auto &c = *static_cast<ItemState *>(data);
                if (obs_sceneitem_get_id(item) == c.id) {
                    c.name = sourceName(obs_sceneitem_get_source(item));
                    c.value = c.visible ? obs_sceneitem_visible(item) : obs_sceneitem_locked(item);
                    return false;
                }
                return true;
            }, &itemState);
            if (!itemState.value.has_value()) return {State::Unknown, "Unknown: scene item missing; edit and select it again"};
            actual = itemState.name + (*itemState.value ? ": ON" : ": OFF"); state = checklist::boolean(itemState.value);
        } else if (r->kind == Kind::Filter) {
            Source filter(obs_source_get_filter_by_name(s.get(), t.extra.toUtf8().constData()), obs_source_release);
            if (!filter) return {State::Unknown, "Unknown: filter not found"};
            state = checklist::boolean(obs_source_enabled(filter.get()));
        } else if (QString::fromUtf8(r->target) == "audio") {
            if (!(obs_source_get_output_flags(s.get()) & OBS_SOURCE_AUDIO)) return {State::Unknown, "Unknown: source has no audio"};
            if (t.rule == "unmuted" || t.rule == "muted") state = checklist::boolean(obs_source_muted(s.get()), t.rule == "muted");
            else if (t.rule == "volume" || t.rule == "sync") {
                const double value = t.rule == "volume" ? double(obs_source_get_volume(s.get())) * 100.0 : double(obs_source_get_sync_offset(s.get())) / 1000000.0;
                actual = QString::number(value, 'f', 2) + (t.rule == "volume" ? "%" : " ms"); state = checklist::numeric(value, t.threshold, t.comparison);
            } else if (t.rule == "monitor") {
                const int mode = int(obs_source_get_monitoring_type(s.get()));
                actual = mode == 0 ? "Monitor off" : mode == 1 ? "Monitor only" : "Monitor and output";
                state = checklist::boolean(mode == t.extra.toInt());
            } else if (t.rule == "track") state = checklist::boolean((obs_source_get_audio_mixers(s.get()) & (1u << (t.extra.toInt() - 1))) != 0);
        } else if (QString::fromUtf8(r->target) == "media") {
            if (!(obs_source_get_output_flags(s.get()) & OBS_SOURCE_CONTROLLABLE_MEDIA)) return {State::Unknown, "Unknown: source has no controllable media"};
            const auto media = obs_source_media_get_state(s.get());
            if (media == OBS_MEDIA_STATE_NONE || media == OBS_MEDIA_STATE_ERROR) return {State::Unknown, "Unknown: media unavailable or playback error"};
            state = checklist::boolean(media == (t.rule == "media_playing" ? OBS_MEDIA_STATE_PLAYING : t.rule == "media_paused" ? OBS_MEDIA_STATE_PAUSED : OBS_MEDIA_STATE_ENDED));
        } else if (t.rule == "active") state = checklist::boolean(obs_source_active(s.get()));
        else if (t.rule == "showing") state = checklist::boolean(obs_source_showing(s.get()));
        else if (t.rule == "enabled") state = checklist::boolean(obs_source_enabled(s.get()));
    }
    state = checklist::invert(state, t.expected);
    const QString label = state == State::Done ? "Done" : state == State::NotDone ? "Not done" : "Unknown";
    return {state, label + (actual.isEmpty() ? "" : " - current: " + actual)};
}
QStringList targetNames(const Rule &rule)
{
    const QString category = QString::fromUtf8(rule.target); QStringList names;
    if (category == "profile" || category == "collection") {
        char **list = category == "profile" ? obs_frontend_get_profiles() : obs_frontend_get_scene_collections();
        if (list) { for (size_t i = 0; list[i]; ++i) names.append(QString::fromUtf8(list[i])); bfree(list); }
    } else if (!category.isEmpty()) {
        if (category != "scene") {
            struct Context { QStringList *names; QString kind; } c{&names, category};
            obs_enum_sources([](void *data, obs_source_t *s) {
                auto &ctx = *static_cast<Context *>(data); const auto flags = obs_source_get_output_flags(s);
                if ((ctx.kind != "audio" || (flags & OBS_SOURCE_AUDIO)) && (ctx.kind != "media" || (flags & OBS_SOURCE_CONTROLLABLE_MEDIA))) ctx.names->append(sourceName(s));
                return true;
            }, &c);
            if (category == "audio" || category == "source") for (uint32_t channel = 0; channel < 6; ++channel) {
                Source s(obs_get_output_source(channel), obs_source_release);
                if (s && (category != "audio" || (obs_source_get_output_flags(s.get()) & OBS_SOURCE_AUDIO))) names.append(sourceName(s.get()));
            }
        }
        if (category == "scene" || category == "source") {
            obs_frontend_source_list list = {}; obs_frontend_get_scenes(&list);
            for (size_t i = 0; i < list.sources.num; ++i) names.append(sourceName(list.sources.array[i]));
            obs_frontend_source_list_free(&list);
        }
    }
    names.removeDuplicates(); names.sort(Qt::CaseInsensitive); return names;
}


class Checklist final : public QWidget {
    std::vector<Task> tasks;
    todo::Appearance appearance;
    QString baseStyle;
    QTableWidget *table;
    QLabel *summary, *message;
    QProgressBar *progress;
    QTimer timer;
    QString path;
    bool writable = true;

    QByteArray serialize() const { return todo::encode(tasks, appearance); }
    bool writeFile(const QString &dest, const QByteArray &data)
    {
        QSaveFile f(dest);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
            message->setText("Could not save: " + dest + ". Your changes are only in memory. Use Export backup.");
            return false;
        }
        return true;
    }
    void save()
    {
        if (!writable) return;
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            message->setText("Cannot create settings folder. Use Export backup to keep changes.");
            return;
        }
        if (writeFile(path, serialize())) message->clear();
    }
    void load()
    {
        char *p = obs_module_config_path("tasks.json");
        if (p) { path = QString::fromUtf8(p); bfree(p); }
        if (path.isEmpty()) { writable = false; message->setText("OBS settings path unavailable. Restart OBS."); return; }
        if (!QFileInfo::exists(path)) return;
        QFile file(path);
        std::vector<Task> loaded;
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024 || !todo::decode(file.readAll(), loaded, appearance)) {
            writable = false;
            message->setText("Saved tasks could not be read. Original file preserved at " + path + ". Import a valid backup to recover.");
            return;
        }
        tasks = std::move(loaded);
    }
    void rebuild()
    {
        QSignalBlocker block(table);
        table->setRowCount(0);
        for (const auto &t : tasks) {
            const int row = table->rowCount(); table->insertRow(row);
            auto *name = new QTableWidgetItem(t.name);
            name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | (t.manual ? Qt::ItemIsUserCheckable : Qt::NoItemFlags));
            if (t.manual) name->setCheckState(t.done ? Qt::Checked : Qt::Unchecked);
            name->setToolTip(t.name);
            table->setItem(row, 0, name);
            auto *kind = new QTableWidgetItem(t.manual ? "Manual" : "Auto");
            const auto *r = findRule(t.rule);
            QString condition;
            if (!t.manual && r) {
                condition = QString::fromUtf8(r->label) + (t.target.isEmpty() ? "" : " / " + t.target);
                if (!t.extra.isEmpty()) condition += " / " + t.extra;
                if (r->kind == Kind::Number) condition += QString(" %1 %2").arg(t.comparison == 0 ? ">=" : t.comparison == 1 ? "<=" : "=").arg(t.threshold);
                condition += t.expected ? " / required: Yes" : " / required: No";
            }
            kind->setToolTip(t.manual ? "Tick this task yourself" : condition);
            name->setToolTip(t.name + (condition.isEmpty() ? "" : "\n" + condition));
            kind->setTextAlignment(Qt::AlignCenter);
            table->setItem(row, 1, kind);
            table->setItem(row, 2, new QTableWidgetItem);
        }
        refresh();
    }
    void edit(int index)
    {
        if (!writable) { QMessageBox::warning(this, "Settings unavailable", message->text()); return; }
        if (index < 0 && tasks.size() >= 200) { QMessageBox::information(this, "Checklist full", "Maximum 200 tasks."); return; }
        const Task old = index >= 0 ? tasks.at(size_t(index)) : Task{};
        QDialog dialog(this); dialog.setWindowTitle(index < 0 ? "Add task" : "Edit task"); dialog.resize(520, 440);
        QFormLayout form(&dialog); form.setContentsMargins(20, 20, 20, 20); form.setHorizontalSpacing(14); form.setVerticalSpacing(12);
        QComboBox type; type.addItems({"Manual - I tick it myself", "Automatic - OBS checks it"}); type.setCurrentIndex(old.manual ? 0 : 1);
        QLineEdit name(old.name); name.setMaxLength(180); name.setPlaceholderText("Give your task a clear name");
        QComboBox rule; for (const auto &r : rules) rule.addItem(r.label, r.id); rule.setMaxVisibleItems(14);
        if (!old.manual) rule.setCurrentIndex(rule.findData(old.rule));
        QComboBox target; target.setEditable(true); target.setInsertPolicy(QComboBox::NoInsert); target.lineEdit()->setMaxLength(200);
        QComboBox extra;
        QComboBox expected; expected.addItem("Yes / ON / matches", true); expected.addItem("No / OFF / does not match", false); expected.setCurrentIndex(old.expected ? 0 : 1);
        QComboBox comparison; comparison.addItems({"At least (>=)", "At most (<=)", "Equals"}); comparison.setCurrentIndex(old.comparison);
        QDoubleSpinBox threshold; threshold.setRange(-1000000, 1000000); threshold.setDecimals(2); threshold.setValue(old.threshold);
        QLabel help; help.setWordWrap(true); help.setTextFormat(Qt::PlainText); help.setMinimumWidth(300);
        form.addRow("Task type", &type); form.addRow("Task name", &name); form.addRow("Automatic check", &rule);
        form.addRow("OBS target", &target); form.addRow("Filter / item / mode", &extra);
        form.addRow("Comparison", &comparison); form.addRow("Value", &threshold); form.addRow("Required result", &expected);
        form.addRow(&help);
        auto fillExtras = [&](const QString &preserve) {
            QSignalBlocker block(extra); extra.clear(); const auto *r = findRule(rule.currentData().toString());
            extra.setEditable(r && r->kind == Kind::Filter); if (extra.isEditable()) { extra.setInsertPolicy(QComboBox::NoInsert); extra.lineEdit()->setMaxLength(200); }
            if (!r) return;
            if (r->kind == Kind::Monitor) { extra.addItem("Monitor off", "0"); extra.addItem("Monitor only (mute output)", "1"); extra.addItem("Monitor and output", "2"); }
            else if (r->kind == Kind::Track) { for (int i = 1; i <= 6; ++i) extra.addItem(QString("Track %1").arg(i), QString::number(i)); }
            else if (r->kind == Kind::Filter || r->kind == Kind::Item) {
                auto s = getSource(target.currentText());
                if (s && r->kind == Kind::Filter) obs_source_enum_filters(s.get(), [](obs_source_t *, obs_source_t *filter, void *data) {
                    const auto name = sourceName(filter); static_cast<QComboBox *>(data)->addItem(name, name);
                }, &extra);
                else if (s) {
                    auto *scene = obs_scene_from_source(s.get());
                    if (scene) obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
                        const auto id = QString::number(obs_sceneitem_get_id(item));
                        static_cast<QComboBox *>(data)->addItem(sourceName(obs_sceneitem_get_source(item)) + " (#" + id + ")", id); return true;
                    }, &extra);
                }
            }
            if (!preserve.isEmpty()) {
                const int found = extra.findData(preserve);
                if (found >= 0) extra.setCurrentIndex(found);
                else if (r->kind == Kind::Filter) extra.setEditText(preserve);
                else if (r->kind == Kind::Item) { extra.addItem("Unavailable item (#" + preserve + ")", preserve); extra.setCurrentIndex(extra.count() - 1); }
            }
        };
        auto fillTargets = [&](const QString &preserve) {
            QSignalBlocker block(target); target.clear(); const auto *r = findRule(rule.currentData().toString());
            if (r) target.addItems(targetNames(*r));
            if (!preserve.isEmpty()) target.setEditText(preserve);
        };
        auto fields = [&] {
            const bool automatic = type.currentIndex() == 1; const auto *r = findRule(rule.currentData().toString());
            form.setRowVisible(&rule, automatic);
            form.setRowVisible(&target, automatic && r && r->target[0]);
            form.setRowVisible(&extra, automatic && r && (r->kind == Kind::Filter || r->kind == Kind::Item || r->kind == Kind::Monitor || r->kind == Kind::Track));
            form.setRowVisible(&comparison, automatic && r && r->kind == Kind::Number);
            form.setRowVisible(&threshold, automatic && r && r->kind == Kind::Number);
            form.setRowVisible(&expected, automatic);
            if (r) {
                const QString category = QString::fromUtf8(r->target);
                static_cast<QLabel *>(form.labelForField(&target))->setText(category == "scene" ? "Scene" : category == "profile" ? "Profile" : category == "collection" ? "Scene collection" : "Source");
                static_cast<QLabel *>(form.labelForField(&extra))->setText(r->kind == Kind::Filter ? "Filter" : r->kind == Kind::Item ? "Scene item" : r->kind == Kind::Track ? "Audio track" : "Monitoring mode");
                threshold.setSuffix(rule.currentData() == QVariant("volume") ? " %" : rule.currentData() == QVariant("sync") ? " ms" : rule.currentData() == QVariant("fps") ? " fps" : " px");
            }
            help.setText(!automatic ? "Type any task, then tick it after you do it." : r ? QString::fromUtf8(r->help) + "\nUnknown never counts as done, even when you require No / OFF." : "Choose an automatic check.");
        };
        fillTargets(old.target); fillExtras(old.extra); fields();
        connect(&type, &QComboBox::currentIndexChanged, &dialog, [&](int) { fields(); });
        connect(&rule, &QComboBox::currentIndexChanged, &dialog, [&](int) {
            fillTargets({}); fillExtras({}); fields();
            const QString id = rule.currentData().toString();
            threshold.setValue(id == "volume" ? 100.0 : id == "fps" ? 60.0 : id == "width" ? 1920.0 : id == "height" ? 1080.0 : 0.0);
        });
        connect(&target, &QComboBox::currentTextChanged, &dialog, [&](const QString &) { fillExtras({}); });
        QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel); form.addRow(&buttons);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            const bool manual = type.currentIndex() == 0; const auto *r = findRule(rule.currentData().toString());
            const bool extraRequired = r && (r->kind == Kind::Item || r->kind == Kind::Monitor || r->kind == Kind::Track);
            if (name.text().trimmed().isEmpty() || (!manual && (!r || (r->target[0] && target.currentText().trimmed().isEmpty()) || (r->kind == Kind::Filter && extra.currentText().trimmed().isEmpty()) || (extraRequired && !extra.currentData().isValid())))) {
                QMessageBox::warning(&dialog, "Missing details", "Enter a task name and choose all required targets."); return;
            }
            dialog.accept();
        });
        if (dialog.exec() != QDialog::Accepted) return;
        Task task{name.text().trimmed(), rule.currentData().toString(), target.currentText().trimmed(), type.currentIndex() == 0, false};
        const auto *r = findRule(task.rule);
        task.extra = r && r->kind == Kind::Filter ? extra.currentText().trimmed() : extra.currentData().toString();
        task.expected = expected.currentData().toBool(); task.threshold = threshold.value(); task.comparison = comparison.currentIndex();
        task.done = task.manual && old.manual && old.done;
        if (index < 0) tasks.push_back(task); else tasks.at(size_t(index)) = task;
        save(); rebuild();
    }
    void applyAppearance()
    {
        QFont body = font(); body.setPointSize(appearance.fontSize); setFont(body);
        if (auto *title = findChild<QLabel *>("TodoTitle")) { QFont f = body; f.setBold(true); f.setPointSize(appearance.fontSize + 3); title->setFont(f); }
        for (auto *label : findChildren<QLabel *>()) if (label->property("caption").toBool()) { QFont f = body; f.setPointSize(qMax(9, appearance.fontSize - 1)); label->setFont(f); }
        QString style = baseStyle;
        if (!appearance.accent.isEmpty()) {
            const QColor accent(appearance.accent); const QString ink = accent.lightness() > 150 ? "#121620" : "#ffffff";
            style += QString("QWidget#TodoDock QPushButton#AddTask { background: %1; border-color: %1; color: %2; } QWidget#TodoDock QProgressBar::chunk { background: %1; } QWidget#TodoDock QPushButton:hover { border-color: %1; }").arg(accent.name(), ink);
        }
        setStyleSheet(style);
        table->verticalHeader()->setDefaultSectionSize(qMax(appearance.rowHeight, appearance.fontSize * 2 + 12));
        timer.setInterval(appearance.refreshSeconds * 1000);
    }
    void customize()
    {
        if (!writable) { QMessageBox::warning(this, "Settings unavailable", message->text()); return; }
        QDialog dialog(this); dialog.setWindowTitle("Customize Todo Plugin");
        QFormLayout form(&dialog); form.setContentsMargins(20, 20, 20, 20); form.setSpacing(12);
        QString color = appearance.accent;
        QPushButton choose(color.isEmpty() ? "Use OBS theme" : color);
        QPushButton theme("Reset color to OBS theme");
        QSpinBox size; size.setRange(9, 18); size.setValue(appearance.fontSize); size.setSuffix(" pt");
        QSpinBox spacing; spacing.setRange(34, 60); spacing.setValue(appearance.rowHeight); spacing.setSuffix(" px");
        QSpinBox interval; interval.setRange(1, 10); interval.setValue(appearance.refreshSeconds); interval.setSuffix(" seconds");
        form.addRow("Accent color", &choose); form.addRow(&theme); form.addRow("Text size", &size); form.addRow("Row spacing", &spacing); form.addRow("Refresh interval", &interval);
        connect(&choose, &QPushButton::clicked, &dialog, [&] {
            const QColor selected = QColorDialog::getColor(color.isEmpty() ? palette().color(QPalette::Highlight) : QColor(color), &dialog, "Choose accent color");
            if (selected.isValid()) { color = selected.name(); choose.setText(color); }
        });
        connect(&theme, &QPushButton::clicked, &dialog, [&] { color.clear(); choose.setText("Use OBS theme"); });
        QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel); form.addRow(&buttons);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        appearance = {color, size.value(), spacing.value(), interval.value()}; applyAppearance(); save(); rebuild();
    }
    void moveTask(int direction)
    {
        const int row = table->currentRow(), next = row + direction;
        if (!writable || row < 0 || next < 0 || next >= int(tasks.size())) return;
        std::swap(tasks[size_t(row)], tasks[size_t(next)]); save(); rebuild(); table->selectRow(next);
    }

public:
    explicit Checklist(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(320, 300);
        setObjectName("TodoDock");
        baseStyle = QString::fromUtf8(R"(
            QWidget#TodoDock { background: palette(window); }
            QWidget#TodoDock QPushButton {
                padding: 7px 12px; min-height: 18px;
                border: 1px solid palette(mid); border-radius: 6px;
                background: palette(button); color: palette(button-text);
            }
            QWidget#TodoDock QPushButton:hover { border-color: palette(highlight); }
            QWidget#TodoDock QPushButton:pressed { background: palette(mid); }
            QWidget#TodoDock QPushButton:focus { border: 2px solid palette(highlight); padding: 6px 11px; }
            QWidget#TodoDock QPushButton#AddTask {
                background: palette(highlight); color: palette(highlighted-text);
                border-color: palette(highlight); font-weight: 600;
            }
            QWidget#TodoDock QTableWidget {
                border: 1px solid palette(mid); border-radius: 8px;
                background: palette(base); alternate-background-color: palette(alternate-base);
                selection-background-color: palette(highlight);
                selection-color: palette(highlighted-text); outline: 0;
            }
            QWidget#TodoDock QTableWidget::item { padding: 6px; border: none; }
            QWidget#TodoDock QHeaderView::section {
                background: palette(window); color: palette(text);
                border: none; border-bottom: 1px solid palette(mid);
                padding: 8px 6px; font-weight: 600;
            }
            QWidget#TodoDock QProgressBar {
                border: none; border-radius: 3px; background: palette(mid);
            }
            QWidget#TodoDock QProgressBar::chunk { border-radius: 3px; background: palette(highlight); }
        )");
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 16, 16, 12); layout->setSpacing(12);
        auto *title = new QLabel("Todo Plugin"); title->setObjectName("TodoTitle"); QFont font = title->font(); font.setBold(true); font.setPointSize(font.pointSize() + 3); title->setFont(font); layout->addWidget(title);
        auto *subtitle = new QLabel("Your pre-stream checklist"); subtitle->setProperty("caption", true);
        layout->addWidget(subtitle);
        summary = new QLabel; summary->setWordWrap(true); layout->addWidget(summary);
        progress = new QProgressBar; progress->setTextVisible(false); progress->setFixedHeight(6); layout->addWidget(progress);
        auto *actions = new QHBoxLayout;
        auto *add = new QPushButton("+ Add Task"); add->setObjectName("AddTask"); auto *reset = new QPushButton("New stream"); actions->addWidget(add); actions->addWidget(reset); layout->addLayout(actions);
        table = new QTableWidget(0, 3); table->setHorizontalHeaderLabels({"Task", "Type", "Status"}); table->verticalHeader()->hide();
        table->setShowGrid(false); table->setAlternatingRowColors(true);
        table->verticalHeader()->setDefaultSectionSize(42);
        table->horizontalHeader()->setHighlightSections(false);
        table->horizontalHeader()->setMinimumSectionSize(55);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::SingleSelection); table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setWordWrap(false);
        layout->addWidget(table);
        auto *bottom = new QHBoxLayout; auto *editButton = new QPushButton("Edit"); auto *remove = new QPushButton("Delete"); bottom->addWidget(editButton); bottom->addWidget(remove); bottom->addStretch();
        auto *up = new QToolButton; auto *down = new QToolButton; up->setArrowType(Qt::UpArrow); down->setArrowType(Qt::DownArrow); up->setFixedSize(32, 32); down->setFixedSize(32, 32); bottom->addWidget(up); bottom->addWidget(down);
        up->setToolTip("Move selected task up"); down->setToolTip("Move selected task down");
        connect(up, &QToolButton::clicked, this, [this] { moveTask(-1); });
        connect(down, &QToolButton::clicked, this, [this] { moveTask(1); });
        layout->addLayout(bottom);
        auto *backup = new QHBoxLayout; auto *exportButton = new QPushButton("Export backup"); auto *importButton = new QPushButton("Import backup"); backup->addWidget(exportButton); backup->addWidget(importButton); layout->addLayout(backup);
        message = new QLabel; message->setWordWrap(true); message->setTextFormat(Qt::PlainText); message->setStyleSheet("color: #e4b96a;"); layout->addWidget(message);
        auto *note = new QLabel("Select a task to edit or reorder it"); note->setProperty("caption", true);
        note->setToolTip("Manual ticks stay saved until New stream. This checklist does not block Start Streaming.");
        note->setWordWrap(true);
        QFont smallFont = note->font(); smallFont.setPointSizeF(qMax(8.0, smallFont.pointSizeF() - 1.0));
        note->setFont(smallFont); subtitle->setFont(smallFont);
        layout->addWidget(note);
        auto *custom = new QPushButton("Customize"); layout->addWidget(custom);
        connect(custom, &QPushButton::clicked, this, [this] { customize(); });
        load(); applyAppearance(); rebuild();
        connect(add, &QPushButton::clicked, this, [this] { edit(-1); });
        connect(editButton, &QPushButton::clicked, this, [this] { if (table->currentRow() >= 0) edit(table->currentRow()); });
        connect(remove, &QPushButton::clicked, this, [this] {
            const int row = table->currentRow(); if (!writable || row < 0) return;
            if (QMessageBox::question(this, "Delete task", "Delete the selected task?") != QMessageBox::Yes) return;
            tasks.erase(tasks.begin() + row); save(); rebuild();
        });
        connect(reset, &QPushButton::clicked, this, [this] {
            if (!writable || QMessageBox::question(this, "New stream", "Clear all manual ticks? Your task list stays saved.") != QMessageBox::Yes) return;
            for (auto &t : tasks) t.done = false;
            save(); rebuild();
        });
        connect(table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
            if (item->column() != 0) return;
            auto &t = tasks.at(size_t(item->row()));
            if (!writable) { QSignalBlocker block(table); item->setCheckState(t.done ? Qt::Checked : Qt::Unchecked); return; }
            if (t.manual) { t.done = item->checkState() == Qt::Checked; save(); refresh(); }
        });
        connect(exportButton, &QPushButton::clicked, this, [this] {
            const auto dest = QFileDialog::getSaveFileName(this, "Export checklist", "stream-checklist.json", "JSON files (*.json)");
            if (!dest.isEmpty() && writeFile(dest, serialize())) message->setText("Backup exported.");
        });
        connect(importButton, &QPushButton::clicked, this, [this] {
            const auto src = QFileDialog::getOpenFileName(this, "Import checklist", {}, "JSON files (*.json)"); if (src.isEmpty()) return;
            QFile f(src); std::vector<Task> imported; todo::Appearance importedAppearance;
            if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024 || !todo::decode(f.readAll(), imported, importedAppearance)) { QMessageBox::warning(this, "Invalid backup", "This is not a valid native checklist backup."); return; }
            if (QMessageBox::question(this, "Replace checklist", "Replace the task list with this backup?") != QMessageBox::Yes) return;
            if (path.isEmpty()) { message->setText("OBS settings path unavailable. Restart OBS."); return; }
            if (!writable && QFileInfo::exists(path)) {
                const auto preserved = path + ".preserved-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
                if (!QFile::copy(path, preserved)) { message->setText("Cannot preserve the original settings file. Import cancelled."); return; }
            }
            writable = true; tasks = std::move(imported); appearance = importedAppearance; applyAppearance(); save(); rebuild();
        });
        connect(&timer, &QTimer::timeout, this, &Checklist::refresh); timer.start(appearance.refreshSeconds * 1000);
    }
    void stop() { timer.stop(); }
    void refresh()
    {
        QSignalBlocker block(table);
        int done = 0;
        for (size_t i = 0; i < tasks.size(); ++i) {
            const auto result = evaluate(tasks[i]);
            auto *cell = table->item(int(i), 2);
            const bool ok = result.state == State::Done;
            cell->setText(ok ? "Done" : result.state == State::NotDone ? "Not done" : "Unknown");
            cell->setToolTip(result.detail);
            cell->setTextAlignment(Qt::AlignCenter);
            QFont statusFont = table->font(); statusFont.setWeight(QFont::DemiBold); cell->setFont(statusFont);
            const bool dark = palette().color(QPalette::Base).lightness() < 128;
            cell->setForeground(QColor(ok ? (dark ? "#65ca91" : "#187445") : result.state == State::NotDone ? (dark ? "#ed8989" : "#b32936") : (dark ? "#e4b96a" : "#8b5c08")));
            if (ok) ++done;
        }
        summary->setText(tasks.empty() ? "Add your first pre-stream task" : done == int(tasks.size()) ? "All tasks complete. Ready to stream." : QString("%1 of %2 tasks complete").arg(done).arg(qulonglong(tasks.size())));
        progress->setRange(0, tasks.empty() ? 1 : int(tasks.size())); progress->setValue(done);
    }
};
QPointer<Checklist> dock;
bool exited = false;
void frontendEvent(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_EXIT) {
        exited = true;
        if (dock) dock->stop();
    }
}
}
bool obs_module_load(void) { return true; }
void obs_module_post_load(void)
{
    auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
    if (!main) { blog(LOG_ERROR, "[stream-checklist] OBS main window unavailable"); return; }
    dock = new Checklist(main);
    if (!obs_frontend_add_dock_by_id(dockId, "Todo Plugin", dock.data())) {
        delete dock.data();
        blog(LOG_ERROR, "[stream-checklist] Could not register dock"); return;
    }
    obs_frontend_add_event_callback(frontendEvent, nullptr);
}
void obs_module_unload(void)
{
    if (!exited) {
        obs_frontend_remove_event_callback(frontendEvent, nullptr);
        if (dock) { dock->stop(); obs_frontend_remove_dock(dockId); }
    }
    if (dock) delete dock.data();
}
