//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/result_content_key.hpp"

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"

namespace alcedo {
namespace {

auto MixExtent(ContentHash& hash, Extent2D extent) -> void {
  hash.MixU32(extent.width);
  hash.MixU32(extent.height);
}

auto MixRect(ContentHash& hash, RectI rect) -> void {
  hash.MixI32(rect.x);
  hash.MixI32(rect.y);
  hash.MixI32(rect.width);
  hash.MixI32(rect.height);
}

auto MixMatrix(ContentHash& hash, const Matrix3x3& matrix) -> void {
  for (float value : matrix.m) {
    hash.MixF32(value);
  }
}

auto MixPreparedSource(ContentHash& hash, const PreparedSourceKey& key) -> void {
  hash.MixU64(key.encoded_content_hash);
  hash.MixU64(key.encoded_byte_count);
  hash.MixU32(static_cast<std::uint32_t>(key.input_kind));
  hash.MixU64(key.cfa_hash);
  hash.MixU64(key.dng_warp_hash);
  hash.MixU32(key.downsample_passes);
  MixRect(hash, key.sensor_active_area);
  hash.MixI32(key.orientation_flip);
  hash.MixU32(key.preparation_version);
}

auto MixLinearization(ContentHash& hash, const RawLinearizationParams& linearization) -> void {
  for (float value : linearization.black_level) {
    hash.MixF32(value);
  }
  for (float value : linearization.white_level) {
    hash.MixF32(value);
  }
  for (float value : linearization.cam_mul) {
    hash.MixF32(value);
  }
  hash.MixI32(linearization.apply_as_shot_wb);
  hash.MixI32(linearization.black_tile_width);
  hash.MixI32(linearization.black_tile_height);
  for (float value : linearization.pattern_black) {
    hash.MixF32(value);
  }
}

auto MixSensorDevelopParams(ContentHash& hash, const DevelopPayload& params) -> void {
  hash.MixText(params.demosaic_method);
  hash.MixBool(params.highlights_reconstruct);
  hash.MixBool(params.lens_enabled);
  hash.MixBool(params.apply_vignetting);
  hash.MixBool(params.apply_distortion);
  hash.MixBool(params.apply_tca);
  hash.MixBool(params.apply_crop);
  hash.MixBool(params.auto_scale);
  hash.MixBool(params.use_user_scale);
  hash.MixF32(params.user_scale);
  hash.MixBool(params.projection_enabled);
  hash.MixText(params.target_projection);
  hash.MixText(params.lens_profile_db_path);
}

auto MixCameraColorParams(ContentHash& hash, const DevelopPayload& params) -> void {
  hash.MixBool(params.use_camera_wb);
  hash.MixF32(params.user_wb);
  hash.MixText(params.wb_mode);
  hash.MixF32(params.custom_cct);
  hash.MixF32(params.custom_tint);
  hash.MixF32(params.as_shot_cct);
  hash.MixF32(params.as_shot_tint);
  const auto& profile = params.camera_profile;
  hash.MixBool(profile.color_matrices_valid);
  hash.MixBool(profile.forward_matrices_valid);
  hash.MixBool(profile.as_shot_neutral_valid);
  hash.MixBool(profile.calibration_illuminants_valid);
  for (double value : profile.color_matrix_1) {
    hash.MixF64(value);
  }
  for (double value : profile.color_matrix_2) {
    hash.MixF64(value);
  }
  for (double value : profile.forward_matrix_1) {
    hash.MixF64(value);
  }
  for (double value : profile.forward_matrix_2) {
    hash.MixF64(value);
  }
  for (double value : profile.as_shot_neutral) {
    hash.MixF64(value);
  }
  for (float value : profile.cam_mul) {
    hash.MixF32(value);
  }
  hash.MixF64(profile.color_matrix_1_cct);
  hash.MixF64(profile.color_matrix_2_cct);
}

auto MixGrade(ContentHash& hash, const ColorGradeNodeModel& grade) -> void {
  hash.MixBool(grade.Enabled());
  hash.MixF32(grade.Mix());
  hash.MixU64(grade.AdjustmentCount());
  for (std::size_t index = 0; index < grade.AdjustmentCount(); ++index) {
    hash.MixText(grade.AdjustmentIdAt(index).Value());
    hash.MixText(grade.AdjustmentAt(index).Type().Text());
    hash.MixText(grade.AdjustmentAt(index).ToJson().dump());
  }
}

}  // namespace

auto HashPreparedSourceKey(const PreparedSourceKey& key) -> ContentKey {
  ContentHash hash;
  MixPreparedSource(hash, key);
  return hash.Key();
}

auto HashResolvedRenderGeometry(const ResolvedRenderGeometry& geometry) -> ContentKey {
  ContentHash hash;
  MixExtent(hash, geometry.decoded_extent);
  MixExtent(hash, geometry.full_reference_extent);
  MixExtent(hash, geometry.edit_extent);
  MixExtent(hash, geometry.render_extent);
  MixMatrix(hash, geometry.decoded_to_reference);
  MixMatrix(hash, geometry.reference_to_edit);
  MixMatrix(hash, geometry.edit_to_render);
  MixMatrix(hash, geometry.reference_to_render);
  MixMatrix(hash, geometry.render_to_reference);
  MixMatrix(hash, geometry.render_to_decoded);
  MixRect(hash, geometry.required_decoded_region);
  MixRect(hash, geometry.required_reference_region);
  hash.MixU32(static_cast<std::uint32_t>(geometry.filter));
  hash.MixU32(geometry.gpu_data.decoded_width);
  hash.MixU32(geometry.gpu_data.decoded_height);
  hash.MixU32(geometry.gpu_data.render_width);
  hash.MixU32(geometry.gpu_data.render_height);
  hash.MixU32(geometry.gpu_data.filter);
  for (float value : geometry.gpu_data.render_to_decoded) {
    hash.MixF32(value);
  }
  for (float value : geometry.gpu_data.border_rgba) {
    hash.MixF32(value);
  }
  hash.MixU32(kGeometryImplementationVersion);
  return hash.Key();
}

auto BuildFrameResultContentKeys(const ExecutionPlan& plan, const PreparedRawInput& input,
                                 const PipelineDocument& document) -> FrameResultContentKeys {
  FrameResultContentKeys keys;
  keys.sensor_extent = ImageExtent{plan.source.develop_output_extent.width,
                                   plan.source.develop_output_extent.height};
  keys.geometry_extent =
      ImageExtent{plan.geometry.render_extent.width, plan.geometry.render_extent.height};

  const auto* develop = document.Develop();
  const auto  params  = develop == nullptr ? DevelopPayload{} : develop->Params().Params();

  ContentHash sensor;
  MixPreparedSource(sensor, input.source_key);
  MixLinearization(sensor, input.linearization);
  MixSensorDevelopParams(sensor, params);
  sensor.MixU32(static_cast<std::uint32_t>(input.input_kind));
  sensor.MixU32(kSensorDevelopImplementationVersion);
  keys.sensor_linear = sensor.Key();

  ContentHash geometry;
  geometry.MixKey(keys.sensor_linear);
  geometry.MixKey(HashResolvedRenderGeometry(plan.geometry));
  keys.geometry_scene_source = geometry.Key();

  ContentHash camera;
  camera.MixKey(keys.geometry_scene_source);
  MixCameraColorParams(camera, params);
  camera.MixU32(kCameraColorImplementationVersion);
  keys.develop_image = camera.Key();

  if (plan.primary_grade_mask) {
    ContentHash mask;
    mask.MixKey(keys.geometry_scene_source);
    mask.MixText(plan.primary_grade_mask->node_id.Value());
    mask.MixU32(static_cast<std::uint32_t>(plan.primary_grade_mask->kind));
    mask.MixU32(kMaskImplementationVersion);
    keys.mask = mask.Key();
  }

  ContentHash grade;
  grade.MixKey(keys.develop_image);
  if (document.PrimaryGrade() != nullptr) {
    MixGrade(grade, *document.PrimaryGrade());
  }
  grade.MixKey(keys.mask);
  grade.MixU32(kPrimaryGradeImplementationVersion);
  keys.primary_grade = grade.Key();

  ContentHash drt;
  drt.MixKey(keys.primary_grade);
  if (document.Drt() != nullptr) {
    drt.MixText(document.Drt()->Params().ToJson().dump());
  }
  drt.MixU32(kDrtImplementationVersion);
  keys.drt_display = drt.Key();
  return keys;
}

}  // namespace alcedo
