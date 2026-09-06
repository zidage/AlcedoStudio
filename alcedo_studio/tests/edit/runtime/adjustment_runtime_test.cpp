//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/adjustment_runtime.hpp"

#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/runtime/byte_range.hpp"
#include "edit/runtime/grade_parameter_slot.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"

namespace alcedo {
namespace {

auto MakeGeometry(std::uint32_t extent, const ResolutionRequest& resolution = {},
                  const ViewRequest& view = {}) -> ResolvedRenderGeometry {
  return ResolveRenderGeometry(MakeSourceGeometry({extent, extent}, {extent, extent}), {}, view,
                               resolution, {});
}

}  // namespace

TEST(GpuDagAdjustmentRuntime, SharpenAndClarityRadiiScaleWithPreviewResolution) {
  SharpenModel sharpen;
  sharpen.SetAmount(100.0f);
  sharpen.SetRadius(8.0f);
  ClarityModel clarity;
  clarity.SetValue(80.0f);

  const auto full = MakeGeometry(256);
  const auto full_sharpen =
      MakeGradeNeighborParams(sharpen, AdjustmentBehavior::Sharpen, full);
  const auto full_clarity =
      MakeGradeNeighborParams(clarity, AdjustmentBehavior::Clarity, full);

  ResolutionRequest half;
  half.render_scale = 0.5f;
  const auto half_geometry = MakeGeometry(256, half);
  const auto half_sharpen =
      MakeGradeNeighborParams(sharpen, AdjustmentBehavior::Sharpen, half_geometry);
  const auto half_clarity =
      MakeGradeNeighborParams(clarity, AdjustmentBehavior::Clarity, half_geometry);

  EXPECT_EQ(full.render_extent, (Extent2D{256, 256}));
  EXPECT_EQ(half_geometry.render_extent, (Extent2D{128, 128}));
  EXPECT_NEAR(full_sharpen.sigma_x, 8.0f, 1.0e-5f);
  EXPECT_NEAR(half_sharpen.sigma_x, 4.0f, 1.0e-4f);
  EXPECT_EQ(full_sharpen.radius, 15U);
  EXPECT_EQ(half_sharpen.radius, 12U);
  EXPECT_NEAR(half_clarity.sigma_x, full_clarity.sigma_x * 0.5f, 1.0e-4f);
  EXPECT_LT(half_clarity.radius, full_clarity.radius);
  EXPECT_GT(half_clarity.radius, 0U);
}

TEST(GpuDagAdjustmentRuntime, HalationAndFilmGrainNeighborhoodsScaleWithMaxEdge) {
  HalationModel halation;
  halation.SetValue(1.0f);
  FilmGrainModel grain;
  grain.SetValue(0.75f);

  const auto full = MakeGeometry(256);
  const auto full_halation =
      MakeGradeNeighborParams(halation, AdjustmentBehavior::Halation, full);
  const auto full_grain =
      MakeGradeNeighborParams(grain, AdjustmentBehavior::FilmGrain, full);

  ResolutionRequest preview;
  preview.max_edge = 64;
  const auto preview_geometry = MakeGeometry(256, preview);
  const auto preview_halation =
      MakeGradeNeighborParams(halation, AdjustmentBehavior::Halation, preview_geometry);
  const auto preview_grain =
      MakeGradeNeighborParams(grain, AdjustmentBehavior::FilmGrain, preview_geometry);

  EXPECT_EQ(preview_geometry.render_extent, (Extent2D{64, 64}));
  EXPECT_NEAR(full_halation.sigma_x, 7.0f, 1.0e-5f);
  EXPECT_NEAR(preview_halation.sigma_x, 1.75f, 1.0e-4f);
  EXPECT_NEAR(preview_halation.sigma_y, preview_halation.sigma_x, 1.0e-5f);
  EXPECT_NEAR(preview_grain.sigma_x, full_grain.sigma_x * 0.25f, 1.0e-4f);
  EXPECT_LT(preview_grain.radius, full_grain.radius);
  EXPECT_EQ(full_grain.radius, 3U);
  EXPECT_EQ(preview_grain.tap_count, preview_grain.radius + 1U);
}

TEST(GpuDagAdjustmentRuntime, NativeViewportRoiKeepsFullReferenceNeighborhoodSize) {
  SharpenModel sharpen;
  sharpen.SetAmount(50.0f);
  sharpen.SetRadius(6.0f);

  const auto full = MakeGradeNeighborParams(sharpen, AdjustmentBehavior::Sharpen, MakeGeometry(256));

  ViewRequest view;
  view.visible_rect_in_edit_space = {0.25f, 0.25f, 0.5f, 0.5f};
  view.viewport_extent            = {128, 128};
  const auto roi =
      MakeGradeNeighborParams(sharpen, AdjustmentBehavior::Sharpen, MakeGeometry(256, {}, view));

  EXPECT_NEAR(roi.sigma_x, full.sigma_x, 1.0e-4f);
  EXPECT_EQ(roi.radius, full.radius);
}

class DtoOnlyExposureModel : public IOperatorModel {
 public:
  mutable int dto_reads = 0;

  auto Type() const -> OperatorTypeId override { return value_.Type(); }
  auto IsDefault() const -> bool override { return value_.IsDefault(); }
  auto IsDirty() const -> bool override { return value_.IsDirty(); }
  auto MakeFullDto() const -> OperatorParamDto override {
    ++dto_reads;
    return value_.MakeFullDto();
  }
  auto TakeDirtyPatch() -> std::optional<OperatorParamPatchDto> override {
    return value_.TakeDirtyPatch();
  }
  void RestoreDirty(DirtyFieldMask fields) override { value_.RestoreDirty(fields); }
  void MarkAllDirty() override { value_.MarkAllDirty(); }
  auto ToJson() const -> nlohmann::json override { return value_.ToJson(); }
  void LoadJson(const nlohmann::json& json) override { value_.LoadJson(json); }

 private:
  ExposureModel value_;
};

struct HostParameterBackend {
  struct Buffer {};
  struct CommandContext {};

  auto CreateBuffer(std::size_t) -> Buffer { return {}; }
  void UploadBufferRange(Buffer&, std::uint32_t offset, std::span<const std::byte> data,
                         CommandContext&) {
    last_uploads.push_back(ByteRange{offset, static_cast<std::uint32_t>(data.size())});
  }
  void DownloadBufferRange(const Buffer&, std::uint32_t, std::span<std::byte>,
                           CommandContext&) const {}
  [[nodiscard]] auto HasInFlightSubmission() const -> bool { return false; }
  void               NoteHostToDeviceBegin() { last_uploads.clear(); }

  std::vector<ByteRange> last_uploads;
};

auto SlotParams(const ParameterArena<HostParameterBackend>& arena, const ParameterSlotKey& key)
    -> GradeAdjustmentParams {
  const auto& binding = arena.Binding(key);
  GradeAdjustmentParams packed{};
  std::memcpy(&packed, arena.HostSpan().data() + binding.offset, sizeof(packed));
  return packed;
}

TEST(GpuDagAdjustmentRuntime, PackedGradeParamsMatchOwnerFieldsWithoutFullDtoCopy) {
  OperatorModelFullDtoCopyCount::Reset();

  ExposureModel exposure;
  exposure.SetValue(1.25f);
  const auto packed_exposure =
      MakeGradeRuntimeParams(exposure, AdjustmentBehavior::Exposure);
  EXPECT_EQ(packed_exposure.behavior, static_cast<std::uint32_t>(AdjustmentBehavior::Exposure));
  EXPECT_FLOAT_EQ(packed_exposure.values[0], 1.25f);

  Cat02WhiteBalanceModel wb;
  wb.SetEnabled(false);
  wb.SetTemperatureOffset(12.0f);
  wb.SetTintOffset(-3.0f);
  const auto packed_wb = MakeGradeRuntimeParams(wb, AdjustmentBehavior::Cat02WhiteBalance);
  EXPECT_FLOAT_EQ(packed_wb.values[0], 0.0f);
  EXPECT_FLOAT_EQ(packed_wb.values[1], 12.0f);
  EXPECT_FLOAT_EQ(packed_wb.values[2], -3.0f);

  CurveModel curve;
  curve.SetPoints({{0.0f, 0.0f}, {0.5f, 0.6f}, {1.0f, 1.0f}});
  const auto packed_curve = MakeGradeRuntimeParams(curve, AdjustmentBehavior::Curve);
  EXPECT_EQ(packed_curve.count, 3U);
  EXPECT_FLOAT_EQ(packed_curve.values[0], 0.0f);
  EXPECT_FLOAT_EQ(packed_curve.values[1], 0.0f);
  EXPECT_FLOAT_EQ(packed_curve.values[2], 0.5f);
  EXPECT_FLOAT_EQ(packed_curve.values[3], 0.6f);

  HlsModel hls;
  auto     table = hls.AdjustmentTable();
  table[0]       = {0.1f, 0.2f, 0.3f};
  table[7]       = {-0.2f, 0.4f, -0.1f};
  HlsUpdate hls_update;
  hls_update.hls_adj_table = table;
  hls.ApplyUpdate(hls_update);
  const auto packed_hls = MakeGradeRuntimeParams(hls, AdjustmentBehavior::Hls);
  EXPECT_FLOAT_EQ(packed_hls.values[0], 0.1f);
  EXPECT_FLOAT_EQ(packed_hls.values[8], 0.2f);
  EXPECT_FLOAT_EQ(packed_hls.values[16], 0.3f);
  EXPECT_FLOAT_EQ(packed_hls.values[7], -0.2f);
  EXPECT_FLOAT_EQ(packed_hls.values[15], 0.4f);
  EXPECT_FLOAT_EQ(packed_hls.values[23], -0.1f);

  ColorWheelModel        wheel;
  ColorWheelUpdate       wheel_update;
  ColorWheelControlUpdate lift_update;
  lift_update.color_offset      = Vec3f{0.1f, 0.2f, 0.3f};
  lift_update.luminance_offset  = 0.05f;
  ColorWheelControlUpdate gain_update;
  gain_update.color_offset     = Vec3f{1.1f, 0.9f, 0.8f};
  gain_update.luminance_offset = -0.2f;
  wheel_update.lift            = lift_update;
  wheel_update.gain            = gain_update;
  wheel.ApplyUpdate(wheel_update);
  const auto packed_wheel = MakeGradeRuntimeParams(wheel, AdjustmentBehavior::ColorWheel);
  EXPECT_FLOAT_EQ(packed_wheel.values[0], 0.1f);
  EXPECT_FLOAT_EQ(packed_wheel.values[1], 0.2f);
  EXPECT_FLOAT_EQ(packed_wheel.values[2], 0.3f);
  EXPECT_FLOAT_EQ(packed_wheel.values[3], 0.05f);
  EXPECT_FLOAT_EQ(packed_wheel.values[8], 1.1f);
  EXPECT_FLOAT_EQ(packed_wheel.values[9], 0.9f);
  EXPECT_FLOAT_EQ(packed_wheel.values[10], 0.8f);
  EXPECT_FLOAT_EQ(packed_wheel.values[11], -0.2f);

  SharpenModel sharpen;
  sharpen.SetAmount(40.0f);
  sharpen.SetRadius(5.0f);
  sharpen.SetThreshold(0.25f);
  const auto packed_sharpen = MakeGradeRuntimeParams(sharpen, AdjustmentBehavior::Sharpen);
  EXPECT_FLOAT_EQ(packed_sharpen.values[0], 40.0f);
  EXPECT_FLOAT_EQ(packed_sharpen.values[1], 5.0f);
  EXPECT_FLOAT_EQ(packed_sharpen.values[2], 0.25f);

  LmtModel lmt;
  lmt.SetCubePath("C:/looks/test.cube");
  const auto packed_lmt = MakeGradeRuntimeParams(lmt, AdjustmentBehavior::Lmt);
  EXPECT_FLOAT_EQ(packed_lmt.values[0], 1.0f);

  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
}

TEST(GpuDagAdjustmentRuntime, NeighborhoodParamsReadOwnerFieldsWithoutFullDtoCopy) {
  OperatorModelFullDtoCopyCount::Reset();
  SharpenModel sharpen;
  sharpen.SetAmount(80.0f);
  sharpen.SetRadius(6.0f);
  sharpen.SetThreshold(0.4f);
  const auto neighbor =
      MakeGradeNeighborParams(sharpen, AdjustmentBehavior::Sharpen, MakeGeometry(256));
  EXPECT_NEAR(neighbor.amount, 0.8f, 1.0e-5f);
  EXPECT_NEAR(neighbor.threshold, 0.4f, 1.0e-5f);
  EXPECT_NEAR(neighbor.sigma_x, 6.0f, 1.0e-5f);
  EXPECT_EQ(neighbor.enabled, 1U);
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
}

TEST(GpuDagAdjustmentRuntime, GradePackingRejectsDtoOnlyModelWithoutReadingFullDto) {
  DtoOnlyExposureModel fake;
  OperatorModelFullDtoCopyCount::Reset();
  EXPECT_THROW(MakeGradeRuntimeParams(fake, AdjustmentBehavior::Exposure), std::runtime_error);
  EXPECT_EQ(fake.dto_reads, 0);
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
}

TEST(GpuDagAdjustmentRuntime, GradePackingRejectsMismatchedModelTypeWithoutFullDtoCopy) {
  ContrastModel contrast;
  contrast.SetValue(20.0f);
  OperatorModelFullDtoCopyCount::Reset();
  EXPECT_THROW(MakeGradeRuntimeParams(contrast, AdjustmentBehavior::Exposure), std::runtime_error);
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
}

TEST(GpuDagAdjustmentRuntime, GradeRuntimeSlotWritesPackedBytesOnlyWhenDirty) {
  HostParameterBackend backend;
  ParameterArena<HostParameterBackend> arena(backend);
  ParameterSlotKey key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  ExposureModel    model;
  model.SetValue(0.75f);
  OperatorModelFullDtoCopyCount::Reset();

  auto first = BindOrRefreshGradeRuntimeSlot(arena, key, model, AdjustmentBehavior::Exposure);
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(arena.HasPendingUpload());
  EXPECT_FLOAT_EQ(SlotParams(arena, key).values[0], 0.75f);
  EXPECT_FLOAT_EQ(PackedGradeControlValue(arena, key), 0.75f);

  HostParameterBackend::CommandContext context;
  arena.UploadDirty(context);
  first->Commit();
  EXPECT_FALSE(arena.HasPendingUpload());
  EXPECT_FALSE(model.IsDirty());

  auto second = BindOrRefreshGradeRuntimeSlot(arena, key, model, AdjustmentBehavior::Exposure);
  EXPECT_FALSE(second.has_value());
  EXPECT_FALSE(arena.HasPendingUpload());
  EXPECT_FLOAT_EQ(PackedGradeControlValue(arena, key), 0.75f);

  model.SetValue(1.5f);
  auto third = BindOrRefreshGradeRuntimeSlot(arena, key, model, AdjustmentBehavior::Exposure);
  ASSERT_TRUE(third.has_value());
  EXPECT_TRUE(arena.HasPendingUpload());
  EXPECT_FLOAT_EQ(SlotParams(arena, key).values[0], 1.5f);
  arena.UploadDirty(context);
  third->Commit();
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
}

TEST(GpuDagAdjustmentRuntime, WritePackedSlotRejectsSizeMismatch) {
  HostParameterBackend backend;
  ParameterArena<HostParameterBackend> arena(backend);
  ParameterSlotKey key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  const ParameterFieldBinding field{DirtyFieldMask{kGradeRuntimeParamDirtyBit}, 0, 0,
                                    kGradeRuntimeParamBytes};
  arena.BindSlot(key, kGradeRuntimeParamBytes, std::span{&field, 1});
  const float scalar = 1.0f;
  EXPECT_THROW(arena.WritePackedSlot(key, scalar), std::runtime_error);
}

}  // namespace alcedo
