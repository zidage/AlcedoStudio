//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>

namespace alcedo::ui {

/**
 * @brief User-level Library view settings.
 *
 * Owns the layout of the ungrouped Library grid, shown on the Settings "Language / Theme" page:
 * "classic" (default) puts each photo in a 3:2 card of a fixed column count; "justified" puts
 * the photos in rows of one height that fill the view width. Grouped photos always use the
 * classic cells.
 *
 * Stored in QSettings under `library/gridLayout`; unknown stored values read as "classic".
 *
 * Threading: GUI thread only. Side effects: QSettings write and gridLayoutChanged on an
 * accepted change.
 */
class LibraryViewPreferences final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString gridLayout READ grid_layout WRITE setGridLayout NOTIFY gridLayoutChanged)

 public:
  static constexpr auto kGridLayoutSettingKey = "library/gridLayout";
  static constexpr auto kClassicLayout        = "classic";
  static constexpr auto kJustifiedLayout      = "justified";

  explicit LibraryViewPreferences(QObject* parent = nullptr);

  [[nodiscard]] auto        grid_layout() const -> QString { return grid_layout_; }
  /**
   * @brief Persist @p layout as the layout of the ungrouped Library grid.
   * @return false for values other than classic/justified; nothing changes.
   */
  Q_INVOKABLE bool          setGridLayout(const QString& layout);

  [[nodiscard]] static auto IsGridLayout(const QString& layout) -> bool;

 signals:
  void gridLayoutChanged();

 private:
  QString grid_layout_;
};

}  // namespace alcedo::ui
