//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/mask_thumbnail_coordinator.hpp"

#include <QImage>

#include <exception>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"

namespace alcedo::ui {
namespace {

[[nodiscard]] auto TargetNodeString(const alcedo::NodeId& node_id) -> QString {
  return QString::fromStdString(std::string(node_id.Value()));
}

[[nodiscard]] auto TargetMaskString(const alcedo::MaskId& mask_id) -> QString {
  return QString::fromStdString(std::string(mask_id.Value()));
}

[[nodiscard]] auto GradeFor(const alcedo::PipelineDocument& document, const alcedo::NodeId& node_id)
    -> const alcedo::ColorGradeNodeModel* {
  const auto* node = document.Graph().FindNode(node_id);
  return dynamic_cast<const alcedo::ColorGradeNodeModel*>(node);
}

}  // namespace

auto MaskThumbnailCoordinator::TargetHash::operator()(const Target& target) const noexcept
    -> std::size_t {
  const auto a = std::hash<std::string_view>{}(target.node_id.Value());
  const auto b = std::hash<std::string_view>{}(target.mask_id.Value());
  return a ^ (b + 0x9e3779b9 + (a << 6) + (a >> 2));
}

MaskThumbnailCoordinator::MaskThumbnailCoordinator(QObject* parent)
    : QObject(parent), store_(SharedMaskThumbnailImageStore()) {}

void MaskThumbnailCoordinator::SetService(std::shared_ptr<alcedo::MaskThumbnailService> service) {
  if (service_ == service) {
    return;
  }
  service_ = std::move(service);
  for (auto& [target, binding] : bindings_) {
    Invalidate(target);
  }
}

void MaskThumbnailCoordinator::SetImageStore(std::shared_ptr<MaskThumbnailImageStore> store) {
  if (store) {
    store_ = std::move(store);
  }
}

auto MaskThumbnailCoordinator::SetFullReference(alcedo::Extent2D extent) -> bool {
  if (full_reference_.width == extent.width && full_reference_.height == extent.height) {
    return false;
  }
  full_reference_ = extent;
  return true;
}

void MaskThumbnailCoordinator::Clear() {
  if (store_) {
    for (const auto& [target, binding] : bindings_) {
      if (binding.request_id != 0) {
        store_->Remove(binding.request_id);
      }
    }
  }
  bindings_.clear();
}

auto MaskThumbnailCoordinator::MakeTarget(const QString& node_id, const QString& mask_id)
    -> Target {
  return Target{alcedo::NodeId{node_id.toStdString()}, alcedo::MaskId{mask_id.toStdString()}};
}

void MaskThumbnailCoordinator::invalidateTarget(const QString& node_id, const QString& mask_id) {
  Invalidate(MakeTarget(node_id, mask_id));
}

void MaskThumbnailCoordinator::invalidateNode(const QString& node_id) {
  const alcedo::NodeId node{node_id.toStdString()};
  std::vector<Target>  matches;
  for (const auto& [target, binding] : bindings_) {
    if (target.node_id == node) {
      matches.push_back(target);
    }
  }
  for (const auto& target : matches) {
    Invalidate(target);
  }
}

void MaskThumbnailCoordinator::requestCurrent(const QString& node_id, const QString& mask_id) {
  const auto target = MakeTarget(node_id, mask_id);
  const auto found  = bindings_.find(target);
  if (found == bindings_.end() || !found->second.spec.has_value()) {
    return;
  }
  Request(target, *found->second.spec);
}

QString MaskThumbnailCoordinator::thumbnailUrl(const QString& node_id,
                                                const QString& mask_id) const {
  const auto found = bindings_.find(MakeTarget(node_id, mask_id));
  if (found == bindings_.end()) {
    return {};
  }
  return found->second.url;
}

auto MaskThumbnailCoordinator::request_id(const QString& node_id, const QString& mask_id) const
    -> std::uint64_t {
  const auto found = bindings_.find(MakeTarget(node_id, mask_id));
  if (found == bindings_.end()) {
    return 0;
  }
  return found->second.request_id;
}

auto MaskThumbnailCoordinator::current_spec(const QString& node_id, const QString& mask_id) const
    -> std::optional<alcedo::MaskThumbnailSpec> {
  const auto found = bindings_.find(MakeTarget(node_id, mask_id));
  if (found == bindings_.end()) {
    return std::nullopt;
  }
  return found->second.spec;
}

auto MaskThumbnailCoordinator::MakeGeometry(const alcedo::PipelineDocument& document) const
    -> std::optional<alcedo::MaskThumbnailGeometry> {
  if (full_reference_.Empty()) {
    return std::nullopt;
  }
  alcedo::MaskThumbnailGeometry geometry;
  geometry.full_reference    = full_reference_;
  geometry.crop_rect         = document.Geometry().CropRect();
  geometry.rotation_degrees  = document.Geometry().RotationDegrees();
  geometry.expand_to_fit     = document.Geometry().ExpandToFit();
  return geometry;
}

void MaskThumbnailCoordinator::Request(const Target& target, alcedo::MaskThumbnailSpec spec) {
  auto& binding = bindings_[target];
  if (binding.spec.has_value() && *binding.spec == spec && !binding.url.isEmpty()) {
    return;
  }
  if (store_ && binding.request_id != 0) {
    store_->Remove(binding.request_id);
  }
  binding.request_id = next_request_id_++;
  binding.spec       = spec;
  binding.url.clear();
  emit thumbnailUrlChanged(TargetNodeString(target.node_id), TargetMaskString(target.mask_id));
  if (!service_) {
    return;
  }
  service_->Request(spec, this, binding.request_id,
                    [this, target](std::uint64_t request_id, alcedo::MaskThumbnailSpec result_spec,
                                   QImage image, QString error) {
                      OnResult(target, request_id, std::move(result_spec), std::move(image),
                               std::move(error));
                    });
}

void MaskThumbnailCoordinator::Invalidate(const Target& target) {
  auto found = bindings_.find(target);
  if (found == bindings_.end()) {
    return;
  }
  found->second.request_id = next_request_id_++;
}

void MaskThumbnailCoordinator::OnResult(const Target& target, std::uint64_t request_id,
                                        alcedo::MaskThumbnailSpec spec, QImage image,
                                        QString error) {
  auto found = bindings_.find(target);
  if (found == bindings_.end()) {
    return;
  }
  if (found->second.request_id != request_id) {
    return;
  }
  if (!found->second.spec.has_value() || *found->second.spec != spec) {
    return;
  }
  if (!error.isEmpty() || image.isNull() || !store_) {
    found->second.url.clear();
    emit thumbnailUrlChanged(TargetNodeString(target.node_id), TargetMaskString(target.mask_id));
    return;
  }
  found->second.url = store_->Put(request_id, std::move(image));
  emit thumbnailUrlChanged(TargetNodeString(target.node_id), TargetMaskString(target.mask_id));
}

void MaskThumbnailCoordinator::Sync(const alcedo::PipelineDocument&        document,
                                    const alcedo::EditorMaskGroupSnapshot& groups) {
  const auto geometry = MakeGeometry(document);
  std::unordered_set<Target, TargetHash> live;
  if (!geometry.has_value()) {
    Clear();
    return;
  }
  for (const auto& group : groups.groups) {
    const auto* grade = GradeFor(document, group.node_id);
    if (grade == nullptr) {
      continue;
    }
    if (grade->Masks().empty()) {
      continue;
    }
    const Target group_target{group.node_id, alcedo::MaskId{}};
    live.insert(group_target);
    try {
      Request(group_target, MakeGroupMaskThumbnailSpec(*geometry, grade->Masks()));
    } catch (const std::exception&) {
      continue;
    }
    for (const auto& mask : grade->Masks()) {
      const Target mask_target{group.node_id, mask.id};
      live.insert(mask_target);
      try {
        Request(mask_target, MakeSingleMaskThumbnailSpec(*geometry, mask));
      } catch (const std::exception&) {
      }
    }
  }
  std::vector<Target> stale;
  for (const auto& [target, binding] : bindings_) {
    if (live.find(target) == live.end()) {
      stale.push_back(target);
    }
  }
  for (const auto& target : stale) {
    if (store_ && bindings_[target].request_id != 0) {
      store_->Remove(bindings_[target].request_id);
    }
    bindings_.erase(target);
  }
}

}  // namespace alcedo::ui
