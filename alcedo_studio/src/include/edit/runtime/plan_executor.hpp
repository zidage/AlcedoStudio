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
#include "edit/runtime/gpu_node_pass_stats.hpp"
#include "edit/runtime/result_persistence.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"
#include "gpu/transient_allocation_policy.hpp"

namespace alcedo {

class MaskStore;

/**
 * @brief Shared validity skip, encode, cancel, and publish flow for one plan.
 *
 * Skip units match CUDA DAG cache boundaries. A failed encode cancels the
 * incomplete submission and does not publish new revisions. There is no
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
   * @param persistence Which published results this task may look up or replace.
   *        QualityBase uses @ref ResultPersistenceScope::SensorDevelopOnly.
   * @return Display GraphValueId of the compiled plan.
   * @throws std::exception from encode, upload, or submit after CancelRender.
   */
  template <class Device>
  static auto Execute(Device& device, const ExecutionPlan& plan, const PreparedRawInput& input,
                      PipelineDocument& document, MaskStore* mask_store, bool publish_on_success,
                      TransientAllocationPolicy transient_policy =
                          TransientAllocationPolicy::SessionPacked,
                      std::span<const ActiveRasterMaskInput> active_raster_masks = {},
                      ResultPersistenceScope persistence = ResultPersistenceScope::AllCurrentResults)
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
      device.BeginRender();
      workspace.SetResultPersistence(persistence, plan.sensor_linear_output);
      workspace.AlignParameterLayout(plan.static_key.topology_hash);
      auto&      invalidation  = workspace.ResultInvalidation();
      workspace.PrepareResultValidity(plan, document, input, active_raster_masks);
      const ImageExtent sensor_extent{plan.source.develop_output_extent.width,
                                      plan.source.develop_output_extent.height};
      const ImageExtent geometry_extent{plan.geometry.render_extent.width,
                                        plan.geometry.render_extent.height};
      const auto completed     = workspace.Device().CompletedSubmission();
      auto&      stats         = device.PassStats();
      const auto hits_before      = workspace.Images().ContentHitCount();
      const auto misses_before    = workspace.Images().ContentMissCount();
      const auto lookups_before   = workspace.Images().LookupCount();
      const auto revision_before  = workspace.Images().RevisionMissCount();
      const auto represent_before = workspace.Images().RepresentationMissCount();
      const auto publishes_before = workspace.Images().PersistentPublishCount();

      if (BindOrMiss(workspace, invalidation, plan.sensor_linear_output, sensor_extent, completed,
                     stats)) {
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
        workspace.ReleaseStalePublishedImagesAndIdleTextures();
        try {
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
        Record(device, invalidation, plan.sensor_linear_output, sensor_extent);
        ++stats.sensor_develop_execute;
        // Develop intermediates are not a cache. Finish remaining recorded work so
        // backend-owned scratch can be destroyed before Geometry allocates display textures.
        workspace.Device().SynchronizeRecordedWork(device.CommandContext());
      }
      if constexpr (requires(Device& d) { d.ReleaseNeuralDemosaicWorkspace(); }) {
        device.ReleaseNeuralDemosaicWorkspace();
      }
      workspace.TransientBuffers().Reset();
      workspace.TransientBuffers().ReleaseDeviceMemory();

      if (BindOrMiss(workspace, invalidation, plan.geometry_output, geometry_extent, completed,
                     stats)) {
        ++stats.geometry_skip;
      } else {
        PassEncoder<Backend, GpuPassKind::GeometryResample>::Encode(device, plan, input, document,
                                                                    mask_store);
        Record(device, invalidation, plan.geometry_output, geometry_extent);
        ++stats.geometry_execute;
      }
      if (exact_release && plan.encode_geometry_resample) {
        workspace.Device().SynchronizeRecordedWork(device.CommandContext());
        workspace.ReleaseConsumedImage(plan.sensor_linear_output);
      }

      if (BindOrMiss(workspace, invalidation, plan.develop_output, geometry_extent, completed,
                     stats)) {
        ++stats.camera_color_skip;
      } else {
        PassEncoder<Backend, GpuPassKind::CameraToAp1>::Encode(device, plan, input, document,
                                                               mask_store);
        Record(device, invalidation, plan.develop_output, geometry_extent);
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
            if (BindOrMiss(workspace, invalidation, source.effective_output, geometry_extent,
                           completed, stats, TextureFormat::R8)) {
              ++stats.mask_skip;
            } else {
              PassEncoder<Backend, GpuPassKind::MaskEvaluate>::Encode(
                  device, plan, input, document, mask_store, compiled_grade, source,
                  active_raster_masks);
              Record(device, invalidation, source.effective_output, geometry_extent,
                     TextureFormat::R8);
              ++stats.mask_execute;
            }
          }
          if (BindOrMiss(workspace, invalidation, compiled_grade.mask_output, geometry_extent,
                         completed, stats, TextureFormat::R8)) {
            ++stats.mask_union_skip;
          } else {
            PassEncoder<Backend, GpuPassKind::MaskUnion>::Encode(device, plan, input, document,
                                                                 mask_store, compiled_grade);
            Record(device, invalidation, compiled_grade.mask_output, geometry_extent,
                   TextureFormat::R8);
            ++stats.mask_union_execute;
          }
          workspace.TransientBuffers().Reset();
        }

        const GraphValueId grade_scene = compiled_grade.scene_output;
        if (BindOrMiss(workspace, invalidation, grade_scene, geometry_extent, completed, stats)) {
          ++stats.primary_grade_skip;
        } else {
          PassEncoder<Backend, GpuPassKind::PrimaryColorGrade>::Encode(
              device, plan, input, document, mask_store, compiled_grade);
          Record(device, invalidation, grade_scene, geometry_extent);
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

      if (BindOrMiss(workspace, invalidation, plan.display_output, geometry_extent, completed,
                     stats)) {
        ++stats.drt_skip;
      } else {
        PassEncoder<Backend, GpuPassKind::Drt>::Encode(device, plan, input, document, mask_store);
        Record(device, invalidation, plan.display_output, geometry_extent);
        ++stats.drt_execute;
      }
      if (exact_release && plan.display_output != plan.SceneInputForDrt()) {
        workspace.Device().SynchronizeRecordedWork(device.CommandContext());
        workspace.ReleaseConsumedImage(plan.SceneInputForDrt());
      }

      stats.result_content_hits += workspace.Images().ContentHitCount() - hits_before;
      stats.result_content_misses += workspace.Images().ContentMissCount() - misses_before;
      stats.result_revision_misses += workspace.Images().RevisionMissCount() - revision_before;
      stats.result_representation_misses +=
          workspace.Images().RepresentationMissCount() - represent_before;
      stats.persistent_result_lookups += workspace.Images().LookupCount() - lookups_before;
      device.EndRender();
      if (publish_on_success) {
        device.PublishResults();
        stats.persistent_result_publishes +=
            workspace.Images().PersistentPublishCount() - publishes_before;
        invalidation.CompleteMatchingImages(workspace.Images());
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

  template <class Workspace>
  static auto BindOrMiss(Workspace& images_owner, RuntimeInvalidationState& invalidation,
                         const GraphValueId& id, ImageExtent extent, std::uint64_t completed,
                         GpuNodePassStats& stats, TextureFormat format = kResultFormat) -> bool {
    if (!images_owner.PersistsResult(id)) {
      ++stats.result_policy_bypass;
      return false;
    }
    const auto required = invalidation.RequiredRevision(id);
    const auto needed   = invalidation.MakeImageRepresentation(id, extent, format);
    return images_owner.Images().BindValidResult(id, required, needed, completed) != nullptr;
  }

  template <class Device>
  static void Record(Device& device, RuntimeInvalidationState& invalidation, const GraphValueId& id,
                     ImageExtent extent, TextureFormat format = kResultFormat) {
    const auto required = invalidation.RequiredRevision(id);
    const auto needed   = invalidation.MakeImageRepresentation(id, extent, format);
    device.Workspace().Images().RecordUnpublished(id, required, needed,
                                                  device.CommandContext().SubmissionId());
  }
};

}  // namespace alcedo
