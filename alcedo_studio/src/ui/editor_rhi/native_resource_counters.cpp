//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/native_resource_counters.hpp"

namespace alcedo::editor_rhi {

auto NativeResourceCounters::Instance() -> NativeResourceCounters& {
  static NativeResourceCounters instance;
  return instance;
}

void NativeResourceCounters::OnCreateSharedTexture() { ++shared_textures_; }
void NativeResourceCounters::OnDestroySharedTexture() { --shared_textures_; }
void NativeResourceCounters::OnCreateImportedQRhiTexture() { ++imported_qrhi_textures_; }
void NativeResourceCounters::OnDestroyImportedQRhiTexture() { --imported_qrhi_textures_; }
void NativeResourceCounters::OnCreateExternalMemory() { ++external_memories_; }
void NativeResourceCounters::OnDestroyExternalMemory() { --external_memories_; }
void NativeResourceCounters::OnCreateOpenClImage() { ++opencl_images_; }
void NativeResourceCounters::OnDestroyOpenClImage() { --opencl_images_; }

auto NativeResourceCounters::LiveSharedTextures() const -> std::int64_t {
  return shared_textures_.load();
}
auto NativeResourceCounters::LiveImportedQRhiTextures() const -> std::int64_t {
  return imported_qrhi_textures_.load();
}
auto NativeResourceCounters::LiveExternalMemories() const -> std::int64_t {
  return external_memories_.load();
}
auto NativeResourceCounters::LiveOpenClImages() const -> std::int64_t {
  return opencl_images_.load();
}
auto NativeResourceCounters::LiveTotal() const -> std::int64_t {
  return LiveSharedTextures() + LiveImportedQRhiTextures() + LiveExternalMemories() +
         LiveOpenClImages();
}

void NativeResourceCounters::ResetForTest() {
  shared_textures_.store(0);
  imported_qrhi_textures_.store(0);
  external_memories_.store(0);
  opencl_images_.store(0);
}

}  // namespace alcedo::editor_rhi
