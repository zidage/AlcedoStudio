//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/runtime/compiled_mask_stack.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/gpu_node_pass_stats.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "gpu/transient_allocation_policy.hpp"
#include "multi_mask_runtime_test_support.hpp"

namespace alcedo::multi_mask_qualification {

inline constexpr std::uint32_t kResourceWidth  = 16;
inline constexpr std::uint32_t kResourceHeight = 12;

/**
 * @brief Execute/reuse and allocation snapshot for one Mask PlanExecutor run.
 *
 * Host-to-device bytes include kernel-parameter copies. Persistent and active R8
 * bytes come from the Mask texture caches. Signed-distance bytes are the node
 * value buffers used by Brush feather.
 */
struct MaskResourceSnapshot {
  int           mask_count              = 0;
  bool          sources_enabled         = true;
  std::uint64_t mask_execute            = 0;
  std::uint64_t mask_skip               = 0;
  std::uint64_t mask_union_execute      = 0;
  std::uint64_t mask_union_skip         = 0;
  std::uint64_t host_to_device_bytes    = 0;
  std::size_t   persistent_r8_bytes     = 0;
  std::size_t   active_r8_bytes         = 0;
  std::size_t   value_buffer_bytes      = 0;
  std::size_t   signed_distance_bytes   = 0;
  std::size_t   published_result_count  = 0;
  std::uint64_t completed_submission    = 0;
  double        milliseconds            = 0.0;
};

inline void PrintMaskResourceSnapshot(std::string_view label, const MaskResourceSnapshot& snap) {
  std::cout << "multi-mask resource " << label << " mask_count=" << snap.mask_count
            << " enabled=" << (snap.sources_enabled ? 1 : 0)
            << " mask_execute=" << snap.mask_execute << " mask_skip=" << snap.mask_skip
            << " union_execute=" << snap.mask_union_execute
            << " union_skip=" << snap.mask_union_skip
            << " h2d_bytes=" << snap.host_to_device_bytes
            << " persistent_r8_bytes=" << snap.persistent_r8_bytes
            << " active_r8_bytes=" << snap.active_r8_bytes
            << " value_bytes=" << snap.value_buffer_bytes
            << " sdf_bytes=" << snap.signed_distance_bytes
            << " published=" << snap.published_result_count
            << " completed=" << snap.completed_submission << " ms=" << snap.milliseconds
            << '\n';
}

inline auto MakePreparedRgb(std::uint32_t width = kResourceWidth,
                            std::uint32_t height = kResourceHeight) -> PreparedRawInput {
  return RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(width, height),
                                       gpu_dag_test::FullSensor(width, height));
}

inline auto MakeDocument() -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  return document;
}

inline void AddRadialMasks(PipelineDocument& document, int count, bool enabled) {
  auto* grade = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  for (int index = 0; index < count; ++index) {
    RadialMaskSource radial;
    radial.center_x     = 0.25f + 0.06f * static_cast<float>(index);
    radial.center_y     = 0.40f;
    radial.major_radius = 0.35f;
    radial.minor_radius = 0.28f;
    auto mask           = grade_mask_test::MakeRadialMask(
        MaskId{"mask.radial." + std::to_string(index)}, radial);
    mask.enabled = enabled;
    grade_mask_test::AddMask(*grade, std::move(mask));
  }
  document.MarkTopologyDirty();
}

template <class Device>
auto BrushSignedDistanceBytes(Device& device, const NodeId& owner, const MaskId& mask_id)
    -> std::size_t {
  const std::array<GraphValueId, 4> ids = {
      MaskSignedDistanceValue(owner, mask_id),
      MaskScratchValue(owner, mask_id, "distance.horizontal"),
      MaskScratchValue(owner, mask_id, "distance.inside"),
      MaskScratchValue(owner, mask_id, "distance.outside"),
  };
  std::size_t total = 0;
  for (const auto& id : ids) {
    const auto* buffer = device.Workspace().Values().Find(id);
    if (buffer != nullptr) {
      total += buffer->Bytes();
    }
  }
  return total;
}

inline auto StoreRoot(std::string_view suffix) -> std::filesystem::path {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  auto root = std::filesystem::path{"build/tmp/multi_mask_runtime"} / info->name();
  if (!suffix.empty()) {
    root += suffix;
  }
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  return root;
}

template <class Device>
auto CaptureSnapshot(Device& device, int mask_count, bool enabled, double milliseconds)
    -> MaskResourceSnapshot {
  device.WaitIdle();
  const auto& stats     = device.PassStats();
  auto&       workspace = device.Workspace();
  MaskResourceSnapshot snap;
  snap.mask_count             = mask_count;
  snap.sources_enabled        = enabled;
  snap.mask_execute           = stats.mask_execute;
  snap.mask_skip              = stats.mask_skip;
  snap.mask_union_execute     = stats.mask_union_execute;
  snap.mask_union_skip        = stats.mask_union_skip;
  snap.host_to_device_bytes   = workspace.Device().HostToDeviceBytes();
  snap.persistent_r8_bytes    = workspace.MaskTextures().UsedBytes();
  snap.active_r8_bytes        = workspace.ActiveRasterTextures().UsedBytes();
  snap.value_buffer_bytes     = workspace.Values().UsedBytes();
  snap.published_result_count = workspace.Images().PublishedCount();
  snap.completed_submission   = workspace.Device().CompletedSubmission();
  snap.milliseconds           = milliseconds;
  return snap;
}

template <class Device>
auto ExecuteTimed(Device& device, const ExecutionPlan& plan, PreparedRawInput& prepared,
                  PipelineDocument& document, MaskStore* store,
                  std::span<const ActiveRasterMaskInput> active = {}) -> double {
  device.ResetPassStats();
  device.Workspace().Device().ResetCounters();
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(device.Execute(plan, prepared, document, store, true,
                           TransientAllocationPolicy::SessionPacked, active),
            plan.display_output);
  device.WaitIdle();
  const auto ended = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(ended - started).count();
}

/**
 * @brief One, four, and eight enabled Radial Masks execute that many sources and one Union.
 *
 * @pre @p device is idle. Side effects: compiles and executes three independent documents.
 */
template <class Device>
void ExpectEnabledMaskCountsScaleWithMaskList(Device& device) {
  for (int count : {1, 4, 8}) {
    device.WaitIdle();
    device.Workspace().ReleaseSessionResources();
    auto prepared = MakePreparedRgb();
    auto document = MakeDocument();
    AddRadialMasks(document, count, true);
    auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), {});
    const auto cold_ms   = ExecuteTimed(device, plan, prepared, document, nullptr);
    const auto cold_snap = CaptureSnapshot(device, count, true, cold_ms);
    PrintMaskResourceSnapshot("enabled-radial-cold", cold_snap);
    EXPECT_EQ(cold_snap.mask_execute, static_cast<std::uint64_t>(count));
    EXPECT_EQ(cold_snap.mask_skip, 0U);
    EXPECT_EQ(cold_snap.mask_union_execute, 1U);
    EXPECT_EQ(cold_snap.mask_union_skip, 0U);
    EXPECT_GT(cold_snap.completed_submission, 0U);
    EXPECT_GT(cold_snap.published_result_count, 0U);

    const auto reuse_ms   = ExecuteTimed(device, plan, prepared, document, nullptr);
    const auto reuse_snap = CaptureSnapshot(device, count, true, reuse_ms);
    PrintMaskResourceSnapshot("enabled-radial-reuse", reuse_snap);
    EXPECT_EQ(reuse_snap.mask_execute, 0U);
    EXPECT_EQ(reuse_snap.mask_skip, static_cast<std::uint64_t>(count));
    EXPECT_EQ(reuse_snap.mask_union_execute, 0U);
    EXPECT_EQ(reuse_snap.mask_union_skip, 1U);
  }
}

/**
 * @brief A nonempty all-disabled list fills Union with zero and skips source kernels.
 */
template <class Device>
void ExpectDisabledMasksSkipSourceEvaluation(Device& device) {
  auto prepared = MakePreparedRgb();
  auto document = MakeDocument();
  AddRadialMasks(document, 8, false);
  auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), {});
  const auto ms   = ExecuteTimed(device, plan, prepared, document, nullptr);
  const auto snap = CaptureSnapshot(device, 8, false, ms);
  PrintMaskResourceSnapshot("all-disabled", snap);
  EXPECT_EQ(snap.mask_execute, 0U);
  EXPECT_EQ(snap.mask_union_execute, 1U);
  EXPECT_GT(snap.completed_submission, 0U);
}

inline auto MakeFilledRaster(std::uint32_t width, std::uint32_t height, std::uint8_t fill)
    -> MaskAsset {
  MaskAsset asset;
  asset.descriptor.extent = {width, height};
  asset.pixels.assign(static_cast<std::size_t>(width) * height, fill);
  return asset;
}

inline auto MakeActiveInput(const NodeId& owner, const MaskId& id, const MaskAsset& asset,
                            RectI dirty, std::uint64_t revision, std::uint64_t generation)
    -> ActiveRasterMaskInput {
  ActiveRasterMaskInput input;
  input.owner_node_id      = owner;
  input.mask_id            = id;
  input.session_generation = generation;
  input.content_revision   = revision;
  input.descriptor         = asset.descriptor;
  input.pixels             = std::make_shared<const std::vector<std::uint8_t>>(asset.pixels);
  input.dirty_rectangle    = dirty;
  return input;
}

/**
 * @brief After a session texture exists, a later revision uploads only the dirty rectangle.
 *
 * Signed-distance is not allocated when feather radius is zero. Host-to-device bytes
 * may include kernel parameters and must stay below a full R8 raster.
 */
template <class Device>
void ExpectPartialBrushUploadTransfersOnlyDirtyRectangle(Device& device) {
  MaskStore store(StoreRoot("-partial"));
  auto      prepared = MakePreparedRgb();
  auto      document = MakeDocument();
  auto      asset    = MakeFilledRaster(kResourceWidth, kResourceHeight, 40);
  asset.key          = store.Put(asset.descriptor, asset.pixels);
  grade_mask_test::AddBrushMask(document, MaskId{"mask.raster"}, asset.key, asset.descriptor);
  auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), {});
  const RectI full{0, 0, static_cast<std::int32_t>(kResourceWidth),
                   static_cast<std::int32_t>(kResourceHeight)};
  const auto first =
      MakeActiveInput(document.PrimaryGrade()->Id(), MaskId{"mask.raster"}, asset, full, 1, 1);
  const auto full_ms = ExecuteTimed(device, plan, prepared, document, &store, std::span{&first, 1});
  const auto full_snap = CaptureSnapshot(device, 1, true, full_ms);
  PrintMaskResourceSnapshot("brush-full-upload", full_snap);
  EXPECT_EQ(full_snap.mask_execute, 1U);
  EXPECT_EQ(full_snap.mask_union_execute, 1U);
  EXPECT_GT(full_snap.active_r8_bytes, 0U);
  EXPECT_EQ(BrushSignedDistanceBytes(device, document.PrimaryGrade()->Id(), MaskId{"mask.raster"}),
            0U);

  auto dirty_asset = asset;
  for (std::uint32_t y = 1; y < 5; ++y) {
    for (std::uint32_t x = 1; x < 5; ++x) {
      dirty_asset.pixels[y * kResourceWidth + x] = 200;
    }
  }
  const RectI dirty{1, 1, 4, 4};
  const auto second = MakeActiveInput(document.PrimaryGrade()->Id(), MaskId{"mask.raster"},
                                      dirty_asset, dirty, 2, 1);
  const auto dirty_ms =
      ExecuteTimed(device, plan, prepared, document, &store, std::span{&second, 1});
  const auto dirty_snap = CaptureSnapshot(device, 1, true, dirty_ms);
  PrintMaskResourceSnapshot("brush-dirty-upload", dirty_snap);
  EXPECT_EQ(dirty_snap.mask_execute, 1U);
  EXPECT_EQ(dirty_snap.mask_union_execute, 1U);
  multi_mask_test::ExpectDirtyR8RectangleUpload(device.Workspace().Device().LastTextureRectangles(),
                                                dirty, dirty_snap.host_to_device_bytes,
                                                kResourceWidth, kResourceHeight);
}

/**
 * @brief Pixel change with feather radius > 0 rebuilds the full signed-distance field.
 *
 * Dirty rectangles still limit host-to-device Brush bytes. Feather reads the whole
 * raster SDF, so the work region is the full asset extent.
 */
template <class Device>
void ExpectFeatheredBrushDirtyUpdateRecomputesFullSignedDistance(Device& device) {
  MaskStore store(StoreRoot("-feather"));
  auto      prepared = MakePreparedRgb();
  auto      document = MakeDocument();
  auto      asset    = MakeFilledRaster(kResourceWidth, kResourceHeight, 0);
  for (std::uint32_t y = 2; y < 10; ++y) {
    for (std::uint32_t x = 2; x < 10; ++x) {
      asset.pixels[y * kResourceWidth + x] = 255;
    }
  }
  asset.key = store.Put(asset.descriptor, asset.pixels);
  grade_mask_test::AddBrushMask(document, MaskId{"mask.raster"}, asset.key, asset.descriptor, 6.0f);
  auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), {});
  const RectI full{0, 0, static_cast<std::int32_t>(kResourceWidth),
                   static_cast<std::int32_t>(kResourceHeight)};
  const auto first =
      MakeActiveInput(document.PrimaryGrade()->Id(), MaskId{"mask.raster"}, asset, full, 1, 1);
  const auto first_ms =
      ExecuteTimed(device, plan, prepared, document, &store, std::span{&first, 1});
  auto first_snap = CaptureSnapshot(device, 1, true, first_ms);
  first_snap.signed_distance_bytes =
      BrushSignedDistanceBytes(device, document.PrimaryGrade()->Id(), MaskId{"mask.raster"});
  PrintMaskResourceSnapshot("feather-full", first_snap);
  const auto plane_bytes =
      static_cast<std::size_t>(kResourceWidth) * kResourceHeight * sizeof(float);
  EXPECT_GE(first_snap.signed_distance_bytes, plane_bytes);
  const auto* distance = device.Workspace().Values().Find(
      MaskSignedDistanceValue(document.PrimaryGrade()->Id(), MaskId{"mask.raster"}));
  ASSERT_NE(distance, nullptr);
  EXPECT_EQ(distance->Bytes(), plane_bytes);

  auto erased = asset;
  for (std::uint32_t y = 4; y < 8; ++y) {
    for (std::uint32_t x = 4; x < 8; ++x) {
      erased.pixels[y * kResourceWidth + x] = 0;
    }
  }
  const RectI dirty{4, 4, 4, 4};
  const auto second =
      MakeActiveInput(document.PrimaryGrade()->Id(), MaskId{"mask.raster"}, erased, dirty, 2, 1);
  const auto second_ms =
      ExecuteTimed(device, plan, prepared, document, &store, std::span{&second, 1});
  auto second_snap = CaptureSnapshot(device, 1, true, second_ms);
  second_snap.signed_distance_bytes =
      BrushSignedDistanceBytes(device, document.PrimaryGrade()->Id(), MaskId{"mask.raster"});
  PrintMaskResourceSnapshot("feather-dirty-full-sdf", second_snap);
  std::cout << "multi-mask resource feather reason=full signed-distance field; "
               "pixel change with feather_radius>0 cannot reuse the prior SDF\n";
  EXPECT_GE(second_snap.signed_distance_bytes, plane_bytes);
  multi_mask_test::ExpectDirtyR8RectangleUpload(device.Workspace().Device().LastTextureRectangles(),
                                                dirty, second_snap.host_to_device_bytes,
                                                kResourceWidth, kResourceHeight);
}

/**
 * @brief Injected upload failure discards unpublished writes and keeps prior keys.
 *
 * @pre GPU completion is required before inspecting published results.
 */
template <class Device>
void ExpectMaskUploadFailureKeepsPriorPublishedResults(Device& device) {
  MaskStore store(StoreRoot("-upload-fail"));
  auto      prepared = MakePreparedRgb();
  auto      document = MakeDocument();
  auto      first    = MakeFilledRaster(kResourceWidth, kResourceHeight, 180);
  first.key          = store.Put(first.descriptor, first.pixels);
  grade_mask_test::AddBrushMask(document, MaskId{"mask.raster"}, first.key, first.descriptor);
  auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), {});
  ASSERT_EQ(device.Execute(plan, prepared, document, &store), plan.display_output);
  device.WaitIdle();
  auto& images       = device.Workspace().Images();
  auto& invalidation = device.Workspace().ResultInvalidation();
  ASSERT_TRUE(plan.FirstGrade()->mask_stack.has_value());
  const auto source_id   = plan.FirstGrade()->mask_stack->sources[0].effective_output;
  const auto union_id    = plan.FirstGrade()->mask_output;
  const auto grade_id    = plan.FirstGrade()->scene_output;
  const auto source_rev  = images.PublishedRevision(source_id);
  const auto union_rev   = images.PublishedRevision(union_id);
  const auto grade_rev   = images.PublishedRevision(grade_id);
  const auto source_repr = images.PublishedRepresentation(source_id);
  const auto union_repr  = images.PublishedRepresentation(union_id);
  const auto grade_repr  = images.PublishedRepresentation(grade_id);
  ASSERT_NE(source_rev, 0U);

  auto second = MakeFilledRaster(kResourceWidth, kResourceHeight, 40);
  second.key  = store.Put(second.descriptor, second.pixels);
  document.PrimaryGrade()->ReplaceMaskSource(
      MaskId{"mask.raster"},
      grade_mask_test::MakeBrushMask(MaskId{"mask.raster"}, second).source);
  device.Workspace().Device().FailNextUpload();
  EXPECT_THROW((void)device.Execute(plan, prepared, document, &store), std::runtime_error);
  device.WaitIdle();
  const auto completed = device.Workspace().Device().CompletedSubmission();
  EXPECT_TRUE(images.FindValidResult(source_id, source_rev, source_repr, completed));
  EXPECT_TRUE(images.FindValidResult(union_id, union_rev, union_repr, completed));
  EXPECT_TRUE(images.FindValidResult(grade_id, grade_rev, grade_repr, completed));
  EXPECT_EQ(images.PublishedRevision(source_id), source_rev);
  EXPECT_NE(invalidation.RequiredRevision(source_id), source_rev);
}

}  // namespace alcedo::multi_mask_qualification
