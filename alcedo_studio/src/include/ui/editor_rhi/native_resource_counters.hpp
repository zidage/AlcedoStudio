//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>

namespace alcedo::editor_rhi {

// Process-wide counters for native presentation resources. Harness teardown
// fails when any live count is nonzero.
class NativeResourceCounters {
 public:
  static auto Instance() -> NativeResourceCounters&;

  void OnCreateSharedTexture();
  void OnDestroySharedTexture();
  void OnCreateImportedQRhiTexture();
  void OnDestroyImportedQRhiTexture();
  void OnCreateExternalMemory();
  void OnDestroyExternalMemory();
  void OnCreateOpenClImage();
  void OnDestroyOpenClImage();

  [[nodiscard]] auto LiveSharedTextures() const -> std::int64_t;
  [[nodiscard]] auto LiveImportedQRhiTextures() const -> std::int64_t;
  [[nodiscard]] auto LiveExternalMemories() const -> std::int64_t;
  [[nodiscard]] auto LiveOpenClImages() const -> std::int64_t;
  [[nodiscard]] auto LiveTotal() const -> std::int64_t;

  void ResetForTest();

 private:
  NativeResourceCounters() = default;

  std::atomic<std::int64_t> shared_textures_{0};
  std::atomic<std::int64_t> imported_qrhi_textures_{0};
  std::atomic<std::int64_t> external_memories_{0};
  std::atomic<std::int64_t> opencl_images_{0};
};

}  // namespace alcedo::editor_rhi
