//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_mask_segmentation_service.hpp"

#include <optional>
#include <utility>

namespace alcedo {
namespace {

constexpr const char* kClosedMessage = "the AI Mask segmentation service is closed";

}  // namespace

AiMaskSegmentationService::AiMaskSegmentationService(
    std::shared_ptr<void> sidecar_lease, std::unique_ptr<sidecar_client::SegmentationClient> client,
    AiMaskSegmentationTimeouts timeouts)
    : sidecar_lease_(std::move(sidecar_lease)), client_(std::move(client)), timeouts_(timeouts) {
  closed_ = client_ == nullptr;
}

AiMaskSegmentationService::~AiMaskSegmentationService() { Close(); }

auto AiMaskSegmentationService::LocalFailure(std::string message) -> SegmentationCallStatus {
  SegmentationCallStatus status;
  status.ok    = false;
  status.error = std::move(message);
  return status;
}

auto AiMaskSegmentationService::LoadModels(const std::filesystem::path& model_directory,
                                           const std::string& device) -> SegmentationCallStatus {
  if (closed_) {
    return LocalFailure(kClosedMessage);
  }
  return client_->LoadModels(model_directory.string(), device, timeouts_.load);
}

auto AiMaskSegmentationService::Status() -> SegmentationStatus {
  if (closed_) {
    SegmentationStatus status;
    status.status = LocalFailure(kClosedMessage);
    return status;
  }
  return client_->GetStatus(timeouts_.control);
}

auto AiMaskSegmentationService::StartImage(const SegmentationRgb8Image& image,
                                           const std::string&           image_key)
    -> SegmentationSessionResult {
  return StartSession(image, image_key);
}

auto AiMaskSegmentationService::StartCachedImage(const std::string& image_key)
    -> SegmentationSessionResult {
  if (image_key.empty()) {
    SegmentationSessionResult result;
    result.status = LocalFailure("a cached segmentation start needs an image key");
    return result;
  }
  return StartSession(SegmentationRgb8Image{}, image_key);
}

auto AiMaskSegmentationService::StartSession(const SegmentationRgb8Image& image,
                                             const std::string&           image_key)
    -> SegmentationSessionResult {
  SegmentationSessionResult result;
  if (closed_) {
    result.status = LocalFailure(kClosedMessage);
    return result;
  }
  const auto ended = EndImage();
  if (!ended.ok) {
    result.status = ended;
    return result;
  }
  result = client_->StartSession(image, image_key, timeouts_.start);
  if (result.status.ok) {
    session_id_ = result.session_id;
  }
  return result;
}

auto AiMaskSegmentationService::Decode(const SegmentationPrompt& prompt)
    -> SegmentationDecodeResult {
  SegmentationDecodeResult result;
  if (closed_) {
    result.status = LocalFailure(kClosedMessage);
    return result;
  }
  if (session_id_.empty()) {
    result.status = LocalFailure("no segmentation session: start an image first");
    return result;
  }
  return client_->Decode(session_id_, prompt, timeouts_.decode);
}

auto AiMaskSegmentationService::RunTask(SegmentationTask        task,
                                        std::optional<uint32_t> person_index)
    -> SegmentationTaskResult {
  SegmentationTaskResult result;
  if (closed_) {
    result.status = LocalFailure(kClosedMessage);
    return result;
  }
  if (session_id_.empty()) {
    result.status = LocalFailure("no segmentation session: start an image first");
    return result;
  }
  return client_->RunTask(session_id_, task, person_index, timeouts_.task);
}

auto AiMaskSegmentationService::EndImage() -> SegmentationCallStatus {
  if (closed_) {
    return LocalFailure(kClosedMessage);
  }
  if (session_id_.empty()) {
    SegmentationCallStatus status;
    status.ok = true;
    return status;
  }
  auto status = client_->EndSession(session_id_, timeouts_.control);
  if (status.ok) {
    session_id_.clear();
  }
  return status;
}

auto AiMaskSegmentationService::Close() -> SegmentationCallStatus {
  SegmentationCallStatus result;
  result.ok = true;
  if (closed_) {
    sidecar_lease_.reset();
    return result;
  }
  const auto ended    = EndImage();
  const auto unloaded = client_->UnloadModels(timeouts_.control);
  closed_             = true;
  session_id_.clear();
  client_.reset();
  sidecar_lease_.reset();
  if (!ended.ok) {
    return ended;
  }
  return unloaded;
}

}  // namespace alcedo
