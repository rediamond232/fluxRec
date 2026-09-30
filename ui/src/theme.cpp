#include "theme.hpp"

#include "theme_tokens.hpp"

#include <QApplication>
#include <QColor>
#include <QPalette>

#include <array>
#include <utility>

namespace fluxrec::ui {

void applyDarkTheme(QApplication& application) {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(theme::canvas));
    palette.setColor(QPalette::WindowText, QColor(theme::text));
    palette.setColor(QPalette::Base, QColor(theme::surface));
    palette.setColor(QPalette::AlternateBase, QColor(theme::raised));
    palette.setColor(QPalette::ToolTipBase, QColor(theme::raised));
    palette.setColor(QPalette::ToolTipText, QColor(theme::text));
    palette.setColor(QPalette::Text, QColor(theme::text));
    palette.setColor(QPalette::Button, QColor(theme::raised));
    palette.setColor(QPalette::ButtonText, QColor(theme::text));
    palette.setColor(QPalette::BrightText, QColor(theme::danger));
    palette.setColor(QPalette::Link, QColor(theme::primary));
    palette.setColor(QPalette::Highlight, QColor(theme::primary));
    palette.setColor(QPalette::HighlightedText, QColor(theme::primary_ink));
    palette.setColor(QPalette::PlaceholderText, QColor(theme::muted));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(theme::muted));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(theme::muted));
    application.setPalette(palette);

    QString stylesheet = QStringLiteral(R"QSS(
        * {
            color: @text@;
            font-family: "Noto Sans", sans-serif;
            font-size: 10.5pt;
        }
        QWidget { background-color: @canvas@; }
        QMainWindow, QDialog { background-color: @canvas@; }
        QMenuBar, QToolBar, QStatusBar { background-color: @surface@; }
        QMenuBar { border-bottom: 1px solid @line@; }
        QMenuBar::item { padding: 6px 9px; background: transparent; }
        QMenuBar::item:selected, QMenu::item:selected { background: @raised@; color: @text@; }
        QMenu { background-color: @surface@; border: 1px solid @line@; }
        QMenu::item { padding: 6px 24px 6px 10px; }
        QToolBar { spacing: @cluster-gap@px; border: 0; border-bottom: 1px solid @line@; padding: 6px @panel-inset@px; }
        QDockWidget::title { background-color: @surface@; padding: @cluster-gap@px @panel-inset@px; text-align: left; border-bottom: 1px solid @line@; }
        QDockWidget { border: 1px solid @line@; }
        QFrame#panelSurface, QGroupBox { background-color: @surface@; border: 1px solid @line@; border-radius: @radius-default@px; }
        QFrame#metricTile { background-color: @surface@; border: 1px solid @line@; border-radius: @radius-sm@px; }
        QFrame#audioSourceRow { background-color: @surface@; border: 1px solid @line@; border-radius: @radius-sm@px; }
        QGroupBox { margin-top: 9px; padding: 12px 10px 10px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: @muted@; }
        QLabel#eyebrow { color: @muted@; font-size: 8pt; font-weight: 600; }
        QLabel#secondaryText, QLabel#settingsNote { color: @muted@; }
        QLabel#sectionTitle { color: @text@; font-size: 10pt; font-weight: 600; }
        QLabel#pageTitle { color: @text@; font-size: 14pt; font-weight: 600; }
        QLabel#audioState[audioState="streaming"] { color: @success@; font-weight: 600; }
        QLabel#audioState[audioState="connecting"] { color: @primary@; font-weight: 600; }
        QLabel#audioState[audioState="error"] { color: @warning@; font-weight: 600; }
        QLabel#audioMeterLabel, QLabel#desktopAudioStatus, QLabel#microphoneAudioStatus { color: @muted@; }
        QLabel[validation="true"] { color: @warning@; }
        QLabel#metricValue, QLabel#sourceName {
            font-family: "Noto Sans Mono", monospace; font-size: 9pt;
        }
        QLabel#engineState[state="recording"] { color: @danger@; font-weight: 700; }
        QLabel#engineState[state="replay_only"] { color: @primary@; font-weight: 700; }
        QLabel#engineState[state="stopping"], QLabel#engineState[state="failed"],
        QLabel#engineState[state="unavailable"] { color: @warning@; font-weight: 700; }
        QLabel#engineState[state="available"] { color: @success@; font-weight: 700; }
        QLabel#engineState[state="starting"] { color: @primary@; font-weight: 700; }
        QLabel#validationMessage, QLabel#errorText { color: @warning@; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            background-color: @canvas@; border: 1px solid @line@; border-radius: @radius-sm@px;
            padding: 6px 8px; min-height: 20px; selection-background-color: @primary@;
        }
        QListWidget {
            background-color: @canvas@; border: 1px solid @line@; border-radius: @radius-sm@px;
            outline: 0;
        }
        QListWidget::item { padding: 5px 8px; border-left: 2px solid transparent; }
        QListWidget::item:selected {
            background-color: @raised@; color: @primary@; border-left-color: @primary@;
        }
        QComboBox::drop-down { border-left: 1px solid @line@; width: 24px; }
        QComboBox QAbstractItemView { background-color: @surface@; selection-background-color: @raised@; }
        QPushButton {
            background-color: @surface@; border: 1px solid @line@; border-radius: @radius-default@px;
            min-height: @control-height@px; padding: 4px 11px; font-weight: 600;
        }
        QPushButton:hover { background-color: @raised@; }
        QPushButton:pressed { background-color: @line@; }
        QPushButton:disabled { color: @muted@; background-color: @canvas@; border-color: @line@; }
        QPushButton#recordButton { color: @primary-ink@; background-color: @primary@; border-color: @primary@; }
        QPushButton#recordButton:hover { background-color: @focus@; border-color: @focus@; }
        QPushButton#stopButton { color: @text@; background-color: @surface@; }
        QPushButton#stopButton:hover { border-color: @danger@; color: @danger@; }
        QPushButton:focus, QLineEdit:focus, QSpinBox:focus, QComboBox:focus,
        QSlider:focus, QCheckBox:focus, QTabBar::tab:focus, QTableWidget:focus,
        QAbstractItemView:focus {
            border: 2px solid @focus@;
        }
        QSlider::groove:horizontal { height: 5px; background: @line@; border-radius: @radius-sm@px; }
        QSlider::handle:horizontal { width: 16px; margin: -6px 0; border-radius: @radius-sm@px; background: @primary@; }
        QSlider::handle:horizontal:hover { background: @focus@; }
        QProgressBar#peakMeter, QProgressBar#rmsMeter { background: @canvas@; border: 1px solid @line@; border-radius: @radius-sm@px; }
        QProgressBar#peakMeter::chunk { background: @primary@; border-radius: @radius-sm@px; }
        QProgressBar#rmsMeter::chunk { background: @focus@; border-radius: @radius-sm@px; }
        QProgressBar[stale="true"]::chunk { background: @muted@; }
        QTabWidget::pane { border: 1px solid @line@; background-color: @canvas@; }
        QTabBar::tab { background-color: @surface@; border: 1px solid @line@; padding: 7px 12px; }
        QTabBar::tab:selected { background-color: @canvas@; color: @text@; border-bottom-color: @canvas@; }
        QTableWidget { background-color: @canvas@; alternate-background-color: @surface@; gridline-color: @line@; border: 1px solid @line@; }
        QHeaderView::section { background-color: @surface@; color: @muted@; padding: 5px 7px; border: 0; border-right: 1px solid @line@; border-bottom: 1px solid @line@; }
        QScrollBar:vertical { background: @canvas@; width: 12px; margin: 0; }
        QScrollBar::handle:vertical { background: @line@; min-height: 24px; border-radius: @radius-sm@px; margin: 2px; }
        QScrollBar::handle:vertical:hover { background: @muted@; }
        QScrollBar:horizontal { background: @canvas@; height: 12px; margin: 0; }
        QScrollBar::handle:horizontal { background: @line@; min-width: 24px; border-radius: @radius-sm@px; margin: 2px; }
        QScrollBar::handle:horizontal:hover { background: @muted@; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        QPlainTextEdit {
            background-color: @canvas@; border: 1px solid @line@;
            font-family: "Noto Sans Mono", monospace; font-size: 9pt;
        }
        QToolTip { background-color: @surface@; color: @text@; border: 1px solid @line@; padding: 5px; }
        QSplitter::handle { background-color: @line@; }
        QStatusBar::item { border: 0; }
    )QSS");

    const std::array<std::pair<const char*, QString>, 19> substitutions{{
        {"@text@", QString::fromLatin1(theme::text)},
        {"@canvas@", QString::fromLatin1(theme::canvas)},
        {"@surface@", QString::fromLatin1(theme::surface)},
        {"@raised@", QString::fromLatin1(theme::raised)},
        {"@line@", QString::fromLatin1(theme::line)},
        {"@muted@", QString::fromLatin1(theme::muted)},
        {"@primary@", QString::fromLatin1(theme::primary)},
        {"@primary-ink@", QString::fromLatin1(theme::primary_ink)},
        {"@success@", QString::fromLatin1(theme::success)},
        {"@warning@", QString::fromLatin1(theme::warning)},
        {"@danger@", QString::fromLatin1(theme::danger)},
        {"@focus@", QString::fromLatin1(theme::focus)},
        {"@panel-inset@", QString::number(theme::panel_inset)},
        {"@cluster-gap@", QString::number(theme::cluster_gap)},
        {"@control-height@", QString::number(theme::control_height)},
        {"@dense-height@", QString::number(theme::dense_control_height)},
        {"@radius-default@", QString::number(theme::radius_default)},
        {"@radius-sm@", QString::number(theme::radius_sm)},
        {"@radius-lg@", QString::number(theme::radius_lg)},
    }};
    for (const auto& [placeholder, value] : substitutions) {
        stylesheet.replace(QString::fromLatin1(placeholder), value);
    }
    application.setStyleSheet(stylesheet);
}

} // namespace fluxrec::ui
