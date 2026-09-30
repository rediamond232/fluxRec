#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScreen;

namespace fluxrec::ui {

class WaylandOutputPicker final : public QWidget {
    Q_OBJECT

public:
    explicit WaylandOutputPicker(const QString& output_name, QWidget* parent = nullptr);

    [[nodiscard]] QString outputName() const;
    void setOutputName(const QString& output_name);
    void focusInput();
    void setControlAccessibleDescription(const QString& description);

    [[nodiscard]] static bool isWaylandPlatform();
    [[nodiscard]] static QStringList detectedOutputs();
    [[nodiscard]] static bool outputIsDetected(const QString& output_name);
    [[nodiscard]] static QString outputAvailabilityText(const QString& output_name);

signals:
    void outputChanged(const QString& output_name);

private:
    void refreshOutputs();
    void updateStateText();
    void updateManualVisibility();
    void scheduleOutputRefresh(QScreen* screen);

    QComboBox* outputs_ = nullptr;
    QLineEdit* manual_name_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QLabel* availability_ = nullptr;
    QStringList detected_outputs_;
    QString accessible_override_;
};

} // namespace fluxrec::ui
