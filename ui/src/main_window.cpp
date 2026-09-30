#include "main_window.hpp"

#include "audio_mixer_panel.hpp"
#include "preview_item.hpp"
#include "scene_profiles_panel.hpp"
#include "settings_dialog.hpp"
#include "theme_tokens.hpp"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QFrame>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStyle>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>

namespace fluxrec::ui {
namespace {

using Path = QStringList;

bool hasValidOutputResolution(const RecorderSettings& settings) {
    if (settings.output_width == 0 && settings.output_height == 0) return true;
    return settings.output_width >= 4 && settings.output_height >= 4 &&
        settings.output_width <= 16384 && settings.output_height <= 16384 &&
        settings.output_width % 2 == 0 && settings.output_height % 2 == 0;
}

bool hevcAcknowledgementRequired(const RecorderSettings& settings) {
    return settings.codec == QStringLiteral("hevc") && !settings.allow_experimental_hevc;
}

QJsonValue jsonAt(const QJsonObject& object, const Path& path) {
    QJsonValue value(object);
    for (const QString& part : path) {
        if (!value.isObject()) return {};
        value = value.toObject().value(part);
    }
    return value;
}

QString unavailableValue() {
    return QStringLiteral("—");
}

std::optional<double> finiteNumber(const QJsonValue& value) {
    if (!value.isDouble()) return std::nullopt;
    const double number = value.toDouble();
    if (!std::isfinite(number)) return std::nullopt;
    return number;
}

QString formatCount(const QJsonValue& value) {
    const auto number = finiteNumber(value);
    if (!number || *number < 0.0) return unavailableValue();
    return QLocale().toString(static_cast<qulonglong>(*number));
}

QString formatRate(const QJsonValue& value) {
    const auto number = finiteNumber(value);
    if (!number || *number < 0.0) return unavailableValue();
    return QLocale().toString(*number, 'f', 1) + QStringLiteral(" FPS");
}

QString formatMilliseconds(const QJsonValue& value) {
    const auto number = finiteNumber(value);
    if (!number || *number < 0.0) return unavailableValue();
    return QLocale().toString(*number / 1'000'000.0, 'f', 3) + QStringLiteral(" ms");
}

QString formatFileSize(const QJsonValue& value) {
    const auto number = finiteNumber(value);
    if (!number || *number < 0.0) return unavailableValue();
    constexpr double kib = 1024.0;
    if (*number < kib) return QLocale().toString(*number, 'f', 0) + QStringLiteral(" B");
    if (*number < kib * kib) return QLocale().toString(*number / kib, 'f', 1) + QStringLiteral(" KiB");
    if (*number < kib * kib * kib) {
        return QLocale().toString(*number / (kib * kib), 'f', 1) + QStringLiteral(" MiB");
    }
    return QLocale().toString(*number / (kib * kib * kib), 'f', 2) + QStringLiteral(" GiB");
}

QString formatMegabits(const QJsonValue& value) {
    const auto number = finiteNumber(value);
    if (!number || *number < 0.0) return unavailableValue();
    return QLocale().toString(*number / 1'000'000.0, 'f', 2) + QStringLiteral(" Mbps");
}

QString formatPercent(const QJsonValue& value) {
    const auto number = finiteNumber(value);
    if (!number || *number < 0.0) return unavailableValue();
    return QLocale().toString(*number, 'f', 1) + QStringLiteral("%");
}

QLabel* makeLabel(const QString& text, const QString& object_name = {}) {
    auto* label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    if (!object_name.isEmpty()) label->setObjectName(object_name);
    label->setWordWrap(true);
    return label;
}

QFrame* makeRule(QWidget* parent = nullptr) {
    auto* rule = new QFrame(parent);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Plain);
    rule->setFixedHeight(1);
    rule->setObjectName(QStringLiteral("sectionRule"));
    return rule;
}

void setCell(QTableWidget* table, int row, int column, const QString& text,
             const QString& accessible_text = {}, const QString& tool_tip = {}) {
    auto* item = new QTableWidgetItem(text);
    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    if (column > 0) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    const QString description = accessible_text.isEmpty()
        ? (text == unavailableValue() ? QObject::tr("Not reported") : text)
        : accessible_text;
    item->setData(Qt::AccessibleTextRole, description);
    if (!tool_tip.isEmpty()) item->setToolTip(tool_tip);
    table->setItem(row, column, item);
}

QTableWidget* makeTable(const QString& accessible_name,
                        const QStringList& columns,
                        QWidget* parent) {
    auto* table = new QTableWidget(parent);
    table->setObjectName(QStringLiteral("telemetryTable"));
    table->setAccessibleName(accessible_name);
    table->setAccessibleDescription(QObject::tr("Read-only values reported by the recorder."));
    table->setColumnCount(columns.size());
    table->setHorizontalHeaderLabels(columns);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setHighlightSections(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setFocusPolicy(Qt::StrongFocus);
    return table;
}

class MetricTile final : public QFrame {
public:
    explicit MetricTile(const QString& title, QWidget* parent = nullptr)
        : QFrame(parent), value_(nullptr), detail_(nullptr) {
        setObjectName(QStringLiteral("metricTile"));
        setFrameShape(QFrame::StyledPanel);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(theme::panel_inset, 8, theme::panel_inset, 8);
        layout->setSpacing(2);
        auto* title_label = makeLabel(title, QStringLiteral("eyebrow"));
        value_ = makeLabel(unavailableValue(), QStringLiteral("metricValue"));
        detail_ = makeLabel(QObject::tr("Not reported"), QStringLiteral("secondaryText"));
        QFont mono = value_->font();
        mono.setFamily(QStringLiteral("Noto Sans Mono"));
        mono.setPointSize(15);
        mono.setWeight(QFont::DemiBold);
        value_->setFont(mono);
        layout->addWidget(title_label);
        layout->addWidget(value_);
        layout->addWidget(detail_);
        setAccessibleName(title);
        setAccessibleDescription(QObject::tr("No telemetry value has been reported."));
    }

    void setValue(const QString& value, const QString& detail) {
        value_->setText(value);
        detail_->setText(detail);
        setAccessibleDescription(value == unavailableValue()
            ? QObject::tr("Not reported by the recorder.")
            : value + QStringLiteral(". ") + detail);
    }

private:
    QLabel* value_;
    QLabel* detail_;
};

class PreviewCanvas final : public QWidget {
public:
    explicit PreviewCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("previewCanvas"));
        setStatus(QObject::tr("GPU preview unavailable"), QObject::tr(
            "Qt Quick Vulkan rendering could not be initialized. Recording remains available. No CPU pixel copy is used."));
        setMinimumHeight(250);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setStatus(const QString& title, const QString& detail) {
        title_ = title;
        detail_ = detail;
        setAccessibleName(title);
        setAccessibleDescription(detail);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(theme::canvas));
        painter.setRenderHint(QPainter::Antialiasing, false);

        QRectF viewport = QRectF(rect()).adjusted(28, 24, -28, -24);
        constexpr qreal aspect = 16.0 / 9.0;
        if (viewport.width() / std::max<qreal>(viewport.height(), 1.0) > aspect) {
            const qreal width = viewport.height() * aspect;
            viewport.setLeft(viewport.center().x() - width / 2.0);
            viewport.setRight(viewport.center().x() + width / 2.0);
        } else {
            const qreal height = viewport.width() / aspect;
            viewport.setTop(viewport.center().y() - height / 2.0);
            viewport.setBottom(viewport.center().y() + height / 2.0);
        }

        painter.fillRect(viewport, QColor(theme::surface));
        QPen frame_pen(QColor(theme::line));
        frame_pen.setStyle(Qt::DashLine);
        frame_pen.setWidth(1);
        painter.setPen(frame_pen);
        painter.drawRect(viewport);

        const qreal corner = 16.0;
        QPen corner_pen(QColor(theme::primary));
        corner_pen.setWidthF(1.5);
        painter.setPen(corner_pen);
        const QPointF tl = viewport.topLeft();
        const QPointF tr = viewport.topRight();
        const QPointF bl = viewport.bottomLeft();
        const QPointF br = viewport.bottomRight();
        painter.drawLine(tl, tl + QPointF(corner, 0));
        painter.drawLine(tl, tl + QPointF(0, corner));
        painter.drawLine(tr, tr + QPointF(-corner, 0));
        painter.drawLine(tr, tr + QPointF(0, corner));
        painter.drawLine(bl, bl + QPointF(corner, 0));
        painter.drawLine(bl, bl + QPointF(0, -corner));
        painter.drawLine(br, br + QPointF(-corner, 0));
        painter.drawLine(br, br + QPointF(0, -corner));

        painter.setPen(QColor(theme::text));
        QFont title_font = font();
        title_font.setWeight(QFont::DemiBold);
        title_font.setPointSize(11);
        painter.setFont(title_font);
        const QRectF title_rect(viewport.left() + 26, viewport.center().y() - 20,
                                viewport.width() - 52, 28);
        painter.drawText(title_rect, Qt::AlignCenter, title_);
        painter.setPen(QColor(theme::muted));
        QFont detail_font = font();
        detail_font.setPointSize(9);
        painter.setFont(detail_font);
        const QRectF detail_rect(viewport.left() + 26, viewport.center().y() + 8,
                                 viewport.width() - 52, 44);
        painter.drawText(detail_rect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                         detail_);
    }

private:
    QString title_;
    QString detail_;
};

class StatisticsPanel final : public QWidget {
public:
    explicit StatisticsPanel(QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(theme::panel_inset, 10, theme::panel_inset, theme::panel_inset);
        layout->setSpacing(theme::cluster_gap);

        auto* header = new QHBoxLayout;
        header->setSpacing(theme::cluster_gap);
        header->addWidget(makeLabel(QObject::tr("Pipeline readings"), QStringLiteral("sectionTitle")));
        header->addStretch(1);
        freshness_ = makeLabel(QObject::tr("No telemetry yet"), QStringLiteral("secondaryText"));
        freshness_->setAccessibleName(QObject::tr("Telemetry freshness"));
        header->addWidget(freshness_);
        layout->addLayout(header);

        auto* rates = new QGridLayout;
        rates->setHorizontalSpacing(theme::cluster_gap);
        rates->setVerticalSpacing(theme::cluster_gap);
        const QStringList rate_names{
            QObject::tr("Captured FPS"), QObject::tr("Unique FPS"),
            QObject::tr("Encode FPS"), QObject::tr("Mux FPS")};
        for (int i = 0; i < rate_names.size(); ++i) {
            rates_[static_cast<std::size_t>(i)] = new MetricTile(rate_names[i], this);
            rates->addWidget(rates_[static_cast<std::size_t>(i)], 0, i);
        }
        layout->addLayout(rates);

        auto* detail_line = new QHBoxLayout;
        detail_line->setSpacing(theme::panel_inset);
        file_size_ = makeLabel(QObject::tr("Output file: Not reported"), QStringLiteral("secondaryText"));
        output_bitrate_ = makeLabel(QObject::tr("Output bitrate: Not reported"), QStringLiteral("secondaryText"));
        replay_memory_ = makeLabel(QObject::tr("Replay RAM: Not reported"), QStringLiteral("secondaryText"));
        file_size_->setAccessibleName(QObject::tr("Output file size"));
        output_bitrate_->setAccessibleName(QObject::tr("Output bitrate"));
        replay_memory_->setAccessibleName(QObject::tr("Replay buffer memory"));
        detail_line->addWidget(file_size_);
        detail_line->addWidget(makeRule(this));
        detail_line->addWidget(output_bitrate_);
        detail_line->addWidget(makeRule(this));
        detail_line->addWidget(replay_memory_);
        layout->addLayout(detail_line);

        detail_tabs_ = new QTabWidget(this);
        detail_tabs_->setDocumentMode(true);
        timing_table_ = makeTable(QObject::tr("Stage latency percentiles"),
                                  {QObject::tr("Stage"), QObject::tr("Average"),
                                   QObject::tr("P50"), QObject::tr("P95"),
                                   QObject::tr("P99"), QObject::tr("P99.9"),
                                   QObject::tr("Maximum")}, detail_tabs_);
        queue_table_ = makeTable(QObject::tr("Pipeline queue occupancy"),
                                 {QObject::tr("Queue"), QObject::tr("Occupancy"),
                                  QObject::tr("High-water"), QObject::tr("Capacity")}, detail_tabs_);
        count_table_ = makeTable(QObject::tr("Frame and drop counts"),
                                 {QObject::tr("Counter"), QObject::tr("Reported value")}, detail_tabs_);
        count_table_->setObjectName(QStringLiteral("frameCountsTable"));
        count_table_->setAccessibleDescription(QObject::tr(
            "Read-only frame counts from the recorder. Identity digest coverage is reported only "
            "when GPU frame identity diagnostics are enabled; otherwise it remains Not reported."));
        system_table_ = makeTable(QObject::tr("Recorder and device utilization"),
                                  {QObject::tr("Metric"), QObject::tr("Reported value")}, detail_tabs_);
        detail_tabs_->addTab(timing_table_, QObject::tr("Stage latency"));
        detail_tabs_->addTab(queue_table_, QObject::tr("Queues"));
        detail_tabs_->addTab(count_table_, QObject::tr("Frame counts"));
        auto* average_page = new QWidget(detail_tabs_);
        auto* average_layout = new QVBoxLayout(average_page);
        average_layout->setContentsMargins(0, 8, 0, 0);
        average_layout->setSpacing(theme::cluster_gap);
        average_note_ = makeLabel(
            QObject::tr("Average from the first report through the last report while recording was still running. The second in which you stop is left out. The readings above stay the latest one-second sample."),
            QStringLiteral("secondaryText"));
        average_note_->setObjectName(QStringLiteral("averageFpsNote"));
        average_note_->setAccessibleName(QObject::tr("Average FPS explanation"));
        average_table_ = makeTable(QObject::tr("Average FPS"),
                                   {QObject::tr("Stage"), QObject::tr("Average"),
                                    QObject::tr("Frames included"),
                                    QObject::tr("Seconds included")}, average_page);
        average_table_->setObjectName(QStringLiteral("averageFpsTable"));
        average_table_->setAccessibleDescription(QObject::tr(
            "Session average frame rate. Reports received after Stop are omitted, so the second in which recording stops is not included."));
        average_layout->addWidget(average_note_);
        average_layout->addWidget(average_table_, 1);
        detail_tabs_->addTab(average_page, QObject::tr("Average FPS"));
        detail_tabs_->addTab(system_table_, QObject::tr("System"));
        layout->addWidget(detail_tabs_, 1);

        render_no_snapshot();
    }

    void beginSession() {
        average_ = {};
        render_average();
    }

    void setSnapshot(const TelemetrySnapshot& snapshot, bool include_in_average) {
        snapshot_ = snapshot;
        recording_mode_ = snapshot.values.value(QStringLiteral("recording_mode")).toString();
        if (include_in_average) accumulate_average(snapshot.values);
        render_snapshot();
    }

    void refreshFreshness(RecorderBackend::State state) {
        if (!snapshot_) {
            freshness_->setText(QObject::tr("No telemetry yet"));
            return;
        }
        const qint64 age = snapshot_->received_at.secsTo(QDateTime::currentDateTime());
        const bool active = state == RecorderBackend::State::recording ||
            state == RecorderBackend::State::replay_only ||
            state == RecorderBackend::State::stopping || state == RecorderBackend::State::starting;
        if (active && age > 3) {
            freshness_->setText(QObject::tr("Stale · last report %1 s ago").arg(age));
            freshness_->setProperty("stale", true);
        } else if (active) {
            freshness_->setText(QObject::tr("Live · reported %1 s ago").arg(age));
            freshness_->setProperty("stale", false);
        } else {
            freshness_->setText(QObject::tr("Last report · %1")
                                    .arg(QLocale().toString(snapshot_->received_at.time(), QLocale::ShortFormat)));
            freshness_->setProperty("stale", false);
        }
        freshness_->style()->unpolish(freshness_);
        freshness_->style()->polish(freshness_);
    }

private:
    void render_no_snapshot() {
        recording_mode_.clear();
        for (auto* rate : rates_) {
            rate->setValue(unavailableValue(), QObject::tr("Not reported by the recorder"));
        }
        file_size_->setText(QObject::tr("Output file: Not reported"));
        output_bitrate_->setText(QObject::tr("Output bitrate: Not reported"));
        replay_memory_->setText(QObject::tr("Replay RAM: Not reported"));
        freshness_->setText(QObject::tr("No telemetry yet"));
        render_tables(QJsonObject{});
        render_average();
    }

    void render_snapshot() {
        const QJsonObject values = snapshot_->values;
        const bool replay_only = recording_mode_ == QStringLiteral("replay_only");
        const std::array<Path, 4> rate_paths{{
            {QStringLiteral("fps"), QStringLiteral("captured")},
            {QStringLiteral("fps"), QStringLiteral("unique")},
            {QStringLiteral("fps"), QStringLiteral("encoded")},
            {QStringLiteral("fps"), QStringLiteral("muxed")},
        }};
        const std::array<Path, 4> rate_count_paths{{
            {QStringLiteral("frames"), QStringLiteral("captured")},
            {QStringLiteral("frames"), QStringLiteral("unique")},
            {QStringLiteral("frames"), QStringLiteral("encoded")},
            {QStringLiteral("frames"), QStringLiteral("muxed")},
        }};
        for (std::size_t i = 0; i < rates_.size(); ++i) {
            const QString rate = replay_only && i == 3
                ? QObject::tr("Not used") : formatRate(jsonAt(values, rate_paths[i]));
            const QString count = formatCount(jsonAt(values, rate_count_paths[i]));
            const QString detail = replay_only && i == 3
                ? QObject::tr("Replay-only session · no primary mux")
                : count == unavailableValue()
                ? QObject::tr("Frame count not reported")
                : QObject::tr("%1 frames reported").arg(count);
            rates_[i]->setValue(rate, detail);
        }

        file_size_->setText(replay_only
            ? QObject::tr("Primary output file: Not used")
            : QObject::tr("Output file: %1")
                  .arg(formatFileSize(jsonAt(values, {"storage", "output_file_bytes"}))));
        output_bitrate_->setText(replay_only
            ? QObject::tr("Primary output bitrate: Not used")
            : QObject::tr("Output bitrate: %1")
                  .arg(formatMegabits(jsonAt(values, {"storage", "output_file_bitrate_bps"}))));
        replay_memory_->setText(QObject::tr("Replay RAM: %1")
                                    .arg(formatFileSize(jsonAt(values, {"storage", "replay_payload_bytes"}))));
        render_tables(values, replay_only);
        render_average();
    }

    void accumulate_average(const QJsonObject& values) {
        const auto timestamp = finiteNumber(values.value(QStringLiteral("timestamp_unix_ns")));
        if (!timestamp || *timestamp < 0.0) return;
        const qint64 unix_ns = static_cast<qint64>(*timestamp);
        // The first report already covers the recorder's one-second sample.
        constexpr qint64 report_interval_ns = 1'000'000'000;
        if (!average_.started) {
            average_.window_start_ns = unix_ns - report_interval_ns;
            average_.started = true;
        }
        if (unix_ns < average_.window_start_ns) return;
        average_.end_ns = unix_ns;
        average_.replay_only = values.value(QStringLiteral("recording_mode")).toString() ==
            QStringLiteral("replay_only");
        const std::array<Path, 4> count_paths{{
            {QStringLiteral("frames"), QStringLiteral("captured")},
            {QStringLiteral("frames"), QStringLiteral("unique")},
            {QStringLiteral("frames"), QStringLiteral("encoded")},
            {QStringLiteral("frames"), QStringLiteral("muxed")},
        }};
        for (std::size_t index = 0; index < count_paths.size(); ++index) {
            const auto count = finiteNumber(jsonAt(values, count_paths[index]));
            if (count && *count >= 0.0) average_.frames[index] = *count;
        }
    }

    void render_average() {
        const std::array<QString, 4> stages{{
            QObject::tr("Captured"), QObject::tr("Unique"),
            QObject::tr("Encode"), QObject::tr("Mux"),
        }};
        average_table_->setRowCount(static_cast<int>(stages.size()));
        const double elapsed = average_.started
            ? static_cast<double>(average_.end_ns - average_.window_start_ns) / 1'000'000'000.0
            : 0.0;
        const QString seconds = elapsed > 0.0
            ? QLocale().toString(elapsed, 'f', 1) : unavailableValue();
        for (std::size_t row = 0; row < stages.size(); ++row) {
            setCell(average_table_, static_cast<int>(row), 0, stages[row]);
            const bool mux_unused = average_.replay_only && row == 3;
            if (mux_unused) {
                const QString unused = QObject::tr("Not used");
                setCell(average_table_, static_cast<int>(row), 1, unused);
                setCell(average_table_, static_cast<int>(row), 2, unused);
                setCell(average_table_, static_cast<int>(row), 3, unused);
                continue;
            }
            if (!average_.frames[row] || elapsed <= 0.0) {
                setCell(average_table_, static_cast<int>(row), 1, unavailableValue(),
                        QObject::tr("Not reported. Average FPS starts with the first recorder report and leaves out reports received after Stop."));
                setCell(average_table_, static_cast<int>(row), 2, unavailableValue());
                setCell(average_table_, static_cast<int>(row), 3, unavailableValue());
                continue;
            }
            const double rate = *average_.frames[row] / elapsed;
            const QString rate_text = QLocale().toString(rate, 'f', 1) + QStringLiteral(" FPS");
            const QString frame_text = QLocale().toString(static_cast<qulonglong>(*average_.frames[row]));
            setCell(average_table_, static_cast<int>(row), 1, rate_text,
                    QObject::tr("%1 over %2 seconds, excluding the second in which recording stops.")
                        .arg(rate_text, seconds));
            setCell(average_table_, static_cast<int>(row), 2, frame_text);
            setCell(average_table_, static_cast<int>(row), 3, seconds);
        }
    }

    void render_tables(const QJsonObject& values, bool replay_only = false) {
        const std::array<std::pair<QString, QString>, 17> timing_names{{
            {QObject::tr("Frame time"), QStringLiteral("frame_time")},
            {QObject::tr("Capture request to ready (wall)"), QStringLiteral("capture")},
            {QObject::tr("Capture ready to submit (wall)"),
             QStringLiteral("capture_ready_to_submit_wall")},
            {QObject::tr("GPU conversion"), QStringLiteral("gpu_conversion")},
            {QObject::tr("DMA-BUF import (CPU)"), QStringLiteral("dmabuf_import_cpu")},
            {QObject::tr("Filter submit (CPU)"), QStringLiteral("filter_submit_cpu")},
            {QObject::tr("Filter pull (CPU)"), QStringLiteral("filter_pull_cpu")},
            {QObject::tr("NVENC submission (hardware)"), QStringLiteral("nvenc_submission")},
            {QObject::tr("NVENC completion (hardware)"), QStringLiteral("nvenc_completion")},
            {QObject::tr("NVENC encode API (wall)"),
             QStringLiteral("nvenc_encode_picture_api_wall")},
            {QObject::tr("NVENC bitstream lock (wall)"),
             QStringLiteral("nvenc_lock_bitstream_api_wall")},
            {QObject::tr("NVENC submit to bitstream (wall)"),
             QStringLiteral("nvenc_submit_to_bitstream_wall")},
            {QObject::tr("Encoder send (CPU)"), QStringLiteral("encoder_send_cpu")},
            {QObject::tr("Encoder receive (CPU)"), QStringLiteral("encoder_receive_cpu")},
            {QObject::tr("Submit to packet (wall)"), QStringLiteral("submit_to_packet_wall")},
            {QObject::tr("Mux"), QStringLiteral("mux")},
            {QObject::tr("Disk write"), QStringLiteral("disk_write")},
        }};
        timing_table_->setRowCount(static_cast<int>(timing_names.size()));
        const std::array<QString, 6> percentiles{{
            QStringLiteral("average"), QStringLiteral("p50"), QStringLiteral("p95"),
            QStringLiteral("p99"), QStringLiteral("p99_9"), QStringLiteral("max")}};
        for (std::size_t row = 0; row < timing_names.size(); ++row) {
            const bool nvenc_availability_proxy =
                timing_names[row].second == QStringLiteral("nvenc_submit_to_bitstream_wall");
            setCell(timing_table_, static_cast<int>(row), 0, timing_names[row].first, {},
                    nvenc_availability_proxy
                        ? QObject::tr("Host-monotonic time from accepted NVENC submission return to successful bitstream-lock return, matched by frame PTS. Includes queueing, host scheduling, and driver wait; it is not NVENC hardware execution time.")
                        : QString{});
            for (std::size_t column = 0; column < percentiles.size(); ++column) {
                const QString path = QStringLiteral("timings_ns.%1.%2")
                    .arg(timing_names[row].second, percentiles[column]);
                const QStringList pieces = path.split(QLatin1Char('.'));
                const bool primary_mux_stage = replay_only &&
                    (timing_names[row].second == QStringLiteral("mux") ||
                     timing_names[row].second == QStringLiteral("disk_write"));
                setCell(timing_table_, static_cast<int>(row), static_cast<int>(column + 1),
                        primary_mux_stage ? QObject::tr("Not used")
                                          : formatMilliseconds(jsonAt(values, pieces)));
            }
        }

        const std::array<std::pair<QString, QString>, 5> queue_names{{
            {QObject::tr("GPU frame pool"), QStringLiteral("gpu_frame_pool")},
            {QObject::tr("Capture"), QStringLiteral("capture")},
            {QObject::tr("Encode"), QStringLiteral("encode")},
            {QObject::tr("Packet"), QStringLiteral("packet")},
            {QObject::tr("Mux"), QStringLiteral("mux")},
        }};
        queue_table_->setRowCount(static_cast<int>(queue_names.size()));
        const std::array<QString, 3> queue_columns{{
            QStringLiteral("occupancy"), QStringLiteral("high_water"), QStringLiteral("capacity")}};
        for (std::size_t row = 0; row < queue_names.size(); ++row) {
            setCell(queue_table_, static_cast<int>(row), 0, queue_names[row].first);
            for (std::size_t column = 0; column < queue_columns.size(); ++column) {
                const bool primary_mux_queue = replay_only &&
                    queue_names[row].second == QStringLiteral("mux");
                setCell(queue_table_, static_cast<int>(row), static_cast<int>(column + 1),
                        primary_mux_queue ? QObject::tr("Not used")
                            : formatCount(jsonAt(values, {QStringLiteral("queues"), queue_names[row].second,
                                                          queue_columns[column]})));
            }
        }

        const std::array<std::pair<QString, Path>, 21> counter_rows{{
            {QObject::tr("Frames delivered for processing"), {"frames", "captured"}},
            {QObject::tr("Wayland capture completions"), {"capture", "wayland_completed"}},
            {QObject::tr("Capture frames admitted"), {"capture", "admitted"}},
            {QObject::tr("Frames withheld by FPS cap (intentional)"),
             {"capture", "rate_limited"}},
            {QObject::tr("Distinct capture PTS"), {"frames", "compositor"}},
            {QObject::tr("Unique frames"), {"frames", "unique"}},
            {QObject::tr("Submitted to encoder"), {"frames", "submitted"}},
            {QObject::tr("Encoded frames"), {"frames", "encoded"}},
            {QObject::tr("Muxed frames"), {"frames", "muxed"}},
            {QObject::tr("Dropped total"), {"frames", "dropped"}},
            {QObject::tr("Capture drops"), {"frames", "dropped_by_stage", "capture"}},
            {QObject::tr("Conversion drops"), {"frames", "dropped_by_stage", "conversion"}},
            {QObject::tr("Encoder drops"), {"frames", "dropped_by_stage", "encoder"}},
            {QObject::tr("Mux drops"), {"frames", "dropped_by_stage", "mux"}},
            {QObject::tr("Replay drops"), {"frames", "dropped_by_stage", "replay"}},
            {QObject::tr("Replay packet queue drops"), {"replay", "packet_queue_drops"}},
            {QObject::tr("Duplicated frames"), {"frames", "duplicated"}},
            {QObject::tr("Identity digests checked"), {"frames", "identity_checked"}},
            {QObject::tr("Identity checks skipped"), {"frames", "identity_skipped"}},
            {QObject::tr("Identity results unclassified"), {"frames", "identity_unclassified"}},
            {QObject::tr("Encoded payload"), {"storage", "encoded_payload_bytes"}},
        }};
        count_table_->setRowCount(static_cast<int>(counter_rows.size()));
        for (std::size_t row = 0; row < counter_rows.size(); ++row) {
            setCell(count_table_, static_cast<int>(row), 0, counter_rows[row].first);
            const bool primary_mux_counter = replay_only &&
                (counter_rows[row].first == QObject::tr("Muxed frames") ||
                 counter_rows[row].first == QObject::tr("Mux drops"));
            const QString value = primary_mux_counter
                ? QObject::tr("Not used")
                : replay_only && counter_rows[row].first == QObject::tr("Encoded payload")
                    ? QObject::tr("Not used")
                    : counter_rows[row].first == QObject::tr("Encoded payload")
                        ? formatFileSize(jsonAt(values, counter_rows[row].second))
                        : formatCount(jsonAt(values, counter_rows[row].second));
            QString accessible_value;
            if (counter_rows[row].first ==
                    QObject::tr("Frames withheld by FPS cap (intentional)")) {
                accessible_value = value == unavailableValue()
                    ? QObject::tr("Not reported. This counter tracks frames intentionally withheld before pipeline admission to enforce the configured FPS cap; that is expected behavior, not a processing error.")
                    : QObject::tr("%1 frames were intentionally withheld before pipeline admission to enforce the configured FPS cap. This is expected behavior, not a processing error.")
                          .arg(value);
            } else if (counter_rows[row].first == QObject::tr("Identity digests checked")) {
                accessible_value = value == unavailableValue()
                    ? QObject::tr("Not reported. GPU frame identity diagnostics were off or coverage was not reported.")
                    : QObject::tr("%1 GPU frame identity digests completed.").arg(value);
            } else if (counter_rows[row].first == QObject::tr("Identity checks skipped")) {
                accessible_value = value == unavailableValue()
                    ? QObject::tr("Not reported. GPU frame identity diagnostics were off or coverage was not reported.")
                    : QObject::tr("%1 frame identity checks were skipped because the bounded digest readback ring was full.").arg(value);
            } else if (counter_rows[row].first == QObject::tr("Identity results unclassified")) {
                accessible_value = value == unavailableValue()
                    ? QObject::tr("Not reported. GPU frame identity diagnostics were off or coverage was not reported.")
                    : QObject::tr("%1 completed digests could not be classified against the immediately preceding captured frame.").arg(value);
            }
            setCell(count_table_, static_cast<int>(row), 1, value, accessible_value);
        }

        const std::array<std::pair<QString, QString>, 5> system_rows{{
            {QObject::tr("Recorder process CPU"), QStringLiteral("process_cpu_percent")},
            {QObject::tr("GPU utilization"), QStringLiteral("gpu_utilization_percent")},
            {QObject::tr("GPU memory utilization"), QStringLiteral("gpu_memory_utilization_percent")},
            {QObject::tr("NVENC utilization"), QStringLiteral("encoder_utilization_percent")},
            {QObject::tr("NVENC sample period"), QStringLiteral("encoder_sample_period_us")},
        }};
        system_table_->setRowCount(static_cast<int>(system_rows.size()));
        for (std::size_t row = 0; row < system_rows.size(); ++row) {
            setCell(system_table_, static_cast<int>(row), 0, system_rows[row].first);
            const QJsonValue value = jsonAt(values, {QStringLiteral("system"), system_rows[row].second});
            const QString formatted = system_rows[row].second == QStringLiteral("encoder_sample_period_us")
                ? (formatCount(value) == unavailableValue()
                       ? unavailableValue()
                       : formatCount(value) + QStringLiteral(" µs"))
                : formatPercent(value);
            setCell(system_table_, static_cast<int>(row), 1, formatted);
        }
    }

    std::optional<TelemetrySnapshot> snapshot_;
    QString recording_mode_;
    QLabel* freshness_ = nullptr;
    QLabel* file_size_ = nullptr;
    QLabel* output_bitrate_ = nullptr;
    QLabel* replay_memory_ = nullptr;
    std::array<MetricTile*, 4> rates_{};
    QTabWidget* detail_tabs_ = nullptr;
    QTableWidget* timing_table_ = nullptr;
    QTableWidget* queue_table_ = nullptr;
    QTableWidget* count_table_ = nullptr;
    QTableWidget* system_table_ = nullptr;
    QLabel* average_note_ = nullptr;
    QTableWidget* average_table_ = nullptr;
    struct AverageAccumulator {
        bool started = false;
        bool replay_only = false;
        qint64 window_start_ns = 0;
        qint64 end_ns = 0;
        std::array<std::optional<double>, 4> frames{};
    } average_;
};

struct PreviewPanelParts {
    QWidget* panel = nullptr;
    PreviewCanvas* canvas = nullptr;
    QStackedWidget* stack = nullptr;
    QWidget* quick = nullptr;
};

PreviewPanelParts buildPreviewPanel(PreviewChannel* channel, const RecorderSettings& settings,
                                   QWidget* parent) {
    auto* panel = new QWidget(parent);
    auto* root = new QVBoxLayout(panel);
    root->setContentsMargins(theme::panel_inset, theme::panel_inset,
                             theme::panel_inset, 8);
    root->setSpacing(theme::cluster_gap);

    auto* heading = new QHBoxLayout;
    heading->setSpacing(theme::cluster_gap);
    heading->addWidget(makeLabel(QObject::tr("Live preview"), QStringLiteral("pageTitle")));
    heading->addStretch(1);
    auto* output = makeLabel(QObject::tr("OUTPUT · %1").arg(settings.display_name),
                             QStringLiteral("eyebrow"));
    output->setObjectName(QStringLiteral("previewOutputName"));
    output->setAccessibleName(QObject::tr("Configured preview output: %1").arg(settings.display_name));
    heading->addWidget(output);
    root->addLayout(heading);

    auto* preview_stack = new QStackedWidget(panel);
    preview_stack->setObjectName(QStringLiteral("previewContentStack"));
    auto* fallback = new PreviewCanvas(preview_stack);
    QWidget* quick_preview = nullptr;
    const QString platform = qEnvironmentVariable("QT_QPA_PLATFORM");
    if (platform == QStringLiteral("offscreen")) {
        fallback->setStatus(QObject::tr("GPU preview paused in headless mode"), QObject::tr(
            "The offscreen UI smoke test does not initialize Vulkan. Recording remains available, and no CPU pixel copy is used."));
        preview_stack->addWidget(fallback);
        preview_stack->setCurrentWidget(fallback);
    } else {
        auto* quick = new PreviewQuickWidget(channel, preview_stack);
        quick_preview = quick;
        quick->setObjectName(QStringLiteral("gpuPreviewView"));
        quick->setMinimumHeight(250);
        preview_stack->addWidget(quick);
        preview_stack->addWidget(fallback);
        preview_stack->setCurrentWidget(quick);
        QObject::connect(quick, &PreviewQuickWidget::sceneGraphError, panel,
                [channel, preview_stack, quick_preview, fallback](QQuickWindow::SceneGraphError,
                                                                  const QString& message) {
                    const QString reason = message.isEmpty()
                        ? QObject::tr("Qt Quick could not initialize Vulkan rendering.") : message;
                    if (channel) channel->fail_preview(reason);
                    fallback->setStatus(QObject::tr("GPU preview unavailable"), message.isEmpty()
                        ? QObject::tr("Qt Quick could not initialize Vulkan rendering. Recording remains available.")
                        : message + QObject::tr(" Recording remains available."));
                    preview_stack->setCurrentWidget(fallback);
                    quick_preview->hide();
                });
        QObject::connect(quick, &PreviewQuickWidget::statusChanged, panel,
                [channel, preview_stack, quick_preview, fallback](QQuickView::Status status) {
                    if (status != QQuickView::Error) return;
                    const QString reason = QObject::tr("Qt Quick could not load the Vulkan preview scene.");
                    if (channel) channel->fail_preview(reason);
                    fallback->setStatus(QObject::tr("GPU preview unavailable"),
                        reason + QObject::tr(" Recording remains available."));
                    preview_stack->setCurrentWidget(fallback);
                    quick_preview->hide();
                });
    }
    root->addWidget(preview_stack, 1);

    auto* rail = new QHBoxLayout;
    rail->setContentsMargins(4, 2, 4, 0);
    rail->setSpacing(theme::cluster_gap);
    const QStringList stages{
        QObject::tr("WAYLAND"), QObject::tr("VULKAN"),
        QObject::tr("NVENC"), QObject::tr("MATROSKA")};
    for (int i = 0; i < stages.size(); ++i) {
        if (i > 0) {
            auto* arrow = makeLabel(QStringLiteral("›"), QStringLiteral("eyebrow"));
            rail->addWidget(arrow);
        }
        auto* stage = makeLabel(stages[i], QStringLiteral("eyebrow"));
        stage->setObjectName(QStringLiteral("pipelineStage%1").arg(i));
        rail->addWidget(stage);
    }
    rail->addStretch(1);
    auto* note = makeLabel(QObject::tr("Two-pass GPU conversion"), QStringLiteral("secondaryText"));
    note->setObjectName(QStringLiteral("pipelineRouteNote"));
    note->setToolTip(QObject::tr(
        "The default NVENC path converts on the GPU, then copies NV12 from Vulkan into CUDA. The direct native-size conversion path remains under validation after a recurring black-frame fault."));
    note->setAccessibleName(QObject::tr(
        "Two-pass GPU conversion is selected. The direct native-size conversion path remains under validation after a recurring black-frame fault."));
    rail->addWidget(note);
    root->addLayout(rail);
    return {panel, fallback, preview_stack, quick_preview};
}

} // namespace

MainWindow::MainWindow(RecorderSettings settings, QWidget* parent)
    : QMainWindow(parent), settings_(std::move(settings)) {
    normalizeSceneProfiles(settings_);
    setWindowTitle(tr("Fluxrec — Recording workbench"));
    setObjectName(QStringLiteral("fluxrecMainWindow"));
    setMinimumSize(900, 600);
    resize(1540, 980);
    setDockNestingEnabled(true);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks |
                   QMainWindow::AllowTabbedDocks | QMainWindow::GroupedDragging);

    backend_ = new ProcessRecorderBackend(settings_, this);
    create_workspace();
    create_menus();
    create_toolbar();

    connect(backend_, &RecorderBackend::state_changed, this, [this] {
        if (backend_->state() == RecorderBackend::State::replay_only &&
            operation_status_ == tr("Starting replay-only session…")) {
            operation_status_.clear();
        }
        update_controls();
        update_source_panel();
        audio_panel_->setRecordingActive(process_active());
        audio_panel_->setSessionBusy(process_active());
        audio_panel_->setReplayOnlySession(backend_->replay_only_session(),
                                           backend_->audio_enabled_for_session());
        if (backend_->state() == RecorderBackend::State::starting) {
            audio_panel_->beginSession();
        }
        update_output_path();
        auto* statistics = static_cast<StatisticsPanel*>(statistics_panel_);
        if (backend_->state() == RecorderBackend::State::starting) {
            statistics->beginSession();
        }
        statistics->refreshFreshness(backend_->state());
    });
    connect(backend_, &RecorderBackend::telemetry_received, this,
            [this](const TelemetrySnapshot& snapshot) {
                const RecorderBackend::State state = backend_->state();
                const bool include_in_average =
                    state == RecorderBackend::State::starting ||
                    state == RecorderBackend::State::recording ||
                    state == RecorderBackend::State::replay_only;
                static_cast<StatisticsPanel*>(statistics_panel_)->setSnapshot(
                    snapshot, include_in_average);
                audio_panel_->setSnapshot(
                    snapshot.values.value(QStringLiteral("audio")).toObject(), snapshot.received_at);
                update_controls();
            });
    connect(backend_, &RecorderBackend::replay_buffer_changed, this, [this](bool) {
        update_controls();
    });
    connect(backend_, &RecorderBackend::audio_control_changed,
            audio_panel_, &AudioMixerPanel::applyControlChanged);
    connect(backend_, &RecorderBackend::audio_control_failed,
            audio_panel_, &AudioMixerPanel::applyControlFailed);
    connect(backend_, &RecorderBackend::error_changed,
            this, [this](const QString& message) {
                controls_error_label_->setText(message);
                controls_error_label_->setVisible(!message.isEmpty());
                update_controls();
            });
    connect(backend_, &RecorderBackend::diagnostic_line,
            this, [this](const QString& line) {
                engine_log_->appendPlainText(line);
            });
    connect(backend_, &RecorderBackend::recording_finished,
            this, &MainWindow::handle_recording_finished);
    connect(backend_, &RecorderBackend::replay_session_finished,
            this, &MainWindow::handle_replay_session_finished);
    connect(backend_, &RecorderBackend::replay_save_queued, this,
            [this](const QString& path) {
                operation_status_ = tr("Replay queued · %1")
                    .arg(QFileInfo(path).fileName());
                update_controls();
            });
    connect(backend_, &RecorderBackend::replay_saved, this,
            [this](const QString& path, bool success) {
                if (QDir::cleanPath(QFileInfo(path).absoluteFilePath()) !=
                    QDir::cleanPath(QFileInfo(pending_replay_path_).absoluteFilePath())) {
                    engine_log_->appendPlainText(
                        tr("Ignored replay completion for an unexpected path: %1").arg(path));
                    return;
                }
                replay_save_pending_ = false;
                pending_replay_path_.clear();
                if (success) {
                    operation_status_ = tr("Replay saved · %1").arg(QFileInfo(path).fileName());
                    statusBar()->showMessage(tr("Replay saved: %1")
                                                 .arg(QFileInfo(path).fileName()), 10000);
                } else {
                    operation_status_ = tr("Replay could not be saved. See Controls and the engine log.");
                    statusBar()->showMessage(tr("Replay could not be saved. See Controls and Engine log."));
                }
                update_controls();
            });

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(1000);
    connect(refresh_timer_, &QTimer::timeout, this, &MainWindow::refresh_state);
    refresh_timer_->start();

    QSettings persisted;
    const QByteArray saved_state = persisted.value(QStringLiteral("window/state")).toByteArray();
    if (!saved_state.isEmpty()) restoreState(saved_state, 1);
    const QByteArray saved_geometry = persisted.value(QStringLiteral("window/geometry")).toByteArray();
    if (!saved_geometry.isEmpty()) restoreGeometry(saved_geometry);
    else {
        resizeDocks({sources_dock_, controls_dock_}, {240, 280}, Qt::Horizontal);
        resizeDocks({workspace_dock_}, {270}, Qt::Vertical);
    }

    update_source_panel();
    update_output_path();
    update_capture_path();
    update_controls();
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::create_workspace() {
    source_panel_ = new SceneProfilesPanel(settings_.scenes, settings_.selected_scene_id, this);
    sources_dock_ = new QDockWidget(tr("Scenes & sources"), this);
    sources_dock_->setObjectName(QStringLiteral("scenesSourcesDock"));
    sources_dock_->setWidget(source_panel_);
    sources_dock_->setMinimumWidth(205);
    sources_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::LeftDockWidgetArea, sources_dock_);
    connect(source_panel_, &SceneProfilesPanel::configurationChanged,
            this, &MainWindow::apply_scene_configuration);

    const PreviewPanelParts preview = buildPreviewPanel(backend_->preview_channel(), settings_, this);
    preview_canvas_ = preview.canvas;
    preview_stack_ = preview.stack;
    preview_quick_ = preview.quick;
    setCentralWidget(preview.panel);

    auto* controls = new QWidget(this);
    auto* controls_layout = new QVBoxLayout(controls);
    controls_layout->setContentsMargins(theme::panel_inset, theme::panel_inset,
                                        theme::panel_inset, theme::panel_inset);
    controls_layout->setSpacing(theme::panel_inset);

    engine_state_label_ = makeLabel({}, QStringLiteral("engineState"));
    engine_state_label_->setAccessibleName(tr("Engine status"));
    engine_state_label_->setMinimumHeight(theme::control_height);
    controls_layout->addWidget(engine_state_label_);
    state_detail_label_ = makeLabel({}, QStringLiteral("secondaryText"));
    state_detail_label_->setObjectName(QStringLiteral("stateDetail"));
    controls_layout->addWidget(state_detail_label_);

    start_recording_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay),
                                               tr("Start recording"), controls);
    start_recording_button_->setObjectName(QStringLiteral("recordButton"));
    start_recording_button_->setMinimumHeight(theme::control_height + 4);
    start_recording_button_->setAccessibleName(tr("Start recording"));
    start_recording_button_->setToolTip(tr("Start a new hardware encoded MKV recording."));
    controls_layout->addWidget(start_recording_button_);

    stop_recording_button_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaStop),
                                              tr("Stop recording"), controls);
    stop_recording_button_->setObjectName(QStringLiteral("stopButton"));
    stop_recording_button_->setMinimumHeight(theme::control_height + 4);
    stop_recording_button_->setAccessibleName(tr("Stop recording and finalize the MKV file"));
    stop_recording_button_->setToolTip(tr("Ask the recorder to flush and close the current file."));
    controls_layout->addWidget(stop_recording_button_);

    recording_elapsed_label_ = makeLabel(tr("Elapsed · —"), QStringLiteral("secondaryText"));
    recording_elapsed_label_->setObjectName(QStringLiteral("recordingElapsed"));
    recording_elapsed_label_->setAccessibleName(tr("Recording duration"));
    controls_layout->addWidget(recording_elapsed_label_);

    output_file_label_ = makeLabel(tr("No recording file selected"), QStringLiteral("secondaryText"));
    output_file_label_->setObjectName(QStringLiteral("activeOutputPath"));
    output_file_label_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    controls_layout->addWidget(output_file_label_);

    controls_error_label_ = makeLabel({}, QStringLiteral("errorText"));
    controls_error_label_->setObjectName(QStringLiteral("controlsError"));
    controls_error_label_->setAccessibleName(tr("Recorder error"));
    controls_error_label_->setVisible(false);
    controls_layout->addWidget(controls_error_label_);

    controls_layout->addWidget(makeRule(controls));
    controls_layout->addWidget(makeLabel(tr("Replay buffer"), QStringLiteral("sectionTitle")));
    replay_state_label_ = makeLabel(tr("Not active"), QStringLiteral("secondaryText"));
    replay_state_label_->setAccessibleName(tr("Replay buffer status"));
    controls_layout->addWidget(replay_state_label_);

    start_replay_button_ = new QPushButton(tr("Start replay buffer"), controls);
    start_replay_button_->setAccessibleName(tr("Start replay buffer"));
    stop_replay_button_ = new QPushButton(tr("Stop replay buffer"), controls);
    stop_replay_button_->setAccessibleName(tr("Stop replay buffer"));
    save_replay_button_ = new QPushButton(tr("Save replay"), controls);
    save_replay_button_->setAccessibleName(tr("Save replay"));
    controls_layout->addWidget(start_replay_button_);
    controls_layout->addWidget(stop_replay_button_);
    controls_layout->addWidget(save_replay_button_);
    controls_layout->addStretch(1);

    auto* settings_button = new QPushButton(tr("Settings…"), controls);
    settings_button->setObjectName(QStringLiteral("openSettingsButton"));
    settings_button->setAccessibleName(tr("Open recording settings"));
    controls_layout->addWidget(settings_button);

    controls_dock_ = new QDockWidget(tr("Controls"), this);
    controls_dock_->setObjectName(QStringLiteral("controlsDock"));
    controls_dock_->setWidget(controls);
    controls_dock_->setMinimumWidth(230);
    controls_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, controls_dock_);

    workspace_tabs_ = new QTabWidget(this);
    workspace_tabs_->setObjectName(QStringLiteral("lowerWorkspaceTabs"));
    workspace_tabs_->setDocumentMode(true);
    workspace_tabs_->setMovable(false);
    audio_panel_ = new AudioMixerPanel(workspace_tabs_);
    audio_panel_->setAudioConfigured(settings_.audio_enabled);
    audio_panel_->setTracks(effectiveAudioTracks(settings_), settings_.audio_tracks_custom);
    {
        AudioDeviceCatalog catalog;
        (void)catalog.refresh();
        audio_panel_->setDevices(catalog);
    }
    audio_panel_->setControlHandlers(
        [this](const QString& source, double gain) {
            backend_->set_audio_volume(source, gain);
        },
        [this](const QString& source, bool muted) {
            backend_->set_audio_muted(source, muted);
        });
    connect(audio_panel_, &AudioMixerPanel::tracksEdited, this,
            [this](const QList<AudioTrackSettings>& tracks) {
                settings_.audio_tracks = tracks;
                settings_.audio_tracks_custom = true;
                saveSettings(settings_);
                backend_->set_settings(settings_);
            });
    statistics_panel_ = new StatisticsPanel(workspace_tabs_);
    workspace_tabs_->addTab(audio_panel_, tr("Audio mixer"));
    workspace_tabs_->addTab(statistics_panel_, tr("Statistics"));
    workspace_tabs_->setCurrentWidget(statistics_panel_);
    workspace_tabs_->setAccessibleName(tr("Audio and statistics workspace"));

    workspace_dock_ = new QDockWidget(tr("Mixer & statistics"), this);
    workspace_dock_->setObjectName(QStringLiteral("mixerStatisticsDock"));
    workspace_dock_->setWidget(workspace_tabs_);
    workspace_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::BottomDockWidgetArea, workspace_dock_);

    engine_log_ = new QPlainTextEdit(this);
    engine_log_->setReadOnly(true);
    engine_log_->setMaximumBlockCount(500);
    engine_log_->setLineWrapMode(QPlainTextEdit::NoWrap);
    engine_log_->setAccessibleName(tr("Recorder diagnostic log"));
    log_dock_ = new QDockWidget(tr("Engine log"), this);
    log_dock_->setObjectName(QStringLiteral("engineLogDock"));
    log_dock_->setWidget(engine_log_);
    log_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                           QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, log_dock_);
    log_dock_->hide();

    connect(start_recording_button_, &QPushButton::clicked,
            this, &MainWindow::start_recording);
    connect(stop_recording_button_, &QPushButton::clicked,
            this, &MainWindow::stop_recording);
    connect(save_replay_button_, &QPushButton::clicked,
            this, &MainWindow::save_replay);
    connect(start_replay_button_, &QPushButton::clicked,
            this, &MainWindow::start_replay_buffer);
    connect(stop_replay_button_, &QPushButton::clicked,
            backend_, &RecorderBackend::stop_replay_buffer);
    connect(settings_button, &QPushButton::clicked,
            this, &MainWindow::open_settings);
}

void MainWindow::create_menus() {
    auto* file_menu = menuBar()->addMenu(tr("File"));
    settings_action_ = file_menu->addAction(tr("Settings…"));
    settings_action_->setShortcut(QKeySequence::Preferences);
    settings_action_->setStatusTip(tr("Choose the capture target and recording options."));
    connect(settings_action_, &QAction::triggered, this, &MainWindow::open_settings);
    file_menu->addSeparator();
    auto* quit_action = file_menu->addAction(tr("Quit Fluxrec"));
    quit_action->setShortcut(QKeySequence::Quit);
    connect(quit_action, &QAction::triggered, this, &QWidget::close);

    auto* view_menu = menuBar()->addMenu(tr("View"));
    view_menu->addAction(sources_dock_->toggleViewAction());
    view_menu->addAction(controls_dock_->toggleViewAction());
    view_menu->addAction(workspace_dock_->toggleViewAction());
    view_menu->addAction(log_dock_->toggleViewAction());

    auto* help_menu = menuBar()->addMenu(tr("Help"));
    auto* about = help_menu->addAction(tr("About Fluxrec"));
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox dialog(QMessageBox::Information, tr("About Fluxrec"),
                           tr("Fluxrec is a native Wayland recording workbench.\n\nThe preview and performance readings are shown only when their runtime source is connected."),
                           QMessageBox::Ok, this);
        dialog.exec();
    });
}

void MainWindow::create_toolbar() {
    auto* toolbar = addToolBar(tr("Application"));
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    settings_action_->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    toolbar->addAction(settings_action_);
    toolbar->addSeparator();
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    toolbar_state_label_ = makeLabel({}, QStringLiteral("engineState"));
    toolbar_state_label_->setAccessibleName(tr("Engine status"));
    toolbar->addWidget(toolbar_state_label_);
    auto* target = makeLabel(tr("Target · %1").arg(settings_.display_name), QStringLiteral("eyebrow"));
    target->setObjectName(QStringLiteral("toolbarTarget"));
    target->setAccessibleName(tr("Configured capture output: %1").arg(settings_.display_name));
    toolbar->addSeparator();
    toolbar->addWidget(target);
    auto* backend_label = makeLabel({}, QStringLiteral("eyebrow"));
    backend_label->setObjectName(QStringLiteral("toolbarBackend"));
    toolbar->addSeparator();
    toolbar->addWidget(backend_label);
}

void MainWindow::open_settings() {
    SettingsDialog dialog(settings_, this, !process_active());
    if (dialog.exec() != QDialog::Accepted) return;
    const RecorderSettings updated = dialog.settings();
    saveSettings(updated);
    apply_settings(updated);
}

void MainWindow::apply_settings(const RecorderSettings& settings) {
    settings_ = settings;
    normalizeSceneProfiles(settings_);
    backend_->set_settings(settings_);
    source_panel_->setProfiles(settings_.scenes, settings_.selected_scene_id);
    audio_panel_->setAudioConfigured(settings_.audio_enabled);
    audio_panel_->setTracks(effectiveAudioTracks(settings_), settings_.audio_tracks_custom);
    update_source_panel();
    update_output_path();
    update_capture_path();
    update_controls();
}

void MainWindow::apply_scene_configuration() {
    if (process_active()) return;
    settings_.scenes = source_panel_->profiles();
    settings_.selected_scene_id = source_panel_->selectedSceneId();
    settings_.display_name = source_panel_->selectedOutputName();
    normalizeSceneProfiles(settings_);
    source_panel_->setProfiles(settings_.scenes, settings_.selected_scene_id);
    saveSettings(settings_);
    backend_->set_settings(settings_);
    update_source_panel();
    update_output_path();
    update_controls();
}

void MainWindow::start_recording() {
    if (!backend_->can_start_recording()) return;
    operation_status_.clear();
    active_session_output_name_ = settings_.display_name;
    update_source_panel();
    const QString path = makeOutputPath(settings_);
    output_file_label_->setText(path);
    output_file_label_->setToolTip(path);
    state_detail_label_->setText(tr("Preparing a new Matroska recording."));
    backend_->start_recording(path);
}

void MainWindow::stop_recording() {
    backend_->stop_recording();
}

void MainWindow::start_replay_buffer() {
    if (!backend_->can_start_replay_buffer()) return;
    operation_status_.clear();
    if (backend_->can_start_replay_only_session()) {
        active_session_output_name_ = settings_.display_name;
        output_file_label_->setText(tr("Replay-only session · no primary output file"));
        output_file_label_->setToolTip(tr(
            "Replay-only capture writes no primary Matroska file. Use Save replay to export retained video and enabled audio history."));
        operation_status_ = tr("Starting replay-only session…");
    }
    update_source_panel();
    backend_->start_replay_buffer();
    update_controls();
}

void MainWindow::save_replay() {
    if (!backend_->can_save_replay()) return;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QDir directory(settings_.output_directory);
    const QString base = QStringLiteral("Fluxrec_Replay_%1").arg(stamp);
    QString path = directory.absoluteFilePath(base + QStringLiteral(".mkv"));
    for (int suffix = 2; QFileInfo::exists(path); ++suffix) {
        path = directory.absoluteFilePath(QStringLiteral("%1_%2.mkv")
                                              .arg(base)
                                              .arg(suffix, 2, 10, QLatin1Char('0')));
    }
    replay_save_pending_ = true;
    pending_replay_path_ = path;
    operation_status_ = tr("Saving replay…");
    update_controls();
    backend_->save_replay(path);
}

void MainWindow::refresh_state() {
    update_controls();
    static_cast<StatisticsPanel*>(statistics_panel_)->refreshFreshness(backend_->state());
    audio_panel_->refreshFreshness();
    if (backend_->state() == RecorderBackend::State::recording ||
        backend_->state() == RecorderBackend::State::replay_only ||
        backend_->state() == RecorderBackend::State::stopping) {
        update_elapsed_time();
    }
}

void MainWindow::update_elapsed_time() {
    if (!recording_clock_.isValid()) return;
    const qint64 seconds = recording_clock_.elapsed() / 1000;
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds / 60) % 60;
    const qint64 remainder = seconds % 60;
    recording_elapsed_label_->setText(tr("Elapsed · %1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(remainder, 2, 10, QLatin1Char('0')));
}

void MainWindow::handle_recording_finished(const QString& output_path, bool success) {
    recording_elapsed_label_->setText(tr("Elapsed · —"));
    active_session_output_name_.clear();
    if (success) {
        operation_status_ = tr("Recording saved · %1")
            .arg(QFileInfo(output_path).fileName());
        statusBar()->showMessage(tr("Recording finalized: %1")
                                     .arg(QFileInfo(output_path).fileName()), 10000);
    } else {
        operation_status_ = tr("Recording could not be finalized. See Controls and the engine log.");
        statusBar()->showMessage(tr("Recording failed. See Controls and Engine log for details."));
    }
    update_source_panel();
    update_controls();
    if (close_after_session_ && success) {
        close_after_session_ = false;
        QTimer::singleShot(0, this, &QWidget::close);
    } else if (close_after_session_) {
        close_after_session_ = false;
    }
}

void MainWindow::handle_replay_session_finished(bool success) {
    recording_elapsed_label_->setText(tr("Elapsed · —"));
    active_session_output_name_.clear();
    if (success) {
        operation_status_ = tr("Replay-only session stopped · no primary recording file was written");
        statusBar()->showMessage(tr("Replay-only session stopped cleanly."), 10000);
    } else {
        operation_status_ = tr("Replay-only session failed. See Controls and the engine log.");
        statusBar()->showMessage(tr("Replay-only session failed. See Controls and Engine log."));
    }
    update_source_panel();
    update_controls();
    if (close_after_session_ && success) {
        close_after_session_ = false;
        QTimer::singleShot(0, this, &QWidget::close);
    } else if (close_after_session_) {
        close_after_session_ = false;
    }
}

void MainWindow::update_controls() {
    const RecorderBackend::State current = backend_->state();
    const bool busy = current == RecorderBackend::State::starting ||
        current == RecorderBackend::State::recording || current == RecorderBackend::State::replay_only ||
        current == RecorderBackend::State::stopping;
    const bool replay_only_session = backend_->replay_only_session();
    const bool idle_replay_only_start = backend_->can_start_replay_only_session();
    const bool settings_gate_relevant = current == RecorderBackend::State::available ||
        current == RecorderBackend::State::failed;
    const bool hevc_blocked = settings_gate_relevant && hevcAcknowledgementRequired(settings_);
    const bool resolution_invalid = settings_gate_relevant && !hasValidOutputResolution(settings_);
    const bool bitrate_invalid = settings_gate_relevant &&
        settings_.rate_control == QStringLiteral("bitrate") &&
        (settings_.bitrate_mbps < 1 || settings_.bitrate_mbps > 500);
    const bool qp_invalid = settings_gate_relevant &&
        settings_.rate_control == QStringLiteral("constant_qp") &&
        (settings_.qp < 0 || settings_.qp > 51);
    start_recording_button_->setEnabled(backend_->can_start_recording());
    stop_recording_button_->setEnabled(current == RecorderBackend::State::starting ||
                                        current == RecorderBackend::State::recording ||
                                        current == RecorderBackend::State::replay_only);
    stop_recording_button_->setText(replay_only_session
        ? tr("Stop replay-only session") : tr("Stop recording"));
    stop_recording_button_->setAccessibleName(replay_only_session
        ? tr("Stop replay-only session")
        : tr("Stop recording and finalize the MKV file"));
    start_replay_button_->setText(idle_replay_only_start
        ? tr("Start replay-only session") : tr("Start replay buffer"));
    start_replay_button_->setAccessibleName(start_replay_button_->text());
    start_replay_button_->setEnabled(backend_->can_start_replay_buffer());
    stop_replay_button_->setEnabled(backend_->can_stop_replay_buffer());
    save_replay_button_->setEnabled(backend_->can_save_replay() && !replay_save_pending_);

    const bool replay_available = backend_->replay_buffer_available();
    const bool replay_active = backend_->replay_buffer_active();
    const bool replay_pending = backend_->replay_control_pending();
    if (replay_pending) {
        replay_state_label_->setText(backend_->replay_control_target_active()
            ? tr("Starting replay buffer…") : tr("Stopping replay buffer…"));
        replay_state_label_->setAccessibleDescription(tr(
            "Waiting for the recorder to confirm the requested replay buffer state."));
    } else if (replay_available && !backend_->replay_buffer_reported()) {
        replay_state_label_->setText(tr("Replay state not reported yet"));
        replay_state_label_->setAccessibleDescription(tr(
            "The recorder reports replay support, but has not yet reported whether the replay buffer is active."));
    } else if (replay_available && replay_active) {
        replay_state_label_->setText(replay_only_session
            ? tr("Replay-only session · active packet history")
            : tr("Active · recording compressed packet history"));
        replay_state_label_->setAccessibleDescription(tr(
            "The recorder reports that the replay buffer is active and retaining compressed packets. No primary recording file is written in a replay-only session."));
    } else if (replay_available) {
        replay_state_label_->setText(tr("Stopped · retained history can still be saved"));
        replay_state_label_->setAccessibleDescription(tr(
            "The replay buffer is stopped. Its retained packet history remains available to save."));
    } else if (backend_->replay_buffer_reported()) {
        replay_state_label_->setText(tr("Off · no replay buffer in this session"));
        replay_state_label_->setAccessibleDescription(tr(
            "The recorder reports that no replay buffer is available in this session."));
    } else if (current == RecorderBackend::State::unavailable &&
               settings_.replay_history_seconds > 0) {
        replay_state_label_->setText(tr("Configured · recorder not found"));
        replay_state_label_->setAccessibleDescription(tr(
            "Replay history is configured, but no recorder executable is available. Set its path in Settings."));
    } else if (settings_.replay_history_seconds > 0 && busy) {
        replay_state_label_->setText(tr("Waiting for replay status from recorder…"));
        replay_state_label_->setAccessibleDescription(tr(
            "Replay history is configured. Waiting for the recorder to report whether a packet buffer is available."));
    } else if (hevc_blocked && settings_.replay_history_seconds > 0) {
        replay_state_label_->setText(tr("HEVC experimental · acknowledgement required"));
        replay_state_label_->setAccessibleDescription(tr(
            "Replay capture is blocked until HEVC limitations are acknowledged in Settings or H.264 is selected."));
    } else if (resolution_invalid && settings_.replay_history_seconds > 0) {
        replay_state_label_->setText(tr("Output resolution needs correction"));
        replay_state_label_->setAccessibleDescription(tr(
            "The saved output dimensions are invalid. Choose native capture or valid even dimensions in Settings."));
    } else if (settings_.replay_history_seconds > 0) {
        replay_state_label_->setText(tr("Configured · ready to start replay-only capture"));
        replay_state_label_->setAccessibleDescription(tr(
            "Replay history is configured. Start a recording or start a replay-only capture session."));
    } else {
        replay_state_label_->setText(tr("Off · enable history in Settings"));
        replay_state_label_->setAccessibleDescription(tr(
            "Replay history is disabled. Choose a history duration in Settings before starting a recording."));
    }
    const QString start_recording_reason = backend_->can_start_recording()
        ? tr("Start a new MKV recording with the configured capture settings.")
        : current == RecorderBackend::State::unavailable
            ? tr("Choose an available fluxrec-record executable in Settings.")
            : busy
                ? tr("Stop the active recording or replay-only session before starting another.")
                : hevc_blocked
                ? tr("HEVC is blocked until you acknowledge its experimental capture limitations in Settings, or select H.264.")
                : resolution_invalid
                    ? tr("Choose native capture dimensions or valid even output dimensions in Settings.")
                    : bitrate_invalid
                        ? tr("Set the video bitrate to a value from 1 to 500 Mbps in Settings.")
                        : qp_invalid
                            ? tr("Set CQP to a QP value from 0 to 51 in Settings.")
                        : tr("Resolve the recorder error before starting again.");
    start_recording_button_->setToolTip(start_recording_reason);
    start_recording_button_->setAccessibleDescription(start_recording_reason);
    const QString stop_recording_reason = stop_recording_button_->isEnabled()
        ? replay_only_session
            ? tr("Stop replay-only capture and let the recorder finish its active work. No primary MKV file is written.")
            : tr("Ask the recorder to flush and finalize the current MKV file.")
        : current == RecorderBackend::State::stopping
            ? replay_only_session
                ? tr("The replay-only session is already stopping.")
                : tr("The recorder is already finalizing the current MKV file.")
            : tr("Start a recording or replay-only session to enable this action.");
    stop_recording_button_->setToolTip(stop_recording_reason);
    stop_recording_button_->setAccessibleDescription(stop_recording_reason);
    const QString start_replay_reason = backend_->can_start_replay_buffer()
        ? idle_replay_only_start
            ? settings_.audio_enabled
                ? tr("Launch replay-only capture. It retains video and audio history without writing a primary MKV file.")
                : tr("Launch replay-only capture. It retains video history without writing a primary MKV file.")
            : tr("Start a new compressed-packet replay history window.")
        : replay_pending
            ? tr("Wait for the current replay control request to finish.")
            : !replay_available
                ? current == RecorderBackend::State::unavailable
                    ? tr("Choose an available fluxrec-record executable in Settings.")
                    : hevc_blocked
                        ? tr("HEVC is blocked until you acknowledge its experimental capture limitations in Settings, or select H.264.")
                    : resolution_invalid
                        ? tr("Choose native capture dimensions or valid even output dimensions in Settings.")
                    : bitrate_invalid
                        ? tr("Set the video bitrate to a value from 1 to 500 Mbps in Settings.")
                    : qp_invalid
                        ? tr("Set CQP to a QP value from 0 to 51 in Settings.")
                    : backend_->replay_buffer_reported()
                    ? tr("The recorder reports no replay buffer in this session.")
                    : settings_.replay_history_seconds > 0
                        ? tr("Waiting for the recorder to report replay status.")
                        : tr("Set replay history in Settings before starting a capture session.")
                : backend_->replay_buffer_reported()
                    ? tr("The replay buffer is already active.")
                    : tr("Waiting for the recorder to report replay state.");
    const QString stop_replay_reason = backend_->can_stop_replay_buffer()
        ? tr("Stop adding packets to replay history. Existing history remains available to save.")
        : replay_pending
            ? tr("Wait for the current replay control request to finish.")
            : !replay_available
                ? backend_->replay_buffer_reported()
                    ? tr("No replay buffer is available in this session.")
                    : tr("Waiting for the recorder to report replay status.")
                : !backend_->replay_buffer_reported()
                    ? tr("Waiting for the recorder to report replay state.")
                    : tr("The replay buffer is already stopped.");
    start_replay_button_->setToolTip(start_replay_reason);
    stop_replay_button_->setToolTip(stop_replay_reason);
    start_replay_button_->setAccessibleDescription(start_replay_button_->toolTip());
    stop_replay_button_->setAccessibleDescription(stop_replay_button_->toolTip());
    const QString save_replay_reason = replay_save_pending_
        ? tr("Waiting for the recorder to confirm the replay file.")
        : backend_->can_save_replay()
            ? replay_active
                ? tr("Save the retained replay history to a new Matroska file.")
                : tr("Save the retained replay history from the stopped buffer to a new Matroska file.")
            : backend_->state() != RecorderBackend::State::recording &&
              backend_->state() != RecorderBackend::State::replay_only
                ? tr("Start recording or a replay-only session with replay history enabled in Settings.")
                : replay_pending
                    ? tr("Wait for the replay mode change to be acknowledged.")
                    : !backend_->replay_buffer_reported()
                        ? tr("Waiting for the recorder to report replay state.")
                        : !replay_available
                            ? tr("The recorder reports no replay buffer in this session.")
                            : tr("Replay history is not available while the recorder is stopping.");
    save_replay_button_->setToolTip(save_replay_reason);
    save_replay_button_->setAccessibleDescription(save_replay_reason);

    QString status;
    QString detail;
    QString state_name;
    switch (current) {
    case RecorderBackend::State::unavailable:
        status = tr("ENGINE · NOT FOUND");
        detail = tr("Choose fluxrec-record in Settings to enable recording.");
        state_name = tr("Engine executable not found");
        break;
    case RecorderBackend::State::available:
        status = tr("ENGINE · AVAILABLE");
        detail = hevc_blocked
            ? tr("HEVC is selected but blocked until you acknowledge the experimental result in Settings, or choose H.264.")
            : resolution_invalid
                ? tr("Output dimensions are invalid. Choose native capture size or valid even dimensions in Settings.")
            : bitrate_invalid
                    ? tr("The video bitrate must be from 1 to 500 Mbps. Adjust it in Settings.")
                : qp_invalid
                    ? tr("The CQP value must be from 0 to 51. Adjust it in Settings.")
                    : tr("Ready to start the recorder process.");
        state_name = tr("Engine executable available");
        break;
    case RecorderBackend::State::starting:
        status = tr("ENGINE · STARTING");
        detail = replay_only_session
            ? settings_.audio_enabled
                ? tr("Starting replay-only capture. Video and audio will feed replay history; no primary MKV file is written.")
                : tr("Starting replay-only capture. Video will feed replay history; no primary MKV file is written.")
            : tr("Waiting for the recorder process to start.");
        state_name = tr("Recorder starting");
        break;
    case RecorderBackend::State::recording:
        status = tr("ENGINE · RECORDING");
        detail = tr("The recorder process is active.");
        state_name = tr("Recorder process active");
        break;
    case RecorderBackend::State::replay_only:
        status = tr("ENGINE · REPLAY ONLY");
        detail = settings_.audio_enabled
            ? tr("Video and audio are feeding replay history; no primary recording file is written.")
            : tr("Video is feeding replay history; no primary recording file is written.");
        state_name = settings_.audio_enabled
            ? tr("Replay-only capture session active; video and audio are feeding replay history")
            : tr("Replay-only capture session active; video is feeding replay history");
        break;
    case RecorderBackend::State::stopping:
        status = replay_only_session ? tr("ENGINE · STOPPING REPLAY ONLY")
                                     : tr("ENGINE · FINALIZING");
        detail = replay_only_session
            ? tr("Waiting for the replay-only recorder session to stop cleanly.")
            : tr("Waiting for the recorder to flush and close the MKV file.");
        state_name = replay_only_session ? tr("Replay-only session stopping")
                                         : tr("Recording finalizing");
        break;
    case RecorderBackend::State::failed:
        status = tr("ENGINE · ERROR");
        detail = tr("Inspect the error and engine log, then retry.");
        state_name = tr("Recorder error");
        break;
    }
    engine_state_label_->setText(status);
    toolbar_state_label_->setText(status);
    engine_state_label_->setAccessibleDescription(state_name);
    toolbar_state_label_->setAccessibleDescription(state_name);
    const char* visual_state = current == RecorderBackend::State::recording
        ? "recording" : current == RecorderBackend::State::replay_only
            ? "replay_only" : current == RecorderBackend::State::failed
            ? "failed" : current == RecorderBackend::State::unavailable
                ? "unavailable" : current == RecorderBackend::State::stopping
                    ? "stopping" : current == RecorderBackend::State::starting
                        ? "starting" : "available";
    engine_state_label_->setProperty("state", QString::fromLatin1(visual_state));
    toolbar_state_label_->setProperty("state", QString::fromLatin1(visual_state));
    for (QLabel* label : {engine_state_label_, toolbar_state_label_}) {
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
    const QString current_detail = current == RecorderBackend::State::failed &&
            !controls_error_label_->text().isEmpty()
        ? controls_error_label_->text() : detail;
    state_detail_label_->setText(operation_status_.isEmpty() ? current_detail : operation_status_);
    state_detail_label_->setToolTip(state_detail_label_->text());

    if ((current == RecorderBackend::State::recording ||
         current == RecorderBackend::State::replay_only) && !recording_clock_.isValid()) {
        recording_clock_.start();
    } else if (current == RecorderBackend::State::available ||
               current == RecorderBackend::State::unavailable ||
               current == RecorderBackend::State::failed) {
        recording_clock_.invalidate();
    }
    if (!busy) recording_elapsed_label_->setText(tr("Elapsed · —"));

    QString source_status;
    switch (current) {
    case RecorderBackend::State::unavailable:
        source_status = tr("Engine executable not found");
        break;
    case RecorderBackend::State::available:
        source_status = tr("Ready · not recording");
        break;
    case RecorderBackend::State::starting:
        source_status = replay_only_session
            ? tr("Starting replay-only capture…") : tr("Starting recorder…");
        break;
    case RecorderBackend::State::recording:
        source_status = tr("Recording active");
        break;
    case RecorderBackend::State::replay_only:
        source_status = tr("Replay-only capture active · no primary file");
        break;
    case RecorderBackend::State::stopping:
        source_status = replay_only_session
            ? tr("Stopping replay-only capture…") : tr("Finalizing recording…");
        break;
    case RecorderBackend::State::failed:
        source_status = tr("Recorder failed · see Controls");
        break;
    }
    source_panel_->setSessionContext(source_status, active_session_output_name_, busy);
}

void MainWindow::update_source_panel() {
    if (auto* toolbar_target = findChild<QLabel*>(QStringLiteral("toolbarTarget"))) {
        toolbar_target->setText(tr("Target · %1").arg(settings_.display_name));
        toolbar_target->setToolTip(settings_.display_name);
        toolbar_target->setAccessibleName(tr("Configured capture output: %1")
                                              .arg(settings_.display_name));
    }
    if (auto* preview_output = findChild<QLabel*>(QStringLiteral("previewOutputName"))) {
        preview_output->setText(tr("OUTPUT · %1").arg(settings_.display_name));
        preview_output->setAccessibleName(tr("Configured preview output: %1")
                                              .arg(settings_.display_name));
    }
    update_capture_path();
}

void MainWindow::update_capture_path() {
    const bool direct = settings_.encoder_backend == QStringLiteral("experimental-cuda-egl");
    const QStringList stages = direct
        ? QStringList{tr("WAYLAND"), tr("EGL"), tr("NVENC"), tr("MATROSKA")}
        : QStringList{tr("WAYLAND"), tr("VULKAN"), tr("NVENC"), tr("MATROSKA")};
    for (int index = 0; index < stages.size(); ++index) {
        if (auto* stage = findChild<QLabel*>(QStringLiteral("pipelineStage%1").arg(index))) {
            stage->setText(stages.at(index));
        }
    }
    if (auto* note = findChild<QLabel*>(QStringLiteral("pipelineRouteNote"))) {
        if (direct) {
            note->setText(tr("Direct CUDA EGL"));
            note->setToolTip(tr(
                "Application-level path: DMA-BUF to EGL to CUDA to NVENC, with no explicit raw-pixel copy in the application. This is not allocation-level zero-copy. GPU preview is not used."));
            note->setAccessibleName(tr(
                "Direct CUDA EGL is selected. This is an application-level GPU path, not allocation-level zero-copy. GPU preview is not used."));
        } else {
            note->setText(tr("Two-pass GPU conversion"));
            note->setToolTip(tr(
                "The default path converts on the GPU, then copies NV12 from Vulkan into CUDA. The direct native-size Vulkan conversion path remains under validation after a recurring black-frame fault."));
            note->setAccessibleName(tr(
                "Two-pass GPU conversion is selected. The direct native-size Vulkan conversion path remains under validation after a recurring black-frame fault."));
        }
    }
    if (auto* backend_label = findChild<QLabel*>(QStringLiteral("toolbarBackend"))) {
        const QString label = direct
            ? tr("Backend · Direct CUDA EGL")
            : settings_.encoder_backend == QStringLiteral("vulkan")
                ? tr("Backend · Vulkan Video")
                : tr("Backend · NVENC");
        backend_label->setText(label);
        backend_label->setAccessibleName(label);
    }
    auto* canvas = dynamic_cast<PreviewCanvas*>(preview_canvas_);
    if (!canvas || !preview_stack_) return;
    const bool headless = qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen");
    if (direct) {
        canvas->setStatus(tr("GPU preview not used"), tr(
            "Direct CUDA EGL does not publish a preview. Capture stays on the GPU path, and no CPU pixel copy is used. Recording remains available."));
        preview_stack_->setCurrentWidget(canvas);
        return;
    }
    if (headless || !preview_quick_) {
        canvas->setStatus(tr("GPU preview paused in headless mode"), tr(
            "The offscreen UI smoke test does not initialize Vulkan. Recording remains available, and no CPU pixel copy is used."));
        preview_stack_->setCurrentWidget(canvas);
        return;
    }
    preview_stack_->setCurrentWidget(preview_quick_);
}

void MainWindow::update_output_path() {
    if (backend_->state() == RecorderBackend::State::available ||
        backend_->state() == RecorderBackend::State::unavailable) {
        output_file_label_->setText(tr("New MKV files · %1")
                                        .arg(settings_.output_directory));
        output_file_label_->setToolTip(settings_.output_directory);
    }
}

bool MainWindow::process_active() const noexcept {
    const RecorderBackend::State current = backend_->state();
    return current == RecorderBackend::State::starting ||
        current == RecorderBackend::State::recording || current == RecorderBackend::State::replay_only ||
        current == RecorderBackend::State::stopping;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!process_active()) {
        QSettings persisted;
        persisted.setValue(QStringLiteral("window/state"), saveState(1));
        persisted.setValue(QStringLiteral("window/geometry"), saveGeometry());
        event->accept();
        return;
    }

    const bool replay_only_session = backend_->replay_only_session();
    QDialog confirmation(this);
    confirmation.setWindowTitle(replay_only_session
        ? tr("Stop replay-only session before quitting?")
        : tr("Finish recording before quitting?"));
    confirmation.setModal(true);
    auto* layout = new QVBoxLayout(&confirmation);
    layout->setContentsMargins(theme::panel_inset + 6, theme::panel_inset + 4,
                               theme::panel_inset + 6, theme::panel_inset + 4);
    auto* message = makeLabel(replay_only_session
        ? tr("Fluxrec must ask the replay-only recorder session to stop cleanly before the application exits. No primary MKV file is being written.")
        : tr("Fluxrec must ask the recorder to finalize the current MKV file before the application exits."));
    layout->addWidget(message);
    auto* actions = new QHBoxLayout;
    actions->addStretch(1);
    auto* keep = new QPushButton(replay_only_session
        ? tr("Keep replay-only session") : tr("Keep recording"), &confirmation);
    auto* stop_and_quit = new QPushButton(replay_only_session
        ? tr("Stop replay-only session and quit") : tr("Stop recording and quit"), &confirmation);
    stop_and_quit->setObjectName(QStringLiteral("recordButton"));
    keep->setDefault(true);
    keep->setFocus(Qt::TabFocusReason);
    actions->addWidget(keep);
    actions->addWidget(stop_and_quit);
    layout->addLayout(actions);
    connect(keep, &QPushButton::clicked, &confirmation, &QDialog::reject);
    connect(stop_and_quit, &QPushButton::clicked, &confirmation, &QDialog::accept);

    if (confirmation.exec() != QDialog::Accepted) {
        event->ignore();
        return;
    }
    close_after_session_ = true;
    stop_recording();
    event->ignore();
}

} // namespace fluxrec::ui
