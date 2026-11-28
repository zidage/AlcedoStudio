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
 * The element refers to its image by `image_id_` (the `FileImage` row). The image pool and the
 * image store own the Image object. The import and the store loads link no Image into the
 * element (Sleeve Node Cache Plan, SN7), so the node cache can drop a clean file element and
 * free its memory.
 */
class SleeveFile : public SleeveElement {
 private:
  /// Set only by the legacy loader SleeveManager::LoadToPath, which tests use; read only by the
  /// legacy SleeveView. Production code reads `image_id_`.
  std::shared_ptr<Image> image_;

 public:
  image_id_t image_id_;
  explicit SleeveFile(sl_element_id_t id, file_name_t element_name);
  explicit SleeveFile(sl_element_id_t id, file_name_t element_name, std::shared_ptr<Image> image);

  auto Clear() -> bool;

  auto Copy(sl_element_id_t new_id) const -> std::shared_ptr<SleeveElement>;
  /// The linked Image of the legacy loader (see image_), or nullptr.
  auto GetImage() -> std::shared_ptr<Image>;
  /// Links @p img and sets image_id_. Legacy loader only: the element then keeps the Image alive
  /// for as long as the node cache holds the element.
  void SetImage(const std::shared_ptr<Image> img);
  ~SleeveFile();
};
};  // namespace alcedo