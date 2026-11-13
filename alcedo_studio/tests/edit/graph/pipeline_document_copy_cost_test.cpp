//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Cost measurements for the executor ownership refactor
// (docs/refactor/2026-09-27-executor-ownership-refactor-plan.md, phase P0).
//
// P2 replaces the whole-document JSON copy with a copy-on-write freeze. The P0 tests record what
// the whole-document copy costs, and what the per-frame static plan key and Grade parameter
// packing cost, for a typical document and for a document with many Masks. Each test also checks
// that the measured operation produced a correct result, so a fast but wrong path cannot pass.
// Those timings are printed with the "[P0 cost]" prefix and are not compared with a limit.
//
// The P2 tests measure one editor tick with the copy-on-write freeze (edit one slider, freeze,
// release the previous frozen document) and require the limit that P0 set from the numbers
// above: at most 0.2 ms median and at most 100 heap allocations in the debug build.
//
// The P2A test measures one Mask handle drag tick (replace one Mask source, freeze, release the
// previous frozen document) and requires the same limit, with an allocation count that does not
// depend on how many Masks the Grade holds.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/develop_compile_source.hpp"
#include "edit/runtime/graph_compiler.hpp"
#ifdef ALCEDO_ENABLE_BRUSH_MASK
#include "edit/mask/brush_stroke.hpp"
#endif

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#define ALCEDO_P0_COUNT_HEAP_ALLOCATIONS 1
#endif

namespace alcedo {
namespace {

#ifdef ALCEDO_P0_COUNT_HEAP_ALLOCATIONS
// The debug CRT heap is shared by every module linked against the debug UCRT, so this hook sees
// allocations made inside the EditGraph and EditRuntime DLLs as well as in this executable.
thread_local bool      t_counting_allocations = false;
thread_local std::size_t t_allocation_count   = 0;

auto CountAllocationHook(int alloc_type, void*, std::size_t, int, long, const unsigned char*, int)
    -> int {
  if (t_counting_allocations && alloc_type == _HOOK_ALLOC) {
    ++t_allocation_count;
  }
  return 1;
}

class AllocationCounter {
 public:
  AllocationCounter() {
    previous_hook_          = _CrtSetAllocHook(&CountAllocationHook);
    t_allocation_count      = 0;
    t_counting_allocations  = true;
  }
  ~AllocationCounter() {
    t_counting_allocations = false;
    _CrtSetAllocHook(previous_hook_);
  }
  AllocationCounter(const AllocationCounter&)            = delete;
  AllocationCounter& operator=(const AllocationCounter&) = delete;

  [[nodiscard]] static auto Count() -> std::size_t { return t_allocation_count; }

 private:
  _CRT_ALLOC_HOOK previous_hook_ = nullptr;
};
#endif

/// Median and minimum wall time of @p iterations calls to @p operation, in microseconds.
struct TimingMicros {
  double median = 0.0;
  double minimum = 0.0;
};

template <class Operation>
auto MeasureMicros(int iterations, Operation&& operation) -> TimingMicros {
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(iterations));
  for (int i = 0; i < iterations; ++i) {
    const auto start = std::chrono::steady_clock::now();
    operation();
    const auto end = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
  }
  std::sort(samples.begin(), samples.end());
  return {samples[samples.size() / 2], samples.front()};
}

/// Heap allocations made by one call to @p operation; -1 when the build cannot count them.
template <class Operation>
auto CountAllocations(Operation&& operation) -> long long {
#ifdef ALCEDO_P0_COUNT_HEAP_ALLOCATIONS
  AllocationCounter counter;
  operation();
  return static_cast<long long>(AllocationCounter::Count());
#else
  operation();
  return -1;
#endif
}

auto RawSensorCompileSource() -> DevelopCompileSource {
  DevelopCompileSource source;
  source.kind                  = DevelopInputKind::BayerCfa;
  source.host_extent           = Extent2D{6048, 4024};
  source.develop_output_extent = Extent2D{6048, 4024};
  source.full_reference_extent = Extent2D{6048, 4024};
  return source;
}

/**
 * @brief Pack every Grade adjustment on the image backbone, as a frame does when all slots are
 *        missing or changed.
 * @return Number of packed slots.
 */
auto PackAllGradeSlots(const PipelineDocument& document) -> std::size_t {
  std::size_t packed = 0;
  for (const auto* grade : ColorGradesOnImageBackbone(document)) {
    for (std::size_t index = 0; index < grade->AdjustmentCount(); ++index) {
      const auto& model    = grade->AdjustmentAt(index);
      const auto  behavior = TryResolveAdjustmentBehavior(model.Type());
      if (!behavior.has_value()) {
        continue;
      }
      const auto params = MakeGradeRuntimeParams(model, *behavior);
      static_cast<void>(params);
      ++packed;
    }
  }
  return packed;
}

/**
 * @brief Document with four Color Grades, each holding 16 Radial and 16 Linear Gradient Masks.
 *
 * The shipped build has no Brush Mask module; when ALCEDO_ENABLE_BRUSH_MASK is on, the primary
 * Grade also receives 8 Brush Masks of 200 strokes with 32 samples each.
 */
auto MakeManyMaskDocument() -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  for (const char* id : {"grade.extra.a", "grade.extra.b", "grade.extra.c"}) {
    const auto errors = AddCleanColorGrade(document, NodeId{"drt"}, NodeId{id});
    if (!errors.empty()) {
      ADD_FAILURE() << "AddCleanColorGrade failed for " << id;
    }
  }
  for (const auto* const_grade : ColorGradesOnImageBackbone(document)) {
    auto* grade =
        dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(const_grade->Id()));
    for (int m = 0; m < 16; ++m) {
      MaskModel radial;
      radial.id     = MaskId{std::string{grade->Id().Value()} + ".radial." + std::to_string(m)};
      radial.source = RadialMaskSource{0.1f + 0.05f * m, 0.5f, 0.2f, 0.1f, 0.3f, 0.05f, 0.1f};
      grade->AddMask(std::move(radial), grade->MaskCount());
      MaskModel linear;
      linear.id     = MaskId{std::string{grade->Id().Value()} + ".linear." + std::to_string(m)};
      linear.source = LinearGradientMaskSource{0.5f, 0.05f * m, 0.0f, 1.0f, 0.2f, 1.0f, 0.0f};
      grade->AddMask(std::move(linear), grade->MaskCount());
    }
  }
#ifdef ALCEDO_ENABLE_BRUSH_MASK
  auto* primary = document.PrimaryGrade();
  for (int m = 0; m < 8; ++m) {
    BrushMaskSource brush;
    for (int s = 0; s < 200; ++s) {
      std::vector<BrushCanonicalSample> samples;
      for (int p = 0; p < 32; ++p) {
        samples.push_back({static_cast<float>(s + p), static_cast<float>(p), 4.0f, 1.0f, 1.0f});
      }
      brush.strokes.push_back(MakeBrushStroke(
          StrokeId{"stroke." + std::to_string(m) + "." + std::to_string(s)},
          BrushStrokeMode::Paint, std::move(samples)));
    }
    MaskModel mask;
    mask.id     = MaskId{"grade.primary.brush." + std::to_string(m)};
    mask.source = std::move(brush);
    primary->AddMask(std::move(mask), primary->MaskCount());
  }
#endif
  document.MarkTopologyChanged();
  return document;
}

void ReportCost(std::string_view document_name, const PipelineDocument& document,
                int iterations) {
  const auto source_json = document.ToJson();
  const auto json_bytes  = source_json.dump().size();

  // Correctness of each measured operation.
  const auto clone = ClonePipelineDocument(document);
  ASSERT_EQ(clone.ToJson(), source_json);
  const auto compile_source = RawSensorCompileSource();
  const auto key            = GraphCompiler::MakeStaticPlanKey(document, compile_source);
  ASSERT_EQ(GraphCompiler::MakeStaticPlanKey(clone, compile_source), key);
  const auto packed_slots = PackAllGradeSlots(document);
  ASSERT_GT(packed_slots, 0u);
  const auto* exposure_grade = document.PrimaryGrade();
  ASSERT_NE(exposure_grade, nullptr);
  const auto& one_slot          = exposure_grade->AdjustmentAt(0);
  const auto  one_slot_behavior = TryResolveAdjustmentBehavior(one_slot.Type());
  ASSERT_TRUE(one_slot_behavior.has_value());

  const auto clone_time = MeasureMicros(iterations, [&] {
    auto copy = ClonePipelineDocument(document);
    static_cast<void>(copy);
  });
  const auto clone_allocations = CountAllocations([&] {
    auto copy = ClonePipelineDocument(document);
    static_cast<void>(copy);
  });
  const auto key_time = MeasureMicros(iterations, [&] {
    const auto k = GraphCompiler::MakeStaticPlanKey(document, compile_source);
    static_cast<void>(k);
  });
  const auto key_allocations = CountAllocations([&] {
    const auto k = GraphCompiler::MakeStaticPlanKey(document, compile_source);
    static_cast<void>(k);
  });
  const auto pack_all_time = MeasureMicros(iterations, [&] {
    static_cast<void>(PackAllGradeSlots(document));
  });
  const auto pack_one_time = MeasureMicros(iterations, [&] {
    const auto params = MakeGradeRuntimeParams(one_slot, *one_slot_behavior);
    static_cast<void>(params);
  });

  std::cout << "[P0 cost] document=" << document_name
            << " nodes=" << document.Graph().Nodes().size()
            << " grades=" << ColorGradesOnImageBackbone(document).size()
            << " json_bytes=" << json_bytes << " iterations=" << iterations << '\n'
            << "[P0 cost]   ClonePipelineDocument median_us=" << clone_time.median
            << " min_us=" << clone_time.minimum << " allocations=" << clone_allocations << '\n'
            << "[P0 cost]   MakeStaticPlanKey median_us=" << key_time.median
            << " min_us=" << key_time.minimum << " allocations=" << key_allocations << '\n'
            << "[P0 cost]   Pack all Grade slots (" << packed_slots
            << ") median_us=" << pack_all_time.median << " min_us=" << pack_all_time.minimum
            << '\n'
            << "[P0 cost]   Pack one Grade slot median_us=" << pack_one_time.median
            << " min_us=" << pack_one_time.minimum << '\n';
}

/// P0 limit for one editor tick: freeze plus one slider edit, in the debug build.
constexpr double      kFreezeTickMedianLimitMicros = 200.0;
constexpr std::size_t kFreezeTickAllocationLimit   = 100;

auto ExposureOf(PipelineDocument& document) -> ExposureModel& {
  auto* model = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  if (model == nullptr) {
    throw std::logic_error("Primary Grade has no Exposure");
  }
  return *model;
}

auto PublishedExposureEv(const PipelineDocument& document) -> float {
  return document.PrimaryGrade()
      ->FindAdjustmentByType(type_ids::Exposure())
      ->ToJson()
      .at("exposure_ev")
      .get<float>();
}

/**
 * @brief One editor tick as EditorSessionService runs it: the owner writes one slider value on
 *        the live document while the GUI still holds the last frozen document, then freezes the
 *        live document and replaces (releases) the previous frozen document.
 *
 * Measures the tick, checks that each frozen document holds the value written before it and
 * that the previous frozen document kept its own value, and requires the P0 limit.
 */
void RequireFreezeTickWithinLimit(std::string_view document_name, PipelineDocument document,
                                  int iterations) {
  auto  published = document.Freeze();
  float value     = 0.0f;
  auto  tick      = [&] {
    value += 0.01f;
    ExposureOf(document).SetValue(value);
    published = document.Freeze();
  };

  // Correctness: the tick copies instead of writing through to a frozen document.
  tick();
  const auto held       = published;
  const auto held_value = value;
  tick();
  ASSERT_FLOAT_EQ(PublishedExposureEv(*held), held_value);
  ASSERT_FLOAT_EQ(PublishedExposureEv(*published), value);
  ASSERT_EQ(published->Develop(), held->Develop());

  const auto time        = MeasureMicros(iterations, tick);
  const auto allocations = CountAllocations(tick);
  std::cout << "[P2 cost] document=" << document_name
            << " freeze+one slider median_us=" << time.median << " min_us=" << time.minimum
            << " allocations=" << allocations << '\n';

  EXPECT_LE(time.median, kFreezeTickMedianLimitMicros);
  if (allocations >= 0) {
    EXPECT_LE(static_cast<std::size_t>(allocations), kFreezeTickAllocationLimit);
  }
}

/// Default document whose primary Grade holds @p mask_count Radial Masks.
auto MakePrimaryGradeMaskDocument(int mask_count) -> PipelineDocument {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  for (int m = 0; m < mask_count; ++m) {
    MaskModel mask;
    mask.id     = MaskId{"mask.radial." + std::to_string(m)};
    mask.source = RadialMaskSource{0.1f + 0.01f * m, 0.5f, 0.2f, 0.1f, 0.3f, 0.05f, 0.1f};
    grade->AddMask(std::move(mask), grade->MaskCount());
  }
  document.MarkTopologyChanged();
  return document;
}

auto RadialCenterX(const PipelineDocument& document, const MaskId& mask_id) -> float {
  return std::get<RadialMaskSource>(document.PrimaryGrade()->FindMask(mask_id)->source).center_x;
}

struct MaskDragTickCost {
  TimingMicros timing;
  long long    allocations = -1;
};

/**
 * @brief One tick of a Mask handle drag as the editor runs it: replace the source of the first
 *        Mask of the primary Grade while the GUI holds the last frozen document, then freeze and
 *        release the previous frozen document.
 *
 * Checks that each frozen document keeps the source written before it, then measures the tick.
 */
auto MeasureMaskDragTick(std::string_view document_name, PipelineDocument document,
                         int iterations) -> MaskDragTickCost {
  const auto mask_id   = document.PrimaryGrade()->MaskAt(0).id;
  auto       published = document.Freeze();
  auto       source    = std::get<RadialMaskSource>(document.PrimaryGrade()->MaskAt(0).source);
  auto       tick      = [&] {
    source.center_x = source.center_x > 0.8f ? 0.2f : source.center_x + 0.001f;
    document.PrimaryGrade()->ReplaceMaskSource(mask_id, source);
    published = document.Freeze();
  };

  // Correctness: the tick stores a new Mask value instead of writing through a frozen document.
  tick();
  const auto held       = published;
  const auto held_value = source.center_x;
  tick();
  EXPECT_FLOAT_EQ(RadialCenterX(*held, mask_id), held_value);
  EXPECT_FLOAT_EQ(RadialCenterX(*published, mask_id), source.center_x);
  const auto* held_grade      = held->PrimaryGrade();
  const auto* published_grade = published->PrimaryGrade();
  for (std::size_t index = 1; index < published_grade->MaskCount(); ++index) {
    EXPECT_EQ(&published_grade->MaskAt(index), &held_grade->MaskAt(index));
  }

  MaskDragTickCost cost;
  cost.timing      = MeasureMicros(iterations, tick);
  cost.allocations = CountAllocations(tick);
  std::cout << "[P2A cost] document=" << document_name
            << " masks=" << document.PrimaryGrade()->MaskCount()
            << " mask drag tick median_us=" << cost.timing.median
            << " min_us=" << cost.timing.minimum << " allocations=" << cost.allocations << '\n';
  return cost;
}

}  // namespace

TEST(PipelineDocumentCopyCost, MaskDragTickCostDoesNotGrowWithMaskCount) {
  const auto eight      = MeasureMaskDragTick("8_masks", MakePrimaryGradeMaskDocument(8), 200);
  const auto thirty_two = MeasureMaskDragTick("32_masks", MakePrimaryGradeMaskDocument(32), 200);
  const auto many_masks = MeasureMaskDragTick("many_masks", MakeManyMaskDocument(), 200);

  // Same document shape, 8 versus 32 Masks: the tick must allocate exactly the same. The many
  // Mask document has four Grades and longer Mask IDs, so it is compared with the limit only.
  if (eight.allocations >= 0) {
    EXPECT_EQ(eight.allocations, thirty_two.allocations);
    EXPECT_LE(static_cast<std::size_t>(many_masks.allocations), kFreezeTickAllocationLimit);
  }
  EXPECT_LE(many_masks.timing.median, kFreezeTickMedianLimitMicros);
}

TEST(PipelineDocumentCopyCost, DefaultDocumentFreezeAndOneSliderEditStayWithinLimit) {
  RequireFreezeTickWithinLimit("default", CreateDefaultPipelineDocument(), 200);
}

TEST(PipelineDocumentCopyCost, ManyMaskDocumentFreezeAndOneSliderEditStayWithinLimit) {
  RequireFreezeTickWithinLimit("many_masks", MakeManyMaskDocument(), 200);
}

TEST(PipelineDocumentCopyCost, DefaultDocumentCloneKeyAndPackingAreCorrectAndReported) {
  const auto document = CreateDefaultPipelineDocument();
  ASSERT_EQ(document.Graph().Nodes().size(), 3u);
  ReportCost("default", document, 200);
}

TEST(PipelineDocumentCopyCost, ManyMaskDocumentCloneKeyAndPackingAreCorrectAndReported) {
  const auto document = MakeManyMaskDocument();
  ASSERT_EQ(ColorGradesOnImageBackbone(document).size(), 4u);
  for (const auto* grade : ColorGradesOnImageBackbone(document)) {
#ifdef ALCEDO_ENABLE_BRUSH_MASK
    ASSERT_GE(grade->MaskCount(), 32u);
#else
    ASSERT_EQ(grade->MaskCount(), 32u);
#endif
  }
  ReportCost("many_masks", document, 50);
}

}  // namespace alcedo
