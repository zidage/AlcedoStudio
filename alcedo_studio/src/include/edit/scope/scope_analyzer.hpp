//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

enum class GpuBackend : uint32_t {
  None   = 0,
  Cuda   = 1,
  Metal  = 2,
  OpenCL = 3,
};

enum class AnalysisDomain : uint32_t {
  DisplayEncoded = 0,
  LinearLight    = 1,
};

struct SharedGpuImageHandle {
  GpuBackend            backend       = GpuBackend::None;
  std::shared_ptr<void> resource      = {};
  int                   width         = 0;
  int                   height        = 0;
  size_t                row_bytes     = 0;
  FramePixelFormat      format        = FramePixelFormat::RGBA32F;
  FrameWriteTargetType  resource_type = FrameWriteTargetType::LinearBuffer;

  explicit operator bool() const { return resource != nullptr && width > 0 && height > 0; }
};

struct SharedGpuBufferHandle {
  GpuBackend            backend    = GpuBackend::None;
  std::shared_ptr<void> resource   = {};
  size_t                size_bytes = 0;

  explicit              operator bool() const { return resource != nullptr && size_bytes > 0; }
};

struct GpuSignalHandle {
  GpuBackend            backend  = GpuBackend::None;
  std::shared_ptr<void> resource = {};

  explicit              operator bool() const { return resource != nullptr; }
};

struct FinalDisplayFrameView {
  SharedGpuImageHandle image              = {};
  int                  width              = 0;
  int                  height             = 0;
  FramePixelFormat     format             = FramePixelFormat::RGBA32F;
  ViewerDisplayConfig  display_config     = {};
  AnalysisDomain       domain             = AnalysisDomain::DisplayEncoded;
  GpuSignalHandle      ready_signal       = {};
  uint64_t             frame_id           = 0;
  // Identity of the image/session that owns the GPU image.
  uint64_t             image_identity     = 0;
  uint64_t             session_epoch      = 0;
  // Render generation of the final display frame, distinct from the analyzer
  // output generation below.
  uint64_t             display_generation = 0;

  explicit             operator bool() const { return image && width > 0 && height > 0; }
};

enum class ScopeType : uint32_t {
  Histogram    = 1u << 0,
  Waveform     = 1u << 1,
  Vectorscope  = 1u << 2,
  Chromaticity = 1u << 3,
};

struct ScopeRequest {
  uint32_t enabled_mask =
      static_cast<uint32_t>(ScopeType::Histogram) | static_cast<uint32_t>(ScopeType::Waveform);
  int histogram_bins      = 256;
  int waveform_width      = 384;
  int waveform_height     = 192;
  int vectorscope_size    = 256;
  int chromaticity_size   = 256;
  int analysis_downsample = 4;
  int target_fps          = 20;
};

struct ScopeOutputSet {
  SharedGpuBufferHandle histogram_buffer   = {};
  SharedGpuImageHandle  waveform_image     = {};
  SharedGpuImageHandle  vectorscope_image  = {};
  SharedGpuImageHandle  chromaticity_image = {};

  int                   histogram_bins     = 0;
  int                   waveform_width     = 0;
  int                   waveform_height    = 0;
  int                   vectorscope_size   = 0;
  int                   chromaticity_size  = 0;

  bool                  histogram_valid    = false;
  bool                  waveform_valid     = false;
  bool                  vectorscope_valid  = false;
  bool                  chromaticity_valid = false;
  uint64_t              generation         = 0;
  uint64_t              image_identity     = 0;
  uint64_t              session_epoch      = 0;
  uint64_t              display_generation = 0;
};

/// Display heights of an RGB histogram. `rgb` holds `bins` heights per channel (R, G, B), each a
/// square-root height in [0, 1] against the display reference of BuildScopeHistogramRenderData.
/// A bin whose count is above the reference shows at 1 and is marked in `peak_cut`.
struct ScopeHistogramRenderData {
  int                  bins                   = 0;
  int                  clip_tail_bins         = 0;
  float                shadow_clip_ratio      = 0.0f;
  float                highlight_clip_ratio   = 0.0f;
  bool                 shadow_clip_warning    = false;
  bool                 highlight_clip_warning = false;
  std::vector<float>   rgb                    = {};
  // One entry per value of `rgb`: 1 when the count is above the display reference.
  std::vector<uint8_t> peak_cut               = {};
  bool                 valid                  = false;
};

struct ScopeWaveformRenderData {
  int                width  = 0;
  int                height = 0;
  std::vector<float> rgba   = {};
  bool               valid  = false;
};

struct ScopeRenderSnapshot {
  ScopeHistogramRenderData histogram          = {};
  ScopeWaveformRenderData  waveform           = {};
  uint64_t                 generation         = 0;
  uint64_t                 image_identity     = 0;
  uint64_t                 session_epoch      = 0;
  uint64_t                 display_generation = 0;
};

class IScopeAnalyzer {
 public:
  virtual ~IScopeAnalyzer() = default;

  virtual void SubmitFrame(const FinalDisplayFrameView& frame, const ScopeRequest& request) = 0;

  virtual auto GetLatestOutput() -> ScopeOutputSet                                          = 0;

  virtual void ResizeResources(const ScopeRequest& request)                                 = 0;

  virtual void ReleaseResources()                                                           = 0;

  /// Synchronously stage a stable, analyzer-owned copy of the final display
  /// frame's GPU input on the render thread, while the pipeline source is
  /// still valid. Returns a FinalDisplayFrameView backed by analyzer-owned
  /// memory plus a readiness signal the analyzer's own execution stream can
  /// wait on; the deferred SubmitFrame later analyzes that staged frame
  /// without touching the pipeline's reused scratch buffers or stream.
  /// Backends whose source is already stable (or tests that do not need
  /// staging) keep the default, which returns the frame unchanged.
  virtual auto StageFrame(const FinalDisplayFrameView& frame, const ScopeRequest& request)
      -> FinalDisplayFrameView {
    (void)request;
    return frame;
  }
};

class IFinalDisplayFrameProvider {
 public:
  virtual ~IFinalDisplayFrameProvider()                                    = default;

  virtual auto GetCurrentDisplayFrameView() const -> FinalDisplayFrameView = 0;
};

class CudaScopeAnalyzer;
class MetalScopeAnalyzer;
class OpenClScopeAnalyzer;

auto CreateCudaScopeAnalyzer() -> std::shared_ptr<IScopeAnalyzer>;
auto CreateMetalScopeAnalyzer() -> std::shared_ptr<IScopeAnalyzer>;
auto CreateOpenClScopeAnalyzer() -> std::shared_ptr<IScopeAnalyzer>;
auto CreateDefaultScopeAnalyzer() -> std::shared_ptr<IScopeAnalyzer>;

auto ReadScopeRenderSnapshot(const ScopeOutputSet& output) -> ScopeRenderSnapshot;

/// Converts R, G, B bin counts (`bins` values per channel) into display heights.
///
/// The display reference is the largest count, limited to kHistogramPeakReferenceLimit times the
/// kHistogramPeakReferencePercentile count of the populated bins. So a narrow peak, for example a
/// sky in a few bins, cannot set the scale: it shows at full height with a cut mark, and the other
/// bins keep a readable height. The height is the square root of count / reference, which shows
/// sparse tones like the waveform density does. Empty bins stay at 0. A clipped tail that raised
/// a clip warning is not used for the reference.
auto BuildScopeHistogramRenderData(const std::vector<uint32_t>& counts, int bins)
    -> ScopeHistogramRenderData;

inline constexpr float kHistogramPeakReferencePercentile = 0.9f;
inline constexpr float kHistogramPeakReferenceLimit      = 4.0f;

}  // namespace alcedo
