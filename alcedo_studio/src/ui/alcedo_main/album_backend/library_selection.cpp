//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/library_selection.hpp"

#include <algorithm>
#include <utility>

namespace alcedo::ui {
namespace {

auto SameImage(const LibrarySelection::SelectedImage& lhs,
               const LibrarySelection::SelectedImage& rhs) -> bool {
  return lhs.element_id == rhs.element_id && lhs.image_id == rhs.image_id &&
         lhs.file_name == rhs.file_name && lhs.is_hdr == rhs.is_hdr;
}

}  // namespace

LibrarySelection::LibrarySelection(const IAlbumCatalog* catalog, QObject* parent)
    : QObject(parent), catalog_(catalog) {}

auto LibrarySelection::Normalized(SelectedImage image) const -> SelectedImage {
  if (image.file_name.isEmpty()) {
    image.file_name = tr("(unnamed)");
  }
  return image;
}

auto LibrarySelection::ItemMap(const SelectedImage& image) const -> QVariantMap {
  return QVariantMap{{QStringLiteral("elementId"), static_cast<uint>(image.element_id)},
                     {QStringLiteral("fileId"), static_cast<uint>(image.element_id)},
                     {QStringLiteral("imageId"), static_cast<uint>(image.image_id)},
                     {QStringLiteral("fileName"), image.file_name},
                     {QStringLiteral("isHdr"), image.is_hdr}};
}

auto LibrarySelection::SelectedImagesById() const -> QVariantMap {
  QVariantMap map;
  for (const auto& [element_id, image] : selected_) {
    map.insert(QString::number(element_id), ItemMap(image));
  }
  return map;
}

auto LibrarySelection::SelectedElementIds() const -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> ids;
  ids.reserve(selected_.size());
  for (const auto& entry : selected_) {
    ids.push_back(entry.first);
  }
  return ids;
}

void LibrarySelection::SetImageSelected(uint elementId, uint imageId, const QString& fileName,
                                        bool isHdr, bool selected) {
  const auto element_id = static_cast<sl_element_id_t>(elementId);
  if (element_id == 0 || selected_.contains(element_id) == selected) {
    return;
  }
  if (selected) {
    selected_.emplace(
        element_id,
        Normalized(SelectedImage{element_id, static_cast<image_id_t>(imageId), fileName, isHdr}));
  } else {
    selected_.erase(element_id);
  }
  emit SelectionChanged();
}

void LibrarySelection::Clear() { Assign({}); }

void LibrarySelection::Replace(const QVariantList& items) {
  std::map<sl_element_id_t, SelectedImage> next;
  for (const QVariant& value : items) {
    const QVariantMap item = value.toMap();
    const auto        element_id =
        static_cast<sl_element_id_t>(item.value(QStringLiteral("elementId")).toUInt());
    if (element_id == 0) {
      continue;
    }
    next[element_id] = Normalized(SelectedImage{
        element_id, static_cast<image_id_t>(item.value(QStringLiteral("imageId")).toUInt()),
        item.value(QStringLiteral("fileName")).toString(),
        item.value(QStringLiteral("isHdr")).toBool()});
  }
  Assign(std::move(next));
}

void LibrarySelection::Replace(std::vector<SelectedImage> images) {
  std::map<sl_element_id_t, SelectedImage> next;
  for (auto& image : images) {
    if (image.element_id != 0) {
      const sl_element_id_t element_id = image.element_id;
      next[element_id]                 = Normalized(std::move(image));
    }
  }
  Assign(std::move(next));
}

auto LibrarySelection::SelectedItems() const -> QVariantList {
  QVariantList items;
  for (const auto& [element_id, image] : selected_) {
    const AlbumItem* row = catalog_ != nullptr ? catalog_->FindAlbumItem(element_id) : nullptr;
    if (row != nullptr) {
      items.push_back(ItemMap(
          Normalized(SelectedImage{element_id, row->image_id, row->file_name, row->is_hdr})));
    } else {
      items.push_back(ItemMap(image));
    }
  }
  return items;
}

void LibrarySelection::PruneElements(const QVariantList& elementIds) {
  std::vector<sl_element_id_t> ids;
  ids.reserve(static_cast<size_t>(elementIds.size()));
  for (const QVariant& value : elementIds) {
    ids.push_back(static_cast<sl_element_id_t>(value.toUInt()));
  }
  Prune(ids);
}

void LibrarySelection::Prune(const std::vector<sl_element_id_t>& element_ids) {
  bool changed = false;
  for (const auto element_id : element_ids) {
    changed = selected_.erase(element_id) > 0 || changed;
  }
  if (changed) {
    emit SelectionChanged();
  }
}

void LibrarySelection::Assign(std::map<sl_element_id_t, SelectedImage> next) {
  const bool same =
      next.size() == selected_.size() &&
      std::equal(next.begin(), next.end(), selected_.begin(), [](const auto& lhs, const auto& rhs) {
        return lhs.first == rhs.first && SameImage(lhs.second, rhs.second);
      });
  if (same) {
    return;
  }
  selected_ = std::move(next);
  emit SelectionChanged();
}

}  // namespace alcedo::ui
