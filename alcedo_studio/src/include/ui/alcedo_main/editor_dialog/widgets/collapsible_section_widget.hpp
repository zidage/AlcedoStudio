//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QFrame>
#include <QMargins>
#include <QString>

class QLabel;
class QToolButton;
class QVBoxLayout;

namespace alcedo::ui {

class CollapsibleSectionWidget final : public QFrame {
 public:
  enum class Chrome {
    Divider,
    Card,
  };

  struct Options {
    const char* title_source        = "";
    const char* subtitle_source     = nullptr;
    bool        uppercase_title     = true;
    bool        show_subtitle       = false;
    bool        subtitle_as_tooltip = true;
    bool        initially_expanded  = true;
    Chrome      chrome              = Chrome::Divider;
    QMargins    root_margins        = QMargins(0, 8, 0, 2);
    QMargins    content_margins     = QMargins(0, 4, 0, 0);
    int         root_spacing        = 4;
    int         content_spacing     = 4;
  };

  explicit CollapsibleSectionWidget(const Options& options, QWidget* parent = nullptr);

  auto ContentLayout() const -> QVBoxLayout* { return content_layout_; }
  auto IsExpanded() const -> bool { return expanded_; }

  void SetExpanded(bool expanded);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void         Build();
  void         ApplyChromeStyle();
  void         UpdateExpandedUi();

  QString      title_source_;
  QString      subtitle_source_;
  bool         uppercase_title_     = true;
  bool         show_subtitle_       = false;
  bool         subtitle_as_tooltip_ = true;
  bool         expanded_            = true;
  Chrome       chrome_              = Chrome::Divider;
  QMargins     root_margins_;
  QMargins     content_margins_;
  int          root_spacing_    = 4;
  int          content_spacing_ = 4;

  QFrame*      header_          = nullptr;
  QToolButton* toggle_button_   = nullptr;
  QLabel*      title_label_     = nullptr;
  QLabel*      subtitle_label_  = nullptr;
  QFrame*      divider_         = nullptr;
  QWidget*     content_         = nullptr;
  QVBoxLayout* content_layout_  = nullptr;
};

}  // namespace alcedo::ui
