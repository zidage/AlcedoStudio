//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <exception>
#include <stdexcept>

#include "edit/input/prepared_raw_input.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/runtime/cuda/cuda_develop_pass.hpp"
#include "edit/runtime/cuda/cuda_drt_pass.hpp"
#include "edit/runtime/cuda/cuda_mask_pass.hpp"
#include "edit/runtime/cuda/cuda_primary_grade_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

constexpr TextureFormat kResultFormat = TextureFormat::Rgba32f;

auto BindOrMiss(CudaRenderWorkspace& images_owner, const GraphValueId& id, ContentKey key,
                ImageExtent extent, std::uint64_t completed,
                TextureFormat format = kResultFormat) -> bool {
  return images_owner.Images().BindValidResult(id, key, extent, format, completed) != nullptr;
}

void Record(CudaRenderDevice& device, const GraphValueId& id, ContentKey key, ImageExtent extent,
            TextureFormat format = kResultFormat) {
  device.Workspace().Images().RecordUnpublished(id, key, extent, format,
                                                device.CommandContext().SubmissionId());
}

}  // namespace

auto CudaRenderDevice::Execute(const ExecutionPlan& plan, const PreparedRawInput& input,
                               PipelineDocument& document, MaskStore* mask_store,
                               bool publish_on_success) -> GraphValueId {
  try {
    auto& workspace = Workspace();
    if (workspace.Textures().ByteBudget() == 0) {
      workspace.Textures().SetByteBudget(DefaultProductTextureBudgetBytes());
    }
    BeginRender();
    const auto keys       = BuildFrameResultContentKeys(plan, input, document);
    const auto completed  = workspace.Device().CompletedSubmission();
    auto&      stats      = pass_stats_;
    const auto hits_before  = workspace.Images().ContentHitCount();
    const auto misses_before = workspace.Images().ContentMissCount();

    if (BindOrMiss(workspace, plan.sensor_linear_output, keys.sensor_linear, keys.sensor_extent,
                   completed)) {
      ++stats.sensor_develop_skip;
    } else {
      const auto h2d_before = workspace.Device().HostToDeviceBytes();
      ExecuteCudaDevelop(*this, plan, input, document);
      stats.source_h2d_bytes += workspace.Device().HostToDeviceBytes() - h2d_before;
      ++stats.source_h2d_count;
      Record(*this, plan.sensor_linear_output, keys.sensor_linear, keys.sensor_extent);
      ++stats.sensor_develop_execute;
    }

    if (BindOrMiss(workspace, plan.geometry_output, keys.geometry_scene_source,
                   keys.geometry_extent, completed)) {
      ++stats.geometry_skip;
    } else {
      ExecuteCudaGeometryResample(*this, plan);
      Record(*this, plan.geometry_output, keys.geometry_scene_source, keys.geometry_extent);
      ++stats.geometry_execute;
    }

    if (BindOrMiss(workspace, plan.develop_output, keys.develop_image, keys.geometry_extent,
                   completed)) {
      ++stats.camera_color_skip;
    } else {
      ExecuteCudaCameraColor(*this, plan, document);
      Record(*this, plan.develop_output, keys.develop_image, keys.geometry_extent);
      ++stats.camera_color_execute;
    }

    if (plan.primary_grade_mask) {
      if (BindOrMiss(workspace, plan.mask_output, keys.mask, keys.geometry_extent, completed,
                     TextureFormat::R8)) {
        ++stats.mask_skip;
      } else {
        (void)ExecuteCudaMask(*this, plan, document, mask_store);
        Record(*this, plan.mask_output, keys.mask, keys.geometry_extent, TextureFormat::R8);
        ++stats.mask_execute;
      }
    }

    if (BindOrMiss(workspace, plan.primary_grade_output, keys.primary_grade, keys.geometry_extent,
                   completed)) {
      ++stats.primary_grade_skip;
    } else {
      (void)ExecuteCudaPrimaryGrade(*this, plan, input, document);
      Record(*this, plan.primary_grade_output, keys.primary_grade, keys.geometry_extent);
      ++stats.primary_grade_execute;
    }

    if (BindOrMiss(workspace, plan.display_output, keys.drt_display, keys.geometry_extent,
                   completed)) {
      ++stats.drt_skip;
    } else {
      (void)ExecuteCudaDrt(*this, plan, document);
      Record(*this, plan.display_output, keys.drt_display, keys.geometry_extent);
      ++stats.drt_execute;
    }

    stats.result_content_hits += workspace.Images().ContentHitCount() - hits_before;
    stats.result_content_misses += workspace.Images().ContentMissCount() - misses_before;
    EndRender();
    if (publish_on_success) {
      PublishResults();
    }
    return plan.display_output;
  } catch (const std::exception& ex) {
    CancelRender();
    ReportError(ex.what());
    throw;
  } catch (...) {
    CancelRender();
    ReportError("CUDA DAG execution failed with an unknown error");
    throw;
  }
}

}  // namespace alcedo
