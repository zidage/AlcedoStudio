//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>

#include "image/image.hpp"
#include "sleeve_element.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief A file element: binds one image and its metadata to a Sleeve element id.
 *
 * Edit history is not stored here. Mini-Git history is keyed by the element id and
 * owned by the pipeline and history services.
 *
 */
class SleeveFile : public SleeveElement {
 private:
  std::shared_ptr<Image> image_;

 public:
  image_id_t image_id_;
  explicit SleeveFile(sl_element_id_t id, file_name_t element_name);
  explicit SleeveFile(sl_element_id_t id, file_name_t element_name, std::shared_ptr<Image> image);

  auto Clear() -> bool;

  auto Copy(sl_element_id_t new_id) const -> std::shared_ptr<SleeveElement>;
  auto GetImage() -> std::shared_ptr<Image>;
  void SetImage(const std::shared_ptr<Image> img);
  ~SleeveFile();
};
};  // namespace alcedo