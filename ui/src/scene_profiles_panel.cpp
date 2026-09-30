#include "scene_profiles_panel.hpp"

#include "theme_tokens.hpp"
#include "wayland_output_picker.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>
#include <array>

namespace fluxrec::ui {
namespace {

class SceneEditDialog final : public QDialog {
public:
    enum class Mode { add, rename, output };

    SceneEditDialog(const QString& title, const QString& name, const QString& output,
                    Mode mode, const QStringList& other_names, QWidget* parent)
        : QDialog(parent), mode_(mode), other_names_(other_names) {
        setWindowTitle(title);
        setModal(true);
        setMinimumWidth(430);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(theme::panel_inset + 8, theme::panel_inset + 6,
                                 theme::panel_inset + 8, theme::panel_inset + 6);
        root->setSpacing(theme::panel_inset);

        auto* description = new QLabel(
            tr("A scene stores one Wayland output as its capture source. Scenes do not combine or composite outputs."),
            this);
        description->setWordWrap(true);
        description->setObjectName(QStringLiteral("sceneEditorDescription"));
        root->addWidget(description);

        auto* form = new QFormLayout;
        form->setHorizontalSpacing(theme::panel_inset);
        form->setVerticalSpacing(theme::cluster_gap);
        name_ = new QLineEdit(name, this);
        name_->setObjectName(QStringLiteral("sceneNameEdit"));
        name_->setMaxLength(80);
        name_->setAccessibleName(tr("Scene name"));
        name_->setClearButtonEnabled(true);
        if (mode_ == Mode::output) name_->setVisible(false);
        if (mode_ != Mode::rename) {
            output_picker_ = new WaylandOutputPicker(output, this);
            output_picker_->setObjectName(QStringLiteral("sceneOutputPicker"));
        }
        if (mode_ == Mode::rename) {
            form->addRow(tr("Scene name"), name_);
        } else if (mode_ == Mode::output) {
            form->addRow(tr("Wayland output"), output_picker_);
        } else {
            form->addRow(tr("Scene name"), name_);
            form->addRow(tr("Wayland output"), output_picker_);
        }
        root->addLayout(form);

        auto* output_note = new QLabel(
            tr("Choose a detected Wayland output. Use manual entry when Qt does not report the output; the recorder confirms availability when it starts."),
            this);
        output_note->setObjectName(QStringLiteral("settingsNote"));
        output_note->setWordWrap(true);
        output_note->setVisible(mode_ != Mode::rename);
        root->addWidget(output_note);

        error_ = new QLabel(this);
        error_->setObjectName(QStringLiteral("validationMessage"));
        error_->setProperty("validation", true);
        error_->setAccessibleName(tr("Scene validation error"));
        error_->setWordWrap(true);
        error_->setVisible(false);
        root->addWidget(error_);

        auto* buttons = new QDialogButtonBox(this);
        auto* cancel = buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
        auto* save = buttons->addButton(mode_ == Mode::add ? tr("Add scene") : tr("Save changes"),
                                        QDialogButtonBox::AcceptRole);
        save->setDefault(true);
        save->setObjectName(QStringLiteral("confirmSceneEditButton"));
        cancel->setObjectName(QStringLiteral("cancelSceneEditButton"));
        QObject::connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        QObject::connect(save, &QPushButton::clicked, this, [this] { accept_if_valid(); });
        const auto clear_error_for = [this](QLineEdit* field) {
            QObject::connect(field, &QLineEdit::textChanged, this, [this, field] {
                if (error_field_ != field) return;
                field->setAccessibleDescription({});
                error_field_ = nullptr;
                error_->clear();
                error_->setAccessibleDescription({});
                error_->setVisible(false);
            });
        };
        clear_error_for(name_);
        if (output_picker_) {
            connect(output_picker_, &WaylandOutputPicker::outputChanged, this, [this] {
                if (error_field_ == output_picker_) clear_validation_error();
            });
        }
        root->addWidget(buttons);
    }

    [[nodiscard]] QString sceneName() const { return name_->text().trimmed(); }
    [[nodiscard]] QString outputName() const {
        return output_picker_ ? output_picker_->outputName() : QString{};
    }

private:
    void accept_if_valid() {
        clear_validation_error();
        const QString scene_name = sceneName();
        if (mode_ != Mode::output &&
            (scene_name.isEmpty() || scene_name.contains(QLatin1Char('\n')) ||
             scene_name.contains(QLatin1Char('\r')))) {
            show_validation_error(name_, tr("Enter one scene name."));
            return;
        }
        if (mode_ != Mode::output && other_names_.contains(scene_name, Qt::CaseInsensitive)) {
            show_validation_error(name_, tr("A scene with that name already exists."));
            return;
        }
        const QString output_name = outputName();
        if (mode_ != Mode::rename &&
            (output_name.isEmpty() || output_name.contains(QLatin1Char('\n')) ||
             output_name.contains(QLatin1Char('\r')))) {
            show_validation_error(output_picker_, tr("Choose an output or enter one Wayland output name."));
            return;
        }
        QDialog::accept();
    }

    void show_validation_error(QWidget* field, const QString& message) {
        error_field_ = field;
        error_->setText(message);
        error_->setAccessibleDescription(message);
        error_->setVisible(true);
        if (field == output_picker_) {
            output_picker_->setControlAccessibleDescription(message);
            output_picker_->focusInput();
        } else {
            field->setAccessibleDescription(message);
            field->setFocus();
        }
    }

    void clear_validation_error() {
        if (output_picker_ && error_field_ == output_picker_) {
            output_picker_->setControlAccessibleDescription({});
        } else if (error_field_) {
            error_field_->setAccessibleDescription({});
        }
        error_field_ = nullptr;
        error_->clear();
        error_->setAccessibleDescription({});
        error_->setVisible(false);
    }

    Mode mode_;
    QStringList other_names_;
    QLineEdit* name_ = nullptr;
    WaylandOutputPicker* output_picker_ = nullptr;
    QLabel* error_ = nullptr;
    QWidget* error_field_ = nullptr;
};

QFrame* makeSurface(QWidget* parent) {
    auto* surface = new QFrame(parent);
    surface->setObjectName(QStringLiteral("panelSurface"));
    surface->setFrameShape(QFrame::StyledPanel);
    return surface;
}

QFrame* makeRule(QWidget* parent) {
    auto* rule = new QFrame(parent);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Plain);
    rule->setFixedHeight(1);
    rule->setObjectName(QStringLiteral("sectionRule"));
    return rule;
}

QLabel* makeSectionTitle(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

} // namespace

SceneProfilesPanel::SceneProfilesPanel(const QList<SceneProfile>& scenes,
                                       const QString& selected_scene_id,
                                       QWidget* parent)
    : QWidget(parent), scenes_(scenes), selected_scene_id_(selected_scene_id) {
    setObjectName(QStringLiteral("sceneProfilesPanel"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(theme::panel_inset, theme::panel_inset,
                             theme::panel_inset, theme::panel_inset);
    root->setSpacing(theme::cluster_gap + 2);

    root->addWidget(makeSectionTitle(tr("Scenes"), this));
    scenes_list_ = new QListWidget(this);
    scenes_list_->setObjectName(QStringLiteral("sceneList"));
    scenes_list_->setAccessibleName(tr("Saved capture scenes"));
    scenes_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    scenes_list_->setMinimumHeight(110);
    root->addWidget(scenes_list_, 1);

    add_button_ = new QPushButton(tr("Add"), this);
    add_button_->setObjectName(QStringLiteral("addSceneButton"));
    add_button_->setAccessibleName(tr("Add scene"));
    add_button_->setMinimumHeight(theme::dense_control_height);
    root->addWidget(add_button_);

    auto* scene_actions = new QHBoxLayout;
    scene_actions->setSpacing(theme::cluster_gap);
    rename_button_ = new QPushButton(tr("Rename"), this);
    rename_button_->setObjectName(QStringLiteral("renameSceneButton"));
    rename_button_->setAccessibleName(tr("Rename selected scene"));
    remove_button_ = new QPushButton(tr("Remove"), this);
    remove_button_->setObjectName(QStringLiteral("removeSceneButton"));
    remove_button_->setAccessibleName(tr("Remove selected scene"));
    for (QPushButton* button : {rename_button_, remove_button_}) {
        button->setMinimumHeight(theme::dense_control_height);
        scene_actions->addWidget(button);
    }
    root->addLayout(scene_actions);

    root->addWidget(makeRule(this));
    root->addWidget(makeSectionTitle(tr("Sources"), this));
    auto* source_surface = makeSurface(this);
    auto* source_layout = new QVBoxLayout(source_surface);
    source_layout->setContentsMargins(theme::panel_inset, theme::panel_inset,
                                      theme::panel_inset, theme::panel_inset);
    source_layout->setSpacing(theme::cluster_gap);
    selected_output_ = new QLabel(source_surface);
    selected_output_->setObjectName(QStringLiteral("sceneSourceName"));
    selected_output_->setAccessibleName(tr("Selected scene capture source"));
    selected_output_->setWordWrap(true);
    source_layout->addWidget(selected_output_);
    output_availability_ = new QLabel(source_surface);
    output_availability_->setObjectName(QStringLiteral("sceneSourceAvailability"));
    output_availability_->setAccessibleName(tr("Selected output availability"));
    output_availability_->setWordWrap(true);
    source_layout->addWidget(output_availability_);
    source_status_ = new QLabel(source_surface);
    source_status_->setObjectName(QStringLiteral("sceneSourceStatus"));
    source_status_->setWordWrap(true);
    source_status_->setAccessibleName(tr("Capture source status"));
    source_layout->addWidget(source_status_);
    edit_output_button_ = new QPushButton(tr("Edit output…"), source_surface);
    edit_output_button_->setObjectName(QStringLiteral("editSceneOutputButton"));
    edit_output_button_->setAccessibleName(tr("Edit selected scene Wayland output"));
    source_layout->addWidget(edit_output_button_);
    root->addWidget(source_surface);

    auto* note = new QLabel(
        tr("Each scene selects one Wayland output. Scene selection applies to the next capture session; no live compositing is performed."),
        this);
    note->setObjectName(QStringLiteral("settingsNote"));
    note->setWordWrap(true);
    root->addWidget(note);

    connect(scenes_list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= scenes_.size()) return;
        const QString next_id = scenes_[row].id;
        if (next_id == selected_scene_id_) return;
        selected_scene_id_ = next_id;
        refreshSourceDetails();
        refreshActionState();
        emit configurationChanged();
    });
    connect(add_button_, &QPushButton::clicked, this, &SceneProfilesPanel::add_scene);
    connect(rename_button_, &QPushButton::clicked, this, &SceneProfilesPanel::rename_scene);
    connect(remove_button_, &QPushButton::clicked, this, &SceneProfilesPanel::remove_scene);
    connect(edit_output_button_, &QPushButton::clicked, this, &SceneProfilesPanel::edit_output);
    if (qGuiApp) {
        const auto refresh_availability = [this](QScreen*) {
            QTimer::singleShot(0, this, [this] { refreshSourceDetails(); });
        };
        connect(qGuiApp, &QGuiApplication::screenAdded, this, refresh_availability);
        connect(qGuiApp, &QGuiApplication::screenRemoved, this, refresh_availability);
    }

    refreshSceneList();
    refreshSourceDetails();
    refreshActionState();
}

QList<SceneProfile> SceneProfilesPanel::profiles() const {
    return scenes_;
}

QString SceneProfilesPanel::selectedSceneId() const {
    return selected_scene_id_;
}

QString SceneProfilesPanel::selectedOutputName() const {
    const int index = selectedIndex();
    return index >= 0 ? scenes_[index].output_name : QString{};
}

void SceneProfilesPanel::setProfiles(const QList<SceneProfile>& scenes,
                                     const QString& selected_scene_id) {
    scenes_ = scenes;
    selected_scene_id_ = selected_scene_id;
    refreshSceneList();
    refreshSourceDetails();
    refreshActionState();
}

void SceneProfilesPanel::setSessionContext(const QString& status,
                                           const QString& active_output_name,
                                           bool session_active) {
    session_status_ = status;
    active_output_name_ = active_output_name;
    session_active_ = session_active;
    refreshSourceDetails();
    refreshActionState();
}

int SceneProfilesPanel::selectedIndex() const {
    for (qsizetype index = 0; index < scenes_.size(); ++index) {
        if (scenes_[index].id == selected_scene_id_) return static_cast<int>(index);
    }
    return -1;
}

QString SceneProfilesPanel::nextSceneName() const {
    const QStringList names = otherSceneNames();
    for (int number = 1; number < 10000; ++number) {
        const QString candidate = tr("Scene %1").arg(number);
        if (!names.contains(candidate, Qt::CaseInsensitive)) return candidate;
    }
    return tr("New scene");
}

QStringList SceneProfilesPanel::otherSceneNames(const QString& exclude_id) const {
    QStringList names;
    for (const SceneProfile& scene : scenes_) {
        if (scene.id != exclude_id) names.push_back(scene.name);
    }
    return names;
}

void SceneProfilesPanel::refreshSceneList() {
    const QSignalBlocker blocker(scenes_list_);
    scenes_list_->clear();
    for (const SceneProfile& scene : scenes_) {
        auto* item = new QListWidgetItem(scene.name, scenes_list_);
        item->setData(Qt::UserRole, scene.id);
        item->setData(Qt::AccessibleTextRole,
                      tr("Scene %1, Wayland output %2").arg(scene.name, scene.output_name));
        item->setToolTip(tr("One Wayland output: %1").arg(scene.output_name));
        if (scene.id == selected_scene_id_) scenes_list_->setCurrentItem(item);
    }
    if (scenes_list_->currentRow() < 0 && !scenes_.isEmpty()) {
        scenes_list_->setCurrentRow(0);
        selected_scene_id_ = scenes_.first().id;
    }
}

void SceneProfilesPanel::refreshSourceDetails() {
    const int index = selectedIndex();
    if (index < 0) {
        selected_output_->setText(tr("No scene selected"));
        output_availability_->setText(tr("No selected output to check."));
        source_status_->setText(session_status_);
        return;
    }
    const SceneProfile& scene = scenes_[index];
    selected_output_->setText(tr("%1 · %2").arg(scene.name, scene.output_name));
    selected_output_->setToolTip(tr("The selected scene records this Wayland output on the next start."));
    output_availability_->setText(WaylandOutputPicker::outputAvailabilityText(scene.output_name));
    output_availability_->setAccessibleDescription(output_availability_->text());
    if (session_active_ && !active_output_name_.isEmpty()) {
        if (active_output_name_ == scene.output_name) {
            source_status_->setText(tr("%1 · active source").arg(session_status_));
        } else {
            source_status_->setText(tr("Active source · %1. Selected output applies after this session stops.")
                                        .arg(active_output_name_));
        }
        source_status_->setAccessibleDescription(source_status_->text());
    } else {
        source_status_->setText(session_status_);
        source_status_->setAccessibleDescription(tr(
            "%1. The displayed output is selected for the next capture session.").arg(session_status_));
    }
}

void SceneProfilesPanel::refreshActionState() {
    const bool has_selection = selectedIndex() >= 0;
    const bool mutable_now = !session_active_;
    scenes_list_->setEnabled(mutable_now);
    add_button_->setEnabled(mutable_now);
    rename_button_->setEnabled(mutable_now && has_selection);
    remove_button_->setEnabled(mutable_now && has_selection && scenes_.size() > 1);
    edit_output_button_->setEnabled(mutable_now && has_selection);
    const QString lock_tip = tr("Stop recording or replay-only capture before changing scenes or sources.");
    const std::array<QWidget*, 5> lock_widgets{
        scenes_list_, add_button_, rename_button_, remove_button_, edit_output_button_};
    for (QWidget* widget : lock_widgets) {
        widget->setToolTip(mutable_now ? QString{} : lock_tip);
    }
    if (scenes_.size() <= 1) {
        remove_button_->setToolTip(mutable_now
            ? tr("At least one scene must remain.") : lock_tip);
    }
}

void SceneProfilesPanel::add_scene() {
    if (session_active_) return;
    SceneEditDialog dialog(tr("Add scene"), nextSceneName(), selectedOutputName(),
                           SceneEditDialog::Mode::add, otherSceneNames(), this);
    if (dialog.exec() != QDialog::Accepted) return;

    const SceneProfile profile{
        QUuid::createUuid().toString(QUuid::WithoutBraces),
        dialog.sceneName(),
        dialog.outputName(),
    };
    scenes_.push_back(profile);
    selected_scene_id_ = profile.id;
    refreshSceneList();
    refreshSourceDetails();
    refreshActionState();
    emit configurationChanged();
}

void SceneProfilesPanel::rename_scene() {
    if (session_active_) return;
    const int index = selectedIndex();
    if (index < 0) return;
    SceneProfile& profile = scenes_[index];
    SceneEditDialog dialog(tr("Rename scene"), profile.name, profile.output_name,
                           SceneEditDialog::Mode::rename,
                           otherSceneNames(profile.id), this);
    if (dialog.exec() != QDialog::Accepted) return;
    profile.name = dialog.sceneName();
    refreshSceneList();
    refreshSourceDetails();
    refreshActionState();
    emit configurationChanged();
}

void SceneProfilesPanel::remove_scene() {
    if (session_active_ || scenes_.size() <= 1) return;
    const int index = selectedIndex();
    if (index < 0) return;
    const SceneProfile removed = scenes_[index];
    QMessageBox confirm(QMessageBox::Question, tr("Remove scene"),
                        tr("Remove “%1” from the saved scene list?").arg(removed.name),
                        QMessageBox::NoButton, this);
    auto* keep = confirm.addButton(tr("Keep scene"), QMessageBox::RejectRole);
    auto* remove = confirm.addButton(tr("Remove scene"), QMessageBox::DestructiveRole);
    confirm.setDefaultButton(keep);
    confirm.setEscapeButton(keep);
    confirm.exec();
    if (confirm.clickedButton() != remove) return;

    scenes_.removeAt(index);
    const int next_index = std::min(index, static_cast<int>(scenes_.size()) - 1);
    selected_scene_id_ = scenes_[next_index].id;
    refreshSceneList();
    refreshSourceDetails();
    refreshActionState();
    emit configurationChanged();
}

void SceneProfilesPanel::edit_output() {
    if (session_active_) return;
    const int index = selectedIndex();
    if (index < 0) return;
    SceneProfile& profile = scenes_[index];
    SceneEditDialog dialog(tr("Edit scene output"), profile.name, profile.output_name,
                           SceneEditDialog::Mode::output, {}, this);
    if (dialog.exec() != QDialog::Accepted) return;
    profile.output_name = dialog.outputName();
    refreshSceneList();
    refreshSourceDetails();
    refreshActionState();
    emit configurationChanged();
}

} // namespace fluxrec::ui
