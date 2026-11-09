//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/library_view_preferences.hpp"

#include <QLatin1String>
#include <QSettings>

namespace alcedo::ui {

LibraryViewPreferences::LibraryViewPreferences(QObject* parent) : QObject(parent) {
  const QString stored =
      QSettings{}
          .value(QLatin1String(kGridLayoutSettingKey), QLatin1String(kClassicLayout))
          .toString();
  grid_layout_ = IsGridLayout(stored) ? stored : QString::fromLatin1(kClassicLayout);
}

auto LibraryViewPreferences::IsGridLayout(const QString& layout) -> bool {
  return layout == QLatin1String(kClassicLayout) || layout == QLatin1String(kJustifiedLayout);
}

bool LibraryViewPreferences::setGridLayout(const QString& layout) {
  if (!IsGridLayout(layout)) {
    return false;
  }
  if (layout == grid_layout_) {
    return true;
  }
  grid_layout_ = layout;
  QSettings{}.setValue(QLatin1String(kGridLayoutSettingKey), grid_layout_);
  emit gridLayoutChanged();
  return true;
}

}  // namespace alcedo::ui
