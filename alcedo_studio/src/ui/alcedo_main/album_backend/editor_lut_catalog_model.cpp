//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_lut_catalog_model.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>

#include <algorithm>
#include <system_error>

namespace alcedo::ui {
namespace {

auto PathToQString(const std::filesystem::path& path) -> QString {
  if (path.empty()) {
    return {};
  }
  return QString::fromStdWString(path.wstring());
}

auto QStringToUtf8(const QString& value) -> std::string {
  const auto bytes = value.toUtf8();
  return {bytes.constData(), static_cast<size_t>(bytes.size())};
}

auto KindString(lut_catalog::LutCatalogEntryKind kind) -> QString {
  switch (kind) {
    case lut_catalog::LutCatalogEntryKind::None:
      return QStringLiteral("none");
    case lut_catalog::LutCatalogEntryKind::MissingCurrent:
      return QStringLiteral("missing");
    case lut_catalog::LutCatalogEntryKind::File:
    default:
      return QStringLiteral("file");
  }
}

}  // namespace

EditorLutCatalogModel::EditorLutCatalogModel(QObject* parent) : EditorAdjustmentModelBase(parent) {
  setFieldKey(QStringLiteral("lut"));
  setLabel(QStringLiteral("LUT"));
  loadFavoriteSettings();
  refresh(false);
}

EditorLutCatalogModel::~EditorLutCatalogModel() = default;

void EditorLutCatalogModel::loadFavoriteSettings() {
  QSettings settings;
  favoritePaths_ = settings.value(QStringLiteral("editor/lutPanel/favoritePaths")).toStringList();
}

void EditorLutCatalogModel::saveFavoriteSettings() const {
  QSettings settings;
  settings.setValue(QStringLiteral("editor/lutPanel/favoritePaths"), favoritePaths_);
}

void EditorLutCatalogModel::setFavoritePaths(const QStringList& paths) {
  if (favoritePaths_ == paths)
    return;
  favoritePaths_ = paths;
  saveFavoriteSettings();
  emit favoritePathsChanged();
}

void EditorLutCatalogModel::toggleFavoritePath(const QString& path) {
  const QString trimmed = path.trimmed();
  if (trimmed.isEmpty())
    return;
  const int idx = favoritePaths_.indexOf(trimmed);
  if (idx >= 0) {
    favoritePaths_.removeAt(idx);
  } else {
    favoritePaths_.append(trimmed);
  }
  saveFavoriteSettings();
  emit favoritePathsChanged();
}

bool EditorLutCatalogModel::isFavoritePath(const QString& path) const {
  return !path.trimmed().isEmpty() && favoritePaths_.contains(path.trimmed());
}

void EditorLutCatalogModel::setSelectedPath(const QString& path) {
  // Load-only path writes often re-apply the same snapshot value after a
  // settled select. Skip work and signals so QML does not rebuild or twitch.
  if (path == selectedPath_) {
    return;
  }
  selectedPath_     = path;
  selectedPathUtf8_ = QStringToUtf8(path);
  applySelectionHighlight();
  emit selectedPathChanged();
}

void EditorLutCatalogModel::setFilterText(const QString& text) {
  if (filterText_ == text) {
    return;
  }
  filterText_ = text;
  emit filterTextChanged();
  rebuildEntriesView();
}

void EditorLutCatalogModel::refresh(bool force) {
  catalog_ = lut_catalog::BuildCatalog(selectedPathUtf8_, force);
  directoryText_ =
      lut_catalog::FormatDirectoryDisplayText(catalog_.directory_);
  statusText_        = lut_catalog::CatalogStatusText(catalog_);
  std::error_code ec;
  canOpenDirectory_  = !catalog_.directory_.empty() &&
                      std::filesystem::is_directory(catalog_.directory_, ec) && !ec;
  rebuildEntriesView();
  emit catalogChanged();
}

void EditorLutCatalogModel::selectPath(const QString& path) {
  if (path == selectedPath_) {
    return;
  }
  selectedPath_     = path;
  selectedPathUtf8_ = QStringToUtf8(path);
  applySelectionHighlight();
  emit selectedPathChanged();
  submitSettled();
  emit settledCommitted();
}

auto EditorLutCatalogModel::selectRelative(int step) -> bool {
  if (step == 0 || entries_.isEmpty()) {
    return false;
  }
  std::vector<int> selectable;
  selectable.reserve(static_cast<size_t>(entries_.size()));
  for (int i = 0; i < entries_.size(); ++i) {
    const auto map = entries_[i].toMap();
    if (map.value(QStringLiteral("selectable")).toBool()) {
      selectable.push_back(i);
    }
  }
  if (selectable.empty()) {
    return false;
  }
  int current_pos = 0;
  for (int i = 0; i < static_cast<int>(selectable.size()); ++i) {
    if (selectable[static_cast<size_t>(i)] == selectedIndex_) {
      current_pos = i;
      break;
    }
  }
  const int next_pos =
      (current_pos + step) % static_cast<int>(selectable.size());
  const int wrapped =
      next_pos < 0 ? next_pos + static_cast<int>(selectable.size()) : next_pos;
  const auto map  = entries_[selectable[static_cast<size_t>(wrapped)]].toMap();
  const auto path = map.value(QStringLiteral("path")).toString();
  if (path == selectedPath_) {
    return false;
  }
  selectPath(path);
  return true;
}

void EditorLutCatalogModel::clearSelection() { selectPath(QString()); }

auto EditorLutCatalogModel::paramsJson() const -> QString { return buildParamsJson(); }

auto EditorLutCatalogModel::defaultLutPath() const -> QString {
  return QString::fromStdString(lut_catalog::DefaultLutPath(catalog_));
}

auto EditorLutCatalogModel::directoryPath() const -> QString {
  return PathToQString(catalog_.directory_);
}

void EditorLutCatalogModel::rebuildEntriesView() {
  entries_.clear();
  const QString filter = filterText_.trimmed();
  for (const auto& entry : catalog_.entries_) {
    if (!filter.isEmpty() && entry.kind_ == lut_catalog::LutCatalogEntryKind::File) {
      if (!entry.display_name_.contains(filter, Qt::CaseInsensitive) &&
          !entry.secondary_text_.contains(filter, Qt::CaseInsensitive)) {
        continue;
      }
    }
    QVariantMap map;
    map.insert(QStringLiteral("kind"), KindString(entry.kind_));
    map.insert(QStringLiteral("path"), QString::fromStdString(entry.path_));
    map.insert(QStringLiteral("displayName"), entry.display_name_);
    map.insert(QStringLiteral("secondaryText"), entry.secondary_text_);
    map.insert(QStringLiteral("statusText"), entry.status_text_);
    map.insert(QStringLiteral("selectable"), entry.selectable_);
    map.insert(QStringLiteral("valid"), entry.valid_);
    map.insert(QStringLiteral("fileSize"), static_cast<qlonglong>(entry.file_size_bytes_));
    map.insert(QStringLiteral("lutEdge"), entry.edge3d_);
    map.insert(QStringLiteral("lutSize1d"), entry.size1d_);
    map.insert(QStringLiteral("modifiedTimeSortKey"),
               static_cast<qlonglong>(entry.modified_time_sort_key_));
    {
      QString typeText;
      if (entry.edge3d_ > 0) {
        typeText = QStringLiteral("3D %1").arg(entry.edge3d_);
      } else if (entry.size1d_ > 0) {
        typeText = QStringLiteral("1D %1").arg(entry.size1d_);
      }
      map.insert(QStringLiteral("lutTypeBadge"), typeText);
    }
    map.insert(QStringLiteral("selected"), entry.path_ == selectedPathUtf8_ ||
                                               (entry.kind_ == lut_catalog::LutCatalogEntryKind::None &&
                                                selectedPathUtf8_.empty()));
    entries_.push_back(map);
  }
  applySelectionHighlight();
  emit entriesChanged();
}

void EditorLutCatalogModel::applySelectionHighlight() {
  // Update selected flags and selectedIndex only. Do not emit entriesChanged:
  // a full list reset on every click makes QML ListViews rebuild, jump
  // contentY, and pin the selected row to the bottom of the viewport.
  // Selection is communicated via selectedPathChanged; UIs should derive
  // highlight from selectedPath / selectedIndex.
  selectedIndex_ = -1;
  for (int i = 0; i < entries_.size(); ++i) {
    auto map = entries_[i].toMap();
    const QString path = map.value(QStringLiteral("path")).toString();
    const QString kind = map.value(QStringLiteral("kind")).toString();
    const bool selected =
        (path == selectedPath_) ||
        (selectedPath_.isEmpty() && kind == QStringLiteral("none"));
    map.insert(QStringLiteral("selected"), selected);
    entries_[i] = map;
    if (selected) {
      selectedIndex_ = i;
    }
  }
}

void EditorLutCatalogModel::submitSettled() { submitNow(buildParamsJson(), true); }

auto EditorLutCatalogModel::buildParamsJson() const -> QString {
  QJsonObject root;
  root.insert(QStringLiteral("ocio_lmt"), selectedPath_);
  return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

}  // namespace alcedo::ui
