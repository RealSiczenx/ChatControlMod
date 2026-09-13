#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-audio-controls.h>
#include <obs.h>

#include <QApplication>
#include <QBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_name(void) { return "Split Audio Mixer Docks"; }
MODULE_EXPORT const char *obs_module_description(void)
{
    return "Separate audio mixer docks for the OBS main canvas and Aitum Vertical.";
}

namespace {

enum class MixerKind { Main, Vertical };

static bool audio_source(obs_source_t *s)
{
    return s && !obs_obj_is_private(s) && !obs_source_is_scene(s) && !obs_source_is_group(s) &&
           (obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO);
}

class Meter final : public QWidget {
public:
    explicit Meter(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(7);
        timer_.setInterval(33);
        connect(&timer_, &QTimer::timeout, this, [this] {
            const float target = peak_.load(std::memory_order_relaxed);
            shown_ = std::max(target, shown_ - 1.5f);
            update();
        });
        timer_.start();
    }

    void setPeak(float db)
    {
        if (!std::isfinite(db)) db = -60.0f;
        peak_.store(std::clamp(db, -60.0f, 0.0f), std::memory_order_relaxed);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Mid));
        const float n = std::clamp((shown_ + 60.0f) / 60.0f, 0.0f, 1.0f);
        const int w = int(n * width());
        QColor c = shown_ > -9.0f ? QColor(220, 70, 70) :
                   shown_ > -20.0f ? QColor(220, 185, 60) : QColor(65, 185, 95);
        p.fillRect(0, 0, w, height(), c);
    }

private:
    std::atomic<float> peak_{-60.0f};
    float shown_ = -60.0f;
    QTimer timer_;
};

class Strip final : public QFrame {
public:
    explicit Strip(obs_source_t *source, QWidget *parent = nullptr) : QFrame(parent)
    {
        weak_ = obs_source_get_weak_source(source);
        setFrameShape(QFrame::StyledPanel);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(6, 4, 6, 4);
        root->setSpacing(3);

        auto *top = new QHBoxLayout;
        name_ = new QLabel(QString::fromUtf8(obs_source_get_name(source)), this);
        top->addWidget(name_, 1);
        mute_ = new QPushButton("M", this);
        mute_->setCheckable(true);
        mute_->setFixedSize(26, 22);
        mute_->setChecked(obs_source_muted(source));
        connect(mute_, &QPushButton::toggled, this, [this](bool v) { withSource([v](obs_source_t *s) { obs_source_set_muted(s, v); }); });
        top->addWidget(mute_);
        root->addLayout(top);

        auto *vol = new QHBoxLayout;
        slider_ = new QSlider(Qt::Horizontal, this);
        slider_->setRange(-600, 100);
        slider_->setSingleStep(5);
        setSlider(source);
        connect(slider_, &QSlider::valueChanged, this, [this](int v) {
            if (syncing_) return;
            const float db = float(v) / 10.0f;
            withSource([db](obs_source_t *s) { obs_source_set_volume(s, obs_db_to_mul(db)); });
        });
        vol->addWidget(slider_, 1);
        db_ = new QLabel(this);
        db_->setMinimumWidth(52);
        vol->addWidget(db_);
        root->addLayout(vol);

        meter_ = new Meter(this);
        root->addWidget(meter_);

        vm_ = obs_volmeter_create(OBS_FADER_LOG);
        if (vm_) {
            obs_volmeter_set_peak_meter_type(vm_, SAMPLE_PEAK_METER);
            obs_volmeter_add_callback(vm_, meterCallback, this);
            obs_volmeter_attach_source(vm_, source);
        }

        sync_.setInterval(300);
        connect(&sync_, &QTimer::timeout, this, [this] { syncUi(); });
        sync_.start();
        syncUi();
    }

    ~Strip() override
    {
        if (vm_) {
            obs_volmeter_remove_callback(vm_, meterCallback, this);
            obs_volmeter_detach_source(vm_);
            obs_volmeter_destroy(vm_);
        }
        if (weak_) obs_weak_source_release(weak_);
    }

private:
    template<class F> void withSource(F fn)
    {
        if (!weak_) return;
        obs_source_t *s = obs_weak_source_get_source(weak_);
        if (!s) return;
        fn(s);
        obs_source_release(s);
    }

    void setSlider(obs_source_t *s)
    {
        float db = obs_mul_to_db(obs_source_get_volume(s));
        if (!std::isfinite(db)) db = -60.0f;
        db = std::clamp(db, -60.0f, 10.0f);
        slider_->setValue(int(std::lround(db * 10.0f)));
        db_->setText(QString::number(db, 'f', 1) + " dB");
    }

    void syncUi()
    {
        withSource([this](obs_source_t *s) {
            syncing_ = true;
            name_->setText(QString::fromUtf8(obs_source_get_name(s)));
            mute_->setChecked(obs_source_muted(s));
            setSlider(s);
            syncing_ = false;
        });
    }

    static void meterCallback(void *data, const float[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
                              const float[MAX_AUDIO_CHANNELS])
    {
        auto *self = static_cast<Strip *>(data);
        if (!self || !self->meter_) return;
        float p = -60.0f;
        for (size_t i = 0; i < MAX_AUDIO_CHANNELS; ++i)
            if (std::isfinite(peak[i])) p = std::max(p, peak[i]);
        self->meter_->setPeak(p);
    }

    obs_weak_source_t *weak_ = nullptr;
    obs_volmeter_t *vm_ = nullptr;
    QLabel *name_ = nullptr;
    QLabel *db_ = nullptr;
    QPushButton *mute_ = nullptr;
    QSlider *slider_ = nullptr;
    Meter *meter_ = nullptr;
    QTimer sync_;
    bool syncing_ = false;
};

class MixerDock final : public QWidget {
public:
    explicit MixerDock(MixerKind kind, QWidget *parent = nullptr) : QWidget(parent), kind_(kind)
    {
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(4, 4, 4, 4);
        title_ = new QLabel(kind == MixerKind::Main ? "MAIN AUDIO" : "VERTICAL AUDIO", this);
        QFont f = title_->font();
        f.setBold(true);
        title_->setFont(f);
        root->addWidget(title_);

        scroll_ = new QScrollArea(this);
        scroll_->setWidgetResizable(true);
        scroll_->setFrameShape(QFrame::NoFrame);
        content_ = new QWidget(scroll_);
        layout_ = new QVBoxLayout(content_);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(4);
        layout_->addStretch(1);
        scroll_->setWidget(content_);
        root->addWidget(scroll_, 1);
    }

    void rebuild(const std::vector<obs_source_t *> &sources)
    {
        while (layout_->count() > 1) {
            QLayoutItem *item = layout_->takeAt(0);
            if (item->widget()) delete item->widget();
            delete item;
        }
        for (obs_source_t *s : sources)
            layout_->insertWidget(layout_->count() - 1, new Strip(s, content_));
        title_->setText(QString(kind_ == MixerKind::Main ? "MAIN AUDIO  (%1)" : "VERTICAL AUDIO  (%1)").arg(sources.size()));
    }

private:
    MixerKind kind_;
    QLabel *title_ = nullptr;
    QScrollArea *scroll_ = nullptr;
    QWidget *content_ = nullptr;
    QVBoxLayout *layout_ = nullptr;
};

struct EnumCtx {
    obs_canvas_t *main = nullptr;
    std::vector<obs_source_t *> mainSources;
    std::vector<obs_source_t *> verticalSources;
};

static bool enumSource(void *data, obs_source_t *s)
{
    auto *ctx = static_cast<EnumCtx *>(data);
    if (!ctx || !audio_source(s)) return true;

    bool vertical = false;
    obs_canvas_t *canvas = obs_source_get_canvas(s);
    if (canvas) {
        vertical = canvas != ctx->main;
        obs_canvas_release(canvas);
    }

    obs_source_t *ref = obs_source_get_ref(s);
    if (!ref) return true;
    if (vertical) ctx->verticalSources.push_back(ref);
    else ctx->mainSources.push_back(ref);
    return true;
}

class Controller final : public QObject {
public:
    Controller()
    {
        main_ = new MixerDock(MixerKind::Main);
        vertical_ = new MixerDock(MixerKind::Vertical);
        obs_frontend_add_dock_by_id("SplitAudioMixer.Main", "Main Audio Mixer", main_);
        obs_frontend_add_dock_by_id("SplitAudioMixer.Vertical", "Vertical Audio Mixer", vertical_);
        timer_.setInterval(1500);
        connect(&timer_, &QTimer::timeout, this, [this] { refresh(); });
        timer_.start();
        refresh();
    }

    ~Controller() override
    {
        timer_.stop();
        obs_frontend_remove_dock("SplitAudioMixer.Main");
        obs_frontend_remove_dock("SplitAudioMixer.Vertical");
    }

    void refresh()
    {
        EnumCtx ctx;
        ctx.main = obs_get_main_canvas();
        if (!ctx.main) return;
        obs_enum_all_sources(enumSource, &ctx);
        main_->rebuild(ctx.mainSources);
        vertical_->rebuild(ctx.verticalSources);
        for (auto *s : ctx.mainSources) obs_source_release(s);
        for (auto *s : ctx.verticalSources) obs_source_release(s);
        obs_canvas_release(ctx.main);
    }

private:
    QPointer<MixerDock> main_;
    QPointer<MixerDock> vertical_;
    QTimer timer_;
};

Controller *controller = nullptr;

} // namespace

bool obs_module_load(void)
{
    controller = new Controller;
    blog(LOG_INFO, "[Split Audio Mixer] loaded");
    return true;
}

void obs_module_unload(void)
{
    delete controller;
    controller = nullptr;
}
