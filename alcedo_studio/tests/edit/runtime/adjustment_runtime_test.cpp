//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/adjustment_runtime.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/operators/models/tone_curve_spline.hpp"
#include "edit/runtime/byte_range.hpp"
#include "edit/runtime/camera_color_gpu_params.hpp"
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

  const auto full         = MakeGeometry(256);
  const auto full_sharpen = MakeGradeNeighborParams(sharpen, AdjustmentBehavior::Sharpen, full);
  const auto full_clarity = MakeGradeNeighborParams(clarity, AdjustmentBehavior::Clarity, full);

  ResolutionRequest half;
  half.render_scale        = 0.5f;
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

TEST(GpuDagAdjustmentRuntime, HalationNeighborhoodScalesWithMaxEdge) {
  HalationModel halation;
  halation.SetValue(1.0f);

  const auto full          = MakeGeometry(256);
  const auto full_halation = MakeGradeNeighborParams(halation, AdjustmentBehavior::Halation, full);

  ResolutionRequest preview;
  preview.max_edge            = 64;
  const auto preview_geometry = MakeGeometry(256, preview);
  const auto preview_halation =
      MakeGradeNeighborParams(halation, AdjustmentBehavior::Halation, preview_geometry);

  EXPECT_EQ(preview_geometry.render_extent, (Extent2D{64, 64}));
  EXPECT_NEAR(full_halation.sigma_x, 7.0f, 1.0e-5f);
  EXPECT_NEAR(preview_halation.sigma_x, 1.75f, 1.0e-4f);
  EXPECT_NEAR(preview_halation.sigma_y, preview_halation.sigma_x, 1.0e-5f);
}

TEST(GpuDagAdjustmentRuntime, NativeViewportRoiKeepsFullReferenceNeighborhoodSize) {
  SharpenModel sharpen;
  sharpen.SetAmount(50.0f);
  sharpen.SetRadius(6.0f);

  const auto full =
      MakeGradeNeighborParams(sharpen, AdjustmentBehavior::Sharpen, MakeGeometry(256));

  ViewRequest view;
  view.visible_rect_in_edit_space = {0.25f, 0.25f, 0.5f, 0.5f};
  view.viewport_extent            = {128, 128};
  const auto roi =
      MakeGradeNeighborParams(sharpen, AdjustmentBehavior::Sharpen, MakeGeometry(256, {}, view));

  EXPECT_NEAR(roi.sigma_x, full.sigma_x, 1.0e-4f);
  EXPECT_EQ(roi.radius, full.radius);
}

// Plan 'film_grain_format_and_stock_plan.md', G4: the neighborhood parameters of Film Grain carry
// the strength for the DRT/Post schedule only. The size, the noise, and the blur of the grain come
// from the grain plan of the render (MakeFilmGrainPlan), on every backend.
TEST(GpuDagAdjustmentRuntime, FilmGrainNeighborhoodCarriesOnlyTheStrength) {
  FilmGrainModel grain;
  grain.SetValue(1.6f);
  ResolutionRequest preview;
  preview.max_edge = 64;
  for (const auto& geometry : {MakeGeometry(256), MakeGeometry(256, preview)}) {
    const auto params = MakeGradeNeighborParams(grain, AdjustmentBehavior::FilmGrain, geometry);
    EXPECT_EQ(params.behavior, static_cast<std::uint32_t>(AdjustmentBehavior::FilmGrain));
    EXPECT_EQ(params.enabled, 1U);
    EXPECT_FLOAT_EQ(params.amount, 1.6f);
    EXPECT_EQ(params.radius, 0U);
    EXPECT_EQ(params.tap_count, 0U);
  }
  grain.SetValue(0.0f);
  EXPECT_EQ(MakeGradeNeighborParams(grain, AdjustmentBehavior::FilmGrain, MakeGeometry(256)).enabled,
            0U);
}

class DtoOnlyExposureModel : public IOperatorModel {
 public:
  mutable int dto_reads = 0;

  auto        Type() const -> OperatorTypeId override { return value_.Type(); }
  auto        IsDefault() const -> bool override { return value_.IsDefault(); }
  auto        Revision() const -> ParameterRevision override { return value_.Revision(); }
  auto        FieldsRevision(DirtyFieldMask fields) const -> ParameterRevision override {
    return value_.FieldsRevision(fields);
  }
  void CopyRevisionsFrom(const IOperatorModel& source) override {
    const auto* typed = dynamic_cast<const DtoOnlyExposureModel*>(&source);
    value_.CopyRevisionsFrom(typed != nullptr ? typed->value_ : source);
  }
  auto Clone() const -> std::unique_ptr<IOperatorModel> override {
    return std::make_unique<DtoOnlyExposureModel>(*this);
  }
  auto        MakeFullDto() const -> OperatorParamDto override {
    ++dto_reads;
    return value_.MakeFullDto();
  }
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
    if (fail_upload) {
      throw std::runtime_error("HostParameterBackend: upload failed");
    }
    last_uploads.push_back(ByteRange{offset, static_cast<std::uint32_t>(data.size())});
    if (device.size() < offset + data.size()) {
      device.resize(offset + data.size());
    }
    std::memcpy(device.data() + offset, data.data(), data.size());
  }
  void                   DownloadBufferRange(const Buffer&, std::uint32_t, std::span<std::byte>,
                                             CommandContext&) const {}
  [[nodiscard]] auto     HasInFlightSubmission() const -> bool { return false; }
  void                   NoteHostToDeviceBegin() { last_uploads.clear(); }

  /// values[0] of the Grade slot that starts at @p offset in the uploaded device bytes.
  [[nodiscard]] auto     UploadedValue(std::uint32_t offset) const -> float {
    float value = 0.0f;
    std::memcpy(&value, device.data() + offset + offsetof(GradeAdjustmentParams, values),
                    sizeof(value));
    return value;
  }

  std::vector<ByteRange> last_uploads;
  std::vector<std::byte> device;
  bool                   fail_upload = false;
};

auto SlotParams(const ParameterArena<HostParameterBackend>& arena, const ParameterSlotKey& key)
    -> GradeAdjustmentParams {
  const auto&           binding = arena.Binding(key);
  GradeAdjustmentParams packed{};
  std::memcpy(&packed, arena.HostSpan().data() + binding.offset, sizeof(packed));
  return packed;
}

TEST(GpuDagAdjustmentRuntime, PackedGradeParamsMatchOwnerFieldsWithoutFullDtoCopy) {
  OperatorModelFullDtoCopyCount::Reset();

  ExposureModel exposure;
  exposure.SetValue(1.25f);
  const auto packed_exposure = MakeGradeRuntimeParams(exposure, AdjustmentBehavior::Exposure);
  EXPECT_EQ(packed_exposure.behavior, static_cast<std::uint32_t>(AdjustmentBehavior::Exposure));
  EXPECT_FLOAT_EQ(packed_exposure.values[0], 1.25f);

  Cat02WhiteBalanceModel wb;
  const auto packed_default_wb =
      MakeGradeRuntimeParams(wb, AdjustmentBehavior::Cat02WhiteBalance);
  EXPECT_EQ(packed_default_wb.behavior,
            static_cast<std::uint32_t>(AdjustmentBehavior::Cat02WhiteBalance));
  EXPECT_FLOAT_EQ(packed_default_wb.values[0], 0.0f);
  wb.ApplyUpdate(Cat02WhiteBalanceUpdate{std::nullopt, 4500.0f, -3.0f});
  const auto packed_wb      = MakeGradeRuntimeParams(wb, AdjustmentBehavior::Cat02WhiteBalance);
  const auto expected_wb    = BuildAp1Cat02WhiteBalanceMatrix(4500.0, -3.0);
  EXPECT_FLOAT_EQ(packed_wb.values[0], 1.0f);
  for (std::size_t i = 0; i < expected_wb.size(); ++i) {
    EXPECT_FLOAT_EQ(packed_wb.values[i + 1], expected_wb[i]) << i;
  }
  wb.SetEnabled(false);
  const auto packed_disabled_wb =
      MakeGradeRuntimeParams(wb, AdjustmentBehavior::Cat02WhiteBalance);
  EXPECT_FLOAT_EQ(packed_disabled_wb.values[0], 0.0f);

  CurveModel curve;
  curve.SetPoints({{0.0f, 0.0f}, {0.5f, 0.6f}, {1.0f, 1.0f}});
  const auto packed_curve = MakeCurveGradeRuntimeParams(curve)[0];
  const auto curve_spline = tone_curve::SolveToneCurveSpline(curve.Points());
  // x in values[0..15], y in values[16..31], slope in values[32..47].
  EXPECT_EQ(packed_curve.count, 3U);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(packed_curve.values[i], curve_spline.x[i]) << i;
    EXPECT_EQ(packed_curve.values[16 + i], curve_spline.y[i]) << i;
    EXPECT_EQ(packed_curve.values[32 + i], curve_spline.slope[i]) << i;
  }
  EXPECT_FLOAT_EQ(packed_curve.values[1], 0.5f);
  EXPECT_FLOAT_EQ(packed_curve.values[17], 0.6f);

  HlsModel hls;
  auto     table = hls.AdjustmentTable();
  table[0]       = {0.1f, 0.2f, 0.3f};
  table[7]       = {-0.2f, 0.4f, -0.1f};
  HlsUpdate hls_update;
  hls_update.hls_adj_table = table;
  hls.ApplyUpdate(hls_update);
  const auto packed_hls = MakeGradeRuntimeParams(hls, AdjustmentBehavior::Hls);
  EXPECT_EQ(packed_hls.count, 8U);
  EXPECT_FLOAT_EQ(packed_hls.values[0], 0.0f);
  EXPECT_FLOAT_EQ(packed_hls.values[7], 315.0f);
  EXPECT_FLOAT_EQ(packed_hls.values[8], 0.1f);
  EXPECT_FLOAT_EQ(packed_hls.values[9], 0.2f);
  EXPECT_FLOAT_EQ(packed_hls.values[10], 0.3f);
  EXPECT_FLOAT_EQ(packed_hls.values[29], -0.2f);
  EXPECT_FLOAT_EQ(packed_hls.values[30], 0.4f);
  EXPECT_FLOAT_EQ(packed_hls.values[31], -0.1f);
  EXPECT_FLOAT_EQ(packed_hls.values[32], 45.0f);
  EXPECT_FLOAT_EQ(packed_hls.values[39], 45.0f);

  ColorWheelModel         wheel;
  ColorWheelUpdate        wheel_update;
  ColorWheelControlUpdate offset_update;
  offset_update.color_offset     = Vec3f{0.5f, -0.25f, -0.25f};
  offset_update.luminance_offset = 1.0f;
  ColorWheelControlUpdate lift_update;
  lift_update.luminance_offset = 1.2f;
  ColorWheelControlUpdate gain_update;
  gain_update.luminance_offset = -0.6f;
  wheel_update.offset          = offset_update;
  wheel_update.lift            = lift_update;
  wheel_update.gain            = gain_update;
  wheel.ApplyUpdate(wheel_update);
  const auto packed_wheel = MakeGradeRuntimeParams(wheel, AdjustmentBehavior::ColorWheel);
  // Offset: ACEScc increment of (disc + Master) stops.
  EXPECT_FLOAT_EQ(packed_wheel.values[0], 1.5f / 17.52f);
  EXPECT_FLOAT_EQ(packed_wheel.values[1], 0.75f / 17.52f);
  EXPECT_FLOAT_EQ(packed_wheel.values[2], 0.75f / 17.52f);
  // Lift and Gain: stops / tone range (12 stops). Neutral Gamma: exponent 1.
  EXPECT_FLOAT_EQ(packed_wheel.values[3], 0.1f);
  EXPECT_FLOAT_EQ(packed_wheel.values[6], 1.0f);
  EXPECT_FLOAT_EQ(packed_wheel.values[9], -0.05f);
  EXPECT_FLOAT_EQ(packed_wheel.values[12], 0.41358840f - 6.0f / 17.52f);
  EXPECT_FLOAT_EQ(packed_wheel.values[13], 12.0f / 17.52f);

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
  // values[0] carries the configured strength; no reference packs 0 whatever the strength.
  lmt.SetStrength(0.35f);
  EXPECT_FLOAT_EQ(MakeGradeRuntimeParams(lmt, AdjustmentBehavior::Lmt).values[0], 0.35f);
  lmt.SetReference(std::monostate{});
  EXPECT_FLOAT_EQ(MakeGradeRuntimeParams(lmt, AdjustmentBehavior::Lmt).values[0], 0.0f);

  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
}

TEST(GpuDagAdjustmentRuntime, CurvePackingCarriesAllSixteenPointsAndRejectsSeventeen) {
  std::vector<CurvePoint> points;
  for (int i = 0; i < 16; ++i) {
    const float x = static_cast<float>(i) / 15.0f;
    points.push_back({x, x * x});
  }
  CurveModel curve;
  curve.SetPoints(points);
  const auto packed = MakeCurveGradeRuntimeParams(curve)[0];
  EXPECT_EQ(packed.count, 16U);
  for (std::size_t i = 0; i < points.size(); ++i) {
    EXPECT_EQ(packed.values[i], points[i].x) << i;
    EXPECT_EQ(packed.values[16 + i], points[i].y) << i;
  }

  // A Model can load more points than the render supports (LoadJson keeps the stored points);
  // packing names the limit instead of dropping points.
  points.push_back({1.5f, 1.5f});
  curve.SetPoints(points);
  try {
    (void)MakeCurveGradeRuntimeParams(curve);
    ADD_FAILURE() << "17 points were packed";
  } catch (const std::invalid_argument& error) {
    EXPECT_STREQ(error.what(), "Tone curve has 17 control points; a curve has 2 to 16 points");
  }

  // A channel curve has the same limit.
  curve.SetPoints({{0.0f, 0.0f}, {1.0f, 1.0f}});
  CurveUpdate update;
  update.blue = points;
  curve.ApplyUpdate(update);
  EXPECT_THROW((void)MakeCurveGradeRuntimeParams(curve), std::invalid_argument);
}

TEST(GpuDagAdjustmentRuntime, CurvePackingHasOneRecordForEachCurveAndCountZeroForIdentityChannels) {
  CurveModel  curve;
  CurveUpdate update;
  update.points = std::vector<CurvePoint>{{0.0f, 0.0f}, {0.5f, 0.6f}, {1.0f, 1.0f}};
  update.blue   = std::vector<CurvePoint>{{0.0f, 0.1f}, {0.3f, 0.25f}, {0.7f, 0.8f}, {1.0f, 0.9f}};
  curve.ApplyUpdate(update);

  const auto records = MakeCurveGradeRuntimeParams(curve);
  static_assert(sizeof(records) == 4 * sizeof(GradeAdjustmentParams));
  EXPECT_EQ(GradeRuntimeParamBytes(AdjustmentBehavior::Curve), 4 * kGradeRuntimeParamBytes);
  EXPECT_EQ(GradeRuntimeParamBytes(AdjustmentBehavior::Exposure), kGradeRuntimeParamBytes);
  for (const auto& record : records) {
    EXPECT_EQ(record.behavior, static_cast<std::uint32_t>(AdjustmentBehavior::Curve));
  }
  // The RGB curve, then red, green, and blue (CurveChannel order). The identity red and green
  // curves have count 0, which the kernels read as "leave the channel unchanged".
  EXPECT_EQ(records[0].count, 3U);
  EXPECT_EQ(records[1].count, 0U);
  EXPECT_EQ(records[2].count, 0U);
  EXPECT_EQ(records[3].count, 4U);
  const auto blue = tone_curve::SolveToneCurveSpline(*update.blue);
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(records[3].values[i], blue.x[i]) << i;
    EXPECT_EQ(records[3].values[16 + i], blue.y[i]) << i;
    EXPECT_EQ(records[3].values[32 + i], blue.slope[i]) << i;
  }
  // An identity RGB curve keeps its two points, as before the channel curves.
  CurveModel identity;
  EXPECT_EQ(MakeCurveGradeRuntimeParams(identity)[0].count, 2U);

  // The single-record packer names the four-record layout instead of packing one curve.
  EXPECT_THROW((void)MakeGradeRuntimeParams(curve, AdjustmentBehavior::Curve), std::runtime_error);
}

TEST(GpuDagAdjustmentRuntime, CurveSlotBindsFourRecordsAndRepacksOnAChannelEdit) {
  HostParameterBackend                 backend;
  ParameterArena<HostParameterBackend> arena(backend);
  const ParameterSlotKey key{NodeId{"grade.primary"}, AdjustmentInstanceId{"grade.primary.curve"}};
  CurveModel             curve;

  EXPECT_TRUE(BindOrRefreshGradeRuntimeSlot(arena, key, curve, AdjustmentBehavior::Curve));
  EXPECT_EQ(arena.Binding(key).size, 4 * kGradeRuntimeParamBytes);
  EXPECT_FALSE(BindOrRefreshGradeRuntimeSlot(arena, key, curve, AdjustmentBehavior::Curve));

  CurveUpdate update;
  update.red = std::vector<CurvePoint>{{0.0f, 0.0f}, {0.5f, 0.4f}, {1.0f, 1.0f}};
  curve.ApplyUpdate(update);
  EXPECT_TRUE(BindOrRefreshGradeRuntimeSlot(arena, key, curve, AdjustmentBehavior::Curve));
  EXPECT_EQ(arena.Binding(key).size, 4 * kGradeRuntimeParamBytes);

  GradeAdjustmentParams red{};
  std::memcpy(&red, arena.HostSpan().data() + arena.Binding(key).offset + kGradeRuntimeParamBytes,
              sizeof(red));
  EXPECT_EQ(SlotParams(arena, key).count, 2U);
  EXPECT_EQ(red.count, 3U);
  EXPECT_FLOAT_EQ(red.values[17], 0.4f);
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

TEST(GpuDagAdjustmentRuntime, GradeRuntimeSlotWritesPackedBytesOnlyWhenModelRevisionChanges) {
  HostParameterBackend                 backend;
  ParameterArena<HostParameterBackend> arena(backend);
  ParameterSlotKey key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  ExposureModel    model;
  model.SetValue(0.75f);
  OperatorModelFullDtoCopyCount::Reset();

  EXPECT_TRUE(BindOrRefreshGradeRuntimeSlot(arena, key, model, AdjustmentBehavior::Exposure));
  EXPECT_TRUE(arena.HasPendingUpload());
  EXPECT_EQ(arena.AppliedRevision(key), model.Revision());
  EXPECT_FLOAT_EQ(SlotParams(arena, key).values[0], 0.75f);
  EXPECT_FLOAT_EQ(PackedGradeControlValue(arena, key), 0.75f);

  HostParameterBackend::CommandContext context;
  arena.UploadDirty(context);
  EXPECT_FALSE(arena.HasPendingUpload());

  EXPECT_FALSE(BindOrRefreshGradeRuntimeSlot(arena, key, model, AdjustmentBehavior::Exposure));
  EXPECT_FALSE(arena.HasPendingUpload());
  EXPECT_FLOAT_EQ(PackedGradeControlValue(arena, key), 0.75f);

  model.SetValue(1.5f);
  EXPECT_TRUE(BindOrRefreshGradeRuntimeSlot(arena, key, model, AdjustmentBehavior::Exposure));
  EXPECT_TRUE(arena.HasPendingUpload());
  EXPECT_EQ(arena.AppliedRevision(key), model.Revision());
  EXPECT_FLOAT_EQ(SlotParams(arena, key).values[0], 1.5f);
  arena.UploadDirty(context);
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
}

TEST(GpuDagAdjustmentRuntime, GradeRuntimeSlotPackingInOneArenaLeavesOtherArenaStale) {
  // Two render workspaces read one Model. Packing in the first must not hide the change from
  // the second (the audit R7 defect: a one-shot render took the change from the editor).
  HostParameterBackend                 backend;
  ParameterArena<HostParameterBackend> session(backend);
  ParameterArena<HostParameterBackend> one_shot(backend);
  ParameterSlotKey key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  ExposureModel    model;
  model.SetValue(0.25f);
  EXPECT_TRUE(BindOrRefreshGradeRuntimeSlot(session, key, model, AdjustmentBehavior::Exposure));

  model.SetValue(1.25f);
  EXPECT_TRUE(BindOrRefreshGradeRuntimeSlot(one_shot, key, model, AdjustmentBehavior::Exposure));
  EXPECT_FLOAT_EQ(PackedGradeControlValue(one_shot, key), 1.25f);

  EXPECT_TRUE(BindOrRefreshGradeRuntimeSlot(session, key, model, AdjustmentBehavior::Exposure));
  EXPECT_FLOAT_EQ(PackedGradeControlValue(session, key), 1.25f);
  EXPECT_EQ(session.AppliedRevision(key), one_shot.AppliedRevision(key));
}

TEST(GpuDagAdjustmentRuntime, GradeRuntimeSlotFailedUploadKeepsPackedBytesQueued) {
  HostParameterBackend                 backend;
  ParameterArena<HostParameterBackend> arena(backend);
  ParameterSlotKey key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  ExposureModel    model;
  model.SetValue(0.5f);
  ASSERT_TRUE(BindOrRefreshGradeRuntimeSlot(arena, key, model, AdjustmentBehavior::Exposure));

  HostParameterBackend::CommandContext context;
  backend.fail_upload = true;
  EXPECT_THROW(arena.UploadDirty(context), std::runtime_error);
  EXPECT_TRUE(arena.HasPendingUpload());

  // The Model did not change, so the slot is not packed again; the retry uploads the same bytes.
  EXPECT_FALSE(BindOrRefreshGradeRuntimeSlot(arena, key, model, AdjustmentBehavior::Exposure));
  backend.fail_upload = false;
  arena.UploadDirty(context);
  EXPECT_FALSE(arena.HasPendingUpload());
  EXPECT_FLOAT_EQ(backend.UploadedValue(arena.Binding(key).offset), 0.5f);
}

TEST(GpuDagAdjustmentRuntime, WritePackedSlotRejectsSizeMismatch) {
  HostParameterBackend                 backend;
  ParameterArena<HostParameterBackend> arena(backend);
  ParameterSlotKey            key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  const ParameterFieldBinding field{DirtyFieldMask{kGradeRuntimeParamDirtyBit}, 0, 0,
                                    kGradeRuntimeParamBytes};
  arena.BindSlot(key, kGradeRuntimeParamBytes, std::span{&field, 1});
  const float scalar = 1.0f;
  EXPECT_THROW(arena.WritePackedSlot(key, scalar), std::runtime_error);
}

TEST(GpuDagAdjustmentRuntime, CameraColorPackedSlotWriteDoesNotCopyFullDto) {
  HostParameterBackend                 backend;
  ParameterArena<HostParameterBackend> arena(backend);
  ParameterSlotKey                     key{NodeId{"develop"}, kDevelopCameraColorSlot};
  CameraColorGpuParams                 params{};
  params.camera_to_ap1[0] = 1.5f;
  params.camera_to_ap1[4] = 0.8f;
  params.camera_to_ap1[8] = 1.1f;
  OperatorModelFullDtoCopyCount::Reset();
  arena.BindOrWritePackedSlot(key, DirtyFieldMask{DevelopDirty::WhiteBalance}, params);
  EXPECT_TRUE(arena.HasPendingUpload());
  EXPECT_EQ(arena.Binding(key).size, sizeof(CameraColorGpuParams));
  CameraColorGpuParams stored{};
  std::memcpy(&stored, arena.HostSpan().data() + arena.Binding(key).offset, sizeof(stored));
  EXPECT_FLOAT_EQ(stored.camera_to_ap1[0], 1.5f);
  EXPECT_FLOAT_EQ(stored.camera_to_ap1[4], 0.8f);
  EXPECT_FLOAT_EQ(stored.camera_to_ap1[8], 1.1f);
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
}

}  // namespace alcedo
