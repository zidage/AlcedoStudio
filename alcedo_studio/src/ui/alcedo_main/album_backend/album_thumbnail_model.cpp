//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/album_thumbnail_model.hpp"

#include <QDate>
#include <algorithm>

namespace alcedo::ui {

namespace {
QString formatAperture(double v) {
  if (v <= 0.0) return QStringLiteral("--");
  return QString::number(v, 'f', 1);
}

QString formatFocalLength(double v) {
  if (v <= 0.0) return QStringLiteral("--");
  return QString::number(v, 'f', 0);
}
}  // namespace

AlbumThumbnailModel::AlbumThumbnailModel(QObject* parent) : QAbstractListModel(parent) {}

int AlbumThumbnailModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;
  return static_cast<int>(rows_.size());
}

QVariant AlbumThumbnailModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
    return {};
  }
  return roleValue(rows_[static_cast<size_t>(index.row())], role);
}

QVariant AlbumThumbnailModel::roleValue(const AlbumItem& item, int role) const {
  switch (role) {
    case ElementId:
      return static_cast<uint>(item.element_id);
    case FileId:
      return static_cast<uint>(item.file_id);
    case ImageId:
      return static_cast<uint>(item.image_id);
    case FolderId:
      return static_cast<uint>(item.folder_id);
    case ScopeType:
      return item.scope_type;
    case FileName:
      return item.file_name.isEmpty() ? QStringLiteral("(unnamed)") : item.file_name;
    case CameraModel:
      return item.camera_model.isEmpty() ? QStringLiteral("Unknown") : item.camera_model;
    case Extension:
      return item.extension.isEmpty() ? QStringLiteral("--") : item.extension;
    case Iso:
      return item.iso;
    case Aperture:
      return formatAperture(item.aperture);
    case FocalLength:
      return formatFocalLength(item.focal_length);
    case CaptureDate:
      return item.capture_date.isValid() ? item.capture_date.toString(QStringLiteral("yyyy-MM-dd"))
                                         : QStringLiteral("--");
    case ImportDate:
      return item.import_date.isValid() ? item.import_date.toString(QStringLiteral("yyyy-MM-dd"))
                                        : QStringLiteral("--");
    case Rating:
      return item.rating;
    case IsHdr:
      return item.is_hdr;
    case Tags:
      return item.tags;
    case Accent:
      return item.accent;
    case ThumbUrl:
      return item.thumb_data_url;
    case ThumbLoading:
      return item.thumb_loading;
    case ThumbMissingSource:
      return item.thumb_missing_source;
    case ThumbErrorText:
      return item.thumb_error_text;
    default:
      return {};
  }
}

QHash<int, QByteArray> AlbumThumbnailModel::roleNames() const {
  return {
      {ElementId, "elementId"},
      {FileId, "fileId"},
      {ImageId, "imageId"},
      {FolderId, "folderId"},
      {ScopeType, "scopeType"},
      {FileName, "fileName"},
      {CameraModel, "cameraModel"},
      {Extension, "extension"},
      {Iso, "iso"},
      {Aperture, "aperture"},
      {FocalLength, "focalLength"},
      {CaptureDate, "captureDate"},
      {ImportDate, "importDate"},
      {Rating, "rating"},
      {IsHdr, "isHdr"},
      {Tags, "tags"},
      {Accent, "accent"},
      {ThumbUrl, "thumbUrl"},
      {ThumbLoading, "thumbLoading"},
      {ThumbMissingSource, "thumbMissingSource"},
      {ThumbErrorText, "thumbErrorText"},
  };
}

void AlbumThumbnailModel::setHasMore(bool v) {
  if (has_more_ == v) return;
  has_more_ = v;
  emit hasMoreChanged();
}

void AlbumThumbnailModel::setLoading(bool v) {
  if (loading_ == v) return;
  loading_ = v;
  emit loadingChanged();
}

void AlbumThumbnailModel::resetModel(const std::vector<AlbumItem>& items, size_t totalCount) {
  const bool total_count_changed = total_count_ != totalCount;
  beginResetModel();
  rows_ = items;
  total_count_ = totalCount;
  rebuildElementIdIndex();
  setHasMore(rows_.size() < total_count_);
  endResetModel();
  emit countChanged();
  if (total_count_changed) {
    emit totalCountChanged();
  }
}

void AlbumThumbnailModel::appendPage(const std::vector<AlbumItem>& newItems) {
  if (newItems.empty()) return;
  const int oldCount = static_cast<int>(rows_.size());
  beginInsertRows(QModelIndex(), oldCount, oldCount + static_cast<int>(newItems.size()) - 1);
  rows_.insert(rows_.end(), newItems.begin(), newItems.end());
  for (const auto& item : newItems) {
    if (item.element_id > 0) {
      element_id_to_row_[item.element_id] = oldCount + static_cast<int>(&item - newItems.data());
    }
  }
  setHasMore(rows_.size() < total_count_);
  endInsertRows();
  emit countChanged();
}

void AlbumThumbnailModel::updateThumbnailState(sl_element_id_t elementId, const QString& dataUrl,
                                               bool loading, bool missingSource,
                                               const QString& errorText) {
  const int row = rowByElementId(elementId);
  if (row < 0 || row >= static_cast<int>(rows_.size())) {
    // A stale element_id_to_row_ entry (queued update arriving after a
    // resetModel/appendPage that restructured rows_) would otherwise index out
    // of bounds and corrupt the heap. updateRating/updateHdrFlag already guard
    // this; mirror them here — this is exactly the line the 0xc0000374 crash
    // surfaced on.
    return;
  }

  auto& item = rows_[static_cast<size_t>(row)];
  if (item.thumb_data_url == dataUrl && item.thumb_loading == loading &&
      item.thumb_missing_source == missingSource && item.thumb_error_text == errorText) {
    return;
  }

  item.thumb_data_url = dataUrl;
  item.thumb_loading = loading;
  item.thumb_missing_source = missingSource;
  item.thumb_error_text = errorText;

  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {ThumbUrl, ThumbLoading, ThumbMissingSource, ThumbErrorText});
  emit thumbnailUpdated(static_cast<uint>(elementId), dataUrl, loading, missingSource, errorText);
}

bool AlbumThumbnailModel::updateRating(sl_element_id_t elementId, image_id_t imageId, int rating) {
  bool updated = false;

  auto update_row = [&](int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
      return;
    }
    auto& item = rows_[static_cast<size_t>(row)];
    if (imageId != 0 && item.image_id != imageId) {
      return;
    }
    if (item.rating == rating) {
      return;
    }

    item.rating = rating;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {Rating});
    updated = true;
  };

  if (elementId != 0) {
    update_row(rowByElementId(static_cast<uint>(elementId)));
    return updated;
  }

  if (imageId == 0) {
    return false;
  }
  for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
    update_row(row);
  }
  return updated;
}

bool AlbumThumbnailModel::updateHdrFlag(sl_element_id_t elementId, image_id_t imageId,
                                        bool isHdr) {
  const int row = rowByElementId(elementId);
  if (row < 0) return false;

  auto& item = rows_[static_cast<size_t>(row)];
  if (imageId != 0 && item.image_id != imageId) return false;
  if (item.is_hdr == isHdr) return false;

  item.is_hdr = isHdr;
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {IsHdr});
  return true;
}

QVariantMap AlbumThumbnailModel::getItemAt(int idx) const {
  if (idx < 0 || idx >= static_cast<int>(rows_.size())) return {};
  const auto& item = rows_[static_cast<size_t>(idx)];
  return {
      {"elementId", static_cast<uint>(item.element_id)},
      {"fileId", static_cast<uint>(item.file_id)},
      {"imageId", static_cast<uint>(item.image_id)},
      {"folderId", static_cast<uint>(item.folder_id)},
      {"scopeType", item.scope_type},
      {"fileName", item.file_name.isEmpty() ? QStringLiteral("(unnamed)") : item.file_name},
      {"rating", item.rating},
      {"isHdr", item.is_hdr},
  };
}

QVariantList AlbumThumbnailModel::getItemsInRange(int firstIndex, int lastIndex) const {
  QVariantList result;
  if (rows_.empty()) return result;

  const int first = std::clamp(std::min(firstIndex, lastIndex), 0, static_cast<int>(rows_.size()) - 1);
  const int last  = std::clamp(std::max(firstIndex, lastIndex), 0, static_cast<int>(rows_.size()) - 1);
  result.reserve(last - first + 1);
  for (int idx = first; idx <= last; ++idx) {
    result.push_back(getItemAt(idx));
  }
  return result;
}

QVariantList AlbumThumbnailModel::getThumbnailStatesInRange(int firstIndex, int lastIndex) const {
  QVariantList result;
  if (rows_.empty()) return result;

  const int first = std::clamp(std::min(firstIndex, lastIndex), 0, static_cast<int>(rows_.size()) - 1);
  const int last  = std::clamp(std::max(firstIndex, lastIndex), 0, static_cast<int>(rows_.size()) - 1);
  result.reserve(last - first + 1);
  for (int idx = first; idx <= last; ++idx) {
    const auto& item = rows_[static_cast<size_t>(idx)];
    result.push_back(QVariantMap{
        {"elementId", static_cast<uint>(item.element_id)},
        {"imageId", static_cast<uint>(item.image_id)},
        {"thumbUrl", item.thumb_data_url},
        {"thumbLoading", item.thumb_loading},
    });
  }
  return result;
}

int AlbumThumbnailModel::rowByElementId(uint elementId) const {
  const auto it = element_id_to_row_.find(elementId);
  return it != element_id_to_row_.end() ? it->second : -1;
}

void AlbumThumbnailModel::rebuildElementIdIndex() {
  element_id_to_row_.clear();
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i].element_id > 0) {
      element_id_to_row_[rows_[i].element_id] = static_cast<int>(i);
    }
  }
}

}  // namespace alcedo::ui
