//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/justified_layout_model.hpp"

#include <qqml.h>

#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace alcedo::ui {
namespace {

const QList<int> kTileRoles{JustifiedLayoutModel::TileX, JustifiedLayoutModel::TileY,
                            JustifiedLayoutModel::TileWidth, JustifiedLayoutModel::TileHeight};

auto             Finite(qreal value) -> qreal { return std::isfinite(value) ? value : 0.0; }

}  // namespace

JustifiedLayoutModel::JustifiedLayoutModel(QObject* parent) : QAbstractListModel(parent) {}

int JustifiedLayoutModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(slot_photo_.size());
}

int JustifiedLayoutModel::slotOfIndex(int index) const {
  if (index < first_ || index >= first_ + window_count_) {
    return -1;
  }
  return window_slot_[static_cast<size_t>(index - first_)];
}

QVariant JustifiedLayoutModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
    return {};
  }
  const int source_row = slot_photo_[static_cast<size_t>(index.row())];
  if (role == PhotoIndex) {
    return source_row;
  }
  if (role == TileX || role == TileY || role == TileWidth || role == TileHeight) {
    const auto tile = source_row >= 0 ? layout_.TileRect(source_row) : std::nullopt;
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
  if (!model) {
    return {};
  }
  if (source_row < 0) {
    // An empty slot: typed values, so that the required properties of its delegate stay valid.
    static const AlbumItem kEmptyItem{};
    return model->roleValue(kEmptyItem, role);
  }
  return model->data(model->index(source_row, 0), role);
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
  PlaceWindowInOrder();
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
  UpdateWindow(WindowChange::kTiles);
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
  UpdateWindow(tiles_changed ? WindowChange::kTiles : WindowChange::kViewport);
  return viewport_y_;
}

void JustifiedLayoutModel::setViewportY(qreal value) {
  value = Finite(value);
  if (value == viewport_y_) {
    return;
  }
  viewport_y_ = value;
  emit viewportChanged();
  UpdateWindow(WindowChange::kViewport);
}

void JustifiedLayoutModel::setViewportHeight(qreal value) {
  value = std::max<qreal>(0.0, Finite(value));
  if (value == viewport_height_) {
    return;
  }
  viewport_height_ = value;
  emit viewportChanged();
  UpdateWindow(WindowChange::kViewport);
}

void JustifiedLayoutModel::setOverscan(qreal value) {
  value = std::max<qreal>(0.0, Finite(value));
  if (value == overscan_) {
    return;
  }
  overscan_ = value;
  emit viewportChanged();
  UpdateWindow(WindowChange::kViewport);
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

void JustifiedLayoutModel::PlaceWindowInOrder() {
  const auto wanted = WantedWindow();
  window_count_     = std::max(0, wanted.last - wanted.first + 1);
  first_            = window_count_ > 0 ? wanted.first : 0;
  slot_photo_.resize(static_cast<size_t>(window_count_));
  window_slot_.resize(static_cast<size_t>(window_count_));
  for (int i = 0; i < window_count_; ++i) {
    slot_photo_[static_cast<size_t>(i)]  = first_ + i;
    window_slot_[static_cast<size_t>(i)] = i;
  }
}

void JustifiedLayoutModel::UpdateWindow(WindowChange change) {
  const auto wanted    = WantedWindow();
  const int  new_count = std::max(0, wanted.last - wanted.first + 1);
  const int  new_first = new_count > 0 ? wanted.first : 0;
  const int  old_first = first_;
  const int  old_count = window_count_;
  const int  old_slots = rowCount();
  const auto finish    = [&]() {
    if (old_first != first_ || old_count != window_count_) {
      emit windowChanged();
    }
    if (change != WindowChange::kViewport) {
      ++tiles_revision_;
      emit tilesChanged();
    }
  };

  // A layout or source change that leaves fewer than half of the slots in use removes the slots
  // from new_count on; a photo in a removed slot moves to a kept one.
  const bool shrink        = change != WindowChange::kViewport && new_count < old_slots / 2;
  const int  kept_slots    = shrink ? new_count : old_slots;
  const auto in_new_window = [&](int photo) {
    return photo >= new_first && photo < new_first + new_count;
  };
  // A photo that leaves (or whose slot is removed) empties its slot; a photo that stays keeps it.
  std::vector<char> photo_changed(static_cast<size_t>(old_slots), 0);
  std::vector<int>  next_window_slot(static_cast<size_t>(new_count), -1);
  for (int photo = old_first; photo < old_first + old_count; ++photo) {
    const int slot = window_slot_[static_cast<size_t>(photo - old_first)];
    if (in_new_window(photo) && slot < kept_slots) {
      next_window_slot[static_cast<size_t>(photo - new_first)] = slot;
    } else {
      slot_photo_[static_cast<size_t>(slot)]   = -1;
      photo_changed[static_cast<size_t>(slot)] = 1;
    }
  }
  // A photo without a slot takes the lowest empty kept slot, or a new slot after the last one.
  std::vector<int> empty_slots;
  for (int slot = 0; slot < kept_slots; ++slot) {
    if (slot_photo_[static_cast<size_t>(slot)] < 0) {
      empty_slots.push_back(slot);
    }
  }
  size_t           next_empty = 0;
  std::vector<int> appended;
  for (int photo = new_first; photo < new_first + new_count; ++photo) {
    auto& slot = next_window_slot[static_cast<size_t>(photo - new_first)];
    if (slot >= 0) {
      continue;
    }
    if (next_empty < empty_slots.size()) {
      slot                                     = empty_slots[next_empty++];
      slot_photo_[static_cast<size_t>(slot)]   = photo;
      photo_changed[static_cast<size_t>(slot)] = 1;
    } else {
      slot = old_slots + static_cast<int>(appended.size());
      appended.push_back(photo);
    }
  }
  const auto set_window = [&]() {
    first_        = new_first;
    window_count_ = new_count;
    window_slot_  = std::move(next_window_slot);
    slot_photo_.resize(static_cast<size_t>(kept_slots));
    slot_photo_.insert(slot_photo_.end(), appended.begin(), appended.end());
  };
  if (kept_slots < old_slots) {
    beginRemoveRows(QModelIndex(), kept_slots, old_slots - 1);
    set_window();
    endRemoveRows();
  } else if (!appended.empty()) {
    beginInsertRows(QModelIndex(), old_slots, old_slots + static_cast<int>(appended.size()) - 1);
    set_window();
    endInsertRows();
  } else {
    set_window();
  }

  // Slots that existed before and stay: every role when the photo of the slot changed (or an
  // index can name another photo), the tile roles when only the tiles moved, nothing otherwise.
  // Runs of slots with the same notification share one dataChanged.
  enum class Notify { kNone, kTiles, kAll };
  const auto notify_of = [&](int slot) {
    const bool has_photo = slot_photo_[static_cast<size_t>(slot)] >= 0;
    if (photo_changed[static_cast<size_t>(slot)] != 0 ||
        (change == WindowChange::kPhotos && has_photo)) {
      return Notify::kAll;
    }
    return change == WindowChange::kTiles && has_photo ? Notify::kTiles : Notify::kNone;
  };
  const int notified  = std::min(old_slots, kept_slots);
  int       run_first = 0;
  Notify    run       = Notify::kNone;
  for (int slot = 0; slot <= notified; ++slot) {
    const Notify notify = slot < notified ? notify_of(slot) : Notify::kNone;
    if (slot < notified && notify == run) {
      continue;
    }
    if (run != Notify::kNone && slot > run_first) {
      emit dataChanged(index(run_first, 0), index(slot - 1, 0),
                       run == Notify::kTiles ? kTileRoles : QList<int>{});
    }
    run_first = slot;
    run       = notify;
  }
  finish();
}

void JustifiedLayoutModel::OnSourceReset() {
  auto* model = thumbnailModel();
  layout_.Reset(model ? AspectRatios(0, model->count() - 1) : std::vector<double>{});
  // A reset installs another result (a filter, a search, a collection), which shows from its
  // first photo. The old viewport can lie below the end of a shorter result, where the window
  // has no photo.
  const bool viewport_moved = viewport_y_ != 0.0;
  viewport_y_               = 0.0;
  PlaceWindowInOrder();
  endResetModel();
  ++tiles_revision_;
  emit tilesChanged();
  emit windowChanged();
  if (viewport_moved) {
    emit viewportChanged();
    emit viewportReset();
  }
}

void JustifiedLayoutModel::OnSourceRowsInserted(const QModelIndex& parent, int first, int last) {
  if (parent.isValid() || last < first) {
    return;
  }
  layout_.Insert(first, AspectRatios(first, last));
  UpdateWindow(WindowChange::kPhotos);
}

void JustifiedLayoutModel::OnSourceRowsRemoved(const QModelIndex& parent, int first, int last) {
  if (parent.isValid() || last < first) {
    return;
  }
  layout_.Remove(first, last - first + 1);
  UpdateWindow(WindowChange::kPhotos);
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
  // The slots of the photos of the window in the range, by runs of consecutive slots.
  const int forwarded_first = std::max(first, first_);
  const int forwarded_last  = std::min(last, first_ + window_count_ - 1);
  int       run_first       = -1;
  int       run_last        = -1;
  for (int photo = forwarded_first; photo <= forwarded_last; ++photo) {
    const int slot = window_slot_[static_cast<size_t>(photo - first_)];
    if (run_first >= 0 && slot == run_last + 1) {
      run_last = slot;
      continue;
    }
    if (run_first >= 0) {
      emit dataChanged(index(run_first, 0), index(run_last, 0), roles);
    }
    run_first = slot;
    run_last  = slot;
  }
  if (run_first >= 0) {
    emit dataChanged(index(run_first, 0), index(run_last, 0), roles);
  }
  if (tiles_changed) {
    UpdateWindow(WindowChange::kTiles);
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
