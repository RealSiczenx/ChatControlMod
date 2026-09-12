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
#include "checks.hpp"

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void)
{ return "Offline native pre-stream checklist with automatic OBS checks and manual tasks."; }
MODULE_EXPORT const char *obs_module_name(void) { return "Todo Plugin"; }
MODULE_EXPORT const char *obs_module_author(void) { return "Todo Plugin contributors"; }

namespace {
constexpr auto dockId = "stream-checklist-native";
using checklist::State;
struct Rule { const char *id; const char *label; bool target; };
const Rule rules[] = {
    {"unmuted", "Audio input is unmuted", true},
    {"muted", "Audio input is muted", true},
    {"scene", "Program scene matches", true},
    {"record", "Recording is running (not paused)", false},
    {"replay", "Replay buffer is running", false},
    {"camera", "Virtual camera is running", false},
    {"studio", "Studio Mode is enabled", false},
    {"active", "Source is active in an output", true},
};
const Rule *findRule(const QString &id)
{ for (const auto &r : rules) if (id == QLatin1String(r.id)) return &r; return nullptr; }
struct Task { QString name, rule, target; bool manual = true; bool done = false; };
struct Result { State state; QString detail; };
QString sourceName(obs_source_t *s) { return QString::fromUtf8(obs_source_get_name(s)); }
Result evaluate(const Task &t)
{
    State state = State::Unknown;
    if (t.manual) state = checklist::boolean(t.done);
    else if (t.rule == "record") state = checklist::recording(obs_frontend_recording_active(), obs_frontend_recording_paused());
    else if (t.rule == "replay") state = checklist::boolean(obs_frontend_replay_buffer_active());
    else if (t.rule == "camera") state = checklist::boolean(obs_frontend_virtualcam_active());
    else if (t.rule == "studio") state = checklist::boolean(obs_frontend_preview_program_mode_active());
    else if (t.rule == "scene") {
        auto *s = obs_frontend_get_current_scene();
        if (!s) return {State::Unknown, "Unknown: no Program scene"};
        const QString current = sourceName(s);
        obs_source_release(s);
        state = checklist::boolean(current == t.target);
        if (state == State::NotDone) return {state, "Not done: current scene is " + current};
    } else if (t.rule == "unmuted" || t.rule == "muted" || t.rule == "active") {
        auto *s = obs_get_source_by_name(t.target.toUtf8().constData());
        if (!s) return {State::Unknown, "Unknown: source not found"};
        if (t.rule == "active") state = checklist::boolean(obs_source_active(s));
        else if (obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO)
            state = checklist::boolean(obs_source_muted(s), t.rule == "muted");
        obs_source_release(s);
        if (state == State::Unknown) return {state, "Unknown: source has no audio"};
    }
    return {state, state == State::Done ? "Done" : state == State::NotDone ? "Not done" : "Unknown: unsupported check"};
}

class Checklist final : public QWidget {
    std::vector<Task> tasks;
    QTableWidget *table;
    QLabel *summary, *message;
    QProgressBar *progress;
    QTimer timer;
    QString path;
    bool writable = true;

    QByteArray serialize() const
    {
        QJsonArray a;
        for (const auto &t : tasks) a.append(QJsonObject{{"name", t.name}, {"manual", t.manual}, {"done", t.done}, {"rule", t.rule}, {"target", t.target}});
        return QJsonDocument(QJsonObject{{"version", 1}, {"tasks", a}}).toJson();
    }
    bool parse(const QByteArray &bytes, std::vector<Task> &out)
    {
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
        const auto root = doc.object();
        if (root.value("version").toInt() != 1 || !root.value("tasks").isArray()) return false;
        const auto array = root.value("tasks").toArray();
        if (array.size() > 200) return false;
        for (const auto value : array) {
            if (!value.isObject()) return false;
            const auto o = value.toObject();
            if (!o.value("name").isString() || !o.value("manual").isBool() || !o.value("done").isBool()) return false;
            Task t{o.value("name").toString().trimmed(), o.value("rule").toString(), o.value("target").toString().trimmed(), o.value("manual").toBool(), o.value("done").toBool()};
            const auto *r = findRule(t.rule);
            if (t.name.isEmpty() || t.name.size() > 180 || t.target.size() > 200 || (!t.manual && (!r || (r->target && t.target.isEmpty())))) return false;
            out.push_back(t);
        }
        return true;
    }
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
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024 || !parse(file.readAll(), loaded)) {
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
            kind->setToolTip(t.manual ? "Tick this task yourself" : QString::fromUtf8(r->label) + (t.target.isEmpty() ? "" : ": " + t.target));
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
        QDialog dialog(this); dialog.setWindowTitle(index < 0 ? "Add task" : "Edit task"); dialog.resize(460, 300);
        QFormLayout form(&dialog);
        QComboBox type; type.addItems({"Manual - I tick it myself", "Automatic - OBS checks it"}); type.setCurrentIndex(old.manual ? 0 : 1);
        QLineEdit name(old.name); name.setMaxLength(180); name.setPlaceholderText("e.g. Post my stream link");
        QComboBox rule; for (const auto &r : rules) rule.addItem(r.label, r.id);
        if (!old.manual) rule.setCurrentIndex(rule.findData(old.rule));
        QComboBox target; target.setEditable(true); target.setInsertPolicy(QComboBox::NoInsert); target.lineEdit()->setMaxLength(200);
        QLabel help; help.setWordWrap(true); help.setTextFormat(Qt::PlainText);
        form.addRow("Task type", &type); form.addRow("Task name", &name); form.addRow("OBS check", &rule); form.addRow("OBS name", &target); form.addRow(&help);
        auto populate = [&] {
            const auto id = rule.currentData().toString();
            const QString previous = target.currentText(); target.clear();
            QStringList names;
            if (id == "scene") {
                obs_frontend_source_list list = {};
                obs_frontend_get_scenes(&list);
                for (size_t i = 0; i < list.sources.num; ++i) names.append(sourceName(list.sources.array[i]));
                obs_frontend_source_list_free(&list);
            } else {
                struct Context { QStringList *names; bool audio; } context{&names, id != "active"};
                obs_enum_sources([](void *data, obs_source_t *s) {
                    auto &c = *static_cast<Context *>(data);
                    if (!c.audio || (obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO)) c.names->append(sourceName(s));
                    return true;
                }, &context);
                // Global audio devices can be outside ordinary input enumeration.
                for (uint32_t channel = 0; channel < 6; ++channel) {
                    auto *s = obs_get_output_source(channel);
                    if (s) { if (!context.audio || (obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO)) names.append(sourceName(s)); obs_source_release(s); }
                }
            }
            names.removeDuplicates(); names.sort(Qt::CaseInsensitive); target.addItems(names); target.setEditText(previous);
        };
        auto fields = [&] {
            const bool automatic = type.currentIndex() == 1;
            const auto *r = findRule(rule.currentData().toString());
            rule.setEnabled(automatic); target.setEnabled(automatic && r && r->target);
            help.setText(!automatic ? "Type any task and tick it after you do it." : rule.currentData().toString() == "scene" ? "Matches the live Program scene, not Studio Mode preview." : rule.currentData().toString() == "active" ? "Active means OBS uses the source in an output. This does not prove that its picture is correct." : r && r->target ? "Choose or type the exact OBS name. Audio checks test only mute state, not sound or device health." : "Checks the current OBS state automatically once per second.");
        };
        populate(); target.setEditText(old.target); fields();
        connect(&type, &QComboBox::currentIndexChanged, &dialog, [&](int) { fields(); });
        connect(&rule, &QComboBox::currentIndexChanged, &dialog, [&](int) { populate(); fields(); });
        QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel); form.addRow(&buttons);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            const bool manual = type.currentIndex() == 0;
            const auto *r = findRule(rule.currentData().toString());
            if (name.text().trimmed().isEmpty() || (!manual && r && r->target && target.currentText().trimmed().isEmpty())) {
                QMessageBox::warning(&dialog, "Missing details", "Enter a task name and the required OBS name."); return;
            }
            dialog.accept();
        });
        if (dialog.exec() != QDialog::Accepted) return;
        Task task{name.text().trimmed(), rule.currentData().toString(), target.currentText().trimmed(), type.currentIndex() == 0, false};
        task.done = task.manual && old.manual && old.done;
        if (index < 0) tasks.push_back(task); else tasks.at(size_t(index)) = task;
        save(); rebuild();
    }
public:
    explicit Checklist(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(300, 260);
        auto *layout = new QVBoxLayout(this);
        auto *title = new QLabel("Todo Plugin"); QFont font = title->font(); font.setBold(true); font.setPointSize(font.pointSize() + 3); title->setFont(font); layout->addWidget(title);
        summary = new QLabel; layout->addWidget(summary);
        progress = new QProgressBar; progress->setTextVisible(false); progress->setMaximumHeight(8); layout->addWidget(progress);
        auto *actions = new QHBoxLayout;
        auto *add = new QPushButton("+ Add Task"); auto *reset = new QPushButton("New stream"); actions->addWidget(add); actions->addWidget(reset); layout->addLayout(actions);
        table = new QTableWidget(0, 3); table->setHorizontalHeaderLabels({"Task", "Type", "Status"}); table->verticalHeader()->hide();
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::SingleSelection); table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setWordWrap(false);
        layout->addWidget(table);
        auto *bottom = new QHBoxLayout; auto *editButton = new QPushButton("Edit"); auto *remove = new QPushButton("Delete"); bottom->addWidget(editButton); bottom->addWidget(remove); bottom->addStretch(); layout->addLayout(bottom);
        auto *backup = new QHBoxLayout; auto *exportButton = new QPushButton("Export backup"); auto *importButton = new QPushButton("Import backup"); backup->addWidget(exportButton); backup->addWidget(importButton); layout->addLayout(backup);
        message = new QLabel; message->setWordWrap(true); message->setTextFormat(Qt::PlainText); message->setStyleSheet("color: #e4b96a;"); layout->addWidget(message);
        auto *note = new QLabel("Offline checks every second. Manual ticks stay saved until New stream. This checklist does not block Start Streaming."); note->setWordWrap(true); layout->addWidget(note);
        load(); rebuild();
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
            QFile f(src); std::vector<Task> imported;
            if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024 || !parse(f.readAll(), imported)) { QMessageBox::warning(this, "Invalid backup", "This is not a valid native checklist backup."); return; }
            if (QMessageBox::question(this, "Replace checklist", "Replace the task list with this backup?") != QMessageBox::Yes) return;
            if (path.isEmpty()) { message->setText("OBS settings path unavailable. Restart OBS."); return; }
            if (!writable && QFileInfo::exists(path)) {
                const auto preserved = path + ".preserved-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
                if (!QFile::copy(path, preserved)) { message->setText("Cannot preserve the original settings file. Import cancelled."); return; }
            }
            writable = true; tasks = std::move(imported); save(); rebuild();
        });
        connect(&timer, &QTimer::timeout, this, &Checklist::refresh); timer.start(1000);
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
            cell->setForeground(QColor(ok ? "#65ca91" : result.state == State::NotDone ? "#ed8989" : "#e4b96a"));
            if (ok) ++done;
        }
        summary->setText(tasks.empty() ? "Add your first pre-stream task" : done == int(tasks.size()) ? "All tasks done - ready!" : QString("%1 / %2 done - unfinished tasks remain").arg(done).arg(qulonglong(tasks.size())));
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
