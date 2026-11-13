//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/sleeve_element/sleeve_file.hpp"

#include <cstdint>
#include <memory>

#include "sleeve/sleeve_element/sleeve_element.hpp"

namespace alcedo {
SleeveFile::~SleeveFile() {}
SleeveFile::SleeveFile(sl_element_id_t id, file_name_t element_name)
    : SleeveElement(id, element_name), image_id_(0) {
  type_ = ElementType::FILE;
}
SleeveFile::SleeveFile(sl_element_id_t id, file_name_t element_name, std::shared_ptr<Image> image)
    : SleeveElement(id, element_name), image_id_(0) {
  image_ = image;
  if (image_) {
    image_id_ = image_->image_id_;
  }
  type_  = ElementType::FILE;
}

auto SleeveFile::Clear() -> bool {
  // FIXME: Add implementation
  return true;
}

auto SleeveFile::Copy(uint32_t new_id) const -> std::shared_ptr<SleeveElement> {
  std::shared_ptr<SleeveFile> new_file = std::make_shared<SleeveFile>(new_id, element_name_);
  // The image object is still reused
  new_file->image_                     = image_;
  new_file->image_id_                  = image_id_;
  return new_file;
}

auto SleeveFile::GetImage() -> std::shared_ptr<Image> { return image_; }
void SleeveFile::SetImage(const std::shared_ptr<Image> img) {
  image_    = img;
  image_id_ = img->image_id_;
}
};  // namespace alcedo
