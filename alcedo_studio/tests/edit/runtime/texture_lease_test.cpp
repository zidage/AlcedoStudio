//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda_workspace_test_support.hpp"

namespace alcedo {
namespace {

using cuda_workspace_test::CudaWorkspaceFixture;

TEST_F(CudaWorkspaceFixture, TextureLeasePreventsReleaseUntilCudaSubmissionCompletes) {
  CudaRenderDevice device;
  auto&            textures = device.Workspace().Textures();
  constexpr std::uint32_t kSize = 64;

  device.BeginRender();
  auto lease_a = textures.Acquire({kSize, kSize, TextureFormat::R8});
  const auto handle_a = lease_a.Handle();
  auto lease_b = textures.Acquire({kSize, kSize, TextureFormat::R8});
  EXPECT_TRUE(textures.Contains(handle_a));
  device.EndRender();

  lease_a.Release();
  lease_b.Release();
  textures.ReleaseUnleased();
  EXPECT_TRUE(textures.Contains(handle_a));

  device.BeginRender();
  textures.ReleaseUnleased();
  EXPECT_FALSE(textures.Contains(handle_a));
  device.EndRender();
  device.WaitIdle();
}

}  // namespace
}  // namespace alcedo
