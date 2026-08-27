//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <exception>
#include <stdexcept>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/pass_encoder.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {

class MaskStore;

/**
 * @brief Shared content-key skip, encode, cancel, and publish flow for one plan.
 *
 * Skip units match CUDA DAG cache boundaries. A failed encode cancels the
 * incomplete submission and does not publish new content keys. There is no
 * CPU or alternate-backend substitute.
 *
 * @tparam Backend Render backend whose PassEncoder specializations perform GPU work.
 */
template <class Backend>
class PlanExecutor {
 public:
  /**
   * @brief Execute compiled passes that miss the result cache.
   *
   * @param device Session or one-shot device that owns workspace and stats.
   * @param publish_on_success When true, publishes unpublished writes after EndRender.
   *        Product present paths pass false and publish after the sink succeeds.
   * @return Display GraphValueId of the compiled plan.
   * @throws std::exception from encode, upload, or submit after CancelRender.
   */
  template <class Device>
  static auto Execute(Device& device, const ExecutionPlan& plan, const PreparedRawInput& input,
                      PipelineDocument& document, MaskStore* mask_store, bool publish_on_success)
      -> GraphValueId {
    try {
      auto& workspace = device.Workspace();
      if constexpr (requires(Backend& backend, const ExecutionPlan& compiled) {
                      backend.WarmUpPlan(compiled);
                    }) {
        workspace.Device().WarmUpPlan(plan);
      }
      if (workspace.Textures().ByteBudget() == 0) {
        workspace.Textures().SetByteBudget(Backend::DefaultTextureBudgetBytes());
      }
      device.BeginRender();
      const auto keys          = BuildFrameResultContentKeys(plan, input, document);
      const auto completed     = workspace.Device().CompletedSubmission();
      auto&      stats         = device.PassStats();
      const auto hits_before   = workspace.Images().ContentHitCount();
      const auto misses_before = workspace.Images().ContentMissCount();

      if (BindOrMiss(workspace, plan.sensor_linear_output, keys.sensor_linear, keys.sensor_extent,
                     completed)) {
        ++stats.sensor_develop_skip;
      } else {
        const auto h2d_before = workspace.Device().HostToDeviceBytes();
        if (plan.Contains(GpuPassKind::UploadRgb)) {
          PassEncoder<Backend, GpuPassKind::UploadRgb>::Encode(device, plan, input, document,
                                                               mask_store);
        } else {
          PassEncoder<Backend, GpuPassKind::UploadRaw>::Encode(device, plan, input, document,
                                                               mask_store);
        }
        stats.source_h2d_bytes += workspace.Device().HostToDeviceBytes() - h2d_before;
        ++stats.source_h2d_count;
        Record(device, plan.sensor_linear_output, keys.sensor_linear, keys.sensor_extent);
        ++stats.sensor_develop_execute;
        // RCD planes are not a cache. Wait this stream so pack has finished, then
        // cudaFree / Metal free the slab before Geometry allocates display textures.
        workspace.Device().SynchronizeRecordedWork(device.CommandContext());
      }
      workspace.TransientBuffers().Reset();
      workspace.TransientBuffers().ReleaseDeviceMemory();

      if (BindOrMiss(workspace, plan.geometry_output, keys.geometry_scene_source,
                     keys.geometry_extent, completed)) {
        ++stats.geometry_skip;
      } else {
        PassEncoder<Backend, GpuPassKind::GeometryResample>::Encode(device, plan, input, document,
                                                                    mask_store);
        Record(device, plan.geometry_output, keys.geometry_scene_source, keys.geometry_extent);
        ++stats.geometry_execute;
      }

      if (BindOrMiss(workspace, plan.develop_output, keys.develop_image, keys.geometry_extent,
                     completed)) {
        ++stats.camera_color_skip;
      } else {
        PassEncoder<Backend, GpuPassKind::CameraToAp1>::Encode(device, plan, input, document,
                                                               mask_store);
        Record(device, plan.develop_output, keys.develop_image, keys.geometry_extent);
        ++stats.camera_color_execute;
      }

      if (plan.primary_grade_mask) {
        if (BindOrMiss(workspace, plan.mask_output, keys.mask, keys.geometry_extent, completed,
                       TextureFormat::R8)) {
          ++stats.mask_skip;
        } else {
          PassEncoder<Backend, GpuPassKind::MaskEvaluate>::Encode(device, plan, input, document,
                                                                  mask_store);
          Record(device, plan.mask_output, keys.mask, keys.geometry_extent, TextureFormat::R8);
          ++stats.mask_execute;
        }
        workspace.TransientBuffers().Reset();
      }

      if (BindOrMiss(workspace, plan.primary_grade_output, keys.primary_grade, keys.geometry_extent,
                     completed)) {
        ++stats.primary_grade_skip;
      } else {
        PassEncoder<Backend, GpuPassKind::PrimaryColorGrade>::Encode(device, plan, input, document,
                                                                     mask_store);
        Record(device, plan.primary_grade_output, keys.primary_grade, keys.geometry_extent);
        ++stats.primary_grade_execute;
      }

      if (BindOrMiss(workspace, plan.display_output, keys.drt_display, keys.geometry_extent,
                     completed)) {
        ++stats.drt_skip;
      } else {
        PassEncoder<Backend, GpuPassKind::Drt>::Encode(device, plan, input, document, mask_store);
        Record(device, plan.display_output, keys.drt_display, keys.geometry_extent);
        ++stats.drt_execute;
      }

      stats.result_content_hits += workspace.Images().ContentHitCount() - hits_before;
      stats.result_content_misses += workspace.Images().ContentMissCount() - misses_before;
      device.EndRender();
      if (publish_on_success) {
        device.PublishResults();
      }
      return plan.display_output;
    } catch (const std::exception& ex) {
      device.CancelRender();
      device.ReportError(ex.what());
      throw;
    } catch (...) {
      device.CancelRender();
      device.ReportError("DAG execution failed with an unknown error");
      throw;
    }
  }

 private:
  static constexpr TextureFormat kResultFormat = TextureFormat::Rgba32f;

  template <class Workspace>
  static auto BindOrMiss(Workspace& images_owner, const GraphValueId& id, ContentKey key,
                         ImageExtent extent, std::uint64_t completed,
                         TextureFormat format = kResultFormat) -> bool {
    return images_owner.Images().BindValidResult(id, key, extent, format, completed) != nullptr;
  }

  template <class Device>
  static void Record(Device& device, const GraphValueId& id, ContentKey key, ImageExtent extent,
                     TextureFormat format = kResultFormat) {
    device.Workspace().Images().RecordUnpublished(id, key, extent, format,
                                                  device.CommandContext().SubmissionId());
  }
};

}  // namespace alcedo
