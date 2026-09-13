#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <obs-audio-controls.h>

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_name(void)
{
    return "Vertical Audio Mixer Dock";
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Adds one separate audio mixer dock for Aitum Vertical/non-main OBS canvases while leaving the normal OBS mixer untouched.";
}

namespace {

static constexpr const char *kDockId = "VerticalAudioMixer.Dock";
static constexpr const char *kDockTitle = "Vertical Audio Mixer";

static bool source_has_audio(obs_source_t *source)
{
    if (!source || obs_obj_is_private(source))
        return false;
    if (obs_source_is_scene(source) || obs_source_is_group(source))
        return false;
    return (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0;
}

static QString source_uuid(obs_source_t *source)
{
    const char *uuid = source ? obs_source_get_uuid(source) : nullptr;
    return QString::fromUtf8(uuid ? uuid : "");
}

static QString source_name(obs_source_t *source)
{
    const char *name = source ? obs_source_get_name(source) : nullptr;
    return QString::fromUtf8(name ? name : "");
}

class LevelBar final : public QWidget {
public:
    explicit LevelBar(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(8);
        setMaximumHeight(8);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        timer_.setInterval(33);
        connect(&timer_, &QTimer::timeout, this, [this] {
            const float target = peakDb_.load(std::memory_order_relaxed);
            displayDb_ = std::max(target, displayDb_ - 1.6f);
            if (!std::isfinite(displayDb_))
                displayDb_ = -60.0f;
            update();
        });
        timer_.start();
    }

    void setPeakDb(float db)
    {
        if (!std::isfinite(db))
            db = -60.0f;
        peakDb_.store(std::clamp(db, -60.0f, 6.0f), std::memory_order_relaxed);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = rect();

        QColor bg = palette().color(QPalette::Mid);
        bg.setAlpha(90);
        p.fillRect(r, bg);

        const float norm = std::clamp((displayDb_ + 60.0f) / 60.0f, 0.0f, 1.0f);
        const int fillW = static_cast<int>(std::round(norm * r.width()));

        QColor fill;
        if (displayDb_ >= -9.0f)
            fill = QColor(225, 80, 80);
        else if (displayDb_ >= -20.0f)
            fill = QColor(220, 190, 65);
        else
            fill = QColor(70, 190, 100);

        p.fillRect(QRect(r.left(), r.top(), fillW, r.height()), fill);
    }

private:
    std::atomic<float> peakDb_{-60.0f};
    float displayDb_ = -60.0f;
    QTimer timer_;
};

class AudioStrip final : public QFrame {
public:
    explicit AudioStrip(obs_source_t *source, QWidget *parent = nullptr) : QFrame(parent)
    {
        setFrameShape(QFrame::StyledPanel);
        setFrameShadow(QFrame::Plain);
        setMinimumHeight(62);

        uuid_ = source_uuid(source);
        weak_ = obs_source_get_weak_source(source);

        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(7, 5, 7, 5);
        outer->setSpacing(4);

        auto *top = new QHBoxLayout;
        top->setSpacing(5);

        name_ = new QLabel(source_name(source), this);
        name_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        top->addWidget(name_, 1);

        mute_ = new QPushButton(QStringLiteral("M"), this);
        mute_->setCheckable(true);
        mute_->setFixedSize(26, 22);
        mute_->setToolTip(QStringLiteral("Mute"));
        mute_->setChecked(obs_source_muted(source));
        connect(mute_, &QPushButton::toggled, this, [this](bool checked) {
            if (syncing_)
                return;
            withSource([checked](obs_source_t *s) { obs_source_set_muted(s, checked); });
        });
        top->addWidget(mute_);

        auto *menuButton = new QToolButton(this);
        menuButton->setText(QStringLiteral("⋮"));
        menuButton->setPopupMode(QToolButton::InstantPopup);
        menuButton->setFixedSize(26, 22);
        auto *menu = new QMenu(menuButton);

        QAction *filters = menu->addAction(QStringLiteral("Filters"));
        connect(filters, &QAction::triggered, this, [this] {
            withSource([](obs_source_t *s) { obs_frontend_open_source_filters(s); });
        });

        QAction *properties = menu->addAction(QStringLiteral("Properties"));
        connect(properties, &QAction::triggered, this, [this] {
            withSource([](obs_source_t *s) { obs_frontend_open_source_properties(s); });
        });

        menuButton->setMenu(menu);
        top->addWidget(menuButton);
        outer->addLayout(top);

        auto *bottom = new QHBoxLayout;
        bottom->setSpacing(6);

        slider_ = new QSlider(Qt::Horizontal, this);
        slider_->setRange(-600, 100);
        slider_->setSingleStep(5);
        slider_->setPageStep(20);
        slider_->setToolTip(QStringLiteral("Volume"));
        setSliderFromSource(source);
        connect(slider_, &QSlider::valueChanged, this, [this](int value) {
            if (syncing_)
                return;
            const float db = static_cast<float>(value) / 10.0f;
            withSource([db](obs_source_t *s) { obs_source_set_volume(s, obs_db_to_mul(db)); });
        });
        bottom->addWidget(slider_, 1);

        dbLabel_ = new QLabel(this);
        dbLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        dbLabel_->setMinimumWidth(52);
        bottom->addWidget(dbLabel_);
        outer->addLayout(bottom);

        meter_ = new LevelBar(this);
        outer->addWidget(meter_);

        volmeter_ = obs_volmeter_create(OBS_FADER_LOG);
        if (volmeter_) {
            obs_volmeter_set_peak_meter_type(volmeter_, SAMPLE_PEAK_METER);
            obs_volmeter_add_callback(volmeter_, &AudioStrip::meterCallback, this);
            obs_volmeter_attach_source(volmeter_, source);
        }

        syncTimer_.setInterval(400);
        connect(&syncTimer_, &QTimer::timeout, this, [this] { syncUi(); });
        syncTimer_.start();
        syncUi();
    }

    ~AudioStrip() override
    {
        syncTimer_.stop();
        if (volmeter_) {
            obs_volmeter_remove_callback(volmeter_, &AudioStrip::meterCallback, this);
            obs_volmeter_detach_source(volmeter_);
            obs_volmeter_destroy(volmeter_);
            volmeter_ = nullptr;
        }
        if (weak_) {
            obs_weak_source_release(weak_);
            weak_ = nullptr;
        }
    }

    QString uuid() const { return uuid_; }

private:
    template<typename Fn>
    void withSource(Fn &&fn)
    {
        if (!weak_)
            return;
        obs_source_t *source = obs_weak_source_get_source(weak_);
        if (!source)
            return;
        fn(source);
        obs_source_release(source);
    }

    void setSliderFromSource(obs_source_t *source)
    {
        float db = obs_mul_to_db(obs_source_get_volume(source));
        if (!std::isfinite(db))
            db = -60.0f;
        const int value = std::clamp(static_cast<int>(std::lround(db * 10.0f)), -600, 100);
        slider_->setValue(value);
        dbLabel_->setText(QString::number(db, 'f', 1) + QStringLiteral(" dB"));
    }

    void syncUi()
    {
        withSource([this](obs_source_t *source) {
            syncing_ = true;
            name_->setText(source_name(source));
            mute_->setChecked(obs_source_muted(source));
            setSliderFromSource(source);
            syncing_ = false;
        });
    }

    static void meterCallback(void *param, const float[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
                              const float[MAX_AUDIO_CHANNELS])
    {
        auto *self = static_cast<AudioStrip *>(param);
        if (!self)
            return;

        float highest = -60.0f;
        for (size_t i = 0; i < MAX_AUDIO_CHANNELS; ++i) {
            if (std::isfinite(peak[i]))
                highest = std::max(highest, peak[i]);
        }

        if (self->meter_)
            self->meter_->setPeakDb(highest);
    }

    QString uuid_;
    obs_weak_source_t *weak_ = nullptr;
    obs_volmeter_t *volmeter_ = nullptr;
    QLabel *name_ = nullptr;
    QPushButton *mute_ = nullptr;
    QSlider *slider_ = nullptr;
    QLabel *dbLabel_ = nullptr;
    LevelBar *meter_ = nullptr;
    QTimer syncTimer_;
    bool syncing_ = false;
};

class VerticalMixer final : public QWidget {
public:
    explicit VerticalMixer(QWidget *parent = nullptr) : QWidget(parent)
    {
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(4, 4, 4, 4);
        root->setSpacing(4);

        auto *header = new QHBoxLayout;
        auto *title = new QLabel(QStringLiteral("VERTICAL"), this);
        QFont font = title->font();
        font.setBold(true);
        title->setFont(font);
        header->addWidget(title);

        count_ = new QLabel(this);
        count_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        header->addWidget(count_, 1);
        root->addLayout(header);

        auto *line = new QFrame(this);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        root->addWidget(line);

        auto *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);

        content_ = new QWidget(scroll);
        layout_ = new QVBoxLayout(content_);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(4);
        layout_->addStretch(1);
        scroll->setWidget(content_);
        root->addWidget(scroll, 1);

        empty_ = new QLabel(QStringLiteral("No vertical-canvas audio sources found"), content_);
        empty_->setAlignment(Qt::AlignCenter);
        empty_->setWordWrap(true);
        layout_->insertWidget(0, empty_);
    }

    void sync(const std::vector<obs_source_t *> &sources)
    {
        QHash<QString, obs_source_t *> desired;
        for (obs_source_t *source : sources)
            desired.insert(source_uuid(source), source);

        const auto existing = strips_.keys();
        for (const QString &uuid : existing) {
            if (desired.contains(uuid))
                continue;
            AudioStrip *strip = strips_.take(uuid);
            layout_->removeWidget(strip);
            delete strip;
        }

        std::vector<obs_source_t *> sorted = sources;
        std::sort(sorted.begin(), sorted.end(), [](obs_source_t *a, obs_source_t *b) {
            return source_name(a).localeAwareCompare(source_name(b)) < 0;
        });

        int row = 0;
        for (obs_source_t *source : sorted) {
            const QString uuid = source_uuid(source);
            AudioStrip *strip = strips_.value(uuid, nullptr);
            if (!strip) {
                strip = new AudioStrip(source, content_);
                strips_.insert(uuid, strip);
            }
            layout_->removeWidget(strip);
            layout_->insertWidget(row++, strip);
        }

        empty_->setVisible(sorted.empty());
        const int count = static_cast<int>(sorted.size());
        count_->setText(QStringLiteral("%1 source%2").arg(count).arg(count == 1 ? "" : "s"));
    }

private:
    QLabel *count_ = nullptr;
    QWidget *content_ = nullptr;
    QVBoxLayout *layout_ = nullptr;
    QLabel *empty_ = nullptr;
    QHash<QString, AudioStrip *> strips_;
};

struct ScanState {
    obs_canvas_t *mainCanvas = nullptr;
    QSet<QString> verticalMembers;
};

static void mark_source_recursive(obs_source_t *source, ScanState *state)
{
    if (!source || !state)
        return;

    const QString uuid = source_uuid(source);
    if (!uuid.isEmpty()) {
        if (state->verticalMembers.contains(uuid))
            return;
        state->verticalMembers.insert(uuid);
    }

    obs_scene_t *scene = obs_group_or_scene_from_source(source);
    if (!scene)
        return;

    obs_scene_enum_items(
        scene,
        [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
            auto *scan = static_cast<ScanState *>(param);
            if (!item || !scan)
                return true;
            obs_source_t *child = obs_sceneitem_get_source(item);
            if (child)
                mark_source_recursive(child, scan);
            return true;
        },
        state);
}

static bool enum_vertical_canvas(void *param, obs_canvas_t *canvas)
{
    auto *state = static_cast<ScanState *>(param);
    if (!state || !canvas || canvas == state->mainCanvas)
        return true;

    obs_canvas_enum_scenes(
        canvas,
        [](void *innerParam, obs_source_t *sceneSource) {
            auto *scan = static_cast<ScanState *>(innerParam);
            if (sceneSource)
                mark_source_recursive(sceneSource, scan);
            return true;
        },
        state);

    return true;
}

static bool collect_vertical_audio(void *param, obs_source_t *source)
{
    auto *ctx = static_cast<std::pair<ScanState *, std::vector<obs_source_t *> *> *>(param);
    if (!ctx || !source || !source_has_audio(source))
        return true;

    const QString uuid = source_uuid(source);
    if (uuid.isEmpty() || !ctx->first->verticalMembers.contains(uuid))
        return true;

    if (obs_source_t *ref = obs_source_get_ref(source))
        ctx->second->push_back(ref);
    return true;
}

class Controller final : public QObject {
public:
    Controller()
    {
        verticalDock_ = new VerticalMixer;
        dockAdded_ = obs_frontend_add_dock_by_id(kDockId, kDockTitle, verticalDock_);
        if (!dockAdded_) {
            blog(LOG_WARNING, "[Vertical Audio Mixer] Could not add dock (duplicate id?)");
            delete verticalDock_.data();
            verticalDock_ = nullptr;
            return;
        }

        refreshTimer_.setInterval(1000);
        connect(&refreshTimer_, &QTimer::timeout, this, [this] { refresh(); });
        refreshTimer_.start();

        obs_frontend_add_event_callback(&Controller::frontendEvent, this);
        refresh();
    }

    ~Controller() override
    {
        refreshTimer_.stop();
        obs_frontend_remove_event_callback(&Controller::frontendEvent, this);

        if (dockAdded_) {
            obs_frontend_remove_dock(kDockId);
            dockAdded_ = false;
            verticalDock_ = nullptr;
        } else if (verticalDock_) {
            delete verticalDock_.data();
            verticalDock_ = nullptr;
        }
    }

    void refresh()
    {
        if (!verticalDock_)
            return;

        ScanState state;
        state.mainCanvas = obs_get_main_canvas();
        if (!state.mainCanvas)
            return;

        obs_enum_canvases(enum_vertical_canvas, &state);

        std::vector<obs_source_t *> sources;
        std::pair<ScanState *, std::vector<obs_source_t *> *> ctx{&state, &sources};
        obs_enum_all_sources(collect_vertical_audio, &ctx);

        verticalDock_->sync(sources);

        for (obs_source_t *source : sources)
            obs_source_release(source);
        obs_canvas_release(state.mainCanvas);
    }

private:
    static void frontendEvent(enum obs_frontend_event event, void *data)
    {
        auto *self = static_cast<Controller *>(data);
        if (!self)
            return;

        switch (event) {
        case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
        case OBS_FRONTEND_EVENT_SCENE_CHANGED:
        case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
            QMetaObject::invokeMethod(self, [self] { self->refresh(); }, Qt::QueuedConnection);
            break;
        default:
            break;
        }
    }

    QPointer<VerticalMixer> verticalDock_;
    QTimer refreshTimer_;
    bool dockAdded_ = false;
};

Controller *gController = nullptr;
bool gFrontendCallbackRegistered = false;

static void moduleFrontendEvent(enum obs_frontend_event event, void *)
{
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING || gController)
        return;

    QMetaObject::invokeMethod(qApp, [] {
        if (!gController)
            gController = new Controller;
    }, Qt::QueuedConnection);
}

} // namespace

bool obs_module_load(void)
{
    blog(LOG_INFO, "[Vertical Audio Mixer] loading");
    obs_frontend_add_event_callback(moduleFrontendEvent, nullptr);
    gFrontendCallbackRegistered = true;
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[Vertical Audio Mixer] unloading");
    if (gFrontendCallbackRegistered) {
        obs_frontend_remove_event_callback(moduleFrontendEvent, nullptr);
        gFrontendCallbackRegistered = false;
    }
    delete gController;
    gController = nullptr;
}
