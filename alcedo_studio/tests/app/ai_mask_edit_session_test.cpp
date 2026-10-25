//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SM5.2: the AI Mask edit session over a fake segmentation runner, and the host composition of
// the coverage.

#include "app/ai_mask_edit_session.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <opencv2/core/mat.hpp>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "app/ai_mask_coverage_composition.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kLogitSize = 256;
constexpr Extent2D      kSessionExtent{64, 48};
constexpr const char*   kModelSetId   = "sam2.1-tiny+people-sky-head-p1r2e15+s3od-synth";
constexpr const char*   kFingerprint  = "0123456789abcdef0123456789abcdef";
constexpr const char*   kFingerprint2 = "fedcba9876543210fedcba9876543210";

auto                    Ok() -> SegmentationCallStatus {
  SegmentationCallStatus status;
  status.ok = true;
  return status;
}

/// Logits of +8 inside the normalized rectangle and -8 outside.
auto RectLogits(float x0, float y0, float x1, float y1) -> std::vector<float> {
  std::vector<float> logits(static_cast<std::size_t>(kLogitSize) * kLogitSize, -8.0f);
  for (std::uint32_t row = 0; row < kLogitSize; ++row) {
    for (std::uint32_t column = 0; column < kLogitSize; ++column) {
      const float x = (static_cast<float>(column) + 0.5f) / kLogitSize;
      const float y = (static_cast<float>(row) + 0.5f) / kLogitSize;
      if (x >= x0 && x < x1 && y >= y0 && y < y1) {
        logits[static_cast<std::size_t>(row) * kLogitSize + column] = 8.0f;
      }
    }
  }
  return logits;
}

auto AlphaResult(std::uint32_t width, std::uint32_t height, float value,
                 SegmentationBox box = {0.0f, 0.0f, 1.0f, 1.0f}) -> SegmentationTaskResult {
  SegmentationTaskResult result;
  result.status      = Ok();
  result.found       = true;
  result.mask_kind   = SegmentationTaskMaskKind::kAlpha;
  result.mask_width  = width;
  result.mask_height = height;
  result.alpha_values.assign(static_cast<std::size_t>(width) * height, value);
  result.mask_box = box;
  return result;
}

auto NotFoundResult(std::string message) -> SegmentationTaskResult {
  SegmentationTaskResult result;
  result.status  = Ok();
  result.found   = false;
  result.message = std::move(message);
  return result;
}

auto Person(std::uint32_t index, SegmentationBox box, bool in_subject) -> SegmentationPerson {
  SegmentationPerson person;
  person.index      = index;
  person.box        = box;
  person.score      = 0.9f;
  person.in_subject = in_subject;
  return person;
}

/**
 * Answers decodes with rectangle logits and tasks with configured results; records every call.
 *
 * A click (one point, no box) returns three levels: squares of half size 0.05, 0.1, and 0.2
 * around the point, selected index 1. Another prompt returns one mask: the bounding rectangle of
 * the Add points and the box, widened by 0.02.
 */
class FakeRunner final : public IAiMaskSegmentationRunner {
 public:
  auto Decode(const SegmentationPrompt& prompt) -> SegmentationDecodeResult override {
    prompts.push_back(prompt);
    ++calls;
    SegmentationDecodeResult result;
    if (decode_failure.has_value()) {
      result.status.ok    = false;
      result.status.error = *decode_failure;
      return result;
    }
    result.status     = Ok();
    result.logit_size = kLogitSize;
    if (prompt.points.size() == 1 && !prompt.box.has_value()) {
      const auto& point = prompt.points.front();
      for (const float half : {0.05f, 0.1f, 0.2f}) {
        SegmentationMaskLevel level;
        level.logits = RectLogits(point.x - half, point.y - half, point.x + half, point.y + half);
        result.masks.push_back(std::move(level));
      }
      result.selected_index = 1;
      return result;
    }
    float x0 = 1.0f, y0 = 1.0f, x1 = 0.0f, y1 = 0.0f;
    for (const auto& point : prompt.points) {
      if (point.label == SegmentationPointLabel::kAdd) {
        x0 = std::min(x0, point.x);
        y0 = std::min(y0, point.y);
        x1 = std::max(x1, point.x);
        y1 = std::max(y1, point.y);
      }
    }
    if (prompt.box.has_value()) {
      x0 = std::min(x0, prompt.box->x0);
      y0 = std::min(y0, prompt.box->y0);
      x1 = std::max(x1, prompt.box->x1);
      y1 = std::max(y1, prompt.box->y1);
    }
    SegmentationMaskLevel level;
    level.logits = RectLogits(x0 - 0.02f, y0 - 0.02f, x1 + 0.02f, y1 + 0.02f);
    result.masks.push_back(std::move(level));
    return result;
  }

  auto RunTask(SegmentationTask task, std::optional<uint32_t> person_index)
      -> SegmentationTaskResult override {
    task_calls.emplace_back(task, person_index);
    ++calls;
    if (task == SegmentationTask::kPerson) {
      const auto found = persons.find(person_index.value_or(999));
      if (found == persons.end()) {
        SegmentationTaskResult result;
        result.status.error = "person index outside the People list";
        return result;
      }
      return found->second;
    }
    const auto found = tasks.find(task);
    if (found == tasks.end()) {
      SegmentationTaskResult result;
      result.status.error = "task failed";
      return result;
    }
    return found->second;
  }

  int                                                               calls = 0;
  std::vector<SegmentationPrompt>                                   prompts;
  std::vector<std::pair<SegmentationTask, std::optional<uint32_t>>> task_calls;
  std::map<SegmentationTask, SegmentationTaskResult>                tasks;
  std::map<uint32_t, SegmentationTaskResult>                        persons;
  std::optional<std::string>                                        decode_failure;
};

auto SessionImage(std::string fingerprint = kFingerprint) -> AiMaskSessionImage {
  return AiMaskSessionImage{kSessionExtent, kModelSetId, std::move(fingerprint)};
}

auto Stroke(std::vector<std::pair<float, float>> points) -> AiMaskPromptStep {
  AiMaskPromptStep step;
  for (const auto& [x, y] : points) {
    step.points.push_back(AiMaskPromptPoint{x, y, AiMaskComponentOperation::Add});
  }
  return step;
}

auto Click(float x, float y) -> AiMaskPromptStep { return Stroke({{x, y}}); }

auto At(std::span<const std::uint8_t> coverage, std::uint32_t x, std::uint32_t y) -> std::uint8_t {
  return coverage[static_cast<std::size_t>(y) * kSessionExtent.width + x];
}

auto CoverageCopy(const AiMaskEditSession& session) -> std::vector<std::uint8_t> {
  return {session.Coverage().begin(), session.Coverage().end()};
}

auto Applied(const AiMaskStepResult& result) -> bool {
  return result.status == AiMaskStepStatus::Applied;
}

// ---------------------------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------------------------

TEST(AiMaskEditSessionTest, LogitsConvertToSessionSizeWithThresholdZero) {
  constexpr Extent2D kExtent{1024, 768};
  const auto         quadrant = RectLogits(0.0f, 0.0f, 0.5f, 0.5f);
  const auto         coverage =
      ConvertModelOutputToCoverage(AiMaskSamLogits{kLogitSize, quadrant}, kExtent);
  ASSERT_EQ(coverage.size(), static_cast<std::size_t>(1024) * 768);
  // Independent reference: bilinear sampling with half-pixel centers, edge values repeated.
  const auto sample = [&quadrant](double gx, double gy) {
    const auto clamp_index = [](long value) {
      return static_cast<std::size_t>(std::clamp<long>(value, 0, kLogitSize - 1));
    };
    const long   x0 = static_cast<long>(std::floor(gx));
    const long   y0 = static_cast<long>(std::floor(gy));
    const double fx = gx - static_cast<double>(x0);
    const double fy = gy - static_cast<double>(y0);
    const auto   at = [&](long x, long y) {
      return static_cast<double>(quadrant[clamp_index(y) * kLogitSize + clamp_index(x)]);
    };
    return (1 - fy) * ((1 - fx) * at(x0, y0) + fx * at(x0 + 1, y0)) +
           fy * ((1 - fx) * at(x0, y0 + 1) + fx * at(x0 + 1, y0 + 1));
  };
  std::size_t mismatches = 0;
  for (std::uint32_t y = 0; y < kExtent.height; ++y) {
    for (std::uint32_t x = 0; x < kExtent.width; ++x) {
      const double       value    = sample((x + 0.5) * kLogitSize / kExtent.width - 0.5,
                                           (y + 0.5) * kLogitSize / kExtent.height - 0.5);
      const std::uint8_t expected = value > 0.0 ? 255 : 0;
      mismatches += coverage[static_cast<std::size_t>(y) * kExtent.width + x] != expected;
    }
  }
  EXPECT_EQ(mismatches, 0U);
  // The positive quadrant of the grid is the top-left quadrant of the coverage.
  EXPECT_EQ(coverage[static_cast<std::size_t>(383) * 1024 + 510], 255);
  EXPECT_EQ(coverage[static_cast<std::size_t>(382) * 1024 + 511], 255);
  EXPECT_EQ(coverage[static_cast<std::size_t>(383) * 1024 + 512], 0);
  EXPECT_EQ(coverage[static_cast<std::size_t>(384) * 1024 + 100], 0);

  // A smooth grid with many sign changes: the values equal cv::resize then > 0.
  std::vector<float> wave(static_cast<std::size_t>(kLogitSize) * kLogitSize);
  for (std::uint32_t row = 0; row < kLogitSize; ++row) {
    for (std::uint32_t column = 0; column < kLogitSize; ++column) {
      wave[static_cast<std::size_t>(row) * kLogitSize + column] =
          std::sin(static_cast<float>(column) * 0.11f) + std::cos(static_cast<float>(row) * 0.07f) -
          0.3f;
    }
  }
  const auto converted = ConvertModelOutputToCoverage(AiMaskSamLogits{kLogitSize, wave}, kExtent);
  cv::Mat    reference;
  cv::resize(cv::Mat(kLogitSize, kLogitSize, CV_32F, wave.data()), reference, cv::Size(1024, 768),
             0.0, 0.0, cv::INTER_LINEAR);
  mismatches         = 0;
  std::size_t inside = 0;
  for (int y = 0; y < reference.rows; ++y) {
    for (int x = 0; x < reference.cols; ++x) {
      const std::uint8_t expected = reference.at<float>(y, x) > 0.0f ? 255 : 0;
      inside += expected == 255;
      mismatches += converted[static_cast<std::size_t>(y) * 1024 + x] != expected;
    }
  }
  EXPECT_EQ(mismatches, 0U);
  EXPECT_GT(inside, 0U);
  EXPECT_LT(inside, converted.size());
}

TEST(AiMaskEditSessionTest, AlphaGridPastesThroughMaskBox) {
  constexpr Extent2D kExtent{1024, 768};
  // The 32 x 32 grid fills 256 x 192 pixels: gain 8, so 0.5625 gives (0.0625 * 8 + 0.5) = 1.
  AiMaskAlphaGrid person{32, 32, std::vector<float>(32 * 32, 0.5625f), {0.5f, 0.5f, 0.75f, 0.75f}};
  const auto      coverage   = ConvertModelOutputToCoverage(person, kExtent);
  const auto      expected   = std::uint8_t{255};
  std::size_t     mismatches = 0;
  for (std::uint32_t y = 0; y < kExtent.height; ++y) {
    for (std::uint32_t x = 0; x < kExtent.width; ++x) {
      const bool inside = x >= 512 && x < 768 && y >= 384 && y < 576;
      mismatches +=
          coverage[static_cast<std::size_t>(y) * kExtent.width + x] != (inside ? expected : 0);
    }
  }
  EXPECT_EQ(mismatches, 0U);

  // Alpha values outside [0, 1] clamp to 255 and 0.
  AiMaskAlphaGrid over{4, 4, std::vector<float>(16, 1.4f), {0.0f, 0.0f, 1.0f, 1.0f}};
  AiMaskAlphaGrid under{4, 4, std::vector<float>(16, -0.3f), {0.0f, 0.0f, 1.0f, 1.0f}};
  const auto      high = ConvertModelOutputToCoverage(over, kSessionExtent);
  const auto      low  = ConvertModelOutputToCoverage(under, kSessionExtent);
  EXPECT_TRUE(std::all_of(high.begin(), high.end(), [](std::uint8_t v) { return v == 255; }));
  EXPECT_TRUE(std::all_of(low.begin(), low.end(), [](std::uint8_t v) { return v == 0; }));

  // A grid at the session scale has gain 1: the values stay round(alpha * 255).
  AiMaskAlphaGrid same{
      kSessionExtent.width,
      kSessionExtent.height,
      std::vector<float>(static_cast<std::size_t>(kSessionExtent.width) * kSessionExtent.height,
                         0.6f),
      {0.0f, 0.0f, 1.0f, 1.0f}};
  const auto kept = ConvertModelOutputToCoverage(same, kSessionExtent);
  EXPECT_TRUE(std::all_of(kept.begin(), kept.end(), [](std::uint8_t v) { return v == 153; }));
}

TEST(AiMaskEditSessionTest, AlphaGridEdgeHasNoGridSteps) {
  // A binary grid of a straight edge, upscaled 4 times like an S3OD alpha on a 4096 analysis image.
  // The bilinear upscale alone keeps the grid steps: 0.75 px (slope 1/2) and 1.38 px (slope 1/4)
  // off the line. The coverage edge must stay near the line (#522).
  constexpr std::uint32_t        kGrid  = 64;
  constexpr std::uint32_t        kScale = 4;
  constexpr Extent2D             kExtent{kGrid * kScale, kGrid * kScale};
  const std::pair<float, double> cases[] = {{0.5f, 0.5}, {0.25f, 1.0}};
  for (const auto& [slope, tolerance] : cases) {
    std::vector<float> values(static_cast<std::size_t>(kGrid) * kGrid);
    for (std::uint32_t y = 0; y < kGrid; ++y) {
      for (std::uint32_t x = 0; x < kGrid; ++x) {
        const bool inside =
            static_cast<float>(y) + 0.5f > 8.0f + slope * (static_cast<float>(x) + 0.5f);
        values[static_cast<std::size_t>(y) * kGrid + x] = inside ? 1.0f : 0.0f;
      }
    }
    const auto coverage = ConvertModelOutputToCoverage(
        AiMaskAlphaGrid{kGrid, kGrid, values, {0.0f, 0.0f, 1.0f, 1.0f}}, kExtent);
    // The edge row of each column from its covered area, away from the left and right borders.
    double worst = 0.0;
    for (std::uint32_t x = 16; x + 16 < kExtent.width; ++x) {
      double covered = 0.0;
      for (std::uint32_t y = 0; y < kExtent.height; ++y) {
        covered += coverage[static_cast<std::size_t>(y) * kExtent.width + x] / 255.0;
      }
      const double edge  = static_cast<double>(kExtent.height) - covered;
      const double ideal = 8.0 * kScale + slope * (static_cast<double>(x) + 0.5);
      worst              = std::max(worst, std::abs(edge - ideal));
    }
    EXPECT_LT(worst, tolerance) << "slope " << slope;
  }
}

TEST(AiMaskEditSessionTest, FoldAddsWithMaximumAndSubtractsWithInverse) {
  const std::vector<std::uint8_t> start{0, 100, 200, 255};
  const std::vector<std::uint8_t> component{50, 50, 250, 0};

  auto                            added = start;
  FoldAiMaskCoverage(added, component, AiMaskComponentOperation::Add);
  EXPECT_EQ(added, (std::vector<std::uint8_t>{50, 100, 250, 255}));

  auto subtracted = start;
  FoldAiMaskCoverage(subtracted, component, AiMaskComponentOperation::Subtract);
  EXPECT_EQ(subtracted, (std::vector<std::uint8_t>{0, 100, 5, 255}));

  auto wrong_size = start;
  EXPECT_THROW(FoldAiMaskCoverage(wrong_size, std::vector<std::uint8_t>{1, 2},
                                  AiMaskComponentOperation::Add),
               std::invalid_argument);
  EXPECT_EQ(wrong_size, start);
}

// ---------------------------------------------------------------------------------------------
// Prompt steps
// ---------------------------------------------------------------------------------------------

TEST(AiMaskEditSessionTest, PromptStepsChainMaskInputInsideAGroup) {
  FakeRunner        runner;
  AiMaskEditSession session(runner, SessionImage());

  ASSERT_TRUE(Applied(session.AddPromptStep(Stroke({{0.2f, 0.2f}, {0.3f, 0.25f}, {0.4f, 0.3f}}),
                                            AiMaskComponentOperation::Add)));
  ASSERT_TRUE(Applied(session.AddPromptStep(Stroke({{0.5f, 0.5f}, {0.6f, 0.55f}}),
                                            AiMaskComponentOperation::Subtract)));

  ASSERT_EQ(runner.prompts.size(), 2U);
  EXPECT_TRUE(runner.prompts[0].mask_input.empty());
  ASSERT_EQ(runner.prompts[1].points.size(), 5U);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(runner.prompts[1].points[i].label, SegmentationPointLabel::kAdd);
  }
  // Inside an open group the Subtract tool mode is the decoder label of the new points.
  EXPECT_EQ(runner.prompts[1].points[3].label, SegmentationPointLabel::kSubtract);
  EXPECT_EQ(runner.prompts[1].points[4].label, SegmentationPointLabel::kSubtract);
  EXPECT_FLOAT_EQ(runner.prompts[1].points[3].x, 0.5f);
  // The second decode takes the selected logits of the first as mask_input.
  EXPECT_EQ(runner.prompts[1].mask_input,
            RectLogits(0.2f - 0.02f, 0.2f - 0.02f, 0.4f + 0.02f, 0.3f + 0.02f));

  ASSERT_EQ(session.Recipe().components.size(), 1U);
  const auto& group = std::get<AiMaskPromptGroup>(session.Recipe().components[0].content);
  ASSERT_EQ(group.steps.size(), 2U);
  EXPECT_EQ(session.Recipe().components[0].operation, AiMaskComponentOperation::Add);
  EXPECT_EQ(group.steps[1].points[0].label, AiMaskComponentOperation::Subtract);
  // The coverage is the last decode: the rectangle of the three Add points.
  EXPECT_EQ(At(session.Coverage(), 19, 12), 255);  // (0.30, 0.26)
  EXPECT_EQ(At(session.Coverage(), 38, 26), 0);    // (0.60, 0.55): Subtract points
}

TEST(AiMaskEditSessionTest, SubtractStrokeAfterTaskStartsSubtractGroup) {
  FakeRunner runner;
  runner.tasks[SegmentationTask::kSubject] = AlphaResult(16, 12, 1.0f);
  AiMaskEditSession session(runner, SessionImage());

  ASSERT_TRUE(Applied(session.AddTask(AiMaskTaskKind::Subject, AiMaskComponentOperation::Add)));
  EXPECT_EQ(At(session.Coverage(), 20, 20), 255);
  ASSERT_TRUE(Applied(session.AddPromptStep(Stroke({{0.2f, 0.2f}, {0.4f, 0.4f}}),
                                            AiMaskComponentOperation::Subtract)));

  ASSERT_EQ(session.Recipe().components.size(), 2U);
  const auto& component = session.Recipe().components[1];
  EXPECT_EQ(component.operation, AiMaskComponentOperation::Subtract);
  const auto& group = std::get<AiMaskPromptGroup>(component.content);
  EXPECT_EQ(group.steps[0].points[0].label, AiMaskComponentOperation::Add);
  ASSERT_EQ(runner.prompts.size(), 1U);
  EXPECT_EQ(runner.prompts[0].points[0].label, SegmentationPointLabel::kAdd);
  EXPECT_TRUE(runner.prompts[0].mask_input.empty());
  // The stroke region (0.18 to 0.42) is removed from the Subject; the rest stays.
  EXPECT_EQ(At(session.Coverage(), 19, 14), 0);  // (0.30, 0.30)
  EXPECT_EQ(At(session.Coverage(), 50, 40), 255);
  EXPECT_EQ(At(session.Coverage(), 5, 3), 255);
}

TEST(AiMaskEditSessionTest, UndoAndRedoNeedNoModelCall) {
  FakeRunner runner;
  runner.tasks[SegmentationTask::kSubject] = AlphaResult(16, 12, 0.5f);
  AiMaskEditSession session(runner, SessionImage());
  const auto        empty = CoverageCopy(session);

  ASSERT_TRUE(Applied(session.AddTask(AiMaskTaskKind::Subject, AiMaskComponentOperation::Add)));
  const auto subject = CoverageCopy(session);
  ASSERT_TRUE(Applied(
      session.AddPromptStep(Stroke({{0.1f, 0.1f}, {0.3f, 0.3f}}), AiMaskComponentOperation::Add)));
  const auto first_stroke = CoverageCopy(session);
  ASSERT_TRUE(Applied(
      session.AddPromptStep(Stroke({{0.6f, 0.6f}, {0.8f, 0.8f}}), AiMaskComponentOperation::Add)));
  const auto second_stroke = CoverageCopy(session);
  ASSERT_EQ(runner.calls, 3);
  ASSERT_NE(subject, first_stroke);
  ASSERT_NE(first_stroke, second_stroke);

  ASSERT_TRUE(session.Undo());
  EXPECT_EQ(CoverageCopy(session), first_stroke);
  ASSERT_TRUE(session.Undo());
  EXPECT_EQ(CoverageCopy(session), subject);
  EXPECT_EQ(session.Recipe().components.size(), 1U);
  ASSERT_TRUE(session.Redo());
  EXPECT_EQ(CoverageCopy(session), first_stroke);
  ASSERT_TRUE(session.Redo());
  EXPECT_EQ(CoverageCopy(session), second_stroke);
  EXPECT_FALSE(session.Redo());
  EXPECT_EQ(runner.calls, 3);

  ASSERT_TRUE(session.Undo());
  ASSERT_TRUE(session.Undo());
  ASSERT_TRUE(session.Undo());
  EXPECT_FALSE(session.Undo());
  EXPECT_EQ(CoverageCopy(session), empty);
  EXPECT_TRUE(session.Recipe().components.empty());
  EXPECT_EQ(runner.calls, 3);
}

TEST(AiMaskEditSessionTest, NewStepAfterUndoDropsRedo) {
  FakeRunner        runner;
  AiMaskEditSession session(runner, SessionImage());
  ASSERT_TRUE(Applied(session.AddPromptStep(Click(0.3f, 0.3f), AiMaskComponentOperation::Add)));
  ASSERT_TRUE(Applied(session.AddPromptStep(Click(0.6f, 0.6f), AiMaskComponentOperation::Add)));
  ASSERT_TRUE(session.Undo());
  ASSERT_TRUE(session.CanRedo());

  ASSERT_TRUE(Applied(session.SetInputMode(AiMaskInputMode::Task)));

  EXPECT_FALSE(session.CanRedo());
  EXPECT_EQ(session.StateCount(), 3U);
  EXPECT_EQ(std::get<AiMaskPromptGroup>(session.Recipe().components[0].content).steps.size(), 1U);
}

// ---------------------------------------------------------------------------------------------
// People
// ---------------------------------------------------------------------------------------------

TEST(AiMaskEditSessionTest, PeopleClickUsesInSubjectThenToggles) {
  FakeRunner runner;
  auto       people                       = AlphaResult(16, 12, 0.5625f);
  people.people                           = {Person(0, {0.0f, 0.0f, 0.25f, 0.5f}, true),
                                             Person(1, {0.5f, 0.5f, 0.75f, 1.0f}, false)};
  runner.tasks[SegmentationTask::kPeople] = people;
  runner.persons[0]                       = AlphaResult(8, 8, 1.0f, {0.0f, 0.0f, 0.25f, 0.5f});
  AiMaskEditSession session(runner, SessionImage());

  ASSERT_TRUE(Applied(session.AddTask(AiMaskTaskKind::People, AiMaskComponentOperation::Add)));
  ASSERT_EQ(session.People().size(), 2U);
  EXPECT_TRUE(session.PersonIncluded(0));
  EXPECT_FALSE(session.PersonIncluded(1));
  // Gain 4 (16 x 12 to 64 x 48): round(((0.5625 - 0.5) * 4 + 0.5) * 255) = round(191.25).
  EXPECT_EQ(At(session.Coverage(), 5, 5), 191);

  ASSERT_TRUE(Applied(session.TogglePerson(0)));
  const auto& first = std::get<AiMaskTaskComponent>(session.Recipe().components[0].content);
  ASSERT_EQ(first.person_toggles.size(), 1U);
  EXPECT_EQ(first.person_toggles[0].operation, AiMaskComponentOperation::Subtract);
  EXPECT_EQ(first.person_toggles[0].box, (AiMaskBox{0.0f, 0.0f, 0.25f, 0.5f}));
  EXPECT_FALSE(session.PersonIncluded(0));
  EXPECT_EQ(At(session.Coverage(), 5, 5), 0);      // inside the person crop
  EXPECT_EQ(At(session.Coverage(), 40, 30), 191);  // outside it: unchanged

  ASSERT_TRUE(Applied(session.TogglePerson(0)));
  const auto& second = std::get<AiMaskTaskComponent>(session.Recipe().components[0].content);
  ASSERT_EQ(second.person_toggles.size(), 2U);
  EXPECT_EQ(second.person_toggles[1].operation, AiMaskComponentOperation::Add);
  EXPECT_TRUE(session.PersonIncluded(0));
  EXPECT_EQ(At(session.Coverage(), 5, 5), 255);
  // The person result ran once; the second toggle reads the stored output.
  EXPECT_EQ(runner.task_calls.size(), 2U);
}

TEST(AiMaskEditSessionTest, PersonNotFoundMakesNoStep) {
  FakeRunner runner;
  auto       people                       = AlphaResult(16, 12, 0.8f);
  people.people                           = {Person(0, {0.0f, 0.0f, 0.25f, 0.5f}, true),
                                             Person(1, {0.5f, 0.5f, 0.75f, 1.0f}, false)};
  runner.tasks[SegmentationTask::kPeople] = people;
  runner.persons[1]                       = NotFoundResult("no alpha >= 0.5 in the person crop");
  AiMaskEditSession session(runner, SessionImage());

  EXPECT_EQ(session.TogglePerson(0).status, AiMaskStepStatus::Rejected);
  ASSERT_TRUE(Applied(session.AddTask(AiMaskTaskKind::People, AiMaskComponentOperation::Add)));
  const auto coverage = CoverageCopy(session);
  const auto states   = session.StateCount();

  const auto result   = session.TogglePerson(1);
  EXPECT_EQ(result.status, AiMaskStepStatus::NotFound);
  EXPECT_EQ(result.message, "no alpha >= 0.5 in the person crop");
  EXPECT_EQ(session.StateCount(), states);
  EXPECT_EQ(CoverageCopy(session), coverage);
  EXPECT_FALSE(session.PersonIncluded(1));
  EXPECT_TRUE(
      std::get<AiMaskTaskComponent>(session.Recipe().components[0].content).person_toggles.empty());

  const auto calls = runner.calls;
  EXPECT_EQ(session.TogglePerson(2).status, AiMaskStepStatus::Rejected);
  EXPECT_EQ(runner.calls, calls);
}

TEST(AiMaskEditSessionTest, FailedModelCallKeepsState) {
  FakeRunner runner;
  runner.tasks[SegmentationTask::kSubject] = AlphaResult(16, 12, 1.0f);
  AiMaskEditSession session(runner, SessionImage());
  ASSERT_TRUE(Applied(session.AddTask(AiMaskTaskKind::Subject, AiMaskComponentOperation::Add)));
  const auto recipe     = session.Recipe();
  const auto coverage   = CoverageCopy(session);

  runner.decode_failure = "the decoder session failed";
  const auto decoded = session.AddPromptStep(Click(0.5f, 0.5f), AiMaskComponentOperation::Subtract);
  EXPECT_EQ(decoded.status, AiMaskStepStatus::Failed);
  EXPECT_EQ(decoded.message, "the decoder session failed");

  const auto sky = session.AddTask(AiMaskTaskKind::Sky, AiMaskComponentOperation::Add);
  EXPECT_EQ(sky.status, AiMaskStepStatus::Failed);
  EXPECT_EQ(sky.message, "task failed");

  // A Sky mask of another size is an invalid output.
  SegmentationTaskResult wrong_size;
  wrong_size.status      = Ok();
  wrong_size.found       = true;
  wrong_size.mask_kind   = SegmentationTaskMaskKind::kBinary;
  wrong_size.mask_width  = 32;
  wrong_size.mask_height = 24;
  wrong_size.mask_r8.assign(32 * 24, 255);
  runner.tasks[SegmentationTask::kSky] = wrong_size;
  EXPECT_EQ(session.AddTask(AiMaskTaskKind::Sky, AiMaskComponentOperation::Add).status,
            AiMaskStepStatus::Failed);

  EXPECT_EQ(session.Recipe(), recipe);
  EXPECT_EQ(CoverageCopy(session), coverage);
  EXPECT_EQ(session.StateCount(), 2U);
}

TEST(AiMaskEditSessionTest, EdgeAndModeStepsNeedNoModelCall) {
  FakeRunner        runner;
  AiMaskEditSession session(runner, SessionImage());
  ASSERT_TRUE(Applied(
      session.AddPromptStep(Stroke({{0.2f, 0.2f}, {0.4f, 0.4f}}), AiMaskComponentOperation::Add)));
  const auto coverage = CoverageCopy(session);

  ASSERT_TRUE(Applied(session.SetEdgeAdjustment(AiMaskEdgeAdjustment{2.0f, -1})));
  EXPECT_EQ(runner.calls, 1);
  EXPECT_EQ(CoverageCopy(session), coverage);
  EXPECT_EQ(session.Edge(), (AiMaskEdgeAdjustment{2.0f, -1}));
  EXPECT_EQ(session.SetEdgeAdjustment(AiMaskEdgeAdjustment{2.0f, -1}).status,
            AiMaskStepStatus::Rejected);
  EXPECT_EQ(session.SetEdgeAdjustment(AiMaskEdgeAdjustment{51.0f, 0}).status,
            AiMaskStepStatus::Rejected);
  ASSERT_TRUE(session.Undo());
  EXPECT_EQ(session.Edge(), AiMaskEdgeAdjustment{});
  ASSERT_TRUE(session.Redo());
  EXPECT_EQ(session.Edge(), (AiMaskEdgeAdjustment{2.0f, -1}));

  // A Feather step keeps the group open: the next stroke joins it.
  ASSERT_TRUE(Applied(
      session.AddPromptStep(Stroke({{0.5f, 0.5f}, {0.6f, 0.6f}}), AiMaskComponentOperation::Add)));
  EXPECT_EQ(session.Recipe().components.size(), 1U);

  ASSERT_TRUE(Applied(session.SetInputMode(AiMaskInputMode::Task)));
  ASSERT_TRUE(Applied(session.SetInputMode(AiMaskInputMode::Prompt)));
  EXPECT_EQ(session.SetInputMode(AiMaskInputMode::Prompt).status, AiMaskStepStatus::Rejected);
  EXPECT_EQ(runner.calls, 2);
  ASSERT_TRUE(Applied(
      session.AddPromptStep(Stroke({{0.7f, 0.1f}, {0.8f, 0.2f}}), AiMaskComponentOperation::Add)));
  EXPECT_EQ(session.Recipe().components.size(), 2U);
  EXPECT_TRUE(runner.prompts.back().mask_input.empty());
  EXPECT_EQ(runner.calls, 3);
}

TEST(AiMaskEditSessionTest, LevelChangeAppliesOnlyToSingleClick) {
  FakeRunner        runner;
  AiMaskEditSession session(runner, SessionImage());
  ASSERT_TRUE(Applied(session.AddPromptStep(Click(0.5f, 0.5f), AiMaskComponentOperation::Add)));
  const auto& clicked = std::get<AiMaskPromptGroup>(session.Recipe().components[0].content);
  EXPECT_EQ(clicked.steps[0].level, 1U);  // the decoder selected_index

  ASSERT_TRUE(Applied(session.SelectLevel(2)));
  EXPECT_EQ(runner.calls, 1);
  EXPECT_EQ(std::get<AiMaskPromptGroup>(session.Recipe().components[0].content).steps[0].level, 2U);
  EXPECT_EQ(CoverageCopy(session),
            ConvertModelOutputToCoverage(
                AiMaskSamLogits{kLogitSize,
                                RectLogits(0.5f - 0.2f, 0.5f - 0.2f, 0.5f + 0.2f, 0.5f + 0.2f)},
                kSessionExtent));
  EXPECT_EQ(session.SelectLevel(3).status, AiMaskStepStatus::Rejected);
  EXPECT_EQ(session.SelectLevel(2).status, AiMaskStepStatus::Rejected);

  FakeRunner        stroke_runner;
  AiMaskEditSession stroked(stroke_runner, SessionImage());
  EXPECT_EQ(stroked.SelectLevel(1).status, AiMaskStepStatus::Rejected);
  ASSERT_TRUE(Applied(
      stroked.AddPromptStep(Stroke({{0.2f, 0.2f}, {0.4f, 0.4f}}), AiMaskComponentOperation::Add)));
  EXPECT_EQ(stroked.SelectLevel(1).status, AiMaskStepStatus::Rejected);
  EXPECT_EQ(stroke_runner.calls, 1);
  EXPECT_EQ(stroked.StateCount(), 2U);
}

TEST(AiMaskEditSessionTest, PointLimitRejectsThe65thPoint) {
  FakeRunner                           runner;
  AiMaskEditSession                    session(runner, SessionImage());
  std::vector<std::pair<float, float>> points;
  for (int i = 0; i < 64; ++i) {
    points.emplace_back(0.1f + 0.01f * static_cast<float>(i), 0.5f);
  }
  ASSERT_TRUE(Applied(session.AddPromptStep(Stroke(points), AiMaskComponentOperation::Add)));
  ASSERT_EQ(runner.calls, 1);

  const auto result = session.AddPromptStep(Click(0.9f, 0.9f), AiMaskComponentOperation::Add);
  EXPECT_EQ(result.status, AiMaskStepStatus::Rejected);
  EXPECT_EQ(result.message, "A prompt group holds at most 64 points.");
  EXPECT_EQ(runner.calls, 1);
  EXPECT_EQ(session.StateCount(), 2U);
}

// ---------------------------------------------------------------------------------------------
// Reopen and commit
// ---------------------------------------------------------------------------------------------

/// Commits a Subject plus stroke session and returns the payload.
auto CommittedSubjectAndStroke() -> std::pair<MaskAsset, AiSegmentationMaskSource> {
  FakeRunner runner;
  runner.tasks[SegmentationTask::kSubject] =
      AlphaResult(16, 12, 1.0f, SegmentationBox{0.25f, 0.25f, 0.75f, 0.75f});
  AiMaskEditSession session(runner, SessionImage());
  EXPECT_TRUE(Applied(session.AddTask(AiMaskTaskKind::Subject, AiMaskComponentOperation::Add)));
  EXPECT_TRUE(Applied(session.AddPromptStep(Stroke({{0.3f, 0.3f}, {0.4f, 0.4f}}),
                                            AiMaskComponentOperation::Subtract)));
  return session.PrepareCommit();
}

TEST(AiMaskEditSessionTest, ReopenShowsStoredCoverageWithoutModelCall) {
  const auto [asset, stored] = CommittedSubjectAndStroke();
  FakeRunner runner;
  runner.tasks[SegmentationTask::kSubject] =
      AlphaResult(16, 12, 1.0f, SegmentationBox{0.25f, 0.25f, 0.75f, 0.75f});
  AiMaskEditSession session(runner, SessionImage(kFingerprint2), stored, asset);

  const auto        placed = PlaceAiMaskAssetInCoverage(asset, kSessionExtent);
  EXPECT_EQ(CoverageCopy(session), placed);
  EXPECT_EQ(session.Recipe(), stored.recipe);
  EXPECT_EQ(session.InputMode(), AiMaskInputMode::Prompt);
  EXPECT_EQ(runner.calls, 0);

  ASSERT_TRUE(Applied(session.SetEdgeAdjustment(AiMaskEdgeAdjustment{3.0f, 2})));
  EXPECT_EQ(runner.calls, 0);
  EXPECT_EQ(CoverageCopy(session), placed);
  // The stored coverage commits the stored fingerprint, not the session one.
  const auto [edge_asset, edge_source] = session.PrepareCommit();
  EXPECT_EQ(edge_asset.key, asset.key);
  EXPECT_EQ(edge_source.geometry_fingerprint, kFingerprint);
  EXPECT_EQ(edge_source.edge, (AiMaskEdgeAdjustment{3.0f, 2}));

  // The first step that changes the components replays all components, then decodes the step.
  ASSERT_TRUE(
      Applied(session.AddPromptStep(Click(0.6f, 0.6f), AiMaskComponentOperation::Subtract)));
  ASSERT_EQ(runner.task_calls.size(), 1U);
  EXPECT_EQ(runner.task_calls[0].first, SegmentationTask::kSubject);
  ASSERT_EQ(runner.prompts.size(), 2U);
  EXPECT_EQ(runner.prompts[0].points.size(), 2U);  // replay of the stored stroke
  EXPECT_TRUE(runner.prompts[0].mask_input.empty());
  EXPECT_EQ(runner.prompts[1].points.size(), 3U);  // the stored group continues
  EXPECT_FALSE(runner.prompts[1].mask_input.empty());
  EXPECT_EQ(runner.calls, 3);
  EXPECT_EQ(session.PrepareCommit().second.geometry_fingerprint, kFingerprint2);

  ASSERT_TRUE(session.Undo());
  EXPECT_EQ(CoverageCopy(session), placed);
  ASSERT_TRUE(session.Undo());
  EXPECT_EQ(CoverageCopy(session), placed);
  const auto [reopened_asset, reopened_source] = session.PrepareCommit();
  EXPECT_EQ(reopened_asset.key, asset.key);
  EXPECT_EQ(reopened_asset.pixels, asset.pixels);
  EXPECT_EQ(reopened_source, stored);
  EXPECT_EQ(runner.calls, 3);
}

// ---------------------------------------------------------------------------------------------
// Shared coverage reads (SM8.1)
// ---------------------------------------------------------------------------------------------

TEST(AiMaskEditSessionTest, QueuedCoverageReadSurvivesNextModelResult) {
  FakeRunner runner;
  runner.tasks[SegmentationTask::kSubject] = AlphaResult(16, 12, 0.5625f);
  runner.tasks[SegmentationTask::kSky] =
      AlphaResult(16, 12, 1.0f, SegmentationBox{0.0f, 0.0f, 1.0f, 0.5f});
  AiMaskEditSession session(runner, SessionImage());
  ASSERT_TRUE(Applied(session.AddTask(AiMaskTaskKind::Subject, AiMaskComponentOperation::Add)));

  // A queued upload holds the read of the Subject coverage.
  auto                                            queued = session.AcquireCoveragePixels();
  const std::weak_ptr<const std::vector<uint8_t>> queued_allocation = queued;
  const std::vector<std::uint8_t>                 subject_bytes     = *queued;
  ASSERT_EQ(subject_bytes, CoverageCopy(session));
  EXPECT_EQ(session.Coverage().data(), queued->data());

  // The next model result composes a new allocation; the queued bytes do not change.
  ASSERT_TRUE(Applied(session.AddTask(AiMaskTaskKind::Sky, AiMaskComponentOperation::Add)));
  const auto current = session.AcquireCoveragePixels();
  EXPECT_NE(current.get(), queued.get());
  EXPECT_NE(*current, subject_bytes);
  EXPECT_EQ(At(*current, 5, 5), 255);
  EXPECT_EQ(*queued, subject_bytes);
  EXPECT_EQ(At(*queued, 5, 5), 191);  // gain 4: round(((0.5625 - 0.5) * 4 + 0.5) * 255)

  // Undo composes the Subject coverage again; the queued read is still a separate allocation.
  ASSERT_TRUE(session.Undo());
  EXPECT_EQ(CoverageCopy(session), subject_bytes);
  EXPECT_EQ(*queued, subject_bytes);
  EXPECT_FALSE(queued_allocation.expired());

  // The session holds no reference to the old allocation: the release of the read frees it.
  queued.reset();
  EXPECT_TRUE(queued_allocation.expired());
  EXPECT_EQ(CoverageCopy(session), subject_bytes);
}

TEST(AiMaskEditSessionTest, EdgeChangeSharesOriginalPixels) {
  FakeRunner runner;
  runner.tasks[SegmentationTask::kSubject] = AlphaResult(16, 12, 0.5f);
  AiMaskEditSession session(runner, SessionImage());
  ASSERT_TRUE(Applied(session.AddTask(AiMaskTaskKind::Subject, AiMaskComponentOperation::Add)));
  const auto original = session.AcquireCoveragePixels();
  const auto calls    = runner.calls;

  // Edge and input-mode steps keep the recipe, so they keep the allocation.
  ASSERT_TRUE(Applied(session.SetEdgeAdjustment(AiMaskEdgeAdjustment{5.0f, 2})));
  EXPECT_EQ(session.AcquireCoveragePixels().get(), original.get());
  ASSERT_TRUE(Applied(session.SetEdgeAdjustment(AiMaskEdgeAdjustment{12.5f, -3})));
  EXPECT_EQ(session.AcquireCoveragePixels().get(), original.get());
  ASSERT_TRUE(Applied(session.SetInputMode(AiMaskInputMode::Task)));
  EXPECT_EQ(session.AcquireCoveragePixels().get(), original.get());
  ASSERT_EQ(session.StateCount(), 5U);

  // Undo and Redo across these steps do not compose again.
  for (int step = 0; step < 3; ++step) {
    ASSERT_TRUE(session.Undo());
    EXPECT_EQ(session.AcquireCoveragePixels().get(), original.get()) << "undo " << step;
  }
  EXPECT_EQ(session.Edge(), AiMaskEdgeAdjustment{});
  for (int step = 0; step < 3; ++step) {
    ASSERT_TRUE(session.Redo());
    EXPECT_EQ(session.AcquireCoveragePixels().get(), original.get()) << "redo " << step;
  }
  EXPECT_EQ(session.Edge(), (AiMaskEdgeAdjustment{12.5f, -3}));
  EXPECT_EQ(runner.calls, calls);
  EXPECT_EQ(session.StateCount(), 5U);

  // Undo to the empty first state changes the recipe and the coverage.
  for (int step = 0; step < 4; ++step) {
    ASSERT_TRUE(session.Undo());
  }
  EXPECT_NE(session.AcquireCoveragePixels().get(), original.get());
  EXPECT_EQ(runner.calls, calls);

  // An opened stored AI Mask shares its stored coverage across edge steps.
  const auto [asset, stored] = CommittedSubjectAndStroke();
  FakeRunner        stored_runner;
  AiMaskEditSession reopened(stored_runner, SessionImage(), stored, asset);
  const auto        stored_pixels = reopened.AcquireCoveragePixels();
  ASSERT_TRUE(Applied(reopened.SetEdgeAdjustment(AiMaskEdgeAdjustment{4.0f, 1})));
  EXPECT_EQ(reopened.AcquireCoveragePixels().get(), stored_pixels.get());
  ASSERT_TRUE(reopened.Undo());
  EXPECT_EQ(reopened.AcquireCoveragePixels().get(), stored_pixels.get());
  EXPECT_EQ(stored_runner.calls, 0);
}

TEST(AiMaskEditSessionTest, ReopenRejectsAnotherAnalysisSize) {
  MaskAsset asset;
  asset.descriptor.extent           = Extent2D{4, 4};
  asset.descriptor.reference_bounds = NormalizedRect{0.5f, 0.5f, 4.0f / 4096.0f, 4.0f / 3072.0f};
  asset.pixels.assign(16, 255);
  asset.key = MakeMaskAssetKey(asset.descriptor, asset.pixels);
  AiSegmentationMaskSource stored;
  stored.asset_key       = asset.key;
  stored.descriptor      = asset.descriptor;
  stored.analysis_extent = Extent2D{4096, 3072};
  stored.recipe.components.push_back(AiMaskComponent{
      AiMaskComponentOperation::Add, AiMaskTaskComponent{AiMaskTaskKind::Subject, {}}});
  stored.model_set_id         = kModelSetId;
  stored.geometry_fingerprint = kFingerprint;
  FakeRunner runner;

  EXPECT_THROW(
      (void)AiMaskEditSession(runner, AiMaskSessionImage{{4096, 2731}, kModelSetId, kFingerprint},
                              stored, asset),
      std::runtime_error);
  EXPECT_NO_THROW((void)AiMaskEditSession(
      runner, AiMaskSessionImage{{4096, 3072}, kModelSetId, kFingerprint}, stored, asset));

  auto other_asset      = asset;
  other_asset.pixels[0] = 0;
  other_asset.key       = MakeMaskAssetKey(other_asset.descriptor, other_asset.pixels);
  EXPECT_THROW(
      (void)AiMaskEditSession(runner, AiMaskSessionImage{{4096, 3072}, kModelSetId, kFingerprint},
                              stored, other_asset),
      std::runtime_error);
  EXPECT_EQ(runner.calls, 0);
}

TEST(AiMaskEditSessionTest, PrepareCommitCropsToCoverageWithBorder) {
  FakeRunner             runner;
  SegmentationTaskResult sky;
  sky.status      = Ok();
  sky.found       = true;
  sky.mask_kind   = SegmentationTaskMaskKind::kBinary;
  sky.mask_width  = kSessionExtent.width;
  sky.mask_height = kSessionExtent.height;
  sky.mask_r8.assign(static_cast<std::size_t>(kSessionExtent.width) * kSessionExtent.height, 0);
  for (std::uint32_t y = 5; y < 15; ++y) {
    for (std::uint32_t x = 10; x < 20; ++x) {
      sky.mask_r8[static_cast<std::size_t>(y) * kSessionExtent.width + x] = 255;
    }
  }
  runner.tasks[SegmentationTask::kSky] = sky;
  AiMaskEditSession session(runner, SessionImage());
  ASSERT_TRUE(Applied(session.AddTask(AiMaskTaskKind::Sky, AiMaskComponentOperation::Add)));
  ASSERT_TRUE(Applied(session.SetEdgeAdjustment(AiMaskEdgeAdjustment{1.5f, 3})));

  const auto [asset, source] = session.PrepareCommit();

  EXPECT_EQ(asset.descriptor.extent, (Extent2D{12, 12}));
  EXPECT_FLOAT_EQ(asset.descriptor.reference_bounds.x, 9.0f / 64.0f);
  EXPECT_FLOAT_EQ(asset.descriptor.reference_bounds.y, 4.0f / 48.0f);
  EXPECT_FLOAT_EQ(asset.descriptor.reference_bounds.w, 12.0f / 64.0f);
  EXPECT_FLOAT_EQ(asset.descriptor.reference_bounds.h, 12.0f / 48.0f);
  ASSERT_EQ(asset.pixels.size(), 144U);
  for (std::uint32_t y = 0; y < 12; ++y) {
    for (std::uint32_t x = 0; x < 12; ++x) {
      const bool border = x == 0 || y == 0 || x == 11 || y == 11;
      EXPECT_EQ(asset.pixels[y * 12 + x], border ? 0 : 255) << x << "," << y;
    }
  }
  EXPECT_EQ(asset.key, MakeMaskAssetKey(asset.descriptor, asset.pixels));
  EXPECT_EQ(source.asset_key, asset.key);
  EXPECT_EQ(source.descriptor, asset.descriptor);
  EXPECT_EQ(source.analysis_extent, kSessionExtent);
  EXPECT_EQ(source.recipe, session.Recipe());
  EXPECT_EQ(source.edge, (AiMaskEdgeAdjustment{1.5f, 3}));
  EXPECT_EQ(source.model_set_id, kModelSetId);
  EXPECT_EQ(source.geometry_fingerprint, kFingerprint);

  // A coverage at the image corner: the border is clipped to the image.
  std::vector<std::uint8_t> corner(
      static_cast<std::size_t>(kSessionExtent.width) * kSessionExtent.height, 0);
  corner[0]          = 255;
  corner[65]         = 128;  // (1, 1)
  const auto cropped = CropAiMaskCoverage(corner, kSessionExtent);
  ASSERT_TRUE(cropped.has_value());
  EXPECT_EQ(cropped->descriptor.extent, (Extent2D{3, 3}));
  EXPECT_FLOAT_EQ(cropped->descriptor.reference_bounds.x, 0.0f);
  EXPECT_EQ(PlaceAiMaskAssetInCoverage(*cropped, kSessionExtent), corner);
}

TEST(AiMaskEditSessionTest, PrepareCommitRejectsEmptyCoverage) {
  FakeRunner runner;
  runner.tasks[SegmentationTask::kSubject] = AlphaResult(16, 12, 1.0f);
  AiMaskEditSession session(runner, SessionImage());
  try {
    (void)session.PrepareCommit();
    FAIL() << "PrepareCommit of an empty session must throw";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "The AI Mask has no coverage.");
  }
  // A Subtract component alone removes from nothing.
  ASSERT_TRUE(
      Applied(session.AddTask(AiMaskTaskKind::Subject, AiMaskComponentOperation::Subtract)));
  EXPECT_THROW((void)session.PrepareCommit(), std::runtime_error);
}

}  // namespace
}  // namespace alcedo
