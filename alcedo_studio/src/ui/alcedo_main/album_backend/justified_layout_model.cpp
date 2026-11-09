//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/justified_layout_model.hpp"

#include <qqml.h>

#include <QVariantMap>
#include <algorithm>
#include <cmath>

namespace alcedo::ui {
namespace {

const QList<int> kTileRoles{JustifiedLayoutModel::TileX, JustifiedLayoutModel::TileY,
                            JustifiedLayoutModel::TileWidth, JustifiedLayoutModel::TileHeight};

auto             Finite(qreal value) -> qreal { return std::isfinite(value) ? value : 0.0; }

}  // namespace

JustifiedLayoutModel::JustifiedLayoutModel(QObject* parent) : QAbstractListModel(parent) {}

int JustifiedLayoutModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : window_count_;
}

QVariant JustifiedLayoutModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= window_count_) {
    return {};
  }
  const int source_row = first_ + index.row();
  if (role == PhotoIndex) {
    return source_row;
  }
  if (role == TileX || role == TileY || role == TileWidth || role == TileHeight) {
    const auto tile = layout_.TileRect(source_row);
    if (!tile.has_value()) {
      return 0.0;
    }
    switch (role) {
      case TileX:
        return tile->x;
      case TileY:
        return tile->y;
      case TileWidth:
        return tile->width;
      default:
        return tile->height;
    }
  }
  auto* model = thumbnailModel();
  return model ? model->data(model->index(source_row, 0), role) : QVariant{};
}

QHash<int, QByteArray> JustifiedLayoutModel::roleNames() const {
  auto names = AlbumThumbnailModel::RoleNameTable();
  names.insert(PhotoIndex, "photoIndex");
  names.insert(TileX, "tileX");
  names.insert(TileY, "tileY");
  names.insert(TileWidth, "tileWidth");
  names.insert(TileHeight, "tileHeight");
  return names;
}

auto JustifiedLayoutModel::thumbnailModel() const -> AlbumThumbnailModel* { return source_.data(); }

void JustifiedLayoutModel::setSource(QObject* source) {
  auto* model = qobject_cast<AlbumThumbnailModel*>(source);
  if (model == source_.data()) {
    return;
  }
  for (const auto& connection : source_connections_) {
    QObject::disconnect(connection);
  }
  source_connections_.clear();
  beginResetModel();
  source_ = model;
  if (model) {
    source_connections_ = {
        connect(model, &QAbstractItemModel::modelAboutToBeReset, this,
                [this]() { beginResetModel(); }),
        connect(model, &QAbstractItemModel::modelReset, this, &JustifiedLayoutModel::OnSourceReset),
        connect(model, &QAbstractItemModel::rowsInserted, this,
                &JustifiedLayoutModel::OnSourceRowsInserted),
        connect(model, &QAbstractItemModel::rowsRemoved, this,
                &JustifiedLayoutModel::OnSourceRowsRemoved),
        connect(model, &QAbstractItemModel::dataChanged, this,
                &JustifiedLayoutModel::OnSourceDataChanged),
        connect(model, &AlbumThumbnailModel::totalCountChanged, this,
                [this]() {
                  ++tiles_revision_;
                  emit tilesChanged();
                }),
    };
  }
  layout_.Reset(model ? AspectRatios(0, model->count() - 1) : std::vector<double>{});
  const auto wanted = WantedWindow();
  first_            = wanted.first;
  window_count_     = std::max(0, wanted.last - wanted.first + 1);
  endResetModel();
  ++tiles_revision_;
  emit sourceChanged();
  emit tilesChanged();
  emit windowChanged();
}

auto JustifiedLayoutModel::AspectRatioAt(int source_row) const -> double {
  auto* model = thumbnailModel();
  if (!model || source_row < 0 || source_row >= model->count()) {
    return 0.0;
  }
  return model->items()[static_cast<size_t>(source_row)].aspect_ratio;
}

auto JustifiedLayoutModel::AspectRatios(int first, int last) const -> std::vector<double> {
  std::vector<double> out;
  if (last < first) {
    return out;
  }
  out.reserve(static_cast<size_t>(last - first + 1));
  for (int row = first; row <= last; ++row) {
    out.push_back(AspectRatioAt(row));
  }
  return out;
}

void JustifiedLayoutModel::ApplyGeometry(const JustifiedLayoutGeometry& geometry) {
  if (!layout_.SetGeometry(geometry)) {
    return;
  }
  geometry_ = geometry;
  emit geometryChanged();
  UpdateWindow(true);
}

void JustifiedLayoutModel::setViewWidth(qreal value) {
  auto geometry       = geometry_;
  geometry.view_width = std::max<qreal>(0.0, Finite(value));
  ApplyGeometry(geometry);
}

void JustifiedLayoutModel::setRowHeight(qreal value) {
  auto geometry       = geometry_;
  geometry.row_height = std::max<qreal>(0.0, Finite(value));
  ApplyGeometry(geometry);
}

void JustifiedLayoutModel::setSpacing(qreal value) {
  auto geometry    = geometry_;
  geometry.spacing = std::max<qreal>(0.0, Finite(value));
  ApplyGeometry(geometry);
}

void JustifiedLayoutModel::setCaptionHeight(qreal value) {
  auto geometry           = geometry_;
  geometry.caption_height = std::max<qreal>(0.0, Finite(value));
  ApplyGeometry(geometry);
}

void JustifiedLayoutModel::setGeometry(qreal viewWidth, qreal rowHeight, qreal spacing,
                                       qreal captionHeight) {
  ApplyGeometry(JustifiedLayoutGeometry{
      std::max<qreal>(0.0, Finite(viewWidth)), std::max<qreal>(0.0, Finite(rowHeight)),
      std::max<qreal>(0.0, Finite(spacing)), std::max<qreal>(0.0, Finite(captionHeight))});
}

qreal JustifiedLayoutModel::setGeometryAnchored(qreal viewWidth, qreal rowHeight, qreal spacing,
                                                qreal captionHeight, int anchorIndex,
                                                qreal anchorFraction, qreal anchorViewportY) {
  const JustifiedLayoutGeometry geometry{
      std::max<qreal>(0.0, Finite(viewWidth)), std::max<qreal>(0.0, Finite(rowHeight)),
      std::max<qreal>(0.0, Finite(spacing)), std::max<qreal>(0.0, Finite(captionHeight))};
  const bool tiles_changed = layout_.SetGeometry(geometry);
  if (tiles_changed) {
    geometry_ = geometry;
    emit geometryChanged();
  }
  qreal y = viewport_y_;
  if (const auto tile = layout_.TileRect(anchorIndex); tile.has_value()) {
    y = tile->y + std::clamp<qreal>(Finite(anchorFraction), 0.0, 1.0) * tile->height -
        Finite(anchorViewportY);
  }
  y = std::clamp<qreal>(y, 0.0, std::max<qreal>(0.0, contentHeight() - viewport_height_));
  if (y != viewport_y_) {
    viewport_y_ = y;
    emit viewportChanged();
  }
  UpdateWindow(tiles_changed);
  return viewport_y_;
}

void JustifiedLayoutModel::setViewportY(qreal value) {
  value = Finite(value);
  if (value == viewport_y_) {
    return;
  }
  viewport_y_ = value;
  emit viewportChanged();
  UpdateWindow(false);
}

void JustifiedLayoutModel::setViewportHeight(qreal value) {
  value = std::max<qreal>(0.0, Finite(value));
  if (value == viewport_height_) {
    return;
  }
  viewport_height_ = value;
  emit viewportChanged();
  UpdateWindow(false);
}

void JustifiedLayoutModel::setOverscan(qreal value) {
  value = std::max<qreal>(0.0, Finite(value));
  if (value == overscan_) {
    return;
  }
  overscan_ = value;
  emit viewportChanged();
  UpdateWindow(false);
}

auto JustifiedLayoutModel::contentHeight() const -> qreal {
  auto* model = thumbnailModel();
  if (!model) {
    return 0.0;
  }
  return layout_.EstimatedHeight(std::max(model->count(), static_cast<int>(model->totalCount())));
}

auto JustifiedLayoutModel::WantedWindow() const -> JustifiedRowLayout::IndexRange {
  if (viewport_height_ <= 0.0) {
    return {};
  }
  return layout_.IndexRangeForY(viewport_y_ - overscan_,
                                viewport_y_ + viewport_height_ + overscan_);
}

void JustifiedLayoutModel::UpdateWindow(bool tiles_changed) {
  const auto wanted     = WantedWindow();
  const int  new_first  = wanted.first;
  const int  new_count  = std::max(0, wanted.last - wanted.first + 1);
  const int  old_first  = first_;
  const int  old_count  = window_count_;
  const int  new_end    = new_first + new_count;
  const auto remove_all = [this]() {
    if (window_count_ > 0) {
      beginRemoveRows(QModelIndex(), 0, window_count_ - 1);
      window_count_ = 0;
      endRemoveRows();
    }
  };

  if (new_count == 0) {
    remove_all();
  } else if (window_count_ == 0 || new_first >= first_ + window_count_ || new_end <= first_) {
    remove_all();
    beginInsertRows(QModelIndex(), 0, new_count - 1);
    first_        = new_first;
    window_count_ = new_count;
    endInsertRows();
  } else {
    // The windows overlap: the photos in both keep their rows.
    if (new_first > first_) {
      const int k = new_first - first_;
      beginRemoveRows(QModelIndex(), 0, k - 1);
      first_ += k;
      window_count_ -= k;
      endRemoveRows();
    }
    if (new_end < first_ + window_count_) {
      beginRemoveRows(QModelIndex(), new_end - first_, window_count_ - 1);
      window_count_ = new_end - first_;
      endRemoveRows();
    }
    if (new_first < first_) {
      const int k = first_ - new_first;
      beginInsertRows(QModelIndex(), 0, k - 1);
      first_ = new_first;
      window_count_ += k;
      endInsertRows();
    }
    if (new_end > first_ + window_count_) {
      beginInsertRows(QModelIndex(), window_count_, new_end - first_ - 1);
      window_count_ = new_end - first_;
      endInsertRows();
    }
  }

  if (tiles_changed && window_count_ > 0) {
    emit dataChanged(index(0, 0), index(window_count_ - 1, 0), kTileRoles);
  }
  if (old_first != first_ || old_count != window_count_) {
    emit windowChanged();
  }
  if (tiles_changed) {
    ++tiles_revision_;
    emit tilesChanged();
  }
}

void JustifiedLayoutModel::OnSourceReset() {
  auto* model = thumbnailModel();
  layout_.Reset(model ? AspectRatios(0, model->count() - 1) : std::vector<double>{});
  const auto wanted = WantedWindow();
  first_            = wanted.first;
  window_count_     = std::max(0, wanted.last - wanted.first + 1);
  endResetModel();
  ++tiles_revision_;
  emit tilesChanged();
  emit windowChanged();
}

void JustifiedLayoutModel::OnSourceRowsInserted(const QModelIndex& parent, int first, int last) {
  if (parent.isValid() || last < first) {
    return;
  }
  const int n = last - first + 1;
  if (window_count_ > 0) {
    if (first <= first_) {
      // Photos before the window: the rows keep their photos.
      first_ += n;
    } else if (first < first_ + window_count_) {
      beginInsertRows(QModelIndex(), first - first_, last - first_);
      window_count_ += n;
      endInsertRows();
    }
  }
  layout_.Insert(first, AspectRatios(first, last));
  UpdateWindow(true);
}

void JustifiedLayoutModel::OnSourceRowsRemoved(const QModelIndex& parent, int first, int last) {
  if (parent.isValid() || last < first) {
    return;
  }
  const int n = last - first + 1;
  if (window_count_ > 0) {
    const int end = first_ + window_count_;
    if (last < first_) {
      first_ -= n;
    } else if (first < end) {
      const int removed_first = std::max(first, first_);
      const int removed_last  = std::min(last, end - 1);
      beginRemoveRows(QModelIndex(), removed_first - first_, removed_last - first_);
      window_count_ -= removed_last - removed_first + 1;
      first_ -= std::max(0, first_ - first);
      endRemoveRows();
    }
  }
  layout_.Remove(first, n);
  UpdateWindow(true);
}

void JustifiedLayoutModel::OnSourceDataChanged(const QModelIndex& top_left,
                                               const QModelIndex& bottom_right,
                                               const QList<int>&  roles) {
  if (!top_left.isValid() || !bottom_right.isValid()) {
    return;
  }
  const int first         = top_left.row();
  const int last          = bottom_right.row();
  bool      tiles_changed = false;
  if (roles.isEmpty() || roles.contains(AlbumThumbnailModel::AspectRatio)) {
    for (int row = first; row <= last; ++row) {
      tiles_changed = layout_.SetAspectRatio(row, AspectRatioAt(row)) || tiles_changed;
    }
  }
  const int forwarded_first = std::max(first, first_);
  const int forwarded_last  = std::min(last, first_ + window_count_ - 1);
  if (window_count_ > 0 && forwarded_first <= forwarded_last) {
    emit dataChanged(index(forwarded_first - first_, 0), index(forwarded_last - first_, 0), roles);
  }
  if (tiles_changed) {
    UpdateWindow(true);
  }
}

QRectF JustifiedLayoutModel::tileRect(int index) const {
  const auto tile = layout_.TileRect(index);
  return tile.has_value() ? QRectF(tile->x, tile->y, tile->width, tile->height) : QRectF{};
}

int JustifiedLayoutModel::indexAt(qreal x, qreal y) const { return layout_.IndexAt(x, y); }

int JustifiedLayoutModel::nearestIndex(qreal x, qreal y) const {
  return layout_.NearestIndex(x, y);
}

QVariantList JustifiedLayoutModel::indicesInRect(qreal x, qreal y, qreal width,
                                                 qreal height) const {
  QVariantList out;
  for (const int index : layout_.IndicesInRect(x, y, x + width, y + height)) {
    out.push_back(index);
  }
  return out;
}

qreal JustifiedLayoutModel::rowTopOfIndex(int index) const {
  return layout_.EstimatedRowTop(index);
}

int         JustifiedLayoutModel::indexAtY(qreal y) const { return layout_.EstimatedIndexAtY(y); }

QVariantMap JustifiedLayoutModel::indexRangeForY(qreal y0, qreal y1) const {
  const auto range = layout_.IndexRangeForY(y0, y1);
  return QVariantMap{{QStringLiteral("first"), range.first}, {QStringLiteral("last"), range.last}};
}

void RegisterJustifiedLayoutQmlType() {
  qmlRegisterType<JustifiedLayoutModel>("Alcedo.Main", 1, 0, "JustifiedLayoutModel");
}

}  // namespace alcedo::ui
