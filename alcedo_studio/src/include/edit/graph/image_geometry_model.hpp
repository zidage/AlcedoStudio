//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>

#include "edit/geometry/types.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Focused document geometry update. Omitted fields retain current values.
 */
struct ImageGeometryUpdate {
  std::optional<NormalizedRect> crop_rect;
  std::optional<float>          rotation_degrees;
  std::optional<bool>           expand_to_fit;
};

/**
 * @brief Document-level crop and rotation. Not a user-visible graph node.
 *
 * Viewport ROI and dynamic resolution are render-request data and are not stored.
 */
class ImageGeometryModel {
 public:
  ImageGeometryModel() = default;

  [[nodiscard]] auto CropRect() const -> NormalizedRect { return crop_rect_; }
  [[nodiscard]] auto RotationDegrees() const -> float { return rotation_degrees_; }
  [[nodiscard]] auto ExpandToFit() const -> bool { return expand_to_fit_; }

  void               SetCropRect(NormalizedRect rect) { crop_rect_ = rect; }
  void               SetRotationDegrees(float degrees) { rotation_degrees_ = degrees; }
  void               SetExpandToFit(bool expand) { expand_to_fit_ = expand; }

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
    if (update.expand_to_fit.has_value()) {
      expand_to_fit_ = *update.expand_to_fit;
    }
  }

  [[nodiscard]] auto ToJson() const -> nlohmann::json {
    return {{"crop_rect",
             nlohmann::json::array({crop_rect_.x, crop_rect_.y, crop_rect_.w, crop_rect_.h})},
            {"rotation_degrees", rotation_degrees_},
            {"expand_to_fit", expand_to_fit_}};
  }

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
    if (json.contains("expand_to_fit") && json["expand_to_fit"].is_boolean()) {
      model.expand_to_fit_ = json["expand_to_fit"].get<bool>();
    }
    return model;
  }

 private:
  NormalizedRect crop_rect_{};
  float          rotation_degrees_ = 0.0f;
  bool           expand_to_fit_    = true;
};

}  // namespace alcedo
