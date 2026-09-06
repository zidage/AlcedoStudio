//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/develop_node_model.hpp"

#include "edit/operators/models/json_read.hpp"

namespace alcedo {

auto DevelopParamsModel::IsDefault() const -> bool {
  const auto payload = PayloadCopy();
  return payload.demosaic_method == "default" && payload.highlights_reconstruct &&
         payload.use_camera_wb && payload.user_wb == 7600.0f && payload.wb_mode == "as_shot" &&
         !payload.lens_enabled;
}

auto DevelopParamsModel::DemosaicMethod() const -> std::string {
  return Read([](const DevelopPayload& payload) { return payload.demosaic_method; });
}

auto DevelopParamsModel::HighlightsReconstruct() const -> bool {
  return Read([](const DevelopPayload& payload) { return payload.highlights_reconstruct; });
}

auto DevelopParamsModel::UseCameraWhiteBalance() const -> bool {
  return Read([](const DevelopPayload& payload) { return payload.use_camera_wb; });
}

auto DevelopParamsModel::UserWhiteBalance() const -> float {
  return Read([](const DevelopPayload& payload) { return payload.user_wb; });
}

auto DevelopParamsModel::WhiteBalanceMode() const -> std::string {
  return Read([](const DevelopPayload& payload) { return payload.wb_mode; });
}

auto DevelopParamsModel::CustomCct() const -> float {
  return Read([](const DevelopPayload& payload) { return payload.custom_cct; });
}

auto DevelopParamsModel::CustomTint() const -> float {
  return Read([](const DevelopPayload& payload) { return payload.custom_tint; });
}

auto DevelopParamsModel::AsShotCct() const -> float {
  return Read([](const DevelopPayload& payload) { return payload.as_shot_cct; });
}

auto DevelopParamsModel::AsShotTint() const -> float {
  return Read([](const DevelopPayload& payload) { return payload.as_shot_tint; });
}

auto DevelopParamsModel::LensEnabled() const -> bool {
  return Read([](const DevelopPayload& payload) { return payload.lens_enabled; });
}

auto DevelopParamsModel::ApplyVignetting() const -> bool {
  return Read([](const DevelopPayload& payload) { return payload.apply_vignetting; });
}

auto DevelopParamsModel::ApplyDistortion() const -> bool {
  return Read([](const DevelopPayload& payload) { return payload.apply_distortion; });
}

auto DevelopParamsModel::ApplyTca() const -> bool {
  return Read([](const DevelopPayload& payload) { return payload.apply_tca; });
}

auto DevelopParamsModel::ApplyCrop() const -> bool {
  return Read([](const DevelopPayload& payload) { return payload.apply_crop; });
}

auto DevelopParamsModel::AutoScale() const -> bool {
  return Read([](const DevelopPayload& payload) { return payload.auto_scale; });
}

auto DevelopParamsModel::UseUserScale() const -> bool {
  return Read([](const DevelopPayload& payload) { return payload.use_user_scale; });
}

auto DevelopParamsModel::UserScale() const -> float {
  return Read([](const DevelopPayload& payload) { return payload.user_scale; });
}

auto DevelopParamsModel::ProjectionEnabled() const -> bool {
  return Read([](const DevelopPayload& payload) { return payload.projection_enabled; });
}

auto DevelopParamsModel::TargetProjection() const -> std::string {
  return Read([](const DevelopPayload& payload) { return payload.target_projection; });
}

auto DevelopParamsModel::LensProfileDbPath() const -> std::string {
  return Read([](const DevelopPayload& payload) { return payload.lens_profile_db_path; });
}

void DevelopParamsModel::ApplyRawDecodeUpdate(DevelopRawDecodeUpdate update) {
  MutateWithDirtyFields([update = std::move(update)](DevelopPayload& payload) mutable {
    DirtyFieldMask changed;
    if (update.demosaic_method.has_value() && payload.demosaic_method != *update.demosaic_method) {
      payload.demosaic_method = std::move(*update.demosaic_method);
      changed |= DirtyFieldMask{DevelopDirty::Demosaic};
    }
    if (update.highlights_reconstruct.has_value() &&
        payload.highlights_reconstruct != *update.highlights_reconstruct) {
      payload.highlights_reconstruct = *update.highlights_reconstruct;
      changed |= DirtyFieldMask{DevelopDirty::Highlights};
    }
    if (update.use_camera_wb.has_value() && payload.use_camera_wb != *update.use_camera_wb) {
      payload.use_camera_wb = *update.use_camera_wb;
      changed |= DirtyFieldMask{DevelopDirty::WhiteBalance};
    }
    if (update.user_wb.has_value() && payload.user_wb != *update.user_wb) {
      payload.user_wb = *update.user_wb;
      changed |= DirtyFieldMask{DevelopDirty::WhiteBalance};
    }
    return changed;
  });
}

void DevelopParamsModel::ApplyColorTemperatureUpdate(DevelopColorTemperatureUpdate update) {
  MutateWithDirtyFields([update = std::move(update)](DevelopPayload& payload) mutable {
    bool changed = false;
    if (update.wb_mode.has_value() && payload.wb_mode != *update.wb_mode) {
      payload.wb_mode = std::move(*update.wb_mode);
      changed         = true;
    }
    if (update.custom_cct.has_value() && payload.custom_cct != *update.custom_cct) {
      payload.custom_cct = *update.custom_cct;
      changed            = true;
    }
    if (update.custom_tint.has_value() && payload.custom_tint != *update.custom_tint) {
      payload.custom_tint = *update.custom_tint;
      changed             = true;
    }
    if (update.as_shot_cct.has_value() && payload.as_shot_cct != *update.as_shot_cct) {
      payload.as_shot_cct = *update.as_shot_cct;
      changed             = true;
    }
    if (update.as_shot_tint.has_value() && payload.as_shot_tint != *update.as_shot_tint) {
      payload.as_shot_tint = *update.as_shot_tint;
      changed              = true;
    }
    return changed ? DirtyFieldMask{DevelopDirty::WhiteBalance} : DirtyFieldMask{};
  });
}

void DevelopParamsModel::ApplyLensCalibrationUpdate(DevelopLensCalibrationUpdate update) {
  MutateWithDirtyFields([update = std::move(update)](DevelopPayload& payload) mutable {
    bool changed = false;
    if (update.lens_enabled.has_value() && payload.lens_enabled != *update.lens_enabled) {
      payload.lens_enabled = *update.lens_enabled;
      changed              = true;
    }
    if (update.apply_vignetting.has_value() &&
        payload.apply_vignetting != *update.apply_vignetting) {
      payload.apply_vignetting = *update.apply_vignetting;
      changed                  = true;
    }
    if (update.apply_distortion.has_value() &&
        payload.apply_distortion != *update.apply_distortion) {
      payload.apply_distortion = *update.apply_distortion;
      changed                  = true;
    }
    if (update.apply_tca.has_value() && payload.apply_tca != *update.apply_tca) {
      payload.apply_tca = *update.apply_tca;
      changed           = true;
    }
    if (update.apply_crop.has_value() && payload.apply_crop != *update.apply_crop) {
      payload.apply_crop = *update.apply_crop;
      changed            = true;
    }
    if (update.auto_scale.has_value() && payload.auto_scale != *update.auto_scale) {
      payload.auto_scale = *update.auto_scale;
      changed            = true;
    }
    if (update.use_user_scale.has_value() && payload.use_user_scale != *update.use_user_scale) {
      payload.use_user_scale = *update.use_user_scale;
      changed                = true;
    }
    if (update.user_scale.has_value() && payload.user_scale != *update.user_scale) {
      payload.user_scale = *update.user_scale;
      changed            = true;
    }
    if (update.projection_enabled.has_value() &&
        payload.projection_enabled != *update.projection_enabled) {
      payload.projection_enabled = *update.projection_enabled;
      changed                    = true;
    }
    if (update.target_projection.has_value() &&
        payload.target_projection != *update.target_projection) {
      payload.target_projection = std::move(*update.target_projection);
      changed                   = true;
    }
    if (update.lens_profile_db_path.has_value() &&
        payload.lens_profile_db_path != *update.lens_profile_db_path) {
      payload.lens_profile_db_path = std::move(*update.lens_profile_db_path);
      changed                      = true;
    }
    return changed ? DirtyFieldMask{DevelopDirty::Lens} : DirtyFieldMask{};
  });
}

auto DevelopParamsModel::ToJson() const -> nlohmann::json {
  const auto  payload = PayloadCopy();
  const auto& profile = payload.camera_profile;
  return {{"demosaic_method", payload.demosaic_method},
          {"highlights_reconstruct", payload.highlights_reconstruct},
          {"use_camera_wb", payload.use_camera_wb},
          {"user_wb", payload.user_wb},
          {"wb_mode", payload.wb_mode},
          {"custom_cct", payload.custom_cct},
          {"custom_tint", payload.custom_tint},
          {"as_shot_cct", payload.as_shot_cct},
          {"as_shot_tint", payload.as_shot_tint},
          {"camera_profile",
           {{"dng_profile", DngColorProfileToJson(profile.dng_profile)},
            {"color_matrices_valid", profile.color_matrices_valid},
            {"color_matrix_1", json_util::MakeJsonArray(profile.color_matrix_1.data(), 9)},
            {"color_matrix_2", json_util::MakeJsonArray(profile.color_matrix_2.data(), 9)},
            {"forward_matrices_valid", profile.forward_matrices_valid},
            {"forward_matrix_1", json_util::MakeJsonArray(profile.forward_matrix_1.data(), 9)},
            {"forward_matrix_2", json_util::MakeJsonArray(profile.forward_matrix_2.data(), 9)},
            {"as_shot_neutral_valid", profile.as_shot_neutral_valid},
            {"as_shot_neutral", json_util::MakeJsonArray(profile.as_shot_neutral.data(), 3)},
            {"calibration_illuminants_valid", profile.calibration_illuminants_valid},
            {"color_matrix_1_cct", profile.color_matrix_1_cct},
            {"color_matrix_2_cct", profile.color_matrix_2_cct},
            {"cam_mul", json_util::MakeJsonArray(profile.cam_mul.data(), 3)}}},
          {"lens_enabled", payload.lens_enabled},
          {"apply_vignetting", payload.apply_vignetting},
          {"apply_distortion", payload.apply_distortion},
          {"apply_tca", payload.apply_tca},
          {"apply_crop", payload.apply_crop},
          {"auto_scale", payload.auto_scale},
          {"use_user_scale", payload.use_user_scale},
          {"user_scale", payload.user_scale},
          {"projection_enabled", payload.projection_enabled},
          {"target_projection", payload.target_projection},
          {"lens_profile_db_path", payload.lens_profile_db_path}};
}

void DevelopParamsModel::LoadJson(const nlohmann::json& json) {
  Mutate(DevelopDirty::All, [&json](DevelopPayload& payload) {
    payload.demosaic_method =
        json_util::ReadString(json, "demosaic_method", payload.demosaic_method);
    payload.highlights_reconstruct =
        json_util::ReadBool(json, "highlights_reconstruct", payload.highlights_reconstruct);
    payload.use_camera_wb = json_util::ReadBool(json, "use_camera_wb", payload.use_camera_wb);
    payload.user_wb       = json_util::ReadFloat(json, "user_wb", payload.user_wb);
    payload.wb_mode       = json_util::ReadString(json, "wb_mode", payload.wb_mode);
    payload.custom_cct    = json_util::ReadFloat(json, "custom_cct", payload.custom_cct);
    payload.custom_tint   = json_util::ReadFloat(json, "custom_tint", payload.custom_tint);
    payload.as_shot_cct   = json_util::ReadFloat(json, "as_shot_cct", payload.as_shot_cct);
    payload.as_shot_tint  = json_util::ReadFloat(json, "as_shot_tint", payload.as_shot_tint);
    if (json.contains("camera_profile") && json["camera_profile"].is_object()) {
      const auto& profile_json = json["camera_profile"];
      auto&       profile      = payload.camera_profile;
      profile.dng_profile =
          DngColorProfileFromJson(profile_json.value("dng_profile", nlohmann::json(nullptr)));
      profile.color_matrices_valid =
          json_util::ReadBool(profile_json, "color_matrices_valid", profile.color_matrices_valid);
      json_util::ReadNumberArray(profile_json, "color_matrix_1", profile.color_matrix_1.data(), 9);
      json_util::ReadNumberArray(profile_json, "color_matrix_2", profile.color_matrix_2.data(), 9);
      profile.forward_matrices_valid = json_util::ReadBool(profile_json, "forward_matrices_valid",
                                                           profile.forward_matrices_valid);
      json_util::ReadNumberArray(profile_json, "forward_matrix_1", profile.forward_matrix_1.data(),
                                 9);
      json_util::ReadNumberArray(profile_json, "forward_matrix_2", profile.forward_matrix_2.data(),
                                 9);
      profile.as_shot_neutral_valid =
          json_util::ReadBool(profile_json, "as_shot_neutral_valid", profile.as_shot_neutral_valid);
      json_util::ReadNumberArray(profile_json, "as_shot_neutral", profile.as_shot_neutral.data(),
                                 3);
      profile.calibration_illuminants_valid = json_util::ReadBool(
          profile_json, "calibration_illuminants_valid", profile.calibration_illuminants_valid);
      profile.color_matrix_1_cct =
          json_util::ReadDouble(profile_json, "color_matrix_1_cct", profile.color_matrix_1_cct);
      profile.color_matrix_2_cct =
          json_util::ReadDouble(profile_json, "color_matrix_2_cct", profile.color_matrix_2_cct);
      json_util::ReadNumberArray(profile_json, "cam_mul", profile.cam_mul.data(), 3);
    }
    payload.lens_enabled = json_util::ReadBool(json, "lens_enabled", payload.lens_enabled);
    payload.apply_vignetting =
        json_util::ReadBool(json, "apply_vignetting", payload.apply_vignetting);
    payload.apply_distortion =
        json_util::ReadBool(json, "apply_distortion", payload.apply_distortion);
    payload.apply_tca      = json_util::ReadBool(json, "apply_tca", payload.apply_tca);
    payload.apply_crop     = json_util::ReadBool(json, "apply_crop", payload.apply_crop);
    payload.auto_scale     = json_util::ReadBool(json, "auto_scale", payload.auto_scale);
    payload.use_user_scale = json_util::ReadBool(json, "use_user_scale", payload.use_user_scale);
    payload.user_scale     = json_util::ReadFloat(json, "user_scale", payload.user_scale);
    payload.projection_enabled =
        json_util::ReadBool(json, "projection_enabled", payload.projection_enabled);
    payload.target_projection =
        json_util::ReadString(json, "target_projection", payload.target_projection);
    payload.lens_profile_db_path =
        json_util::ReadString(json, "lens_profile_db_path", payload.lens_profile_db_path);
  });
}

void DevelopParamsModel::ReplaceParams(DevelopPayload payload) {
  MutateWithDirtyFields([payload = std::move(payload)](DevelopPayload& dest) mutable {
    if (dest == payload) {
      return DirtyFieldMask{};
    }
    dest = std::move(payload);
    return DirtyFieldMask{DevelopDirty::All};
  });
}

DevelopNodeModel::DevelopNodeModel(NodeId id) : id_(std::move(id)) {
  outputs_[0] = PortDescriptor{PortId{"image"}, PortDataType::SceneImage, true};
}

auto DevelopNodeModel::InputPorts() const -> std::span<const PortDescriptor> { return {}; }

auto DevelopNodeModel::OutputPorts() const -> std::span<const PortDescriptor> { return outputs_; }

auto DevelopNodeModel::ToJson() const -> nlohmann::json {
  return {{"id", std::string{id_.Value()}},
          {"type", std::string{Type().Text()}},
          {"params", params_.ToJson()}};
}

auto DevelopNodeModel::FromJson(const nlohmann::json& json) -> std::unique_ptr<DevelopNodeModel> {
  auto node = std::make_unique<DevelopNodeModel>(NodeId{json.at("id").get<std::string>()});
  if (json.contains("params") && json["params"].is_object()) {
    node->params_.LoadJson(json["params"]);
  }
  return node;
}

}  // namespace alcedo
