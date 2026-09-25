//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/source_dng_profile_binding.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "image/dng_color_profile_cache.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"

namespace alcedo {

auto SourceImagePath(Storage& storage, sl_element_id_t id) -> std::filesystem::path {
  const auto element = storage.GetElementStore().GetElementById(id);
  const auto file    = std::dynamic_pointer_cast<SleeveFile>(element);
  if (!file || file->image_id_ == 0) {
    throw std::runtime_error("PipelineMgmtService: element " + std::to_string(id) +
                             " has no bound image");
  }
  const auto image = storage.GetImageStore().GetImageById(file->image_id_);
  if (!image) {
    throw std::runtime_error("PipelineMgmtService: image " + std::to_string(file->image_id_) +
                             " of element " + std::to_string(id) + " is not stored");
  }
  return image->image_path_;
}

auto LoadSourceDngColorProfile(Storage& storage, sl_element_id_t id,
                               const DngColorProfileRef& reference) -> DngColorProfilePtr {
  if (!reference.IsReferenced() || reference.IsBound()) {
    return reference.Profile();
  }
  const auto source  = SourceImagePath(storage, id);
  auto       profile = DngColorProfileCache::Shared().Load(source);
  if (!profile) {
    throw std::runtime_error("PipelineMgmtService: source file of element " + std::to_string(id) +
                             " has no DNG profile: " + source.string());
  }
  if (profile->fingerprint != *reference.Fingerprint()) {
    std::cerr << "[Alcedo] DNG profile of element " << id << " changed in the source file ("
              << DngColorProfileFingerprintToText(*reference.Fingerprint()) << " -> "
              << DngColorProfileFingerprintToText(profile->fingerprint)
              << "); the source file profile is used.\n";
  }
  return profile;
}

void BindSourceDngColorProfile(Storage& storage, sl_element_id_t id, PipelineDocument& document) {
  auto* develop = document.Develop();
  if (develop == nullptr) {
    return;
  }
  const auto reference = develop->Params().DngProfile();
  if (reference.IsReferenced() && !reference.IsBound()) {
    develop->Params().BindDngColorProfile(LoadSourceDngColorProfile(storage, id, reference));
  }
}

void BindSourceDngColorProfile(Storage& storage, sl_element_id_t id,
                               RawRuntimeColorContext& context) {
  context.dng_profile_ = LoadSourceDngColorProfile(storage, id, context.dng_profile_);
}

}  // namespace alcedo
