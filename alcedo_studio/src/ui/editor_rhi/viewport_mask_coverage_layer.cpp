//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/viewport_mask_coverage_layer.hpp"

#include <QSize>
#include <algorithm>
#include <utility>

#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/native_resource_counters.hpp"
#include "utils/diagnostics/app_logging.hpp"

using alcedo::diag::editorPresentLog;

namespace alcedo::editor_rhi {

ViewportMaskCoverageLayer::~ViewportMaskCoverageLayer() { Release(); }

void ViewportMaskCoverageLayer::Attach(std::shared_ptr<DirectPresentQueue>       queue,
                                       std::shared_ptr<ViewportMaskCoverageSink> sink) {
  if (queue == queue_ && sink == sink_) {
    return;
  }
  Release();
  queue_ = std::move(queue);
  sink_  = std::move(sink);
}

void ViewportMaskCoverageLayer::FulfillTargetRequests(QRhi* rhi, ILeaseTargetAdapter& adapter,
                                                      std::uint64_t session_epoch,
                                                      std::uint64_t image_identity) {
  if (!queue_ || rhi == nullptr || session_epoch == 0) {
    return;
  }
  for (const auto& request : queue_->DrainSizeRequests()) {
    if (!request.valid() || request.session_epoch != session_epoch ||
        (request.image_identity != 0 && request.image_identity != image_identity)) {
      continue;
    }
    const auto prepare =
        queue_->PrepareWrite(request.width, request.height, session_epoch, image_identity);
    if (prepare.ok && !prepare.need_create) {
      continue;  // An earlier request of this pass created the same size.
    }
    const TargetGeneration generation{queue_->CurrentTargetGeneration(), session_epoch,
                                      request.layer_generation, image_identity};
    auto lease = adapter.CreateTarget(rhi, QSize(request.width, request.height), generation,
                                      LeaseFrameLayer::MaskCoverage);
    if (!lease.has_value()) {
      last_error_ = adapter.lastError();
      qCWarning(editorPresentLog, "[EditorPresent] coverage target allocation failed %dx%d: %s",
                request.width, request.height, last_error_.c_str());
      queue_->FailSizeRequest(request);
      continue;
    }
    DirectPresentQueue::SlotNative native;
    native.backend           = lease->backend;
    native.handle_kind       = lease->handle_kind;
    native.writable_kind     = lease->writable_kind;
    native.native_handle     = lease->native_handle;
    native.writable_resource = lease->writable_resource;
    native.sync_object       = lease->sync_object;
    native.sync_value        = lease->sync_value;
    native.adapter_cookie    = lease->native_handle;
    bool published           = prepare.slot_index >= 0 &&
                     queue_->PublishCreatedSlot(prepare.slot_index, request.width, request.height,
                                                native, session_epoch, image_identity);
    for (int i = 0; !published && i < DirectPresentQueue::kSlotCount; ++i) {
      published = queue_->PublishCreatedSlot(i, request.width, request.height, native,
                                             session_epoch, image_identity);
    }
    if (!published) {
      last_error_ = "the coverage queue has no free slot for a new target";
      adapter.DestroyTarget(*lease);
      queue_->FailSizeRequest(request);
      continue;
    }
    owned_natives_.push_back(*lease);
    last_error_.clear();
  }
}

auto ViewportMaskCoverageLayer::ConsumeNewest(QRhi* rhi, std::uint64_t session_epoch,
                                              std::uint64_t image_identity) -> bool {
  if (!queue_ || rhi == nullptr) {
    return false;
  }
  return queue_->backend() == EditorBackend::Metal
             ? ConsumeMetal(rhi, session_epoch, image_identity)
             : ConsumeQueued(rhi, session_epoch, image_identity);
}

auto ViewportMaskCoverageLayer::ConsumeQueued(QRhi* rhi, std::uint64_t session_epoch,
                                              std::uint64_t image_identity) -> bool {
  const auto frame =
      queue_->ConsumeNewestReady(FrameRole::InteractivePrimary, session_epoch, image_identity);
  if (!frame.has_value()) {
    return false;
  }
  const auto& slot     = frame->slot;
  const auto& metadata = slot.preview_metadata;
  if (metadata.session_epoch != session_epoch || metadata.image_identity != image_identity) {
    // Written for an image session that ended while the frame was in flight.
    queue_->CompleteRendererRead(slot.index);
    return false;
  }
  auto* texture = Import(rhi, slot.width, slot.height, slot.native.native_handle);
  if (texture == nullptr) {
    queue_->CompleteRendererRead(slot.index);
    return false;
  }
  Install(texture, slot.index, {},
          sink_ ? sink_->PresentationOfSlot(slot.index) : MaskCoveragePresentation{});
  return true;
}

auto ViewportMaskCoverageLayer::ConsumeMetal(QRhi* rhi, std::uint64_t session_epoch,
                                             std::uint64_t image_identity) -> bool {
#ifdef HAVE_METAL
  if (!sink_) {
    return false;
  }
  auto pending = sink_->TakePendingMetalCoverage(session_epoch, image_identity);
  if (!pending.has_value()) {
    return false;
  }
  auto* texture =
      Import(rhi, pending->frame.width, pending->frame.height, pending->frame.texture_handle);
  if (texture == nullptr) {
    return false;
  }
  Install(texture, -1, std::move(pending->frame.owner), pending->presentation);
  return true;
#else
  (void)rhi;
  (void)session_epoch;
  (void)image_identity;
  return false;
#endif
}

auto ViewportMaskCoverageLayer::Import(QRhi* rhi, int width, int height,
                                       std::uintptr_t native_handle) -> QRhiTexture* {
  auto* texture = rhi->newTexture(QRhiTexture::RGBA32F, QSize(width, height), 1);
  if (texture == nullptr || !texture->createFrom({static_cast<quint64>(native_handle), 0})) {
    last_error_ = "failed to import a completed coverage frame";
    qCWarning(editorPresentLog, "[EditorPresent] coverage import failed handle=%llu",
              static_cast<unsigned long long>(native_handle));
    delete texture;
    return nullptr;
  }
  texture->setNativeLayout(0);
  NativeResourceCounters::Instance().OnCreateImportedQRhiTexture();
  return texture;
}

void ViewportMaskCoverageLayer::Install(QRhiTexture* texture, int slot_index,
                                        std::shared_ptr<const void>     owner,
                                        const MaskCoveragePresentation& presentation) {
  // The replacement is imported; only now may the previous frame go.
  Release();
  texture_      = texture;
  slot_index_   = slot_index;
  owner_        = std::move(owner);
  presentation_ = presentation;
  last_error_.clear();
}

void ViewportMaskCoverageLayer::Release() {
  if (texture_ != nullptr) {
    texture_->destroy();
    delete texture_;
    texture_ = nullptr;
    NativeResourceCounters::Instance().OnDestroyImportedQRhiTexture();
  }
  if (slot_index_ >= 0 && queue_) {
    // OpenCL acquires the same GL texture for the next write: the last OpenGL sample must finish.
    if (queue_->backend() == EditorBackend::OpenCl) {
      FinishOpenGlBeforeOpenClSharedTextureReuse();
    }
    queue_->CompleteRendererRead(slot_index_);
  }
  slot_index_ = -1;
  owner_.reset();
  presentation_ = {};
}

void ViewportMaskCoverageLayer::DestroyRetiredNatives(ILeaseTargetAdapter& adapter) {
  if (!queue_) {
    return;
  }
  for (const auto& native : queue_->DrainReleasedNatives()) {
    const auto it = std::find_if(owned_natives_.begin(), owned_natives_.end(),
                                 [&](const WritableTargetLease& lease) {
                                   return lease.native_handle == native.native_handle;
                                 });
    if (it != owned_natives_.end()) {
      adapter.DestroyTarget(*it);
      owned_natives_.erase(it);
    }
  }
}

void ViewportMaskCoverageLayer::DestroyNatives(ILeaseTargetAdapter& adapter) {
  for (const auto& lease : owned_natives_) {
    adapter.DestroyTarget(lease);
  }
  owned_natives_.clear();
}

}  // namespace alcedo::editor_rhi
