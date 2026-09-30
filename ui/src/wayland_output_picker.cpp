#include "wayland_output_picker.hpp"

#include "theme_tokens.hpp"

#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace fluxrec::ui {
namespace {

constexpr int output_kind_role = Qt::UserRole + 1;
constexpr int available_output_kind = 1;
constexpr int missing_output_kind = 2;

} // namespace

WaylandOutputPicker::WaylandOutputPicker(const QString& output_name, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("waylandOutputPicker"));
    setAccessibleName(tr("Wayland capture output"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(theme::cluster_gap);

    auto* selector_row = new QHBoxLayout;
    selector_row->setContentsMargins(0, 0, 0, 0);
    selector_row->setSpacing(theme::cluster_gap);
    outputs_ = new QComboBox(this);
    outputs_->setObjectName(QStringLiteral("waylandOutputChoice"));
    outputs_->setAccessibleName(tr("Available Wayland outputs"));
    outputs_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    selector_row->addWidget(outputs_, 1);

    refresh_button_ = new QPushButton(tr("Refresh"), this);
    refresh_button_->setObjectName(QStringLiteral("refreshWaylandOutputsButton"));
    refresh_button_->setAccessibleName(tr("Refresh Wayland output list"));
    refresh_button_->setToolTip(tr("Read outputs currently reported by Qt on Wayland."));
    refresh_button_->setMinimumHeight(theme::dense_control_height);
    selector_row->addWidget(refresh_button_);
    root->addLayout(selector_row);

    manual_name_ = new QLineEdit(this);
    manual_name_->setObjectName(QStringLiteral("waylandOutputManualEdit"));
    manual_name_->setMaxLength(128);
    manual_name_->setPlaceholderText(tr("For example, DP-1"));
    manual_name_->setClearButtonEnabled(true);
    manual_name_->setAccessibleName(tr("Manual Wayland output name"));
    manual_name_->setVisible(false);
    root->addWidget(manual_name_);

    availability_ = new QLabel(this);
    availability_->setObjectName(QStringLiteral("waylandOutputAvailability"));
    availability_->setAccessibleName(tr("Wayland output availability"));
    availability_->setWordWrap(true);
    root->addWidget(availability_);

    connect(refresh_button_, &QPushButton::clicked, this,
            &WaylandOutputPicker::refreshOutputs);
    connect(outputs_, &QComboBox::currentIndexChanged, this, [this](int) {
        updateManualVisibility();
        updateStateText();
        emit outputChanged(outputName());
    });
    connect(manual_name_, &QLineEdit::textChanged, this, [this](const QString& text) {
        const int index = outputs_->currentIndex();
        if (index >= 0 &&
            outputs_->itemData(index, output_kind_role).toInt() == missing_output_kind) {
            const QString value = text.trimmed();
            const int detected_index = outputs_->findData(value, Qt::UserRole);
            if (!value.isEmpty() && detected_index >= 0 &&
                outputs_->itemData(detected_index, output_kind_role).toInt() == available_output_kind) {
                outputs_->setCurrentIndex(detected_index);
            } else {
                outputs_->setItemText(index, value.isEmpty()
                    ? tr("Unavailable · unnamed output") : tr("Unavailable · %1").arg(value));
                outputs_->setItemData(index, value, Qt::UserRole);
            }
        }
        updateStateText();
        emit outputChanged(outputName());
    });

    if (qGuiApp) {
        connect(qGuiApp, &QGuiApplication::screenAdded,
                this, &WaylandOutputPicker::scheduleOutputRefresh);
        connect(qGuiApp, &QGuiApplication::screenRemoved,
                this, &WaylandOutputPicker::scheduleOutputRefresh);
    }

    manual_name_->setText(output_name);
    refreshOutputs();
}

QString WaylandOutputPicker::outputName() const {
    const int index = outputs_->currentIndex();
    if (index < 0 ||
        outputs_->itemData(index, output_kind_role).toInt() == missing_output_kind) {
        return manual_name_->text().trimmed();
    }
    return outputs_->itemData(index, Qt::UserRole).toString().trimmed();
}

void WaylandOutputPicker::setOutputName(const QString& output_name) {
    const QString value = output_name.trimmed();
    const int available_index = outputs_->findData(value, Qt::UserRole);
    if (available_index >= 0 &&
        outputs_->itemData(available_index, output_kind_role).toInt() == available_output_kind) {
        outputs_->setCurrentIndex(available_index);
        manual_name_->setText(value);
    } else {
        int missing_index = -1;
        for (int index = 0; index < outputs_->count(); ++index) {
            if (outputs_->itemData(index, output_kind_role).toInt() == missing_output_kind) {
                missing_index = index;
                break;
            }
        }
        if (missing_index < 0) {
            outputs_->addItem(tr("Unavailable · %1").arg(value), value);
            missing_index = outputs_->count() - 1;
            outputs_->setItemData(missing_index, missing_output_kind, output_kind_role);
        } else {
            outputs_->setItemText(missing_index, value.isEmpty()
                ? tr("Unavailable · unnamed output") : tr("Unavailable · %1").arg(value));
            outputs_->setItemData(missing_index, value, Qt::UserRole);
        }
        manual_name_->setText(value);
        outputs_->setCurrentIndex(missing_index);
    }
    updateManualVisibility();
    updateStateText();
    emit outputChanged(outputName());
}

void WaylandOutputPicker::focusInput() {
    if (manual_name_->isVisible()) manual_name_->setFocus();
    else outputs_->setFocus();
}

void WaylandOutputPicker::setControlAccessibleDescription(const QString& description) {
    accessible_override_ = description;
    updateStateText();
}

bool WaylandOutputPicker::isWaylandPlatform() {
    return qGuiApp != nullptr && qGuiApp->platformName() == QStringLiteral("wayland");
}

QStringList WaylandOutputPicker::detectedOutputs() {
    if (!isWaylandPlatform() || qGuiApp == nullptr) return {};
    QStringList result;
    for (QScreen* screen : qGuiApp->screens()) {
        if (screen == nullptr) continue;
        const QString name = screen->name();
        if (!name.isEmpty() && !result.contains(name, Qt::CaseSensitive)) result.push_back(name);
    }
    std::sort(result.begin(), result.end(), [](const QString& left, const QString& right) {
        return left.compare(right, Qt::CaseSensitive) < 0;
    });
    return result;
}

bool WaylandOutputPicker::outputIsDetected(const QString& output_name) {
    return !output_name.trimmed().isEmpty() &&
        detectedOutputs().contains(output_name.trimmed(), Qt::CaseSensitive);
}

QString WaylandOutputPicker::outputAvailabilityText(const QString& output_name) {
    const QString value = output_name.trimmed();
    if (!isWaylandPlatform()) {
        const QString platform = qGuiApp ? qGuiApp->platformName() : tr("unknown");
        return tr("Qt is using the %1 platform, so compositor output names cannot be verified here. The recorder checks the exact name when it starts.")
            .arg(platform);
    }
    if (value.isEmpty()) {
        return tr("Choose an output reported by this Wayland session or enter its exact name manually.");
    }
    if (outputIsDetected(value)) {
        return tr("Detected in the current Wayland session.");
    }
    if (detectedOutputs().isEmpty()) {
        return tr("No Wayland outputs are currently reported by Qt. Refresh the list or enter the exact output name manually; the recorder checks availability when it starts.");
    }
    return tr("Not currently reported by Qt on Wayland. Refresh the list or choose another output; the recorder checks availability when it starts.");
}

void WaylandOutputPicker::refreshOutputs() {
    const QString current = outputs_->count() == 0
        ? manual_name_->text().trimmed() : outputName();
    detected_outputs_ = detectedOutputs();

    const QSignalBlocker combo_blocker(outputs_);
    const QSignalBlocker edit_blocker(manual_name_);
    outputs_->clear();
    for (const QString& name : detected_outputs_) {
        outputs_->addItem(name, name);
        outputs_->setItemData(outputs_->count() - 1, available_output_kind, output_kind_role);
    }

    int selected_index = outputs_->findData(current, Qt::UserRole);
    if (selected_index >= 0 &&
        outputs_->itemData(selected_index, output_kind_role).toInt() != available_output_kind) {
        selected_index = -1;
    }
    if (selected_index < 0 && !current.isEmpty()) {
        outputs_->addItem(tr("Unavailable · %1").arg(current), current);
        selected_index = outputs_->count() - 1;
        outputs_->setItemData(selected_index, missing_output_kind, output_kind_role);
    }

    outputs_->setCurrentIndex(selected_index);
    manual_name_->setText(current);
    updateManualVisibility();
    updateStateText();
}

void WaylandOutputPicker::updateStateText() {
    const QString value = outputName();
    const int index = outputs_->currentIndex();
    const bool manual_fallback = index < 0 ||
        outputs_->itemData(index, output_kind_role).toInt() == missing_output_kind;
    if (manual_fallback) {
        if (value.isEmpty()) {
            availability_->setText(tr("Manual fallback · enter the exact Wayland output name. The recorder checks availability when it starts."));
        } else {
            availability_->setText(tr("Manual fallback · %1").arg(outputAvailabilityText(value)));
        }
    } else {
        availability_->setText(outputAvailabilityText(value));
    }
    const QString accessible_description = accessible_override_.isEmpty()
        ? availability_->text() : accessible_override_;
    setAccessibleDescription(accessible_description);
    outputs_->setAccessibleDescription(accessible_description);
    manual_name_->setAccessibleDescription(accessible_description);
    outputs_->setToolTip(availability_->text());
    manual_name_->setToolTip(availability_->text());
}

void WaylandOutputPicker::updateManualVisibility() {
    const int index = outputs_->currentIndex();
    const bool needs_manual = index < 0 ||
        outputs_->itemData(index, output_kind_role).toInt() == missing_output_kind;
    manual_name_->setVisible(needs_manual);
    manual_name_->setEnabled(isEnabled());
    refresh_button_->setEnabled(isEnabled());
    outputs_->setEnabled(isEnabled());
}

void WaylandOutputPicker::scheduleOutputRefresh(QScreen*) {
    QTimer::singleShot(0, this, [this] { refreshOutputs(); });
}

} // namespace fluxrec::ui
