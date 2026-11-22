//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// @file grade_order_cost_measurement.hpp
/// @brief Frame time of the Color Grade stage order of issue #900 against the order before it,
///        for the CUDA, OpenCL, and Metal multi-grade tests.
///
/// Issue #900 moved the Local Laplacian stage (Shadows, Highlights) before the Curve. A Grade with
/// an active LLF stage then runs two pointwise passes (Basic Tone before the LLF stage, Color after
/// it) where the order before ran one (Basic Tone and Color fused, then the LLF stage). The
/// measurement renders both orders of the same Grade on the same device and input. The order
/// before #900 comes from the compiled plan: its adjustments are permuted to Basic Tone, Color,
/// Local Tone, with one Pointwise stage and one Local Laplacian stage.
///
/// The measurement runs only when ALCEDO_GRADE_ORDER_MEASUREMENT=1. It prints one table per input
/// size. Only an optimized build gives meaningful times (a Debug CUDA build compiles kernels
/// with -G).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "multi_grade_runtime_test_support.hpp"

namespace alcedo::grade_order_measurement {

/// True when the environment asks for the measurement.
inline auto Enabled() -> bool {
  const char* value = std::getenv("ALCEDO_GRADE_ORDER_MEASUREMENT");
  return value != nullptr && std::string_view{value} == "1";
}

/**
 * @brief @p plan with every Color Grade in the stage order before issue #900.
 *
 * Moves the Local Laplacian adjustments after the pointwise adjustments and replaces the stages
 * with one Pointwise stage and one Local Laplacian stage. A Grade with a Neighborhood adjustment is
 * left as it is (the Color Grade catalog has none).
 */
inline auto WithFusedOrderBeforeIssue900(ExecutionPlan plan) -> ExecutionPlan {
  for (auto& grade : plan.grade_nodes) {
    std::vector<CompiledAdjustment> pointwise;
    std::vector<CompiledAdjustment> local_tone;
    bool                            has_neighborhood = false;
    for (const auto& adjustment : grade.adjustments) {
      if (adjustment.algorithm == CompiledAdjustmentAlgorithm::LocalLaplacian) {
        local_tone.push_back(adjustment);
      } else if (adjustment.algorithm == CompiledAdjustmentAlgorithm::Pointwise) {
        pointwise.push_back(adjustment);
      } else {
        has_neighborhood = true;
      }
    }
    if (has_neighborhood) {
      continue;
    }
    grade.adjustments = pointwise;
    grade.adjustments.insert(grade.adjustments.end(), local_tone.begin(), local_tone.end());
    grade.stages.clear();
    if (!pointwise.empty()) {
      grade.stages.push_back(CompiledGradeStage{CompiledGradeStageKind::Pointwise, 0U,
                                                static_cast<std::uint32_t>(pointwise.size())});
    }
    if (!local_tone.empty()) {
      grade.stages.push_back(CompiledGradeStage{CompiledGradeStageKind::LocalLaplacian,
                                                static_cast<std::uint32_t>(pointwise.size()),
                                                static_cast<std::uint32_t>(local_tone.size())});
    }
  }
  return plan;
}

/// Frame times of one scenario in milliseconds.
struct ScenarioTimes {
  std::string         label;
  std::vector<double> frames;
};

inline auto Percentile(std::vector<double> values, double fraction) -> double {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
  return values[index];
}

/// Input sizes of the measurement: the 2560 preview of NM8.2 and a 4600 x 3064 native plane.
struct InputSize {
  std::uint32_t width  = 0;
  std::uint32_t height = 0;
};

inline constexpr InputSize kInputSizes[] = {{2560U, 1705U}, {4600U, 3064U}};
inline constexpr int       kWarmupFrames = 5;
inline constexpr int       kTimedFrames  = 31;

/**
 * @brief Measure both stage orders on one backend and print the tables.
 *
 * Scenarios: the order before #900 after an Exposure edit, the #900 order after an Exposure edit,
 * and the #900 order after a Saturation edit. An Exposure edit changes the LLF source in both
 * orders, so both rebuild the LLF maps; the difference of these two scenarios is the cost of the
 * extra pointwise pass. The Saturation edit shows the #900 order keeping the LLF maps. (The order
 * before #900 rebuilt them on a Saturation edit, which equals its Exposure scenario.) Each frame
 * time is Execute plus WaitIdle on the host clock.
 *
 * @param backend Label of the table.
 * @param set_input Stores the prepared input of the fixture: void(PreparedRawInput).
 * @param compile Compiles a document for that input: ExecutionPlan(PipelineDocument&).
 * @param render Renders one frame and waits: void(PipelineDocument&, const ExecutionPlan&).
 */
template <class SetInput, class Compile, class Render>
void MeasureGradeStageOrder(std::string_view backend, SetInput set_input, Compile compile,
                            Render render) {
  for (const auto size : kInputSizes) {
    set_input(RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(size.width, size.height),
                                            gpu_dag_test::FullSensor(size.width, size.height)));
    auto        document = multi_grade_test::MakeIdentityGradeDocument();
    const auto& grade_id = document.PrimaryGrade()->Id();
    auto&       exposure =
        multi_grade_test::GradeAdjustment<ExposureModel>(document, grade_id, type_ids::Exposure());
    auto& saturation = multi_grade_test::GradeAdjustment<SaturationModel>(document, grade_id,
                                                                          type_ids::Saturation());
    multi_grade_test::GradeAdjustment<ShadowsModel>(document, grade_id, type_ids::Shadows())
        .SetValue(40.0f);
    multi_grade_test::GradeAdjustment<HighlightsModel>(document, grade_id, type_ids::Highlights())
        .SetValue(-30.0f);
    CurveUpdate lifted;
    lifted.points = std::vector<CurvePoint>{{0.0f, 0.0f}, {0.5f, 0.55f}, {1.0f, 1.0f}};
    multi_grade_test::GradeAdjustment<CurveModel>(document, grade_id, type_ids::Curve())
        .ApplyUpdate(lifted);
    exposure.SetValue(0.2f);
    saturation.SetValue(1.2f);

    const auto split_plan = compile(document);
    const auto fused_plan = WithFusedOrderBeforeIssue900(split_plan);
    ASSERT_EQ(split_plan.FirstGrade()->stages.size(), 3U);
    ASSERT_EQ(fused_plan.FirstGrade()->stages.size(), 2U);

    ScenarioTimes fused_exposure{"before #900, Exposure edit", {}};
    ScenarioTimes split_exposure{"#900, Exposure edit", {}};
    ScenarioTimes split_saturation{"#900, Saturation edit", {}};
    int           edit        = 0;
    const auto    timed_frame = [&](const ExecutionPlan& plan, ScenarioTimes* times,
                                 bool edit_saturation) {
      ++edit;
      if (edit_saturation) {
        saturation.SetValue(1.2f + 0.001f * static_cast<float>(edit % 50));
      } else {
        exposure.SetValue(0.2f + 0.001f * static_cast<float>(edit % 50));
      }
      const auto start = std::chrono::steady_clock::now();
      render(document, plan);
      const auto end = std::chrono::steady_clock::now();
      if (times != nullptr) {
        times->frames.push_back(std::chrono::duration<double, std::milli>(end - start).count());
      }
    };
    // The two Exposure scenarios alternate, and their order swaps every round, so that neither
    // one always follows the other. A frame that reuses the LLF maps leaves the next rebuild more
    // work, so the Saturation scenario runs in a block of its own.
    for (int round = 0; round < kWarmupFrames + kTimedFrames; ++round) {
      const bool timed = round >= kWarmupFrames;
      if (round % 2 == 0) {
        timed_frame(fused_plan, timed ? &fused_exposure : nullptr, false);
        timed_frame(split_plan, timed ? &split_exposure : nullptr, false);
      } else {
        timed_frame(split_plan, timed ? &split_exposure : nullptr, false);
        timed_frame(fused_plan, timed ? &fused_exposure : nullptr, false);
      }
    }
    for (int round = 0; round < kWarmupFrames + kTimedFrames; ++round) {
      timed_frame(split_plan, round >= kWarmupFrames ? &split_saturation : nullptr, true);
    }

    std::printf("\nGrade stage order, %.*s, %ux%u, %d frames each (ms)\n",
                static_cast<int>(backend.size()), backend.data(), size.width, size.height,
                kTimedFrames);
    std::printf("| Scenario | P10 | P50 | P90 |\n| --- | ---: | ---: | ---: |\n");
    for (const auto* times : {&fused_exposure, &split_exposure, &split_saturation}) {
      std::printf("| %s | %.2f | %.2f | %.2f |\n", times->label.c_str(),
                  Percentile(times->frames, 0.1), Percentile(times->frames, 0.5),
                  Percentile(times->frames, 0.9));
    }
    std::printf("Extra pass (P50 difference of the Exposure rows): %.2f ms\n",
                Percentile(split_exposure.frames, 0.5) - Percentile(fused_exposure.frames, 0.5));
    std::fflush(stdout);
  }
}

}  // namespace alcedo::grade_order_measurement
