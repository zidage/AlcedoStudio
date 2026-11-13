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

auto LibrarySelection::ImageFromItem(const QVariantMap& item) const -> SelectedImage {
  return Normalized(
      SelectedImage{static_cast<sl_element_id_t>(item.value(QStringLiteral("elementId")).toUInt()),
                    static_cast<image_id_t>(item.value(QStringLiteral("imageId")).toUInt()),
                    item.value(QStringLiteral("fileName")).toString(),
                    item.value(QStringLiteral("isHdr")).toBool()});
}

auto LibrarySelection::IsSelected(uint elementId) const -> bool {
  return selected_.contains(static_cast<sl_element_id_t>(elementId));
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
  range_added_.clear();
  if (selected) {
    selected_.emplace(
        element_id,
        Normalized(SelectedImage{element_id, static_cast<image_id_t>(imageId), fileName, isHdr}));
  } else {
    selected_.erase(element_id);
  }
  NotifyChanged();
}

void LibrarySelection::Clear() { Assign({}); }

void LibrarySelection::Replace(const QVariantList& items) {
  std::map<sl_element_id_t, SelectedImage> next;
  for (const QVariant& value : items) {
    SelectedImage image = ImageFromItem(value.toMap());
    if (image.element_id != 0) {
      const sl_element_id_t element_id = image.element_id;
      next[element_id]                 = std::move(image);
    }
  }
  Assign(std::move(next));
}

void LibrarySelection::AddItems(const QVariantList& items) {
  range_added_.clear();
  bool changed = false;
  for (const QVariant& value : items) {
    SelectedImage image = ImageFromItem(value.toMap());
    if (image.element_id != 0) {
      const sl_element_id_t element_id = image.element_id;
      changed = selected_.emplace(element_id, std::move(image)).second || changed;
    }
  }
  if (changed) {
    NotifyChanged();
  }
}

void LibrarySelection::SetRangeItems(const QVariantList& items) {
  std::map<sl_element_id_t, SelectedImage> range;
  for (const QVariant& value : items) {
    SelectedImage image = ImageFromItem(value.toMap());
    if (image.element_id != 0) {
      const sl_element_id_t element_id = image.element_id;
      range[element_id]                = std::move(image);
    }
  }
  bool changed = false;
  for (auto it = range_added_.begin(); it != range_added_.end();) {
    if (range.contains(*it)) {
      ++it;
      continue;
    }
    selected_.erase(*it);
    it      = range_added_.erase(it);
    changed = true;
  }
  for (auto& [element_id, image] : range) {
    if (selected_.emplace(element_id, std::move(image)).second) {
      range_added_.insert(element_id);
      changed = true;
    }
  }
  if (changed) {
    NotifyChanged();
  }
}

void LibrarySelection::EndRange() { range_added_.clear(); }

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
    range_added_.erase(element_id);
    changed = selected_.erase(element_id) > 0 || changed;
  }
  if (changed) {
    NotifyChanged();
  }
}

void LibrarySelection::Assign(std::map<sl_element_id_t, SelectedImage> next) {
  range_added_.clear();
  const bool same =
      next.size() == selected_.size() &&
      std::equal(next.begin(), next.end(), selected_.begin(), [](const auto& lhs, const auto& rhs) {
        return lhs.first == rhs.first && SameImage(lhs.second, rhs.second);
      });
  if (same) {
    return;
  }
  selected_ = std::move(next);
  NotifyChanged();
}

void LibrarySelection::NotifyChanged() {
  ++revision_;
  emit SelectionChanged();
}

}  // namespace alcedo::ui
