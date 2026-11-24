//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace alcedo {

/// The steps of a render that the editor shows (AI Denoise integration plan, DN14).
enum class RenderStep : std::uint8_t {
  /// The unpack of the encoded RAW file into the prepared source.
  ReadRaw = 0,
  /// The tile loop of the AI Denoise model.
  AiDenoise,
  /// The Develop passes of the compiled plan.
  Develop,
};

/// The groups of the Develop passes, in the order of PlanExecutor::Execute.
enum class DevelopGroup : std::uint8_t {
  /// Upload, linearization, demosaic, highlight recovery of the sensor input.
  Sensor = 0,
  /// Lens correction and the geometry resample.
  LensAndGeometry,
  /// The camera (or display) to working space conversion.
  Color,
  /// One Color Grade with its Masks.
  ColorGrade,
  /// The diffusion filter and the display rendering transform.
  Display,
};

/**
 * @brief The step of one render and its count (DN14). A value: it is copied through the queues
 *        of the render and of the editor session.
 *
 * `completed` and `total`: for AiDenoise the tiles that finished and the tiles of the run
 * (`total` is 0 until the tile loop planned its tiles); for Develop the 0-based index of the
 * group that runs and the number of groups of the plan; for ReadRaw both 0 (no count).
 */
struct RenderProgress {
  /// The render reads the RAW file (its source is not in the prepared source cache).
  bool         read_raw    = false;
  /// The render runs the model (the denoised source is not in the prepared source cache).
  bool         ai_denoise  = false;
  RenderStep   step        = RenderStep::Develop;
  int          completed   = 0;
  int          total       = 0;
  /// Develop only: the group that runs.
  DevelopGroup group       = DevelopGroup::Sensor;
  /// Develop with DevelopGroup::ColorGrade only: the 0-based Color Grade and their number.
  int          grade_index = 0;
  int          grade_count = 0;

  friend auto  operator==(const RenderProgress&, const RenderProgress&) -> bool = default;
};

/// Receives the progress of a render. Called on the render worker; it must not block.
using RenderProgressCallback = std::function<void(const RenderProgress&)>;

/**
 * @brief Sends the steps of one render to a RenderProgressCallback (DN14).
 *
 * The renderer creates one for a render that reads the RAW file or runs the model; a render that
 * reads only the caches has an inactive reporter and reports nothing. Each call sends one
 * complete value with the steps of the render.
 *
 * Thread: the render worker that owns the render. Not thread-safe.
 */
class RenderProgressReporter {
 public:
  RenderProgressReporter() = default;
  /// Active when @p callback is set and the render reads the RAW file or runs the model.
  RenderProgressReporter(RenderProgressCallback callback, bool read_raw, bool ai_denoise)
      : callback_(read_raw || ai_denoise ? std::move(callback) : RenderProgressCallback{}) {
    value_.read_raw   = read_raw;
    value_.ai_denoise = ai_denoise;
  }

  [[nodiscard]] auto Active() const -> bool { return static_cast<bool>(callback_); }

  /// The unpack of the RAW file starts.
  void               ReadRawStarted() { Send(RenderStep::ReadRaw, 0, 0); }

  /// @p completed of @p total tiles of the model run finished; (0, 0) when the run starts.
  void AiDenoiseTiles(int completed, int total) { Send(RenderStep::AiDenoise, completed, total); }

  /**
   * @brief The Develop group @p index of @p count starts.
   * @param grade_index, grade_count The Color Grade of a DevelopGroup::ColorGrade group.
   */
  void DevelopGroupStarted(DevelopGroup group, int index, int count, int grade_index = 0,
                           int grade_count = 0) {
    if (!Active()) {
      return;
    }
    value_.group       = group;
    value_.grade_index = grade_index;
    value_.grade_count = grade_count;
    Send(RenderStep::Develop, index, count);
  }

 private:
  void Send(RenderStep step, int completed, int total) {
    if (!Active()) {
      return;
    }
    value_.step      = step;
    value_.completed = completed;
    value_.total     = total;
    if (step != RenderStep::Develop) {
      value_.group       = DevelopGroup::Sensor;
      value_.grade_index = 0;
      value_.grade_count = 0;
    }
    callback_(value_);
  }

  RenderProgressCallback callback_;
  RenderProgress         value_;
};

/// The number of Develop groups of a plan with @p grade_count Color Grades: Sensor, Lens and
/// geometry, Color, one for each Color Grade, Display.
[[nodiscard]] constexpr auto DevelopGroupCount(int grade_count) -> int { return 4 + grade_count; }

}  // namespace alcedo
