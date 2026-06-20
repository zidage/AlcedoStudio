//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_dialog/widgets/lut_browser_widget.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>
#include <functional>
#include <utility>

#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/history_cards.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

constexpr int  kListIconSize          = 14;
constexpr int  kCheckBadgeSize        = 14;
constexpr int  kRowHeight             = 34;
constexpr char kFavoriteSettingsKey[] = "editor/lut_browser/favorite_paths";

auto           SettingsPathForEntry(const lut_catalog::LutCatalogEntry& entry) -> QString {
  return QString::fromStdString(entry.path_).trimmed();
}

auto CanFavoriteEntry(const lut_catalog::LutCatalogEntry& entry) -> bool {
  return !SettingsPathForEntry(entry).isEmpty() &&
         (entry.kind_ == lut_catalog::LutCatalogEntryKind::File ||
          entry.kind_ == lut_catalog::LutCatalogEntryKind::MissingCurrent);
}

auto BuildLutEntriesListStyle() -> QString {
  const auto& theme = AppTheme::Instance();
  return QStringLiteral(
             "QListWidget {"
             "  background: transparent;"
             "  border: none;"
             "  padding: 0px;"
             "}"
             "QListWidget::item {"
             "  border: none;"
             "  padding: 0px;"
             "}"
             "QListWidget::item:selected {"
             "  background: transparent;"
             "}"
             "QScrollBar:vertical {"
             "  background: transparent;"
             "  width: 6px;"
             "  margin: 4px 1px 4px 0px;"
             "}"
             "QScrollBar::handle:vertical {"
             "  background: %1;"
             "  border-radius: 3px;"
             "  min-height: 24px;"
             "}"
             "QScrollBar::handle:vertical:hover,"
             "QScrollBar::handle:vertical:pressed {"
             "  background: %2;"
             "}"
             "QScrollBar::add-line:vertical,"
             "QScrollBar::sub-line:vertical {"
             "  height: 0px;"
             "}"
             "QScrollBar::add-page:vertical,"
             "QScrollBar::sub-page:vertical {"
             "  background: transparent;"
             "}")
      .arg(QColor(theme.textMutedColor().red(), theme.textMutedColor().green(),
                  theme.textMutedColor().blue(), 132)
               .name(QColor::HexArgb),
           theme.accentColor().name(QColor::HexRgb));
}

auto BuildSearchEditStyle() -> QString {
  const auto& theme = AppTheme::Instance();
  return QStringLiteral(
             "QLineEdit {"
             "  background: rgba(255, 255, 255, 0.04);"
             "  color: %1;"
             "  border-radius: 6px;"
             "  padding: 0px 8px 0px 24px;"
             "  selection-background-color: %2;"
             "  selection-color: %3;"
             "}"
             "QLineEdit:hover {"
             "  background: rgba(255, 255, 255, 0.08);"
             "}"
             "QLineEdit:focus {"
             "  background: rgba(0, 0, 0, 0.2);"
             "  border: 1px solid %4;"
             "}")
      .arg(theme.textColor().name(QColor::HexRgb), theme.accentColor().name(QColor::HexRgb),
           theme.bgCanvasColor().name(QColor::HexRgb),
           QColor(theme.accentColor().red(), theme.accentColor().green(),
                  theme.accentColor().blue(), 200)
               .name(QColor::HexArgb));
}

auto BuildIconToolButtonStyle() -> QString {
  const auto& theme = AppTheme::Instance();
  return QStringLiteral(
             "QToolButton {"
             "  background: rgba(255, 255, 255, 0.04);"
             "  color: %1;"
             "  border-radius: 6px;"
             "  padding: 0px;"
             "}"
             "QToolButton:hover {"
             "  background: rgba(255, 255, 255, 0.08);"
             "}"
             "QToolButton:pressed {"
             "  background: rgba(255, 255, 255, 0.12);"
             "}"
             "QToolButton::menu-indicator { image: none; width: 0px; }")
      .arg(theme.textColor().name(QColor::HexRgb));
}

auto BuildFilterButtonStyle(bool active) -> QString {
  const auto& theme = AppTheme::Instance();
  if (active) {
    return QStringLiteral(
               "QToolButton {"
               "  color: %1;"
               "  background: %2;"
               "  border: 1px solid %3;"
               "  border-radius: 6px;"
               "  padding: 0px;"
               "}"
               "QToolButton:hover {"
               "  background: %4;"
               "}")
        .arg(theme.bgCanvasColor().name(QColor::HexRgb),
             QColor(theme.accentColor().red(), theme.accentColor().green(),
                    theme.accentColor().blue(), 224)
                 .name(QColor::HexArgb),
             QColor(theme.accentColor().red(), theme.accentColor().green(),
                    theme.accentColor().blue(), 170)
                 .name(QColor::HexArgb),
             theme.accentSecondaryColor().name(QColor::HexRgb));
  }

  return QStringLiteral(
             "QToolButton {"
             "  color: %1;"
             "  background: transparent;"
             "  border: 1px solid transparent;"
             "  border-radius: 6px;"
             "  padding: 0px;"
             "}"
             "QToolButton:hover {"
             "  color: %2;"
             "  background: %3;"
             "}")
      .arg(theme.textMutedColor().name(QColor::HexRgb), theme.textColor().name(QColor::HexRgb),
           QColor(theme.hoverColor().red(), theme.hoverColor().green(), theme.hoverColor().blue(),
                  210)
               .name(QColor::HexArgb));
}

auto EntryMatchesSearchToken(const lut_catalog::LutCatalogEntry& entry, const QString& token)
    -> bool {
  if (token.isEmpty()) {
    return true;
  }

  const QString haystack =
      QStringLiteral("%1 %2 %3")
          .arg(entry.display_name_, entry.secondary_text_, QString::fromStdString(entry.path_))
          .toCaseFolded();
  return haystack.contains(token);
}

auto CompareByNameAsc(const lut_catalog::LutCatalogEntry& a, const lut_catalog::LutCatalogEntry& b)
    -> bool {
  const int name_cmp = QString::compare(a.display_name_, b.display_name_, Qt::CaseInsensitive);
  if (name_cmp != 0) {
    return name_cmp < 0;
  }

  const QString a_path      = QString::fromStdString(a.path_);
  const QString b_path      = QString::fromStdString(b.path_);
  const int     path_cmp_ci = QString::compare(a_path, b_path, Qt::CaseInsensitive);
  if (path_cmp_ci != 0) {
    return path_cmp_ci < 0;
  }
  return QString::compare(a_path, b_path, Qt::CaseSensitive) < 0;
}

auto FormatByteSize(std::uintmax_t bytes) -> QString {
  constexpr double kKiB = 1024.0;
  constexpr double kMiB = 1024.0 * 1024.0;

  if (bytes >= static_cast<std::uintmax_t>(kMiB)) {
    return QStringLiteral("%1MB").arg(static_cast<double>(bytes) / kMiB, 0, 'f', 2);
  }
  if (bytes >= static_cast<std::uintmax_t>(kKiB)) {
    return QStringLiteral("%1KB").arg(static_cast<double>(bytes) / kKiB, 0, 'f', 1);
  }
  if (bytes == 0) {
    return {};
  }
  return QStringLiteral("%1B").arg(static_cast<qulonglong>(bytes));
}

auto BuildTypeBadgeText(const lut_catalog::LutCatalogEntry& entry) -> QString {
  if (entry.edge3d_ > 0) {
    return QStringLiteral("3D %1").arg(entry.edge3d_);
  }
  if (entry.size1d_ > 0) {
    return QStringLiteral("1D %1").arg(entry.size1d_);
  }
  return {};
}

class FavoriteStarButton final : public QToolButton {
 public:
  explicit FavoriteStarButton(QWidget* parent = nullptr) : QToolButton(parent) {
    setCheckable(true);
    setFixedSize(22, 22);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setStyleSheet(QStringLiteral("QToolButton { background: transparent; border: none; }"));
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const auto& theme = AppTheme::Instance();
    if (underMouse() && isEnabled()) {
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(theme.hoverColor().red(), theme.hoverColor().green(),
                        theme.hoverColor().blue(), 210));
      p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 5, 5);
    }

    QPainterPath star;
    star.moveTo(11.0, 3.3);
    star.lineTo(13.55, 8.45);
    star.lineTo(19.25, 9.25);
    star.lineTo(15.1, 13.25);
    star.lineTo(16.1, 18.95);
    star.lineTo(11.0, 16.25);
    star.lineTo(5.9, 18.95);
    star.lineTo(6.9, 13.25);
    star.lineTo(2.75, 9.25);
    star.lineTo(8.45, 8.45);
    star.closeSubpath();

    // Warm gold reads as "favorited" without fighting the accent-colored selection
    // border, and stays legible against the dark panel background.
    const QColor active_color = QColor(0xF5, 0xC5, 0x18);
    QColor       idle_color   = theme.textMutedColor();
    idle_color.setAlpha(isEnabled() ? 185 : 80);

    QPen pen(isChecked() ? active_color : idle_color);
    pen.setWidthF(1.6);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(isChecked() ? QBrush(active_color) : Qt::NoBrush);
    p.drawPath(star);
  }
};

class CheckBadge final : public QWidget {
 public:
  explicit CheckBadge(QWidget* parent = nullptr) : QWidget(parent) {
    setFixedSize(kCheckBadgeSize, kCheckBadgeSize);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto& theme = AppTheme::Instance();
    p.setBrush(theme.accentColor());
    p.setPen(Qt::NoPen);
    const QRectF r(0.5, 0.5, kCheckBadgeSize - 1.0, kCheckBadgeSize - 1.0);
    p.drawEllipse(r);

    QPen pen(Qt::white);
    pen.setWidthF(2.0);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);

    const double w = kCheckBadgeSize;
    QPainterPath path;
    path.moveTo(w * 0.28, w * 0.52);
    path.lineTo(w * 0.44, w * 0.68);
    path.lineTo(w * 0.74, w * 0.36);
    p.drawPath(path);
  }
};

class LutEntryItemWidget final : public QFrame {
 public:
  explicit LutEntryItemWidget(QWidget* parent = nullptr) : QFrame(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 2, 8, 2);
    layout->setSpacing(5);

    favorite_btn_ = new FavoriteStarButton(this);
    QObject::connect(favorite_btn_, &QToolButton::clicked, this, [this]() {
      if (favorite_path_.isEmpty() || !favorite_toggled_) {
        return;
      }
      favorite_toggled_(favorite_path_);
    });

    icon_label_ = new QLabel(this);
    icon_label_->setFixedSize(kListIconSize, kListIconSize);
    icon_label_->setAlignment(Qt::AlignCenter);
    icon_label_->setPixmap(
        QIcon(QStringLiteral(":/panel_icons/box.svg")).pixmap(kListIconSize, kListIconSize));

    title_label_ = new ElidedLabel({}, this);
    title_label_->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textColor()));
    AppTheme::MarkFontRole(title_label_, AppTheme::FontRole::UiCaption);

    type_label_ = MakePillLabel({}, this);
    type_label_->hide();

    size_label_ = new QLabel(this);
    size_label_->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textMutedColor()));
    AppTheme::MarkFontRole(size_label_, AppTheme::FontRole::DataCaption);

    status_label_ = MakePillLabel({}, this);
    status_label_->hide();

    check_badge_ = new CheckBadge(this);
    check_badge_->hide();

    layout->addWidget(favorite_btn_, 0, Qt::AlignVCenter);
    layout->addWidget(icon_label_, 0, Qt::AlignVCenter);
    layout->addWidget(title_label_, 1, Qt::AlignVCenter);
    layout->addWidget(status_label_, 0, Qt::AlignVCenter);
    layout->addWidget(type_label_, 0, Qt::AlignVCenter);
    layout->addWidget(size_label_, 0, Qt::AlignVCenter);
    layout->addWidget(check_badge_, 0, Qt::AlignVCenter);
  }

  void Bind(const lut_catalog::LutCatalogEntry& entry, bool is_favorite,
            std::function<void(const QString&)> favorite_toggled) {
    entry_                  = entry;
    favorite_toggled_       = std::move(favorite_toggled);
    favorite_path_          = SettingsPathForEntry(entry);

    const bool can_favorite = CanFavoriteEntry(entry);
    favorite_btn_->setVisible(can_favorite);
    favorite_btn_->setEnabled(can_favorite);
    favorite_btn_->setToolTip(is_favorite ? Tr("Remove from favorites") : Tr("Add to favorites"));
    {
      const QSignalBlocker blocker(favorite_btn_);
      favorite_btn_->setChecked(is_favorite);
    }

    title_label_->SetRawText(entry.display_name_);

    const QString type_text = BuildTypeBadgeText(entry);
    type_label_->setText(type_text);
    type_label_->setVisible(!type_text.isEmpty());

    const QString size_text = FormatByteSize(entry.file_size_bytes_);
    size_label_->setText(size_text);
    size_label_->setVisible(!size_text.isEmpty());

    status_label_->setVisible(!entry.status_text_.isEmpty());
    status_label_->setText(entry.status_text_);

    if (entry.kind_ == lut_catalog::LutCatalogEntryKind::MissingCurrent) {
      status_label_->setStyleSheet(
          QStringLiteral("QLabel {"
                         "  color: #E5A040;"
                         "  background: rgba(229, 160, 64, 0.12);"
                         "  border: 1px solid rgba(229, 160, 64, 0.3);"
                         "  border-radius: 4px;"
                         "  padding: 2px 8px;"
                         "}"));
    } else if (!entry.valid_) {
      status_label_->setStyleSheet(
          QStringLiteral("QLabel {"
                         "  color: #E05C5C;"
                         "  background: rgba(224, 92, 92, 0.12);"
                         "  border: 1px solid rgba(224, 92, 92, 0.3);"
                         "  border-radius: 4px;"
                         "  padding: 2px 8px;"
                         "}"));
    }

    icon_label_->setVisible(false);
    icon_label_->setStyleSheet(QStringLiteral("QLabel { background: transparent; }"));

    SetSelected(false);
  }

  void SetSelected(bool selected) {
    const auto& theme = AppTheme::Instance();

    selected_ = selected && entry_.selectable_;

    // No fill on selection — only the painted outline (see paintEvent) marks the
    // active row, so text keeps its normal color instead of inverting against a
    // tinted background.
    QString title_color = theme.textColor().name(QColor::HexRgb);
    QString size_color  = theme.textMutedColor().name(QColor::HexRgb);

    if (!entry_.valid_) {
      title_color = theme.textMutedColor().name(QColor::HexRgb);
      size_color  = theme.dividerColor().name(QColor::HexRgb);
    }

    title_label_->setStyleSheet(AppTheme::EditorLabelStyle(QColor(title_color)));
    size_label_->setStyleSheet(AppTheme::EditorLabelStyle(QColor(size_color)));

    check_badge_->setVisible(false);
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Subtle hover wash; lighter when the row is already outlined so the
    // selection border stays the dominant signal.
    if (underMouse() && isEnabled()) {
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(255, 255, 255, selected_ ? 14 : 22));
      p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    }

    if (selected_) {
      const auto& theme = AppTheme::Instance();
      QColor      c     = theme.accentColor();
      c.setAlpha(215);
      QPen pen(c);
      pen.setWidthF(1.4);
      p.setPen(pen);
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(QRectF(rect()).adjusted(0.7, 0.7, -0.7, -0.7), 6, 6);
    }
  }

 private:
  lut_catalog::LutCatalogEntry        entry_{};
  bool                                selected_ = false;
  QString                             favorite_path_{};
  std::function<void(const QString&)> favorite_toggled_{};
  FavoriteStarButton*                 favorite_btn_ = nullptr;
  QLabel*                             icon_label_   = nullptr;
  ElidedLabel*                        title_label_  = nullptr;
  QLabel*                             type_label_   = nullptr;
  QLabel*                             size_label_   = nullptr;
  QLabel*                             status_label_ = nullptr;
  CheckBadge*                         check_badge_  = nullptr;
};

auto MakeIconToolButton(QWidget* parent, const QString& icon_path, const QString& tooltip)
    -> QToolButton* {
  auto* btn = new QToolButton(parent);
  btn->setFixedSize(26, 26);
  btn->setIcon(QIcon(icon_path));
  btn->setIconSize(QSize(14, 14));
  btn->setCursor(Qt::PointingHandCursor);
  btn->setToolTip(tooltip);
  btn->setStyleSheet(BuildIconToolButtonStyle());
  return btn;
}

}  // namespace

LutBrowserWidget::LutBrowserWidget(QWidget* parent) : QWidget(parent) {
  LoadFavoriteSettings();

  const auto& theme = AppTheme::Instance();
  setObjectName(QStringLiteral("LutBrowserCard"));
  setMinimumHeight(380);
  setAttribute(Qt::WA_StyledBackground, true);
  setStyleSheet(QStringLiteral("QWidget#LutBrowserCard {"
                               "  background: %1;"
                               "  border: 1px solid %2;"
                               "  border-radius: %3px;"
                               "}")
                    .arg(QColor(theme.bgPanelColor().red(), theme.bgPanelColor().green(),
                                theme.bgPanelColor().blue(), 220)
                             .name(QColor::HexArgb),
                         QColor(theme.glassStrokeColor().red(), theme.glassStrokeColor().green(),
                                theme.glassStrokeColor().blue(), 140)
                             .name(QColor::HexArgb),
                         QString::number(theme.panelRadius())));

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(8, 8, 8, 8);
  root->setSpacing(7);

  auto* search_row    = new QWidget(this);
  auto* search_layout = new QHBoxLayout(search_row);
  search_layout->setContentsMargins(0, 0, 0, 0);
  search_layout->setSpacing(6);

  auto* search_edit_host = new QWidget(search_row);
  search_edit_host->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  search_edit_host->setFixedHeight(26);
  auto* search_edit_layout = new QHBoxLayout(search_edit_host);
  search_edit_layout->setContentsMargins(0, 0, 0, 0);
  search_edit_layout->setSpacing(0);

  search_edit_ = new QLineEdit(search_edit_host);
  search_edit_->setClearButtonEnabled(true);
  search_edit_->setFixedHeight(26);
  search_edit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  search_edit_->setStyleSheet(BuildSearchEditStyle());
  AppTheme::MarkFontRole(search_edit_, AppTheme::FontRole::UiCaption);
  search_edit_layout->addWidget(search_edit_);

  auto* search_glyph = new QLabel(search_edit_host);
  search_glyph->setPixmap(QIcon(QStringLiteral(":/panel_icons/search.svg")).pixmap(12, 12));
  search_glyph->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  search_glyph->setStyleSheet(QStringLiteral("QLabel { background: transparent; border: none; }"));
  search_glyph->setParent(search_edit_);
  search_glyph->move(8, (26 - 12) / 2);
  search_glyph->show();

  search_layout->addWidget(search_edit_host, 1);

  sort_btn_ =
      MakeIconToolButton(search_row, QStringLiteral(":/panel_icons/sort.svg"), Tr("Sort options"));
  folder_btn_ = MakeIconToolButton(search_row, QStringLiteral(":/panel_icons/folder-open.svg"),
                                   Tr("Open LUT folder"));
  sort_btn_->setPopupMode(QToolButton::InstantPopup);

  auto* sort_menu         = new QMenu(sort_btn_);
  auto* sort_field_group  = new QActionGroup(sort_menu);
  sort_field_name_action_ = sort_menu->addAction(QString{});
  sort_field_time_action_ = sort_menu->addAction(QString{});
  sort_field_name_action_->setCheckable(true);
  sort_field_time_action_->setCheckable(true);
  sort_field_name_action_->setData(static_cast<int>(SortField::Name));
  sort_field_time_action_->setData(static_cast<int>(SortField::ModifiedTime));
  sort_field_group->addAction(sort_field_name_action_);
  sort_field_group->addAction(sort_field_time_action_);
  sort_field_name_action_->setChecked(true);

  sort_menu->addSeparator();
  auto* sort_order_group  = new QActionGroup(sort_menu);
  sort_order_asc_action_  = sort_menu->addAction(QString{});
  sort_order_desc_action_ = sort_menu->addAction(QString{});
  sort_order_asc_action_->setCheckable(true);
  sort_order_desc_action_->setCheckable(true);
  sort_order_asc_action_->setData(static_cast<int>(SortOrder::Ascending));
  sort_order_desc_action_->setData(static_cast<int>(SortOrder::Descending));
  sort_order_group->addAction(sort_order_asc_action_);
  sort_order_group->addAction(sort_order_desc_action_);
  sort_order_asc_action_->setChecked(true);

  sort_menu->addSeparator();
  refresh_action_ = sort_menu->addAction(QString{});
  sort_btn_->setMenu(sort_menu);

  QObject::connect(folder_btn_, &QToolButton::clicked, this,
                   [this]() { emit OpenFolderRequested(); });

  search_layout->addWidget(sort_btn_, 0);
  search_layout->addWidget(folder_btn_, 0);
  root->addWidget(search_row, 0);

  auto* divider = new QFrame(this);
  divider->setFrameShape(QFrame::HLine);
  divider->setFrameShadow(QFrame::Plain);
  divider->setFixedHeight(1);
  divider->setStyleSheet(QStringLiteral("QFrame { background: %1; border: none; }")
                             .arg(QColor(AppTheme::Instance().dividerColor().red(),
                                         AppTheme::Instance().dividerColor().green(),
                                         AppTheme::Instance().dividerColor().blue(), 120)
                                      .name(QColor::HexArgb)));
  root->addWidget(divider, 0);

  auto* browser_body = new QWidget(this);
  auto* body_layout  = new QHBoxLayout(browser_body);
  body_layout->setContentsMargins(0, 0, 0, 0);
  body_layout->setSpacing(8);

  auto* filter_rail = new QWidget(browser_body);
  filter_rail->setObjectName(QStringLiteral("LutFilterRail"));
  filter_rail->setFixedWidth(44);
  filter_rail->setStyleSheet(
      QStringLiteral("QWidget#LutFilterRail {"
                     "  background: %1;"
                     "  border: none;"
                     "  border-radius: 8px;"
                     "}")
          .arg(QColor(theme.bgCanvasColor().red(), theme.bgCanvasColor().green(),
                      theme.bgCanvasColor().blue(), 160)
                   .name(QColor::HexArgb)));
  auto* filter_layout = new QVBoxLayout(filter_rail);
  filter_layout->setContentsMargins(5, 5, 5, 5);
  filter_layout->setSpacing(5);

  favorites_filter_btn_ = new QToolButton(filter_rail);
  favorites_filter_btn_->setText(QString::fromUtf8("\xE2\x98\x85"));
  favorites_filter_btn_->setCheckable(true);
  favorites_filter_btn_->setFixedSize(32, 32);
  favorites_filter_btn_->setCursor(Qt::PointingHandCursor);
  favorites_filter_btn_->setFocusPolicy(Qt::NoFocus);
  AppTheme::MarkFontRole(favorites_filter_btn_, AppTheme::FontRole::DataBodyStrong);

  all_filter_btn_ = new QToolButton(filter_rail);
  all_filter_btn_->setText(QStringLiteral("ALL"));
  all_filter_btn_->setCheckable(true);
  all_filter_btn_->setFixedSize(32, 32);
  all_filter_btn_->setCursor(Qt::PointingHandCursor);
  all_filter_btn_->setFocusPolicy(Qt::NoFocus);
  AppTheme::MarkFontRole(all_filter_btn_, AppTheme::FontRole::DataCaption);

  QObject::connect(favorites_filter_btn_, &QToolButton::clicked, this,
                   [this]() { SetEntryFilter(EntryFilter::Favorites); });
  QObject::connect(all_filter_btn_, &QToolButton::clicked, this,
                   [this]() { SetEntryFilter(EntryFilter::All); });

  filter_layout->addWidget(favorites_filter_btn_, 0, Qt::AlignHCenter);
  filter_layout->addWidget(all_filter_btn_, 0, Qt::AlignHCenter);
  filter_layout->addStretch(1);
  body_layout->addWidget(filter_rail, 0);

  auto* entries_panel  = new QWidget(browser_body);
  auto* entries_layout = new QVBoxLayout(entries_panel);
  entries_layout->setContentsMargins(0, 0, 0, 0);
  entries_layout->setSpacing(5);

  entries_list_ = new QListWidget(entries_panel);
  entries_list_->setSpacing(2);
  entries_list_->setSelectionMode(QAbstractItemView::SingleSelection);
  entries_list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  entries_list_->setFocusPolicy(Qt::NoFocus);
  entries_list_->setStyleSheet(BuildLutEntriesListStyle());
  entries_list_->setFrameShape(QFrame::NoFrame);
  entries_layout->addWidget(entries_list_, 1);

  auto* summary_row    = new QWidget(entries_panel);
  auto* summary_layout = new QHBoxLayout(summary_row);
  summary_layout->setContentsMargins(2, 0, 2, 0);
  summary_layout->setSpacing(6);

  search_summary_label_ = new QLabel(summary_row);
  search_summary_label_->setStyleSheet(
      AppTheme::EditorLabelStyle(AppTheme::Instance().textMutedColor()));
  AppTheme::MarkFontRole(search_summary_label_, AppTheme::FontRole::DataCaption);

  summary_layout->addWidget(search_summary_label_, 0);
  summary_layout->addStretch(1);
  entries_layout->addWidget(summary_row, 0);

  status_label_ = new QLabel(entries_panel);
  status_label_->setWordWrap(true);
  status_label_->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textMutedColor()));
  AppTheme::MarkFontRole(status_label_, AppTheme::FontRole::DataCaption);
  status_label_->hide();
  entries_layout->addWidget(status_label_, 0);

  body_layout->addWidget(entries_panel, 1);
  root->addWidget(browser_body, 1);

  QObject::connect(search_edit_, &QLineEdit::textChanged, this, [this](const QString&) {
    const QString selected_path = entries_list_ && entries_list_->currentItem()
                                      ? entries_list_->currentItem()->data(Qt::UserRole).toString()
                                      : QString{};
    RebuildVisibleEntries(selected_path);
  });

  const auto rebuild_on_sort_change = [this]() {
    const QString selected_path = entries_list_ && entries_list_->currentItem()
                                      ? entries_list_->currentItem()->data(Qt::UserRole).toString()
                                      : QString{};
    RebuildVisibleEntries(selected_path);
  };
  QObject::connect(sort_field_name_action_, &QAction::triggered, this, rebuild_on_sort_change);
  QObject::connect(sort_field_time_action_, &QAction::triggered, this, rebuild_on_sort_change);
  QObject::connect(sort_order_asc_action_, &QAction::triggered, this, rebuild_on_sort_change);
  QObject::connect(sort_order_desc_action_, &QAction::triggered, this, rebuild_on_sort_change);

  QObject::connect(refresh_action_, &QAction::triggered, this,
                   [this]() { emit RefreshRequested(); });

  const auto emit_activation_for_item = [this](QListWidgetItem* item) {
    if (updating_entries_ || !item) {
      return;
    }
    const int row = entries_list_->row(item);
    if (row < 0 || row >= static_cast<int>(visible_entries_.size())) {
      return;
    }
    if (!visible_entries_[static_cast<size_t>(row)].selectable_) {
      return;
    }
    emit LutPathActivated(QString::fromStdString(visible_entries_[static_cast<size_t>(row)].path_));
  };

  QObject::connect(entries_list_, &QListWidget::currentItemChanged, this,
                   [this, emit_activation_for_item](QListWidgetItem* current, QListWidgetItem*) {
                     RefreshSelectionStyles();
                     emit_activation_for_item(current);
                   });
  QObject::connect(
      entries_list_, &QListWidget::itemClicked, this,
      [emit_activation_for_item](QListWidgetItem* item) { emit_activation_for_item(item); });
  QObject::connect(
      entries_list_, &QListWidget::itemActivated, this,
      [emit_activation_for_item](QListWidgetItem* item) { emit_activation_for_item(item); });

  RetranslateUi();
}

void LutBrowserWidget::RetranslateUi() {
  if (title_label_) {
    title_label_->setText(Tr("Looks & LUTs"));
  }
  if (search_edit_) {
    search_edit_->setPlaceholderText(Tr("Search LUTs..."));
  }
  if (sort_field_name_action_) {
    sort_field_name_action_->setText(Tr("Sort by Name"));
  }
  if (sort_field_time_action_) {
    sort_field_time_action_->setText(Tr("Sort by Modified Time"));
  }
  if (sort_order_asc_action_) {
    sort_order_asc_action_->setText(Tr("Ascending"));
  }
  if (sort_order_desc_action_) {
    sort_order_desc_action_->setText(Tr("Descending"));
  }
  if (refresh_action_) {
    refresh_action_->setText(Tr("Refresh"));
  }
  if (folder_btn_) {
    folder_btn_->setToolTip(Tr("Open LUT folder"));
  }
  if (sort_btn_) {
    sort_btn_->setToolTip(Tr("Sort options"));
  }
  if (favorites_filter_btn_) {
    favorites_filter_btn_->setToolTip(Tr("Favorites"));
  }
  if (all_filter_btn_) {
    all_filter_btn_->setToolTip(Tr("All LUTs"));
  }
  RefreshFilterButtonStyles();
  UpdateSearchResultSummary();
}

void LutBrowserWidget::SetDirectoryInfo(const QString& directory_text, const QString& status_text,
                                        bool can_open_directory) {
  if (directory_label_) {
    directory_label_->SetRawText(directory_text);
    directory_label_->setToolTip(directory_text);
  }
  // The normal "%N LUTs available." status duplicates the summary line at the bottom of the
  // browser. Suppress it so only genuine issues (folder missing, invalid LUTs, empty folder)
  // ever surface here.
  if (status_label_) {
    const bool is_count_only_status = status_text.contains(QStringLiteral("LUTs available")) &&
                                      !status_text.contains(QStringLiteral("invalid"));
    if (is_count_only_status) {
      status_label_->clear();
      status_label_->hide();
    } else {
      status_label_->setText(status_text);
      status_label_->setVisible(!status_text.isEmpty());
    }
  }
  if (folder_btn_) {
    folder_btn_->setToolTip(directory_text.isEmpty() ? Tr("Open LUT folder") : directory_text);
    folder_btn_->setEnabled(can_open_directory);
  }
}

void LutBrowserWidget::SetEntries(const std::vector<lut_catalog::LutCatalogEntry>& entries,
                                  const QString& selected_path, bool preserve_scroll_position) {
  source_entries_ = entries;
  RebuildVisibleEntries(selected_path, preserve_scroll_position);
}

auto LutBrowserWidget::SelectRelativeEntry(int step) -> bool {
  if (!entries_list_ || visible_entries_.empty() || step == 0) {
    return false;
  }

  const int direction = step > 0 ? 1 : -1;
  int       index     = entries_list_->currentRow();
  if (index < 0) {
    index = direction > 0 ? -1 : static_cast<int>(visible_entries_.size());
  }

  while (true) {
    index += direction;
    if (index < 0 || index >= static_cast<int>(visible_entries_.size())) {
      break;
    }
    if (!visible_entries_[static_cast<size_t>(index)].selectable_) {
      continue;
    }

    if (entries_list_->currentRow() == index) {
      emit LutPathActivated(
          QString::fromStdString(visible_entries_[static_cast<size_t>(index)].path_));
      return true;
    }

    entries_list_->setCurrentRow(index);
    if (auto* item = entries_list_->item(index)) {
      entries_list_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    }
    return true;
  }

  const int current = entries_list_->currentRow();
  if (current >= 0 && current < static_cast<int>(visible_entries_.size()) &&
      visible_entries_[static_cast<size_t>(current)].selectable_) {
    emit LutPathActivated(
        QString::fromStdString(visible_entries_[static_cast<size_t>(current)].path_));
    return true;
  }
  return false;
}

void LutBrowserWidget::RebuildVisibleEntries(const QString& preferred_selected_path,
                                             bool           preserve_scroll_position) {
  const QString token = search_edit_ ? search_edit_->text().trimmed().toCaseFolded() : QString{};
  QScrollBar*   scroll_bar = entries_list_ ? entries_list_->verticalScrollBar() : nullptr;
  const int     previous_scroll_value =
      preserve_scroll_position && scroll_bar ? scroll_bar->value() : 0;

  std::vector<lut_catalog::LutCatalogEntry> file_entries;
  std::vector<lut_catalog::LutCatalogEntry> special_entries;
  file_entries.reserve(source_entries_.size());
  special_entries.reserve(source_entries_.size());

  for (const auto& entry : source_entries_) {
    if (!EntryMatchesSearchToken(entry, token)) {
      continue;
    }
    if (active_filter_ == EntryFilter::Favorites && !IsFavoriteEntry(entry)) {
      continue;
    }
    if (entry.kind_ == lut_catalog::LutCatalogEntryKind::File) {
      file_entries.push_back(entry);
      continue;
    }
    special_entries.push_back(entry);
  }

  const SortField sort_field = CurrentSortField();
  const SortOrder sort_order = CurrentSortOrder();
  std::sort(file_entries.begin(), file_entries.end(),
            [sort_field, sort_order](const lut_catalog::LutCatalogEntry& a,
                                     const lut_catalog::LutCatalogEntry& b) {
              if (sort_field == SortField::Name) {
                const bool by_name_asc = CompareByNameAsc(a, b);
                if (CompareByNameAsc(a, b) == CompareByNameAsc(b, a)) {
                  return false;
                }
                return sort_order == SortOrder::Ascending ? by_name_asc : !by_name_asc;
              }

              if (a.has_modified_time_ != b.has_modified_time_) {
                return a.has_modified_time_;
              }
              if (a.has_modified_time_ && b.has_modified_time_ &&
                  a.modified_time_sort_key_ != b.modified_time_sort_key_) {
                if (sort_order == SortOrder::Ascending) {
                  return a.modified_time_sort_key_ < b.modified_time_sort_key_;
                }
                return a.modified_time_sort_key_ > b.modified_time_sort_key_;
              }

              const bool by_name_asc = CompareByNameAsc(a, b);
              if (CompareByNameAsc(a, b) == CompareByNameAsc(b, a)) {
                return false;
              }
              return sort_order == SortOrder::Ascending ? by_name_asc : !by_name_asc;
            });

  visible_entries_.clear();
  visible_entries_.reserve(special_entries.size() + file_entries.size());
  visible_entries_.insert(visible_entries_.end(), special_entries.begin(), special_entries.end());
  visible_entries_.insert(visible_entries_.end(), file_entries.begin(), file_entries.end());

  updating_entries_ = true;
  entries_list_->clear();

  int selected_row = -1;
  for (int i = 0; i < static_cast<int>(visible_entries_.size()); ++i) {
    const auto& entry = visible_entries_[static_cast<size_t>(i)];
    auto*       item  = new QListWidgetItem(entries_list_);
    item->setSizeHint(QSize(0, kRowHeight));
    item->setData(Qt::UserRole, QString::fromStdString(entry.path_));

    auto* row_widget = new LutEntryItemWidget(entries_list_);
    row_widget->Bind(entry, IsFavoriteEntry(entry),
                     [this](const QString& path) { ToggleFavoritePath(path); });
    entries_list_->setItemWidget(item, row_widget);

    if (!preferred_selected_path.isEmpty() &&
        QString::fromStdString(entry.path_) == preferred_selected_path) {
      selected_row = i;
    }
  }

  if (selected_row < 0) {
    for (int i = 0; i < static_cast<int>(visible_entries_.size()); ++i) {
      if (visible_entries_[static_cast<size_t>(i)].selectable_) {
        selected_row = i;
        break;
      }
    }
  }
  if (selected_row >= 0) {
    entries_list_->setCurrentRow(selected_row);
  }
  if (preserve_scroll_position && scroll_bar) {
    scroll_bar->setValue(
        std::clamp(previous_scroll_value, scroll_bar->minimum(), scroll_bar->maximum()));
  }

  RefreshSelectionStyles();
  updating_entries_ = false;
  UpdateSearchResultSummary();
}

void LutBrowserWidget::UpdateSearchResultSummary() {
  if (!search_summary_label_) {
    return;
  }

  const int total_file_count =
      static_cast<int>(std::count_if(source_entries_.begin(), source_entries_.end(),
                                     [](const lut_catalog::LutCatalogEntry& entry) {
                                       return entry.kind_ == lut_catalog::LutCatalogEntryKind::File;
                                     }));
  const int visible_file_count =
      static_cast<int>(std::count_if(visible_entries_.begin(), visible_entries_.end(),
                                     [](const lut_catalog::LutCatalogEntry& entry) {
                                       return entry.kind_ == lut_catalog::LutCatalogEntryKind::File;
                                     }));
  const int  favorite_file_count = static_cast<int>(std::count_if(
      source_entries_.begin(), source_entries_.end(),
      [this](const lut_catalog::LutCatalogEntry& entry) {
        return entry.kind_ == lut_catalog::LutCatalogEntryKind::File && IsFavoriteEntry(entry);
      }));
  const bool has_search          = search_edit_ && !search_edit_->text().trimmed().isEmpty();

  if (active_filter_ == EntryFilter::Favorites) {
    if (!has_search) {
      search_summary_label_->setText(Tr("%1 Favorites").arg(favorite_file_count));
      return;
    }
    search_summary_label_->setText(
        Tr("%1/%2 Favorites").arg(visible_file_count).arg(favorite_file_count));
    return;
  }

  if (!has_search) {
    search_summary_label_->setText(Tr("%1 Available").arg(total_file_count));
    return;
  }
  search_summary_label_->setText(
      Tr("%1/%2 Available").arg(visible_file_count).arg(total_file_count));
}

auto LutBrowserWidget::CurrentSortField() const -> SortField {
  if (sort_field_time_action_ && sort_field_time_action_->isChecked()) {
    return SortField::ModifiedTime;
  }
  return SortField::Name;
}

auto LutBrowserWidget::CurrentSortOrder() const -> SortOrder {
  if (sort_order_desc_action_ && sort_order_desc_action_->isChecked()) {
    return SortOrder::Descending;
  }
  return SortOrder::Ascending;
}

void LutBrowserWidget::SetEntryFilter(EntryFilter filter) {
  if (active_filter_ == filter) {
    RefreshFilterButtonStyles();
    return;
  }

  const QString selected_path = entries_list_ && entries_list_->currentItem()
                                    ? entries_list_->currentItem()->data(Qt::UserRole).toString()
                                    : QString{};
  active_filter_              = filter;
  RefreshFilterButtonStyles();
  RebuildVisibleEntries(selected_path, true);
}

void LutBrowserWidget::RefreshFilterButtonStyles() {
  if (favorites_filter_btn_) {
    const bool           active = active_filter_ == EntryFilter::Favorites;
    const QSignalBlocker blocker(favorites_filter_btn_);
    favorites_filter_btn_->setChecked(active);
    favorites_filter_btn_->setStyleSheet(BuildFilterButtonStyle(active));
  }
  if (all_filter_btn_) {
    const bool           active = active_filter_ == EntryFilter::All;
    const QSignalBlocker blocker(all_filter_btn_);
    all_filter_btn_->setChecked(active);
    all_filter_btn_->setStyleSheet(BuildFilterButtonStyle(active));
  }
}

void LutBrowserWidget::LoadFavoriteSettings() {
  favorite_lut_paths_.clear();

  const QStringList values =
      QSettings().value(QString::fromLatin1(kFavoriteSettingsKey)).toStringList();
  for (const QString& value : values) {
    const QString path = value.trimmed();
    if (!path.isEmpty()) {
      favorite_lut_paths_.insert(path);
    }
  }
}

void LutBrowserWidget::SaveFavoriteSettings() const {
  QStringList values;
  values.reserve(favorite_lut_paths_.size());
  for (const QString& path : favorite_lut_paths_) {
    if (!path.trimmed().isEmpty()) {
      values.push_back(path);
    }
  }
  values.sort(Qt::CaseInsensitive);
  QSettings().setValue(QString::fromLatin1(kFavoriteSettingsKey), values);
}

auto LutBrowserWidget::IsFavoritePath(const QString& path) const -> bool {
  return !path.trimmed().isEmpty() && favorite_lut_paths_.contains(path.trimmed());
}

auto LutBrowserWidget::IsFavoriteEntry(const lut_catalog::LutCatalogEntry& entry) const -> bool {
  return CanFavoriteEntry(entry) && IsFavoritePath(SettingsPathForEntry(entry));
}

void LutBrowserWidget::ToggleFavoritePath(const QString& path) {
  const QString normalized_path = path.trimmed();
  if (normalized_path.isEmpty()) {
    return;
  }

  if (favorite_lut_paths_.contains(normalized_path)) {
    favorite_lut_paths_.remove(normalized_path);
  } else {
    favorite_lut_paths_.insert(normalized_path);
  }
  SaveFavoriteSettings();

  const QString selected_path = entries_list_ && entries_list_->currentItem()
                                    ? entries_list_->currentItem()->data(Qt::UserRole).toString()
                                    : QString{};
  RebuildVisibleEntries(selected_path, true);
  RefreshFilterButtonStyles();
}

void LutBrowserWidget::RefreshSelectionStyles() {
  const int current_row = entries_list_ ? entries_list_->currentRow() : -1;
  for (int i = 0; i < entries_list_->count(); ++i) {
    auto* item = entries_list_->item(i);
    if (!item) {
      continue;
    }
    auto* widget = dynamic_cast<LutEntryItemWidget*>(entries_list_->itemWidget(item));
    if (!widget) {
      continue;
    }
    widget->SetSelected(i == current_row);
  }
}

}  // namespace alcedo::ui
