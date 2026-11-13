//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_mask_asset_port.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace alcedo::ui {
namespace {

auto SetError(std::string* error, std::string message) -> bool {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

}  // namespace

EditorSessionMaskAssetPort::EditorSessionMaskAssetPort(
    std::function<std::shared_ptr<alcedo::PipelineMgmtService>()> pipeline_service)
    : pipeline_service_(std::move(pipeline_service)) {}

auto EditorSessionMaskAssetPort::PutMaskAsset(const alcedo::MaskAsset& asset, std::string* error)
    -> bool {
  const auto service = pipeline_service_ ? pipeline_service_() : nullptr;
  if (!service) {
    return SetError(error, "No project is open to store the AI Mask asset");
  }
  try {
    service->PutMaskAsset(asset);
    return true;
  } catch (const std::exception& e) {
    return SetError(error, e.what());
  }
}

auto EditorSessionMaskAssetPort::ReadMaskAsset(const alcedo::MaskAssetKey& key,
                                               alcedo::MaskAsset* asset, std::string* error)
    -> bool {
  if (asset == nullptr) {
    return SetError(error, "The AI Mask asset read has no output");
  }
  const auto service = pipeline_service_ ? pipeline_service_() : nullptr;
  if (!service) {
    return SetError(error, "No project is open to read the AI Mask asset");
  }
  try {
    *asset = service->ReadMaskAsset(key);
    return true;
  } catch (const std::exception& e) {
    return SetError(error, e.what());
  }
}

CurrentProjectMaskAssetReader::CurrentProjectMaskAssetReader(
    std::function<std::shared_ptr<alcedo::PipelineMgmtService>()> pipeline_service)
    : pipeline_service_(std::move(pipeline_service)) {}

auto CurrentProjectMaskAssetReader::Read(const alcedo::MaskAssetKey& key) const
    -> alcedo::MaskAsset {
  const auto service = pipeline_service_ ? pipeline_service_() : nullptr;
  if (!service) {
    throw std::runtime_error("No project is open to read Mask asset " + std::string{key.Value()});
  }
  return service->ReadMaskAsset(key);
}

}  // namespace alcedo::ui
