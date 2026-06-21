//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_dialog/widgets/collapsible_section_widget.hpp"

#include <QAbstractButton>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

constexpr char kLocalizedTextProperty[]      = "puerhlabI18nText";
constexpr char kLocalizedTextUpperProperty[] = "puerhlabI18nTextUpper";
constexpr char kLocalizedToolTipProperty[]   = "puerhlabI18nToolTip";

auto           WithAlphaLocal(const QColor& color, int alpha) -> QColor {
  QColor out = color;
  out.setAlpha(alpha);
  return out;
}

void SetLocalizedText(QObject* object, const char* source, bool uppercase = false) {
  if (!object || source == nullptr) {
    return;
  }
  object->setProperty(kLocalizedTextProperty, source);
  object->setProperty(kLocalizedTextUpperProperty, uppercase);

  QString text = Tr(source);
  if (uppercase) {
    text = text.toUpper();
  }
  if (auto* label = qobject_cast<QLabel*>(object)) {
    label->setText(text);
  } else if (auto* button = qobject_cast<QAbstractButton*>(object)) {
    button->setText(text);
  }
}

void SetLocalizedToolTip(QWidget* widget, const char* source) {
  if (!widget || source == nullptr) {
    return;
  }
  widget->setProperty(kLocalizedToolTipProperty, source);
  const QString tooltip = Tr(source);
  widget->setToolTip(tooltip);
  widget->setAccessibleName(tooltip);
}

auto HasText(const QString& value) -> bool { return !value.trimmed().isEmpty(); }

}  // namespace

CollapsibleSectionWidget::CollapsibleSectionWidget(const Options& options, QWidget* parent)
    : QFrame(parent),
      title_source_(QString::fromUtf8(options.title_source ? options.title_source : "")),
      subtitle_source_(QString::fromUtf8(options.subtitle_source ? options.subtitle_source : "")),
      uppercase_title_(options.uppercase_title),
      show_subtitle_(options.show_subtitle),
      subtitle_as_tooltip_(options.subtitle_as_tooltip),
      expanded_(options.initially_expanded),
      chrome_(options.chrome),
      root_margins_(options.root_margins),
      content_margins_(options.content_margins),
      root_spacing_(options.root_spacing),
      content_spacing_(options.content_spacing) {
  Build();
  ApplyChromeStyle();
  UpdateExpandedUi();
}

void CollapsibleSectionWidget::Build() {
  setFrameShape(QFrame::NoFrame);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(root_margins_);
  root->setSpacing(root_spacing_);

  header_ = new QFrame(this);
  header_->setObjectName(QStringLiteral("EditorCollapsibleSectionHeader"));
  header_->setCursor(Qt::PointingHandCursor);
  header_->setFocusPolicy(Qt::StrongFocus);
  header_->installEventFilter(this);

  auto* header_layout = new QHBoxLayout(header_);
  header_layout->setContentsMargins(0, 0, 0, 0);
  header_layout->setSpacing(6);

  toggle_button_ = new QToolButton(header_);
  toggle_button_->setObjectName(QStringLiteral("EditorCollapsibleSectionToggle"));
  toggle_button_->setAutoRaise(true);
  toggle_button_->setCheckable(true);
  toggle_button_->setCursor(Qt::PointingHandCursor);
  toggle_button_->setFixedSize(18, 18);
  toggle_button_->setFocusPolicy(Qt::NoFocus);
  AppTheme::MarkFontRole(toggle_button_, AppTheme::FontRole::UiCaptionStrong);
  QObject::connect(toggle_button_, &QToolButton::clicked, this,
                   [this]() { SetExpanded(!expanded_); });

  title_label_ = new QLabel(header_);
  title_label_->setObjectName(QStringLiteral("EditorSectionTitle"));
  title_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  title_label_->setStyleSheet(
      AppTheme::EditorLabelStyle(chrome_ == Chrome::Card ? AppTheme::Instance().textColor()
                                                         : AppTheme::Instance().textMutedColor()));
  AppTheme::MarkFontRole(title_label_, chrome_ == Chrome::Card ? AppTheme::FontRole::UiCaptionStrong
                                                               : AppTheme::FontRole::UiOverline);
  SetLocalizedText(title_label_, title_source_.toUtf8().constData(), uppercase_title_);

  header_layout->addWidget(toggle_button_, 0, Qt::AlignVCenter);
  header_layout->addWidget(title_label_, 0, Qt::AlignVCenter);
  header_layout->addStretch(1);
  root->addWidget(header_, 0);

  if (show_subtitle_ && HasText(subtitle_source_)) {
    subtitle_label_ = new QLabel(this);
    subtitle_label_->setObjectName(QStringLiteral("EditorSectionSub"));
    subtitle_label_->setWordWrap(true);
    subtitle_label_->setStyleSheet(
        AppTheme::EditorLabelStyle(AppTheme::Instance().textMutedColor()));
    AppTheme::MarkFontRole(subtitle_label_, AppTheme::FontRole::UiHint);
    SetLocalizedText(subtitle_label_, subtitle_source_.toUtf8().constData(), false);
    root->addWidget(subtitle_label_, 0);
  }

  if (subtitle_as_tooltip_ && HasText(subtitle_source_)) {
    SetLocalizedToolTip(header_, subtitle_source_.toUtf8().constData());
    SetLocalizedToolTip(title_label_, subtitle_source_.toUtf8().constData());
  }

  divider_ = new QFrame(this);
  divider_->setFrameShape(QFrame::HLine);
  divider_->setFixedHeight(1);
  root->addWidget(divider_, 0);

  content_        = new QWidget(this);
  content_layout_ = new QVBoxLayout(content_);
  content_layout_->setContentsMargins(content_margins_);
  content_layout_->setSpacing(content_spacing_);
  root->addWidget(content_, 0);
}

void CollapsibleSectionWidget::ApplyChromeStyle() {
  const auto&  theme = AppTheme::Instance();
  const QColor hover =
      WithAlphaLocal(chrome_ == Chrome::Card ? theme.bgPanelColor() : theme.hoverColor(), 120);

  header_->setStyleSheet(QStringLiteral("QFrame#EditorCollapsibleSectionHeader {"
                                        "  background: transparent;"
                                        "  border: none;"
                                        "  border-radius: 6px;"
                                        "}"
                                        "QFrame#EditorCollapsibleSectionHeader:hover {"
                                        "  background: %1;"
                                        "}")
                             .arg(hover.name(QColor::HexArgb)));
  toggle_button_->setStyleSheet(QStringLiteral("QToolButton#EditorCollapsibleSectionToggle {"
                                               "  color: %1;"
                                               "  background: transparent;"
                                               "  border: none;"
                                               "  padding: 0;"
                                               "}"
                                               "QToolButton#EditorCollapsibleSectionToggle:hover {"
                                               "  color: %2;"
                                               "}")
                                    .arg(theme.textMutedColor().name(QColor::HexRgb),
                                         theme.textColor().name(QColor::HexRgb)));
  divider_->setStyleSheet(
      QStringLiteral("QFrame { background: %1; border: none; }")
          .arg(WithAlphaLocal(theme.dividerColor(), 110).name(QColor::HexArgb)));

  if (chrome_ == Chrome::Card) {
    setObjectName(QStringLiteral("EditorSection"));
    divider_->setVisible(false);
  } else {
    divider_->setVisible(true);
  }
}

void CollapsibleSectionWidget::SetExpanded(bool expanded) {
  if (expanded_ == expanded) {
    return;
  }
  expanded_ = expanded;
  UpdateExpandedUi();
}

void CollapsibleSectionWidget::UpdateExpandedUi() {
  if (content_) {
    content_->setVisible(expanded_);
  }
  if (subtitle_label_) {
    subtitle_label_->setVisible(expanded_);
  }
  if (toggle_button_) {
    toggle_button_->setChecked(expanded_);
    toggle_button_->setText(expanded_ ? QStringLiteral("v") : QStringLiteral(">"));
    SetLocalizedToolTip(toggle_button_, expanded_ ? "Collapse section" : "Expand section");
  }
}

bool CollapsibleSectionWidget::eventFilter(QObject* watched, QEvent* event) {
  if (watched == header_ && event) {
    if (event->type() == QEvent::MouseButtonRelease) {
      const auto* mouse_event = static_cast<QMouseEvent*>(event);
      if (mouse_event->button() == Qt::LeftButton && header_->rect().contains(mouse_event->pos())) {
        SetExpanded(!expanded_);
        return true;
      }
    }
    if (event->type() == QEvent::KeyPress) {
      const auto* key_event = static_cast<QKeyEvent*>(event);
      if (key_event->key() == Qt::Key_Return || key_event->key() == Qt::Key_Enter ||
          key_event->key() == Qt::Key_Space) {
        SetExpanded(!expanded_);
        return true;
      }
    }
  }
  return QFrame::eventFilter(watched, event);
}

}  // namespace alcedo::ui
