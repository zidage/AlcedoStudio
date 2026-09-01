//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/result_content_key.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"

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
  hash.MixU64(profile.dng_profile ? profile.dng_profile->fingerprint : 0);
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

auto MixDrtPost(ContentHash& hash, const DrtNodeModel& drt) -> void {
  hash.MixU64(drt.AdjustmentCount());
  for (std::size_t index = 0; index < drt.AdjustmentCount(); ++index) {
    hash.MixText(drt.AdjustmentIdAt(index).Value());
    hash.MixText(drt.AdjustmentAt(index).Type().Text());
    hash.MixText(drt.AdjustmentAt(index).ToJson().dump());
  }
}

auto MixGradeExcludingLocalToneValues(ContentHash& hash, const ColorGradeNodeModel& grade) -> void {
  hash.MixBool(grade.Enabled());
  hash.MixF32(grade.Mix());
  hash.MixU64(grade.AdjustmentCount());
  for (std::size_t index = 0; index < grade.AdjustmentCount(); ++index) {
    const auto& type = grade.AdjustmentAt(index).Type();
    hash.MixText(grade.AdjustmentIdAt(index).Value());
    hash.MixText(type.Text());
    if (type == type_ids::Shadows() || type == type_ids::Highlights()) {
      continue;
    }
    hash.MixText(grade.AdjustmentAt(index).ToJson().dump());
  }
}

auto MixNormalizedRect(ContentHash& hash, NormalizedRect rect) -> void {
  hash.MixF32(rect.x);
  hash.MixF32(rect.y);
  hash.MixF32(rect.w);
  hash.MixF32(rect.h);
}

auto MixCompiledMask(ContentHash& hash, const PipelineDocument& document, const CompiledMask& mask)
    -> void {
  hash.MixText(mask.owner_id.Value());
  hash.MixText(mask.mask_id.Value());
  hash.MixU32(static_cast<std::uint32_t>(mask.kind));
  const auto* grade =
      dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(mask.owner_id));
  if (grade == nullptr) {
    return;
  }
  const auto* model = grade->FindMask(mask.mask_id);
  if (model == nullptr) {
    return;
  }
  hash.MixBool(model->enabled);
  hash.MixF32(model->opacity);
  hash.MixBool(model->invert);
  hash.MixU32(static_cast<std::uint32_t>(GetMaskSourceKind(model->source)));
  hash.MixBool(model->color_range.has_value());
  hash.MixBool(model->luminance_range.has_value());
  if (const auto* brush = std::get_if<BrushMaskSource>(&model->source)) {
    if (brush->asset_key.has_value()) {
      hash.MixText(brush->asset_key->Value());
    }
    hash.MixU32(brush->descriptor.extent.width);
    hash.MixU32(brush->descriptor.extent.height);
    MixNormalizedRect(hash, brush->descriptor.reference_bounds);
    hash.MixF32(brush->feather_radius);
    return;
  }
  if (const auto* radial = std::get_if<RadialMaskSource>(&model->source)) {
    hash.MixF32(radial->center_x);
    hash.MixF32(radial->center_y);
    hash.MixF32(radial->major_radius);
    hash.MixF32(radial->minor_radius);
    hash.MixF32(radial->rotation);
    hash.MixF32(radial->inner_feather);
    hash.MixF32(radial->outer_feather);
    return;
  }
  if (const auto* gradient = std::get_if<LinearGradientMaskSource>(&model->source)) {
    hash.MixF32(gradient->origin_x);
    hash.MixF32(gradient->origin_y);
    hash.MixF32(gradient->normal_x);
    hash.MixF32(gradient->normal_y);
    hash.MixF32(gradient->transition_distance);
    hash.MixF32(gradient->start_value);
    hash.MixF32(gradient->end_value);
  }
}

auto MixLlfSharedIdentity(ContentHash& hash, const ExecutionPlan& plan,
                          const PreparedRawInput& input, const PipelineDocument& document) -> void {
  MixPreparedSource(hash, input.source_key);
  MixExtent(hash, plan.geometry.full_reference_extent);
  MixExtent(hash, plan.geometry.edit_extent);
  MixMatrix(hash, plan.geometry.reference_to_edit);
  MixExtent(hash, plan.source.host_extent);
  MixExtent(hash, plan.source.develop_output_extent);
  MixExtent(hash, plan.source.full_reference_extent);
  hash.MixU32(static_cast<std::uint32_t>(plan.source.kind));
  hash.MixU32(plan.source.downsample_passes);
  MixRect(hash, plan.source.sensor_active_area);
  const auto* develop = document.Develop();
  MixCameraColorParams(hash, develop == nullptr ? DevelopPayload{} : develop->Params().Params());
}

auto RequireCompiledGrade(const ExecutionPlan& plan, const PipelineDocument& document,
                          const NodeId& grade_id) -> const ColorGradeNodeModel& {
  if (plan.FindGrade(grade_id) == nullptr) {
    throw std::runtime_error("HashLlf: compiled Color Grade '" + std::string{grade_id.Value()} +
                             "' is missing");
  }
  const auto* grade =
      dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(grade_id));
  if (grade == nullptr) {
    throw std::runtime_error("HashLlf: Color Grade '" + std::string{grade_id.Value()} +
                             "' is missing from the document");
  }
  return *grade;
}

auto MixLlfGradeChain(ContentHash& hash, const ExecutionPlan& plan,
                      const PipelineDocument& document, const NodeId& grade_id,
                      bool include_local_tone_values) -> void {
  RequireCompiledGrade(plan, document, grade_id);
  for (const auto& compiled : plan.grade_nodes) {
    const auto* grade =
        dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(compiled.node_id));
    if (grade == nullptr) {
      throw std::runtime_error("HashLlf: Color Grade '" + std::string{compiled.node_id.Value()} +
                               "' is missing from the document");
    }
    hash.MixText(compiled.node_id.Value());
    if (compiled.mask.has_value()) {
      MixCompiledMask(hash, document, *compiled.mask);
    }
    const bool target = compiled.node_id == grade_id;
    if (target && !include_local_tone_values) {
      MixGradeExcludingLocalToneValues(hash, *grade);
    } else {
      MixGrade(hash, *grade);
    }
    if (target) {
      return;
    }
  }
}

}  // namespace

auto HashPreparedSourceKey(const PreparedSourceKey& key) -> ContentKey {
  ContentHash hash;
  MixPreparedSource(hash, key);
  return hash.Key();
}

auto HashLlfReferenceKey(const ExecutionPlan& plan, const PreparedRawInput& input,
                         const PipelineDocument& document, const NodeId& grade_id) -> ContentKey {
  ContentHash hash;
  MixLlfSharedIdentity(hash, plan, input, document);
  MixLlfGradeChain(hash, plan, document, grade_id, /*include_local_tone_values=*/true);
  hash.MixU32(kLlfReferenceImplementationVersion);
  return hash.Key();
}

auto HashLlfReferenceKey(const ExecutionPlan& plan, const PreparedRawInput& input,
                         const PipelineDocument& document) -> ContentKey {
  if (plan.FirstGrade() == nullptr) {
    ContentHash hash;
    MixLlfSharedIdentity(hash, plan, input, document);
    hash.MixU32(kLlfReferenceImplementationVersion);
    return hash.Key();
  }
  return HashLlfReferenceKey(plan, input, document, plan.FirstGrade()->node_id);
}

auto HashLlfSourceKey(const ExecutionPlan& plan, const PreparedRawInput& input,
                      const PipelineDocument& document, const NodeId& grade_id) -> ContentKey {
  ContentHash hash;
  MixLlfSharedIdentity(hash, plan, input, document);
  MixLlfGradeChain(hash, plan, document, grade_id, /*include_local_tone_values=*/false);
  hash.MixU32(kLlfReferenceImplementationVersion);
  return hash.Key();
}

auto HashLlfSourceKey(const ExecutionPlan& plan, const PreparedRawInput& input,
                      const PipelineDocument& document) -> ContentKey {
  if (plan.FirstGrade() == nullptr) {
    ContentHash hash;
    MixLlfSharedIdentity(hash, plan, input, document);
    hash.MixU32(kLlfReferenceImplementationVersion);
    return hash.Key();
  }
  return HashLlfSourceKey(plan, input, document, plan.FirstGrade()->node_id);
}

auto HashBoundInputs(std::span<const CompiledPassInput>        inputs,
                     const std::map<GraphValueId, ContentKey>& produced) -> ContentKey {
  std::vector<CompiledPassInput> ordered(inputs.begin(), inputs.end());
  std::sort(ordered.begin(), ordered.end(),
            [](const CompiledPassInput& lhs, const CompiledPassInput& rhs) {
              return lhs.port < rhs.port;
            });
  ContentHash hash;
  hash.MixU64(ordered.size());
  for (const auto& input : ordered) {
    hash.MixText(input.port.Value());
    const auto it = produced.find(input.source);
    if (it == produced.end()) {
      throw std::runtime_error("HashBoundInputs: missing producer key for " +
                               std::string{input.source.producer.Value()} + "." +
                               std::string{input.source.output_port.Value()});
    }
    hash.MixKey(it->second);
  }
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
  keys.values[plan.sensor_linear_output] = keys.sensor_linear;
  keys.values[plan.geometry_output]      = keys.geometry_scene_source;
  keys.values[plan.develop_output]       = keys.develop_image;

  ContentKey scene = keys.develop_image;
  for (const auto& compiled : plan.grade_nodes) {
    ContentKey mask_key{};
    if (compiled.mask.has_value()) {
      ContentHash mask;
      mask.MixKey(keys.geometry_scene_source);
      MixCompiledMask(mask, document, *compiled.mask);
      mask.MixU32(kMaskImplementationVersion);
      mask_key = mask.Key();
      keys.values[compiled.mask_output] = mask_key;
      if (keys.mask.Empty()) {
        keys.mask = mask_key;
      }
    }
    const auto* grade =
        dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(compiled.node_id));
    if (grade == nullptr) {
      throw std::runtime_error("BuildFrameResultContentKeys: Color Grade '" +
                               std::string{compiled.node_id.Value()} +
                               "' is missing from the document");
    }
    ContentHash grade_hash;
    grade_hash.MixKey(scene);
    MixGrade(grade_hash, *grade);
    grade_hash.MixKey(mask_key);
    grade_hash.MixU32(kPrimaryGradeImplementationVersion);
    scene = grade_hash.Key();
    keys.values[compiled.scene_output] = scene;
    if (keys.primary_grade.Empty()) {
      keys.primary_grade = scene;
    }
  }
  if (keys.primary_grade.Empty()) {
    ContentHash grade;
    grade.MixKey(keys.develop_image);
    grade.MixKey(keys.mask);
    grade.MixU32(kPrimaryGradeImplementationVersion);
    keys.primary_grade = grade.Key();
  }

  ContentHash drt;
  drt.MixKey(scene);
  if (document.Drt() != nullptr) {
    drt.MixText(document.Drt()->Params().ToJson().dump());
    MixDrtPost(drt, *document.Drt());
  }
  if (plan.output_color_override.has_value()) {
    drt.MixU32(static_cast<std::uint32_t>(plan.output_color_override->encoding_space));
    drt.MixU32(static_cast<std::uint32_t>(plan.output_color_override->encoding_eotf));
    drt.MixF32(plan.output_color_override->peak_luminance);
  } else {
    drt.MixU32(0);
  }
  drt.MixU32(kDrtImplementationVersion);
  keys.drt_display = drt.Key();
  keys.values[plan.display_output] = keys.drt_display;
  return keys;
}

}  // namespace alcedo
