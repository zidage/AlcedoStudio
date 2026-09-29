//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <string>

#include "edit/geometry/types.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Width and height of a custom crop aspect ratio. Only the quotient is meaningful.
 */
struct CropAspectRatio {
  float width  = 1.0f;
  float height = 1.0f;

  friend auto operator==(const CropAspectRatio&, const CropAspectRatio&) -> bool = default;
};

/**
 * @brief Focused document geometry update. Omitted fields retain current values.
 */
struct ImageGeometryUpdate {
  std::optional<NormalizedRect>  crop_rect;
  std::optional<float>           rotation_degrees;
  std::optional<std::string>     aspect_preset;
  std::optional<CropAspectRatio> aspect_ratio;
};

/**
 * @brief Document-level crop and rotation. Not a user-visible graph node.
 *
 * The output is the crop frame: an axis-aligned rectangle of `w * W` by `h * H` source pixels
 * centered at `(x + w / 2, y + h / 2)`, through which the source is seen rotated by
 * @ref RotationDegrees about that center. Renders constrain the frame with
 * ClampCropToRotatedSource so no corner leaves the source. @ref AspectPreset and
 * @ref AspectRatio record the Geometry panel's aspect constraint so it is restored with the
 * document; they do not change the render.
 *
 * Viewport ROI and dynamic resolution are render-request data and are not stored.
 */
class ImageGeometryModel {
 public:
  static constexpr const char* kFreeAspectPreset = "free";

  ImageGeometryModel() = default;

  [[nodiscard]] auto CropRect() const -> NormalizedRect { return crop_rect_; }
  [[nodiscard]] auto RotationDegrees() const -> float { return rotation_degrees_; }
  [[nodiscard]] auto AspectPreset() const -> const std::string& { return aspect_preset_; }
  [[nodiscard]] auto AspectRatio() const -> CropAspectRatio { return aspect_ratio_; }

  void               SetCropRect(NormalizedRect rect) { crop_rect_ = rect; }
  void               SetRotationDegrees(float degrees) { rotation_degrees_ = degrees; }

  /**
   * @brief Apply geometry fields together so a validated patch cannot expose partial state.
   */
  void               ApplyUpdate(const ImageGeometryUpdate& update) {
    if (update.crop_rect.has_value()) {
      crop_rect_ = *update.crop_rect;
    }
    if (update.rotation_degrees.has_value()) {
      rotation_degrees_ = *update.rotation_degrees;
    }
    if (update.aspect_preset.has_value()) {
      aspect_preset_ = *update.aspect_preset;
    }
    if (update.aspect_ratio.has_value()) {
      aspect_ratio_ = *update.aspect_ratio;
    }
  }

  [[nodiscard]] auto ToJson() const -> nlohmann::json {
    return {{"crop_rect",
             nlohmann::json::array({crop_rect_.x, crop_rect_.y, crop_rect_.w, crop_rect_.h})},
            {"rotation_degrees", rotation_degrees_},
            {"aspect_preset", aspect_preset_},
            {"aspect_ratio", nlohmann::json::array({aspect_ratio_.width, aspect_ratio_.height})}};
  }

  /**
   * @brief Reads a stored model. Keys written by earlier versions (such as `expand_to_fit`) are
   *        ignored; missing aspect fields keep their defaults.
   */
  static auto FromJson(const nlohmann::json& json) -> ImageGeometryModel {
    ImageGeometryModel model;
    if (json.contains("crop_rect") && json["crop_rect"].is_array() &&
        json["crop_rect"].size() >= 4) {
      model.crop_rect_.x = json["crop_rect"][0].get<float>();
      model.crop_rect_.y = json["crop_rect"][1].get<float>();
      model.crop_rect_.w = json["crop_rect"][2].get<float>();
      model.crop_rect_.h = json["crop_rect"][3].get<float>();
    }
    if (json.contains("rotation_degrees") && json["rotation_degrees"].is_number()) {
      model.rotation_degrees_ = json["rotation_degrees"].get<float>();
    }
    if (json.contains("aspect_preset") && json["aspect_preset"].is_string()) {
      model.aspect_preset_ = json["aspect_preset"].get<std::string>();
    }
    if (json.contains("aspect_ratio") && json["aspect_ratio"].is_array() &&
        json["aspect_ratio"].size() >= 2) {
      model.aspect_ratio_.width  = json["aspect_ratio"][0].get<float>();
      model.aspect_ratio_.height = json["aspect_ratio"][1].get<float>();
    }
    return model;
  }

 private:
  NormalizedRect  crop_rect_{};
  float           rotation_degrees_ = 0.0f;
  std::string     aspect_preset_    = kFreeAspectPreset;
  CropAspectRatio aspect_ratio_{};
};

}  // namespace alcedo
