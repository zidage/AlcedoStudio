//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>

#include "edit/runtime/executor_role.hpp"
#include "edit/runtime/metal/metal_backend.hpp"
#include "edit/runtime/metal/metal_renderer.hpp"
#include "edit/runtime/render_backend.hpp"
#include "edit/runtime/renderer.hpp"

namespace alcedo {
namespace {

TEST(GpuDagRendererTemplate, RendererTemplateInstantiatesMetalWithoutCudaHeaders) {
  const char* files[] = {"renderer.hpp", "plan_executor.hpp", "pass_encoder.hpp",
                         "basic_render_device.hpp", "frame_presenter.hpp"};
  const std::filesystem::path root{ALCEDO_RUNTIME_HEADER_ROOT};
  for (const char* name : files) {
    std::ifstream input(root / name);
    ASSERT_TRUE(input) << name;
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text.find("cuda_runtime"), std::string::npos) << name;
    EXPECT_EQ(text.find("cuda.h"), std::string::npos) << name;
  }

  const auto metal_backend_path =
      std::filesystem::path{ALCEDO_RUNTIME_HEADER_ROOT} / "metal" / "metal_backend.hpp";
  std::ifstream metal_input(metal_backend_path);
  ASSERT_TRUE(metal_input);
  std::string metal_text((std::istreambuf_iterator<char>(metal_input)),
                         std::istreambuf_iterator<char>());
  EXPECT_EQ(metal_text.find("cuda_runtime"), std::string::npos);
  EXPECT_EQ(metal_text.find("cuda.h"), std::string::npos);
  EXPECT_EQ(metal_text.find("Metal/"), std::string::npos);
  EXPECT_EQ(metal_text.find("metal.h"), std::string::npos);

  static_assert(RenderBackend<MetalBackend>);
  static_assert(std::is_same_v<MetalRenderer, Renderer<MetalBackend>>);
  static_assert(sizeof(MetalRenderer) > 0);
  EXPECT_EQ(kMetalDagBackendCapabilityVersion, MetalBackend::kCapabilityVersion);
  EXPECT_NE(kMetalDagBackendCapabilityVersion, 1U);

  for (const auto role : {ExecutorRole::Interactive, ExecutorRole::Batch}) {
    MetalRenderer renderer(role);
    EXPECT_EQ(renderer.Role(), role);
    EXPECT_EQ(renderer.PlanCache().BackendCapabilityVersion(), kMetalDagBackendCapabilityVersion);
    EXPECT_EQ(renderer.Resources().published_result_count, 0U);
    EXPECT_FALSE(renderer.Binding().has_value());
  }
}

}  // namespace
}  // namespace alcedo
