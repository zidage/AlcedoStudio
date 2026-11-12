//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The profile tone stage on OpenCL (dcp_profile_tone_curve_study.md, phase T2).

#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/opencl/opencl_drt_pass.hpp"
#include "edit/runtime/opencl/opencl_pass_encoder.hpp"
#include "edit/runtime/texture_format.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"
#include "profile_tone_gpu_test_support.hpp"

namespace alcedo {
namespace {

using profile_tone_gpu_test::Rgba;

class OpenClProfileToneFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!TryInitializeOpenClRuntime() || !OpenClContext::Instance().Capabilities().image_support) {
      GTEST_SKIP() << "No OpenCL image device: unverified support, not a pass.";
    }
    device_ = std::make_unique<OpenClRenderDevice>();
  }

  /// Run only the DRT pass on @p ap1, with a scene image that holds these values with no change.
  auto RenderDrt(const PipelineDocument& document, std::span<const Rgba> ap1) -> std::vector<Rgba> {
    const auto width               = static_cast<std::uint32_t>(ap1.size());
    auto&      geometry            = plan_.geometry;
    geometry.full_reference_extent = {width, 1};
    geometry.edit_extent           = {width, 1};
    geometry.render_extent         = {width, 1};
    device_->BeginRender();
    auto& workspace = device_->Workspace();
    workspace.PrepareResultValidity(plan_, document, input_);
    const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
    auto& lease = workspace.AcquireImageForWrite(scene_id, {width, 1, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(lease.Texture(), std::as_bytes(ap1),
                                       device_->CommandContext());
    const auto result =
        ExecuteOpenClDrt(*device_, plan_, document, FrameSceneBinding::CachedImage(scene_id));
    std::vector<Rgba> pixels(width);
    auto*             display = workspace.Images().Find(result.output);
    EXPECT_NE(display, nullptr);
    if (display != nullptr) {
      workspace.Device().DownloadTexture2D(
          display->Texture(), std::as_writable_bytes(std::span(pixels)), device_->CommandContext());
    }
    device_->CancelRender();
    return pixels;
  }

  auto Render() -> profile_tone_gpu_test::RenderDrt {
    return [this](const PipelineDocument& document, std::span<const Rgba> ap1) {
      return RenderDrt(document, ap1);
    };
  }

  ExecutionPlan                       plan_;
  PreparedRawInput                    input_;
  std::unique_ptr<OpenClRenderDevice> device_;
};

TEST_F(OpenClProfileToneFixture, NeutralRampRendersTheProfileCurve) {
  std::string report;
  profile_tone_gpu_test::ExpectNeutralRampRendersTheProfileCurve(Render(), &report);
  std::cout << "OpenCL profile tone neutral ramp:\n" << report;
}

TEST_F(OpenClProfileToneFixture, ColorPatchesMatchTheScalarReference) {
  std::string report;
  profile_tone_gpu_test::ExpectPatchesMatchTheScalarReference(Render(), &report);
  std::cout << "OpenCL profile tone color patches:\n" << report;
}

TEST_F(OpenClProfileToneFixture, DocumentsWithoutTheStageRenderUnchangedBytes) {
  profile_tone_gpu_test::ExpectDocumentsWithoutTheStageRenderUnchangedBytes(Render());
}

}  // namespace
}  // namespace alcedo
