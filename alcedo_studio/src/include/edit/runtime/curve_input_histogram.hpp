//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "edit/graph/graph_ids.hpp"

/**
 * @file
 * @brief Histogram of the pixels at the input of the Curve adjustment of one Color Grade (TC3 of
 *        the Tone Curve editor plan, issue #888).
 *
 * The Curve adjustment runs per channel on the ACEScc values of the grade stage, after White
 * Balance, Exposure, Contrast, Whites, and Blacks (`ColorGradeCompileOrder`). The histogram counts
 * the red, green, and blue values at that point, on the axis of the curve: 256 bins on [0, 1].
 * The scope histogram cannot give this: it reads the final display frame, after the curve and the
 * display transform.
 *
 * The grade pass of the requested Color Grade evaluates the adjustments before the Curve on a
 * sampled grid of its input and adds the bins on the device (`GradeExecutor`). The renderer reads
 * the 768 counts after the render and gives them to the sink of the request. A render without a
 * request does no histogram work.
 */
namespace alcedo {

/// Bins of each channel on the curve axis [0, 1].
inline constexpr std::uint32_t kCurveInputHistogramBins         = 256;
/// Red, green, and blue.
inline constexpr std::uint32_t kCurveInputHistogramChannelCount = 3;
/// Counts of one histogram: the red bins, then the green bins, then the blue bins.
inline constexpr std::uint32_t kCurveInputHistogramValueCount =
    kCurveInputHistogramBins * kCurveInputHistogramChannelCount;
/// Longest sampled edge. A larger grade input is sampled every N pixels in x and y, with the
/// smallest N that keeps both sampled edges at or below this length.
inline constexpr std::uint32_t kCurveInputHistogramSampleLongEdge = 1024;

/// Distance in pixels between two samples, in x and in y, for a grade input of this size.
[[nodiscard]] constexpr auto   CurveInputHistogramSampleStep(std::uint32_t width,
                                                             std::uint32_t height) -> std::uint32_t {
  const auto long_edge = std::max(width, height);
  if (long_edge <= kCurveInputHistogramSampleLongEdge) {
    return 1;
  }
  return (long_edge + kCurveInputHistogramSampleLongEdge - 1) / kCurveInputHistogramSampleLongEdge;
}

/// Sampled columns (or rows) of an edge of @p length pixels: the pixels 0, step, 2 * step, ...
[[nodiscard]] constexpr auto CurveInputHistogramSampledLength(std::uint32_t length,
                                                              std::uint32_t step) -> std::uint32_t {
  return (length + step - 1) / step;
}

/// Pixels that the histogram of a grade input of this size counts in each channel.
[[nodiscard]] constexpr auto CurveInputHistogramSampleCount(std::uint32_t width,
                                                            std::uint32_t height) -> std::uint64_t {
  const auto step = CurveInputHistogramSampleStep(width, height);
  return static_cast<std::uint64_t>(CurveInputHistogramSampledLength(width, step)) *
         CurveInputHistogramSampledLength(height, step);
}

/**
 * @brief Bin of one ACEScc value: floor(value * 256), limited to [0, 255].
 *
 * A value below 0 counts in bin 0 and a value at or above 1 in bin 255. A value that is not a
 * number counts in bin 0 (fmax returns its other operand). The CUDA, OpenCL, and Metal kernels use
 * the same operations, so they give the same bin for the same float value.
 */
[[nodiscard]] inline auto CurveInputHistogramBin(float value) -> std::uint32_t {
  const float scaled = std::fmin(std::fmax(value * 256.0f, 0.0f), 255.0f);
  return static_cast<std::uint32_t>(scaled);
}

/**
 * @brief Counts of one render for the curve input of one Color Grade.
 *
 * The identity fields are the frame metadata of the render, so the receiver can drop a result of
 * an image that it no longer shows.
 */
struct CurveInputHistogram {
  NodeId                     grade_id;
  std::uint64_t              image_identity          = 0;
  std::uint64_t              session_epoch           = 0;
  std::uint64_t              presentation_request_id = 0;
  /// Pixels counted in each channel.
  std::uint64_t              sample_count            = 0;
  /// kCurveInputHistogramValueCount counts (red, green, blue). Empty when the render did not
  /// evaluate the Color Grade: the plan does not have it.
  std::vector<std::uint32_t> counts;

  [[nodiscard]] auto         Evaluated() const -> bool { return !counts.empty(); }
};

/**
 * @brief Receiver of the curve input histogram of each render that has a request.
 *
 * Called on the render thread after the photo frame of the render was presented. The
 * implementation must not block on the render thread; it moves the result to its own thread.
 */
class ICurveInputHistogramSink {
 public:
  virtual ~ICurveInputHistogramSink()                                    = default;
  virtual void PublishCurveInputHistogram(CurveInputHistogram histogram) = 0;
};

/**
 * @brief The Color Grade whose curve input an editor render also counts, and its receiver.
 *
 * Presentation state of one render request, as MaskCoverageProbe is. It is not part of the
 * document, the plan, or any result key, so it changes no photo pixel, cache entry, or history
 * entry. Only a render of the whole edit frame carries it: the histogram of a viewport region is
 * not the histogram of the image.
 */
struct CurveInputHistogramRequest {
  NodeId                                    grade_id;
  std::shared_ptr<ICurveInputHistogramSink> sink;

  auto operator==(const CurveInputHistogramRequest&) const -> bool = default;
};

/**
 * @brief What the grade pass of one render did for a CurveInputHistogramRequest.
 *
 * Created by the renderer with @ref grade_id set. The grade pass of that Color Grade records the
 * histogram into the workspace value CurveInputHistogramCountsId and sets @ref recorded. Owner:
 * the render call; lifetime: one render.
 */
struct CurveInputHistogramRecord {
  NodeId        grade_id;
  bool          recorded     = false;
  std::uint64_t sample_count = 0;
};

/// Workspace value that holds the device counts of Color Grade @p grade_id.
[[nodiscard]] inline auto CurveInputHistogramCountsId(const NodeId& grade_id) -> GraphValueId {
  return GraphValueId{grade_id, PortId{"runtime.curve_input_histogram"}};
}

/// Workspace value that holds the parameter offsets of the adjustments before the Curve.
[[nodiscard]] inline auto CurveInputHistogramOrderId(const NodeId& grade_id) -> GraphValueId {
  return GraphValueId{grade_id, PortId{"runtime.curve_input_order"}};
}

/**
 * @brief Read the counts that the grade pass recorded for @p grade_id.
 *
 * @tparam Device Render device with Workspace() and CommandContext().
 * @pre The render that recorded them was submitted. The backend download waits for its work.
 * @throws std::runtime_error when the workspace has no counts for @p grade_id, or from the
 *         backend download.
 */
template <class Device>
[[nodiscard]] auto ReadCurveInputHistogramCounts(Device& device, const NodeId& grade_id)
    -> std::vector<std::uint32_t> {
  auto&       workspace = device.Workspace();
  const auto* buffer    = workspace.Values().Find(CurveInputHistogramCountsId(grade_id));
  if (buffer == nullptr) {
    throw std::runtime_error("Curve input histogram: the render recorded no counts");
  }
  std::vector<std::uint32_t> counts(kCurveInputHistogramValueCount, 0U);
  workspace.Device().DownloadBufferRange(*buffer, 0, std::as_writable_bytes(std::span(counts)),
                                         device.CommandContext());
  return counts;
}

}  // namespace alcedo
