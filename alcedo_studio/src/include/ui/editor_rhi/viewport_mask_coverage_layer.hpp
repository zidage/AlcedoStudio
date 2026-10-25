//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QtGui/rhi/qrhi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui/edit_viewer/mask_coverage_sink.hpp"
#include "ui/editor_rhi/direct_present_queue.hpp"
#include "ui/editor_rhi/frame_presentation_lease.hpp"
#include "ui/editor_rhi/lease_target_adapters.hpp"
#include "ui/editor_rhi/viewport_mask_coverage_sink.hpp"

namespace alcedo::editor_rhi {

/**
 * @brief Render-thread consumer of the coverage presentation channel (SM8.5): the viewport
 *        coverage input.
 *
 * Creates the native RGBA32F targets that the coverage queue requests (through the viewport's
 * lease adapter, layer MaskCoverage), imports the newest completed frame as a QRhiTexture, and
 * keeps the displayed frame until a replacement is imported. Only then does it release the
 * previous frame: CUDA and OpenCL return its queue slot (OpenCL waits for OpenGL first), and Metal
 * drops its texture owner. A failed import keeps the displayed frame.
 *
 * Frames of another image session are never imported: the queue filters its slot identity, and
 * this layer checks the identity that the frame carries.
 *
 * Owner: EditorViewportRenderer. Thread: the scene-graph render thread with the QRhi current.
 * @ref Release and @ref DestroyNatives must run before the QRhi and the adapter are destroyed.
 */
class ViewportMaskCoverageLayer {
 public:
  ViewportMaskCoverageLayer() = default;
  ~ViewportMaskCoverageLayer();
  ViewportMaskCoverageLayer(const ViewportMaskCoverageLayer&)                    = delete;
  auto operator=(const ViewportMaskCoverageLayer&) -> ViewportMaskCoverageLayer& = delete;

  /// Called with the displayed texture just before the layer destroys it, so a draw that binds
  /// the texture can drop its bindings first (EditorViewportRenderer, SM8.6).
  void SetTextureReleaseObserver(std::function<void(const QRhiTexture*)> observer) {
    texture_release_observer_ = std::move(observer);
  }

  /// Use the coverage queue and sink of a viewport. Releases the displayed frame of a previous
  /// queue.
  void Attach(std::shared_ptr<DirectPresentQueue>       queue,
              std::shared_ptr<ViewportMaskCoverageSink> sink);

  /**
   * @brief Create the requested native targets of the current image session through @p adapter.
   *
   * A failed creation fails the request (the producer reports its error) and leaves the displayed
   * frame unchanged.
   */
  void FulfillTargetRequests(QRhi* rhi, ILeaseTargetAdapter& adapter, std::uint64_t session_epoch,
                             std::uint64_t image_identity);

  /// Import the newest completed frame of the image session and then release the previous one.
  /// @return True when a new frame became the displayed frame.
  auto ConsumeNewest(QRhi* rhi, std::uint64_t session_epoch, std::uint64_t image_identity) -> bool;

  /// Release the displayed frame (hide, image change, close, device recreation). Idempotent.
  void Release();

  /// Destroy the native targets that the queue retired, through @p adapter.
  void DestroyRetiredNatives(ILeaseTargetAdapter& adapter);
  /// Destroy every native target that this layer created. @pre @ref Release; the queue no longer
  /// lists the targets (InvalidateTargetGeneration or Shutdown).
  void DestroyNatives(ILeaseTargetAdapter& adapter);

  /// The displayed coverage texture, or null.
  [[nodiscard]] auto Texture() const -> QRhiTexture* { return texture_; }
  /// Identity and photo frame geometry of the displayed frame. @pre Texture() is not null.
  [[nodiscard]] auto Presentation() const -> const MaskCoveragePresentation& {
    return presentation_;
  }
  /// Queue slot of the displayed CUDA or OpenCL frame, or -1.
  [[nodiscard]] auto SlotIndex() const -> int { return slot_index_; }
  /// Number of native targets that this layer created and has not destroyed.
  [[nodiscard]] auto NativeTargetCount() const -> std::size_t { return owned_natives_.size(); }
  /// Last target creation or import error, or empty.
  [[nodiscard]] auto LastError() const -> const std::string& { return last_error_; }

 private:
  auto ConsumeQueued(QRhi* rhi, std::uint64_t session_epoch, std::uint64_t image_identity) -> bool;
  auto ConsumeMetal(QRhi* rhi, std::uint64_t session_epoch, std::uint64_t image_identity) -> bool;
  auto Import(QRhi* rhi, int width, int height, std::uintptr_t native_handle) -> QRhiTexture*;
  void Install(QRhiTexture* texture, int slot_index, std::shared_ptr<const void> owner,
               const MaskCoveragePresentation& presentation);

  std::shared_ptr<DirectPresentQueue>       queue_;
  std::shared_ptr<ViewportMaskCoverageSink> sink_;
  std::vector<WritableTargetLease>          owned_natives_;
  QRhiTexture*                              texture_    = nullptr;
  int                                       slot_index_ = -1;
  std::shared_ptr<const void>               owner_;
  MaskCoveragePresentation                  presentation_{};
  std::string                               last_error_;
  std::function<void(const QRhiTexture*)>   texture_release_observer_;
};

}  // namespace alcedo::editor_rhi
