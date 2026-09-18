//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "edit/runtime/render_backend.hpp"
#include "edit/runtime/renderer.hpp"

namespace alcedo {
namespace {

auto FileContainsForbiddenToken(const std::filesystem::path& path) -> std::string {
  std::ifstream input(path);
  std::string   line;
  int           line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const char* tokens[] = {"image_buffer.hpp", "image_buffer.h", "cuda.h", "cuda_runtime",
                            "OpenCL",           "opencl.h",       "Metal/", "metal.h"};
    for (const char* token : tokens) {
      if (line.find(token) != std::string::npos) {
        return path.filename().string() + ":" + std::to_string(line_number) + " " + token;
      }
    }
  }
  return {};
}

void ScanDirectory(const std::filesystem::path& root, std::vector<std::string>* hits) {
  if (!std::filesystem::exists(root)) {
    hits->push_back("missing directory: " + root.string());
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (entry.path().extension() != ".hpp") {
      continue;
    }
    const auto hit = FileContainsForbiddenToken(entry.path());
    if (!hit.empty()) {
      hits->push_back(hit);
    }
  }
}

}  // namespace

TEST(GpuDagCudaWorkspace, GpuAndRuntimeHeadersDoNotIncludeCudaOrImageBuffer) {
  std::vector<std::string> hits;
  ScanDirectory(std::filesystem::path{ALCEDO_GPU_HEADER_ROOT}, &hits);
  ScanDirectory(std::filesystem::path{ALCEDO_RUNTIME_HEADER_ROOT}, &hits);
  EXPECT_TRUE(hits.empty()) << (hits.empty() ? "" : hits.front());
}

TEST(GpuDagCudaWorkspace, RendererTemplateInstantiatesCudaWithoutMetalHeaders) {
  const char* files[] = {"renderer.hpp",      "plan_executor.hpp", "pass_encoder.hpp",
                         "basic_render_device.hpp", "frame_presenter.hpp", "drt_display.hpp",
                         "render_backend.hpp", "render_device_type.hpp"};
  const std::filesystem::path root{ALCEDO_RUNTIME_HEADER_ROOT};
  for (const char* name : files) {
    const auto hit = FileContainsForbiddenToken(root / name);
    EXPECT_TRUE(hit.empty()) << hit;
    std::ifstream input(root / name);
    std::string   text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text.find("Metal/"), std::string::npos) << name;
    EXPECT_EQ(text.find("metal.h"), std::string::npos) << name;
  }
  static_assert(RenderBackend<CudaBackend>);
  static_assert(std::is_same_v<CudaRenderer, Renderer<CudaBackend>>);
  static_assert(sizeof(CudaRenderer) > 0);
  EXPECT_EQ(kCudaDagBackendCapabilityVersion, CudaBackend::kCapabilityVersion);
}

}  // namespace alcedo
