//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_lut_catalog_model.hpp"

#include <QVariantMap>
#include <algorithm>
#include <system_error>
#include <utility>
#include <variant>

#include "edit/operators/models/lmt_model.hpp"
#include "edit/runtime/lut_resource_resolver.hpp"

namespace alcedo::ui {
namespace {

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
  refresh(false);
}

EditorLutCatalogModel::~EditorLutCatalogModel() = default;

void EditorLutCatalogModel::setLibrary(alcedo::LutLibraryService* library) {
  if (library_ == library) {
    return;
  }
  if (library_) {
    disconnect(library_, nullptr, this, nullptr);
  }
  library_ = library;
  if (library_) {
    connect(library_, &alcedo::LutLibraryService::InventoryChanged, this,
            [this] { refresh(false); });
    connect(library_, &alcedo::LutLibraryService::RootChanged, this, [this] {
      refresh(false);
      emit favoritePathsChanged();
    });
    connect(library_, &alcedo::LutLibraryService::FavoritesChanged, this,
            &EditorLutCatalogModel::favoritePathsChanged);
  }
  emit libraryChanged();
  refresh(false);
  emit favoritePathsChanged();
}

auto EditorLutCatalogModel::relativePathOf(const QString& path) const
    -> std::optional<std::string> {
  const QString trimmed = path.trimmed();
  if (!library_ || trimmed.isEmpty()) {
    return std::nullopt;
  }
  return library_->RelativePathInRoot(alcedo::LutPathFromUtf8(QStringToUtf8(trimmed)));
}

auto EditorLutCatalogModel::favoritePaths() const -> QStringList {
  QStringList paths;
  if (!library_) {
    return paths;
  }
  library_->ForEachEntry([&](const alcedo::LutLibraryEntry& entry) {
    if (library_->IsFavorite(alcedo::LutLibraryPublication::EntryIdOf(entry))) {
      paths.push_back(QString::fromStdString(
          alcedo::LutPathToUtf8(library_->Root() / alcedo::LutPathFromUtf8(entry.relative_path))));
    }
  });
  return paths;
}

auto EditorLutCatalogModel::entryIdOf(const QString& path) const -> std::optional<std::string> {
  const std::optional<std::string> relative = relativePathOf(path);
  std::optional<std::string>       entry_id;
  if (relative) {
    library_->ReadEntry(*relative, [&](const alcedo::LutLibraryEntry& entry) {
      entry_id = alcedo::LutLibraryPublication::EntryIdOf(entry);
    });
  }
  return entry_id;
}

void EditorLutCatalogModel::toggleFavoritePath(const QString& path) {
  const std::optional<std::string> entry_id = entryIdOf(path);
  if (!entry_id) {
    return;
  }
  if (library_->SetFavorite(*entry_id, !library_->IsFavorite(*entry_id)) !=
      alcedo::LutLibraryService::Status::kOk) {
    statusText_ = library_->last_error();
    emit catalogChanged();
  }
}

bool EditorLutCatalogModel::isFavoritePath(const QString& path) const {
  const std::optional<std::string> entry_id = entryIdOf(path);
  return entry_id && library_->IsFavorite(*entry_id);
}

auto EditorLutCatalogModel::referenceForPath(const QString& path) const -> alcedo::LutReference {
  const std::string utf8 = QStringToUtf8(path.trimmed());
  if (utf8.empty()) {
    return std::monostate{};
  }
  if (library_) {
    if (auto reference = library_->ReferenceForPath(alcedo::LutPathFromUtf8(utf8))) {
      return *reference;
    }
  }
  return alcedo::FileLutReference{utf8};
}

void EditorLutCatalogModel::setSelection(const QString& path, alcedo::LutReference reference,
                                         std::string name) {
  selectedReference_ = std::move(reference);
  selectedName_      = std::move(name);
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

void EditorLutCatalogModel::setSelectedPath(const QString& path) {
  setSelection(path, referenceForPath(path), {});
}

void EditorLutCatalogModel::loadSelection(const QVariantMap& lutField) {
  const QString kind = lutField.value(QStringLiteral("referenceKind")).toString();
  const auto    text = [&lutField](const char* key) {
    return QStringToUtf8(lutField.value(QString::fromLatin1(key)).toString());
  };
  alcedo::LutReference reference;
  if (kind == QStringLiteral("official")) {
    reference = alcedo::OfficialLutReference{text("packageId"), text("lutId")};
  } else if (kind == QStringLiteral("library")) {
    reference = alcedo::LibraryLutReference{text("libraryPath")};
  } else if (kind == QStringLiteral("file") || !text("path").empty()) {
    reference = alcedo::FileLutReference{text("path")};
  }
  QString path;
  if (!alcedo::IsEmptyLutReference(reference)) {
    const auto resolution = library_ ? library_->Resources()->Resolve(reference)
                                     : alcedo::DefaultLutResourceResolver()->Resolve(reference);
    path                  = QString::fromStdString(alcedo::LutPathToUtf8(resolution.path));
    if (path.isEmpty()) {
      // No file location is known (an official LUT that no installed package lists).
      path = QString::fromStdString("lut:" + alcedo::DescribeLutReference(reference));
    }
  }
  const bool changed = path != selectedPath_;
  setSelection(path, std::move(reference), text("lutName"));
  if (changed && !path.isEmpty() &&
      lut_catalog::FindEntryIndexForPath(catalog_, selectedPathUtf8_) < 0) {
    refresh(false);
  }
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
  if (force && library_) {
    library_->RefreshInventory();
  }
  catalog_          = lut_catalog::BuildCatalog(library_.data(), selectedPathUtf8_, selectedName_);
  directoryText_    = lut_catalog::FormatDirectoryDisplayText(catalog_.directory_);
  statusText_       = lut_catalog::CatalogStatusText(catalog_);
  canOpenDirectory_ = catalog_.directory_exists_;
  if (!force && lut_catalog::FindEntryIndexForPath(catalog_, selectedPathUtf8_) < 0) {
    // A current LUT that the inventory no longer lists requests one library refresh.
    if (const std::optional<std::string> relative = relativePathOf(selectedPath_)) {
      library_->LocateEntry(*relative);
    }
  }
  rebuildEntriesView();
  emit catalogChanged();
}

auto EditorLutCatalogModel::openDirectory() -> bool {
  if (!library_) {
    return false;
  }
  if (library_->OpenRootDirectory()) {
    return true;
  }
  statusText_ = library_->last_error();
  emit catalogChanged();
  emit openFolderFailed(statusText_);
  return false;
}

void EditorLutCatalogModel::selectPath(const QString& path) {
  if (path == selectedPath_) {
    return;
  }
  std::string name;
  for (const auto& entry : catalog_.entries_) {
    if (entry.kind_ == lut_catalog::LutCatalogEntryKind::File &&
        entry.path_ == QStringToUtf8(path)) {
      name = QStringToUtf8(entry.display_name_);
      break;
    }
  }
  setSelection(path, referenceForPath(path), std::move(name));
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
    map.insert(QStringLiteral("modifiedTimeSortKey"), entry.modified_time_sort_key_);
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

void EditorLutCatalogModel::submitSettled() {
  // A selection keeps the configured strength: the write carries no strength.
  alcedo::EditorLutWrite write;
  write.reference    = selectedReference_;
  write.display_name = selectedName_;
  submitNow(write, true);
}

auto EditorLutCatalogModel::buildParamsJson() const -> QString {
  alcedo::LmtModel model;
  model.SetReference(selectedReference_, selectedName_);
  return QString::fromStdString(model.ToJson().dump());
}

}  // namespace alcedo::ui
