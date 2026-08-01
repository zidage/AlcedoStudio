//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "edit/frame_presentation_types.hpp"
#include "edit/operators/utils/color_utils.hpp"

namespace alcedo {
class IEditViewerSurface;
struct FinalDisplayFrameView;

struct ViewerDisplayConfig {
  ColorUtils::ColorSpace encoding_space = ColorUtils::ColorSpace::REC709;
  ColorUtils::EOTF       encoding_eotf  = ColorUtils::EOTF::GAMMA_2_2;
  float                  peak_luminance = 100.0f;

  auto                   operator==(const ViewerDisplayConfig& other) const -> bool = default;
};

// ViewportRenderRegion and FrameRole are defined in edit/frame_presentation_types.hpp
// so app-layer services can use them without including UI headers.

struct FrameRoiRect {
  float x      = 0.0f;
  float y      = 0.0f;
  float width  = 1.0f;
  float height = 1.0f;
};

struct FramePreviewMetadata {
  FrameRole     frame_role              = FrameRole::InteractivePrimary;
  std::uint64_t preview_generation      = 0;
  std::uint64_t detail_serial           = 0;
  // App-layer render request that produced this frame. Zero is reserved for
  // legacy/non-session producers. The production RHI path returns this exact
  // identity after the frame is sampled by a render pass.
  std::uint64_t presentation_request_id = 0;
  FrameRoiRect  source_roi_norm         = {};
  // Session/image identity copied into final-display scope frames. Keeping
  // these beside the render generation lets an analyzer reject a late frame
  // even when the same durable image is opened again.
  std::uint64_t image_generation        = 0;
  std::uint64_t image_identity          = 0;
  // View-only renders (zoom/pan, resize, and detail ROI refreshes) must not
  // replace the last content frame used by scope analysis.
  bool          scope_update_allowed    = true;
  // A scope-mode switch explicitly requested this frame. An ROI produced for
  // that request is valid scope input; incidental zoom ROI frames are not.
  bool          scope_refresh_requested = false;
};

enum class FramePresentationMode {
  FullFrame,
  ViewportTransformed = FullFrame,
  RoiFrame,
};

struct FrameCompletionSubmission {
  FramePreviewMetadata  metadata{};
  FramePresentationMode mode = FramePresentationMode::FullFrame;
};

enum class FramePixelFormat {
  RGBA32F,
};

enum class FrameMemoryDomain {
  HostVisible,
  CudaDevice,
  OpenClDevice,
};

enum class FrameWriteTargetType {
  LinearBuffer,
  CudaArray,
  OpenClImage,
};

struct FrameWriteMapping {
  void*                data                  = nullptr;
  void*                image_array           = nullptr;
  size_t               row_bytes             = 0;
  FramePixelFormat     pixel_format          = FramePixelFormat::RGBA32F;
  FrameMemoryDomain    memory_domain         = FrameMemoryDomain::HostVisible;
  FrameWriteTargetType target_type           = FrameWriteTargetType::LinearBuffer;
  std::uintptr_t       native_object         = 0;
  void*                cuda_signal_semaphore = nullptr;
  std::uint64_t        cuda_signal_value     = 0;

  explicit             operator bool() const { return data != nullptr || image_array != nullptr; }
};

struct ViewerFrame {
  int                         width     = 0;
  int                         height    = 0;
  size_t                      row_bytes = 0;
  std::shared_ptr<const void> pixels{};
  ViewerDisplayConfig         display_config{};
  FramePresentationMode       presentation_mode = FramePresentationMode::FullFrame;
  FramePreviewMetadata        preview_metadata  = {};

  explicit                    operator bool() const {
    return width > 0 && height > 0 && row_bytes > 0 && pixels != nullptr;
  }
};

struct ViewerGpuFrameUpload {
  int                         width     = 0;
  int                         height    = 0;
  size_t                      row_bytes = 0;
  std::shared_ptr<const void> pixels{};
  ViewerDisplayConfig         display_config{};
  FramePresentationMode       presentation_mode = FramePresentationMode::FullFrame;
  FramePreviewMetadata        preview_metadata  = {};

  explicit                    operator bool() const {
    return width > 0 && height > 0 && row_bytes > 0 && pixels != nullptr;
  }
};

#ifdef HAVE_METAL
struct ViewerMetalFrame {
  int                         width          = 0;
  int                         height         = 0;
  std::uintptr_t              texture_handle = 0;
  std::shared_ptr<const void> owner{};
  ViewerDisplayConfig         display_config{};
  FramePresentationMode       presentation_mode = FramePresentationMode::FullFrame;
  FramePreviewMetadata        preview_metadata  = {};

  explicit                    operator bool() const {
    return width > 0 && height > 0 && texture_handle != 0 && owner != nullptr;
  }
};
#endif

class IFrameSink {
 public:
  virtual ~IFrameSink() {}

  virtual void EnsureSize(int width, int height) = 0;

  virtual auto MapResourceForWrite(
      FrameMemoryDomain preferred_domain = FrameMemoryDomain::CudaDevice) -> FrameWriteMapping = 0;

  virtual void UnmapResource()                                                                 = 0;

  virtual void NotifyFrameReady(const FrameCompletionSubmission& submission)                     = 0;

  // Binds the submission stamped for the in-flight Apply(). Production sinks
  // use this for EnsureSize/render-reference decisions before NotifyFrameReady.
  virtual void BindFrameSubmission(const FrameCompletionSubmission& submission) {
    (void)submission;
  }

  virtual void SubmitHostFrame(const ViewerFrame&) {}
#ifdef HAVE_METAL
  virtual void SubmitMetalFrame(const ViewerMetalFrame&) {}
#endif
  virtual void SubmitFinalDisplayFrame(const FinalDisplayFrameView&) {}

  // Get the size of the frame
  virtual int  GetWidth() const  = 0;
  virtual int  GetHeight() const = 0;

  // Returns ROI parameters derived from the current viewer transform (if any).
  virtual auto GetViewportRenderRegion() const -> std::optional<ViewportRenderRegion> {
    return std::nullopt;
  }

  // Exposes the presentation surface when a sink is backed by a live viewer.
  virtual auto GetViewerSurface() -> IEditViewerSurface* { return nullptr; }
  virtual auto GetViewerSurface() const -> const IEditViewerSurface* { return nullptr; }
};
}  // namespace alcedo
