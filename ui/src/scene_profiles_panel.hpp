#pragma once

#include "recorder_settings.hpp"

#include <QWidget>
#include <QStringList>

class QLabel;
class QListWidget;
class QPushButton;

namespace fluxrec::ui {

class SceneProfilesPanel final : public QWidget {
    Q_OBJECT

public:
    explicit SceneProfilesPanel(const QList<SceneProfile>& scenes,
                                const QString& selected_scene_id,
                                QWidget* parent = nullptr);

    [[nodiscard]] QList<SceneProfile> profiles() const;
    [[nodiscard]] QString selectedSceneId() const;
    [[nodiscard]] QString selectedOutputName() const;

    void setProfiles(const QList<SceneProfile>& scenes, const QString& selected_scene_id);
    void setSessionContext(const QString& status, const QString& active_output_name,
                           bool session_active);

signals:
    void configurationChanged();

private slots:
    void add_scene();
    void rename_scene();
    void remove_scene();
    void edit_output();

private:
    [[nodiscard]] int selectedIndex() const;
    [[nodiscard]] QString nextSceneName() const;
    [[nodiscard]] QStringList otherSceneNames(const QString& exclude_id = {}) const;
    void refreshSceneList();
    void refreshSourceDetails();
    void refreshActionState();

    QList<SceneProfile> scenes_;
    QString selected_scene_id_;
    QString session_status_;
    QString active_output_name_;
    bool session_active_ = false;
    QListWidget* scenes_list_ = nullptr;
    QLabel* selected_output_ = nullptr;
    QLabel* output_availability_ = nullptr;
    QLabel* source_status_ = nullptr;
    QPushButton* add_button_ = nullptr;
    QPushButton* rename_button_ = nullptr;
    QPushButton* remove_button_ = nullptr;
    QPushButton* edit_output_button_ = nullptr;
};

} // namespace fluxrec::ui
