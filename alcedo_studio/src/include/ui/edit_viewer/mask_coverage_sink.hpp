//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

/**
 * @brief Image and request identity of one coverage presentation (SM8.5).
 *
 * @p session_epoch and @p image_identity are the values of the editor viewport for the image
 * whose photo frame the coverage belongs to (`EditorViewportItem::sessionEpoch` and
 * `imageIdentity`). A coverage frame of another image is never published or displayed.
 * @p request_id is the existing operation id of the caller; the newest id wins when two frames
 * wait for the same render pass.
 */
struct MaskCoverageFrameIdentity {
  std::uint64_t request_id     = 0;
  std::uint64_t session_epoch  = 0;
  std::uint64_t image_identity = 0;
};

/**
 * @brief Metadata of one coverage display frame: its identity and the photo frame geometry that
 *        its texels follow.
 *
 * The coverage texture has the size of @p geometry.render_extent, and texel (x, y) is the effective
 * coverage of photo frame pixel (x, y). The viewport samples it with the photo frame geometry.
 */
struct MaskCoveragePresentation {
  MaskCoverageFrameIdentity identity{};
  ResolvedRenderGeometry    geometry{};
};

/// Result of a coverage publication.
enum class MaskCoveragePublication : std::uint8_t {
  /// The frame waits for the render thread of the viewport; it replaces the displayed coverage
  /// when the render thread imports it.
  Published = 0,
  /// The viewport changed to another image (or image session) first. The frame was dropped and
  /// the displayed coverage, if any, belongs to the new image session.
  DiscardedForImageChange,
};

#ifdef HAVE_METAL
/// A completed Metal coverage texture. @p owner keeps the MTLTexture until the renderer releases
/// the frame.
struct MaskCoverageMetalFrame {
  int                         width          = 0;
  int                         height         = 0;
  std::uintptr_t              texture_handle = 0;
  std::shared_ptr<const void> owner{};

  explicit                    operator bool() const {
    return width > 0 && height > 0 && texture_handle != 0 && owner != nullptr;
  }
};
#endif

/**
 * @brief Producer side of the dedicated coverage presentation channel of the editor viewport
 *        (SM8 C5, section 5.2).
 *
 * The channel is separate from the photo IFrameSink: it has its own native targets, it never
 * changes a photo frame, and nothing that it receives reaches scope analysis. Targets are RGBA32F;
 * the red channel holds the effective coverage, green and blue are 0, and alpha is 1.
 *
 * CUDA and OpenCL: MapCoverageTarget, a device write, UnmapCoverageTarget, PublishCoverage. On a
 * failure after a successful map, call AbandonCoverageTarget instead of PublishCoverage. Metal:
 * SubmitMetalCoverage after the command buffer completed.
 *
 * Thread: the render thread of the producing device. The calls of one frame must not interleave
 * with the calls of another frame.
 */
class IMaskCoverageSink {
 public:
  virtual ~IMaskCoverageSink() = default;

  /**
   * @brief Reserve a writable RGBA32F target of @p width x @p height for @p presentation.
   *
   * May wait until the render thread of the viewport created the native target. Returns an empty
   * mapping when no target is available; `presentation_suspended` is then set when the viewport
   * is minimized or hidden. @p preferred_domain is the memory domain of the producing device.
   */
  virtual auto MapCoverageTarget(const MaskCoveragePresentation& presentation, int width,
                                 int height, FrameMemoryDomain preferred_domain)
      -> FrameWriteMapping                                  = 0;
  /// The device write of the mapped target is ordered before any later read of the target.
  virtual void UnmapCoverageTarget()                        = 0;
  /// Hand the written target to the render thread. @pre UnmapCoverageTarget after a successful map.
  virtual auto PublishCoverage() -> MaskCoveragePublication = 0;
  /// Return a mapped or unmapped target without publishing it. No effect without a mapped target.
  virtual void AbandonCoverageTarget()                      = 0;
  /**
   * @brief Hide the displayed coverage and drop every published frame that the render thread has
   *        not displayed yet (SM8.7 ClearMaskCoveragePreview).
   *
   * A frame published before this call is never displayed after it; a frame published after it is
   * displayed normally. @pre No target is mapped. Thread: the producing render thread.
   */
  virtual void ClearCoverage()                              = 0;
#ifdef HAVE_METAL
  /// Hand a completed Metal coverage texture to the render thread.
  virtual auto SubmitMetalCoverage(const MaskCoveragePresentation& presentation,
                                   const MaskCoverageMetalFrame&   frame)
      -> MaskCoveragePublication = 0;
#endif
};

}  // namespace alcedo
