//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/runtime/develop_demosaic.hpp"
#include "edit/runtime/develop_transient.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/compiled_grade_mask.hpp"
#include "edit/runtime/pass_encoder.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "gpu/transient_allocation_policy.hpp"

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
                      PipelineDocument& document, MaskStore* mask_store, bool publish_on_success,
                      TransientAllocationPolicy transient_policy =
                          TransientAllocationPolicy::SessionPacked,
                      std::span<const ActiveRasterMaskInput> active_raster_masks = {})
      -> GraphValueId {
    try {
      auto& workspace = device.Workspace();
      workspace.TransientBuffers().SetAllocationPolicy(transient_policy);
      const bool exact_release =
          transient_policy == TransientAllocationPolicy::ExactRelease;
      if constexpr (requires(Backend& backend, const ExecutionPlan& compiled) {
                      backend.WarmUpPlan(compiled);
                    }) {
        workspace.Device().WarmUpPlan(plan);
      }
      if (workspace.Textures().ByteBudget() == 0) {
        workspace.Textures().SetByteBudget(Backend::DefaultTextureBudgetBytes());
      }
      device.BeginRender();
      workspace.AlignParameterLayout(plan.static_key.topology_hash);
      const auto keys          = BuildFrameResultContentKeys(plan, input, document,
                                                            active_raster_masks);
      const auto completed     = workspace.Device().CompletedSubmission();
      auto&      stats         = device.PassStats();
      const auto hits_before   = workspace.Images().ContentHitCount();
      const auto misses_before = workspace.Images().ContentMissCount();

      if (BindOrMiss(workspace, plan.sensor_linear_output, keys.sensor_linear, keys.sensor_extent,
                     completed)) {
        ++stats.sensor_develop_skip;
      } else {
        const auto* develop_node = document.Develop();
        const auto  develop_method =
            develop_node == nullptr
                ? RawDemosaicMethod::Legacy
                : ResolveDevelopDemosaicMethod(develop_node->Params().Params(), plan.source);
        const bool highlights_reconstruct =
            develop_node != nullptr && develop_node->Params().Params().highlights_reconstruct;
        const auto h2d_before = workspace.Device().HostToDeviceBytes();
        try {
        if constexpr (UsesDevelopTransientArena()) {
          if (transient_policy == TransientAllocationPolicy::SessionPacked) {
            workspace.PrepareDevelopTransients(plan.source, Backend::kCapabilityVersion,
                                               develop_method);
          }
        }
          if (plan.Contains(GpuPassKind::UploadRgb)) {
            PassEncoder<Backend, GpuPassKind::UploadRgb>::Encode(device, plan, input, document,
                                                                 mask_store);
          } else {
            PassEncoder<Backend, GpuPassKind::UploadRaw>::Encode(device, plan, input, document,
                                                                 mask_store);
          }
        } catch (const std::exception& ex) {
          const std::string_view what = ex.what();
          if (what.find("TransientBufferArena") == std::string_view::npos) {
            throw;
          }
          throw std::runtime_error(DescribeDevelopTransientFailure(
              plan.source, RawDemosaicMethodToString(develop_method), highlights_reconstruct,
              ex.what()));
        }
        stats.source_h2d_bytes += workspace.Device().HostToDeviceBytes() - h2d_before;
        ++stats.source_h2d_count;
        Record(device, plan.sensor_linear_output, keys.sensor_linear, keys.sensor_extent);
        ++stats.sensor_develop_execute;
        if constexpr (UsesDevelopTransientArena()) {
          if (transient_policy == TransientAllocationPolicy::SessionPacked) {
            workspace.RecordDevelopTransients(plan.source, Backend::kCapabilityVersion,
                                              develop_method);
          }
        }
        // Develop intermediates are not a cache. Finish the recorded work so backend-owned
        // scratch can be destroyed before Geometry allocates display textures.
        workspace.Device().SynchronizeRecordedWork(device.CommandContext());
      }
      if constexpr (requires(Device& d) { d.ReleaseNeuralDemosaicWorkspace(); }) {
        device.ReleaseNeuralDemosaicWorkspace();
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
      if (exact_release && plan.encode_geometry_resample) {
        workspace.Device().SynchronizeRecordedWork(device.CommandContext());
        workspace.ReleaseConsumedImage(plan.sensor_linear_output);
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
      if (exact_release) {
        workspace.Device().SynchronizeRecordedWork(device.CommandContext());
        workspace.ReleaseConsumedImage(plan.geometry_output);
        if (!plan.encode_geometry_resample) {
          workspace.ReleaseConsumedImage(plan.sensor_linear_output);
        }
      }

      GraphValueId previous_scene = plan.develop_output;
      if (plan.grade_nodes.empty()) {
        ++stats.primary_grade_skip;
      }
      for (const auto& compiled_grade : plan.grade_nodes) {
        if (compiled_grade.mask_stack.has_value()) {
          for (const auto& source : compiled_grade.mask_stack->sources) {
            if (!MaskSourceIsEnabled(document, compiled_grade.node_id, source.mask_id)) {
              continue;
            }
            const auto source_key = keys.Value(source.effective_output);
            if (BindOrMiss(workspace, source.effective_output, source_key, keys.geometry_extent,
                           completed, TextureFormat::R8)) {
              ++stats.mask_skip;
            } else {
              PassEncoder<Backend, GpuPassKind::MaskEvaluate>::Encode(
                  device, plan, input, document, mask_store, compiled_grade, source,
                  active_raster_masks);
              Record(device, source.effective_output, source_key, keys.geometry_extent,
                     TextureFormat::R8);
              ++stats.mask_execute;
            }
          }
          const auto union_key = keys.Value(compiled_grade.mask_output);
          if (BindOrMiss(workspace, compiled_grade.mask_output, union_key, keys.geometry_extent,
                         completed, TextureFormat::R8)) {
            ++stats.mask_union_skip;
          } else {
            PassEncoder<Backend, GpuPassKind::MaskUnion>::Encode(device, plan, input, document,
                                                                 mask_store, compiled_grade);
            Record(device, compiled_grade.mask_output, union_key, keys.geometry_extent,
                   TextureFormat::R8);
            ++stats.mask_union_execute;
          }
          workspace.TransientBuffers().Reset();
        }

        const GraphValueId grade_scene = compiled_grade.scene_output;
        if (BindOrMiss(workspace, grade_scene, keys.Value(grade_scene), keys.geometry_extent,
                       completed)) {
          ++stats.primary_grade_skip;
        } else {
          PassEncoder<Backend, GpuPassKind::PrimaryColorGrade>::Encode(
              device, plan, input, document, mask_store, compiled_grade);
          Record(device, grade_scene, keys.Value(grade_scene), keys.geometry_extent);
          ++stats.primary_grade_execute;
        }
        if (exact_release) {
          workspace.Device().SynchronizeRecordedWork(device.CommandContext());
          if (compiled_grade.mask_stack.has_value()) {
            for (const auto& source : compiled_grade.mask_stack->sources) {
              if (workspace.Images().Find(source.effective_output) != nullptr &&
                  source.effective_output != compiled_grade.mask_output) {
                workspace.ReleaseConsumedImage(source.effective_output);
              }
            }
            workspace.ReleaseConsumedImage(compiled_grade.mask_output);
          }
          if (grade_scene != previous_scene) {
            workspace.ReleaseConsumedImage(previous_scene);
          }
          previous_scene = grade_scene;
        }
      }

      if (BindOrMiss(workspace, plan.display_output, keys.drt_display, keys.geometry_extent,
                     completed)) {
        ++stats.drt_skip;
      } else {
        PassEncoder<Backend, GpuPassKind::Drt>::Encode(device, plan, input, document, mask_store);
        Record(device, plan.display_output, keys.drt_display, keys.geometry_extent);
        ++stats.drt_execute;
      }
      if (exact_release && plan.display_output != plan.SceneInputForDrt()) {
        workspace.Device().SynchronizeRecordedWork(device.CommandContext());
        workspace.ReleaseConsumedImage(plan.SceneInputForDrt());
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
      if constexpr (requires(Device& d) { d.ReleaseNeuralDemosaicWorkspace(); }) {
        device.ReleaseNeuralDemosaicWorkspace();
      }
      device.ReportError(ex.what());
      throw;
    } catch (...) {
      device.CancelRender();
      if constexpr (requires(Device& d) { d.ReleaseNeuralDemosaicWorkspace(); }) {
        device.ReleaseNeuralDemosaicWorkspace();
      }
      device.ReportError("DAG execution failed with an unknown error");
      throw;
    }
  }

 private:
  static constexpr TextureFormat kResultFormat = TextureFormat::Rgba32f;

  static constexpr auto UsesDevelopTransientArena() -> bool {
    if constexpr (requires { Backend::kUsesDevelopTransientArena; }) {
      return Backend::kUsesDevelopTransientArena;
    }
    return true;
  }

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
