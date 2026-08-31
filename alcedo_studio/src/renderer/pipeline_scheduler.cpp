//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "renderer/pipeline_scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "edit/pipeline/pipeline_apply_request.hpp"
#include "image/image_buffer.hpp"
#include "io/image/image_loader.hpp"
#include "renderer/pipeline_task.hpp"
#include "utils/profiler/profiler.hpp"

namespace alcedo {
namespace {
constexpr float kRotationPreviewEpsilon        = 1e-4f;
constexpr float kFullFrameRegionEpsilon        = 1e-4f;
constexpr int   kFastPreviewMaxLongEdge        = 2560;
constexpr int   kQualityBasePreviewMaxLongEdge = 4096;
constexpr int   kHsReferenceMaskMaxLongEdge    = 2048;
constexpr int   kFullResPreviewMaxLongEdge     = 8192;

auto HasActiveGeometryRotation(const std::shared_ptr<CPUPipelineExecutor>& pipeline_executor)
    -> bool {
  if (!pipeline_executor) {
    return false;
  }

  auto&      geometry_stage = pipeline_executor->GetStage(PipelineStageName::Geometry_Adjustment);
  const auto crop_rotate_op = geometry_stage.GetOperator(OperatorType::CROP_ROTATE);
  if (!crop_rotate_op.has_value() || crop_rotate_op.value() == nullptr) {
    return false;
  }

  const auto* entry = crop_rotate_op.value();
  if (!entry->enable_ || !entry->op_) {
    return false;
  }

  const auto params = entry->op_->GetParams();
  if (!params.contains("crop_rotate") || !params["crop_rotate"].is_object()) {
    return false;
  }

  const auto& crop_rotate = params["crop_rotate"];
  if (!crop_rotate.value("enabled", false)) {
    return false;
  }

  const bool enable_crop = crop_rotate.value("enable_crop", false);
  if (!enable_crop) {
    return false;
  }

  const float angle = crop_rotate.value("angle_degrees", 0.0f);
  return std::abs(angle) > kRotationPreviewEpsilon;
}

void HashCombine(std::uint64_t& seed, std::uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
}

void HashJson(std::uint64_t& seed, const nlohmann::json& value) {
  HashCombine(seed, std::hash<std::string>{}(value.dump()));
}

void HashStageOperator(std::uint64_t&                              seed,
                       const std::shared_ptr<CPUPipelineExecutor>& pipeline_executor,
                       PipelineStageName stage_name, OperatorType op_type) {
  if (!pipeline_executor) {
    return;
  }

  auto&      stage = pipeline_executor->GetStage(stage_name);
  const auto entry = stage.GetOperator(op_type);
  if (!entry.has_value() || entry.value() == nullptr) {
    HashCombine(seed, 0);
    return;
  }

  const auto* op_entry = entry.value();
  HashCombine(seed, op_entry->enable_ ? 1 : 0);
  if (op_entry->op_) {
    HashJson(seed, op_entry->op_->GetParams());
  }
}

auto BuildRenderSourceCacheKey(const PipelineTask& task) -> std::uint64_t {
  std::uint64_t key                        = 0xa44b4e45f2f8891fULL;
  bool          has_stable_source_identity = false;
  if (task.pipeline_executor_) {
    const auto bound_file = task.pipeline_executor_->GetBoundFile();
    HashCombine(key, static_cast<std::uint64_t>(bound_file));
    has_stable_source_identity = bound_file != 0;
  }
  if (task.input_desc_) {
    if (task.input_desc_->image_id_ != 0) {
      HashCombine(key, static_cast<std::uint64_t>(task.input_desc_->image_id_));
      has_stable_source_identity = true;
    }
    if (!task.input_desc_->image_path_.empty()) {
      HashCombine(key, std::hash<std::wstring>{}(task.input_desc_->image_path_.wstring()));
      has_stable_source_identity = true;
    }
  }
  if (!has_stable_source_identity) {
    HashCombine(key,
                static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(task.input_.get())));
    HashCombine(
        key, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(task.input_desc_.get())));
  }
  HashStageOperator(key, task.pipeline_executor_, PipelineStageName::Image_Loading,
                    OperatorType::RAW_DECODE);
  HashStageOperator(key, task.pipeline_executor_, PipelineStageName::Image_Loading,
                    OperatorType::LENS_CALIBRATION);
  HashStageOperator(key, task.pipeline_executor_, PipelineStageName::Geometry_Adjustment,
                    OperatorType::CROP_ROTATE);
  return key;
}

auto BuildSourceRoiRect(const std::optional<ViewportRenderRegion>& viewport_region, int region_x,
                        int region_y, float region_scale_x, float region_scale_y) -> FrameRoiRect {
  if (viewport_region.has_value() && viewport_region->reference_width_ > 0 &&
      viewport_region->reference_height_ > 0) {
    const float reference_width =
        static_cast<float>(std::max(1, viewport_region->reference_width_));
    const float reference_height =
        static_cast<float>(std::max(1, viewport_region->reference_height_));
    return {
        std::clamp(static_cast<float>(std::max(0, region_x)) / reference_width, 0.0f, 1.0f),
        std::clamp(static_cast<float>(std::max(0, region_y)) / reference_height, 0.0f, 1.0f),
        std::clamp(region_scale_x, 1e-4f, 1.0f),
        std::clamp(region_scale_y, 1e-4f, 1.0f),
    };
  }
  return {0.0f, 0.0f, 1.0f, 1.0f};
}

auto ViewportTargetLongEdge(const std::optional<ViewportRenderRegion>& viewport_region) -> int {
  if (!viewport_region.has_value()) {
    return 0;
  }
  return std::max(viewport_region->target_width_, viewport_region->target_height_);
}

auto MetadataFromRegion(const FramePreviewMetadata&                base_metadata,
                        const std::optional<ViewportRenderRegion>& viewport_region, int region_x,
                        int region_y, float region_scale_x, float region_scale_y)
    -> FramePreviewMetadata {
  FramePreviewMetadata metadata = base_metadata;
  metadata.source_roi_norm =
      BuildSourceRoiRect(viewport_region, region_x, region_y, region_scale_x, region_scale_y);
  return metadata;
}

auto RenderFrameRoleId(FrameRole role) -> int {
  switch (role) {
    case FrameRole::InteractivePrimary:
      return OperatorParams::kRenderFrameRoleInteractivePrimary;
    case FrameRole::QualityBase:
      return OperatorParams::kRenderFrameRoleQualityBase;
    case FrameRole::DetailPatch:
      return OperatorParams::kRenderFrameRoleDetailPatch;
  }
  return OperatorParams::kRenderFrameRoleInteractivePrimary;
}

void ApplyRenderFrameRole(const std::shared_ptr<CPUPipelineExecutor>& pipeline_executor,
                          const FramePreviewMetadata&                 metadata) {
  if (!pipeline_executor) {
    return;
  }
  pipeline_executor->GetGlobalParams().render_frame_role_ = RenderFrameRoleId(metadata.frame_role);
}

auto LoadViewportRegion(const std::shared_ptr<CPUPipelineExecutor>& pipeline_executor,
                        bool                                        should_use_viewport_region,
                        const std::optional<ViewportRenderRegion>&  requested_region)
    -> std::optional<ViewportRenderRegion> {
  if (!pipeline_executor || !should_use_viewport_region) {
    return std::nullopt;
  }
  if (requested_region) {
    return requested_region;
  }
  return pipeline_executor->GetViewportRenderRegion();
}
}  // namespace

namespace {

auto MakeGeometryRequest(const std::optional<ViewportRenderRegion>& viewport, int max_edge,
                         bool export_quality) -> RenderRequest {
  RenderRequest request;
  if (viewport.has_value() && viewport->reference_width_ > 0 && viewport->reference_height_ > 0) {
    request.view.visible_rect_in_edit_space = {
        static_cast<float>(viewport->x_) / static_cast<float>(viewport->reference_width_),
        static_cast<float>(viewport->y_) / static_cast<float>(viewport->reference_height_),
        viewport->scale_x_, viewport->scale_y_};
    if (viewport->target_width_ > 0 && viewport->target_height_ > 0) {
      request.view.viewport_extent = {static_cast<std::uint32_t>(viewport->target_width_),
                                      static_cast<std::uint32_t>(viewport->target_height_)};
    }
  }
  request.resolution.max_edge = static_cast<std::uint32_t>(std::max(0, max_edge));
  request.resolution.quality  = export_quality ? RenderQuality::Export : RenderQuality::Preview;
  return request;
}

auto DownsampleForRenderType(RenderType render_type) -> ResizeDownsampleAlgorithm {
  switch (render_type) {
    case RenderType::QUALITY_BASE_PREVIEW:
    case RenderType::DETAIL_ROI_PREVIEW:
    case RenderType::FULL_RES_PREVIEW:
    case RenderType::FULL_RES_EXPORT:
      return ResizeDownsampleAlgorithm::Area;
    case RenderType::FAST_PREVIEW:
    case RenderType::THUMBNAIL:
      return ResizeDownsampleAlgorithm::Bilinear;
  }
  return ResizeDownsampleAlgorithm::Bilinear;
}

}  // namespace

auto PipelineTask::MakeApplyRequest() const -> PipelineApplyRequest {
  PipelineApplyRequest request;
  if (!pipeline_executor_) {
    return request;
  }
  request.cancel_requested = cancel_requested_;
  auto&      desc          = options_.render_desc_;
  const auto requested_render_type        = desc.render_type_;

  const bool rotation_active_fast_preview = (requested_render_type == RenderType::FAST_PREVIEW) &&
                                            HasActiveGeometryRotation(pipeline_executor_);

  int        region_x                = desc.x_;
  int        region_y                = desc.y_;
  float      region_scale_x          = desc.scale_factor_x_;
  float      region_scale_y          = desc.scale_factor_y_;
  int        region_reference_width  = 0;
  int        region_reference_height = 0;
  const bool viewport_region_render  = (requested_render_type == RenderType::FAST_PREVIEW ||
                                       requested_render_type == RenderType::DETAIL_ROI_PREVIEW) &&
                                      desc.use_viewport_region_;
  const auto viewport_region = LoadViewportRegion(
      pipeline_executor_, viewport_region_render && !rotation_active_fast_preview,
      desc.viewport_region_);
  if (viewport_region.has_value()) {
    region_x                = viewport_region->x_;
    region_y                = viewport_region->y_;
    region_scale_x          = viewport_region->scale_x_;
    region_scale_y          = viewport_region->scale_y_;
    region_reference_width  = viewport_region->reference_width_;
    region_reference_height = viewport_region->reference_height_;
  }
  (void)region_reference_width;
  (void)region_reference_height;

  FramePreviewMetadata  frame_metadata    = desc.frame_metadata_;
  FramePresentationMode presentation_mode = FramePresentationMode::FullFrame;
  request.sink                            = pipeline_executor_->GetFrameSink();

  auto finish = [&](DecodeRes decode, bool host_output, RenderCachePolicy cache, int max_edge,
                    bool export_quality, std::optional<ViewportRenderRegion> view) {
    request.decode_res          = decode;
    request.require_host_output = host_output;
    request.cache_policy        = cache;
    request.geometry            = MakeGeometryRequest(view, max_edge, export_quality);
    request.submission          = FrameCompletionSubmission{.metadata = frame_metadata,
                                                            .mode     = presentation_mode};
  };

  if (requested_render_type == RenderType::FAST_PREVIEW) {
    frame_metadata.frame_role    = FrameRole::InteractivePrimary;
    const bool full_frame_region = region_x == 0 && region_y == 0 &&
                                   region_scale_x >= (1.0f - kFullFrameRegionEpsilon) &&
                                   region_scale_y >= (1.0f - kFullFrameRegionEpsilon);
    if (rotation_active_fast_preview || full_frame_region) {
      presentation_mode              = FramePresentationMode::ViewportTransformed;
      frame_metadata.source_roi_norm = {};
      finish(DecodeRes::FULL, false, RenderCachePolicy::UseSessionCache, kFastPreviewMaxLongEdge,
             false, viewport_region);
      return request;
    }
    presentation_mode = FramePresentationMode::RoiFrame;
    frame_metadata    = MetadataFromRegion(frame_metadata, viewport_region, region_x, region_y,
                                           region_scale_x, region_scale_y);
    frame_metadata.scope_update_allowed = frame_metadata.scope_refresh_requested;
    finish(DecodeRes::FULL, false, RenderCachePolicy::UseSessionCache, kFastPreviewMaxLongEdge,
           false, viewport_region);
    return request;
  }
  if (requested_render_type == RenderType::QUALITY_BASE_PREVIEW) {
    frame_metadata.frame_role      = FrameRole::QualityBase;
    frame_metadata.source_roi_norm = {};
    presentation_mode              = FramePresentationMode::ViewportTransformed;
    finish(DecodeRes::FULL, false, RenderCachePolicy::UseSessionCache,
           kQualityBasePreviewMaxLongEdge, false, std::nullopt);
    return request;
  }
  if (requested_render_type == RenderType::DETAIL_ROI_PREVIEW) {
    frame_metadata.frame_role = (region_scale_x < (1.0f - 1e-4f) || region_scale_y < (1.0f - 1e-4f))
                                    ? FrameRole::DetailPatch
                                    : FrameRole::QualityBase;
    frame_metadata    = MetadataFromRegion(frame_metadata, viewport_region, region_x, region_y,
                                           region_scale_x, region_scale_y);
    presentation_mode = FramePresentationMode::ViewportTransformed;
    const int detail_target_long_edge = ViewportTargetLongEdge(viewport_region);
    finish(DecodeRes::FULL, false, RenderCachePolicy::UseSessionCache,
           detail_target_long_edge > 0 ? detail_target_long_edge : 0, false, viewport_region);
    return request;
  }
  if (requested_render_type == RenderType::THUMBNAIL) {
    presentation_mode    = FramePresentationMode::ViewportTransformed;
    request.sink         = nullptr;
    request.output_color = std::nullopt;
    finish(desc.decode_res_, true, RenderCachePolicy::BypassSessionCache,
           static_cast<int>(desc.max_edge_), false, std::nullopt);
    return request;
  }
  if (requested_render_type == RenderType::FULL_RES_PREVIEW) {
    presentation_mode = FramePresentationMode::ViewportTransformed;
    finish(DecodeRes::FULL, false, RenderCachePolicy::UseSessionCache, kFullResPreviewMaxLongEdge,
           false, std::nullopt);
    return request;
  }
  if (requested_render_type == RenderType::FULL_RES_EXPORT) {
    presentation_mode    = FramePresentationMode::ViewportTransformed;
    request.sink         = nullptr;
    request.output_color = options_.export_output_color_;
    finish(DecodeRes::FULL, true, RenderCachePolicy::BypassSessionCache, 0, true, std::nullopt);
    return request;
  }
  throw std::runtime_error("[ERROR] PipelineTask: Unknown render type");
}

void PipelineTask::SetExecutorRenderParams() {
  if (!pipeline_executor_) {
    return;
  }
  const auto request = MakeApplyRequest();
  auto&      desc    = options_.render_desc_;
  const auto requested_render_type = desc.render_type_;
  const bool rotation_active_fast_preview = (requested_render_type == RenderType::FAST_PREVIEW) &&
                                            HasActiveGeometryRotation(pipeline_executor_);
  const bool viewport_region_render = (requested_render_type == RenderType::FAST_PREVIEW ||
                                       requested_render_type == RenderType::DETAIL_ROI_PREVIEW) &&
                                      desc.use_viewport_region_;
  const auto viewport_region = LoadViewportRegion(
      pipeline_executor_, viewport_region_render && !rotation_active_fast_preview,
      desc.viewport_region_);
  int   region_x               = desc.x_;
  int   region_y               = desc.y_;
  float region_scale_x         = desc.scale_factor_x_;
  float region_scale_y         = desc.scale_factor_y_;
  int   region_reference_width = 0;
  int   region_reference_height = 0;
  if (viewport_region.has_value()) {
    region_x                = viewport_region->x_;
    region_y                = viewport_region->y_;
    region_scale_x          = viewport_region->scale_x_;
    region_scale_y          = viewport_region->scale_y_;
    region_reference_width  = viewport_region->reference_width_;
    region_reference_height = viewport_region->reference_height_;
  }
  pipeline_executor_->SetCancelRequested(request.cancel_requested);
  pipeline_executor_->SetRenderRequestViewport(viewport_region);
  pipeline_executor_->BindFrameSubmission(request.submission.metadata, request.submission.mode);
  ApplyRenderFrameRole(pipeline_executor_, request.submission.metadata);
  pipeline_executor_->SetResizeDownsampleAlgorithm(DownsampleForRenderType(requested_render_type));
  pipeline_executor_->SetRenderRegion(region_x, region_y, region_scale_x, region_scale_y,
                                      region_reference_width, region_reference_height);
  pipeline_executor_->SetForceCPUOutput(request.require_host_output);
  if (request.geometry.resolution.max_edge > 0) {
    pipeline_executor_->SetRenderRes(false,
                                     static_cast<int>(request.geometry.resolution.max_edge));
  } else {
    pipeline_executor_->SetRenderRes(true);
  }
  pipeline_executor_->SetEnableCache(request.cache_policy == RenderCachePolicy::UseSessionCache);
  pipeline_executor_->SetDecodeRes(request.decode_res);
  pipeline_executor_->GetGlobalParams().render_hs_preserve_source_detail_ =
      request.geometry.resolution.quality == RenderQuality::Export;
}

void PipelineTask::ResetPreviewRenderParams() {
  if (!pipeline_executor_) {
    return;
  }
  // Transition back to fast-preview baseline state.
  pipeline_executor_->SetResizeDownsampleAlgorithm(ResizeDownsampleAlgorithm::Bilinear);
  pipeline_executor_->SetRenderRes(false, kFastPreviewMaxLongEdge);
  pipeline_executor_->SetForceCPUOutput(false);
  pipeline_executor_->SetEnableCache(true);
  pipeline_executor_->SetDecodeRes(DecodeRes::FULL);
  pipeline_executor_->SetCancelRequested(nullptr);
  pipeline_executor_->ReleasePreviewGpuScratch();
}

void PipelineTask::ResetThumbnailRenderParams() {
  if (!pipeline_executor_) {
    return;
  }
  // Transition to full-res preview baseline state.
  pipeline_executor_->SetResizeDownsampleAlgorithm(ResizeDownsampleAlgorithm::Area);
  pipeline_executor_->SetRenderRes(true, kFastPreviewMaxLongEdge);
  pipeline_executor_->SetForceCPUOutput(false);
  pipeline_executor_->SetEnableCache(true);
  pipeline_executor_->SetDecodeRes(DecodeRes::FULL);
  pipeline_executor_->SetCancelRequested(nullptr);
}

PipelineScheduler::PipelineScheduler() : thread_pool_(1) {}

PipelineScheduler::PipelineScheduler(size_t thread_count) : thread_pool_(thread_count) {}

auto PipelineScheduler::IsStaleForSink(IFrameSink* sink, std::uint64_t request_id) -> bool {
  if (!sink || request_id == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(scheduler_lock_);
  const auto                  it = latest_submitted_request_id_.find(sink);
  if (it == latest_submitted_request_id_.end()) {
    return false;
  }
  return request_id < it->second;
}

void PipelineScheduler::MarkSinkApplyStarted(IFrameSink* sink, std::uint64_t request_id) {
  if (!sink || request_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(scheduler_lock_);
  auto&                       latest = latest_submitted_request_id_[sink];
  latest                             = std::max(latest, request_id);
}

void PipelineScheduler::ScheduleWork(std::function<void()> work) {
  thread_pool_.Submit(std::move(work));
}

void PipelineScheduler::ScheduleTask(PipelineTask&& task) {
  {
    std::lock_guard<std::mutex> lock(scheduler_lock_);
    task.task_id_ = id_generator_.GenerateID();
    if (task.request_id_ == 0) {
      task.request_id_ = next_request_id_++;
    } else {
      next_request_id_ = std::max(next_request_id_, task.request_id_ + 1);
    }
    task.options_.render_desc_.frame_metadata_.presentation_request_id = task.request_id_;
  }
  thread_pool_.Submit([this, task = std::move(task)]() mutable {
    std::optional<bool> completion_result;
    std::string         completion_message;
    // Some render paths return from inside the render_lock scope. Record the
    // result there, but invoke the external completion only when this outer
    // guard is destroyed, after every inner lock and render parameter guard.
    auto                completion_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1), [&task, &completion_result, &completion_message](void*) {
          if (!completion_result.has_value() || !task.on_complete_) {
            return;
          }
          try {
            task.on_complete_(*completion_result, std::move(completion_message));
          } catch (...) {
          }
        });
    const auto finish = [&completion_result, &completion_message](bool        success,
                                                                  std::string message = {}) {
      if (completion_result.has_value()) {
        return;
      }
      completion_result  = success;
      completion_message = std::move(message);
    };

    const auto set_blocking_value = [&task](std::shared_ptr<ImageBuffer> value) {
      if (!task.options_.is_blocking_ || !task.result_) {
        return;
      }
      try {
        task.result_->set_value(std::move(value));
      } catch (...) {
      }
    };

    const auto set_blocking_exception = [&task]() {
      if (!task.options_.is_blocking_ || !task.result_) {
        return;
      }
      try {
        task.result_->set_exception(std::current_exception());
      } catch (...) {
      }
    };

    const auto apply_state_transition_after_render = [&task]() {
      const auto render_type = task.options_.render_desc_.render_type_;
      if ((render_type == RenderType::QUALITY_BASE_PREVIEW ||
           render_type == RenderType::DETAIL_ROI_PREVIEW ||
           render_type == RenderType::FULL_RES_PREVIEW) &&
          task.pipeline_executor_) {
        task.pipeline_executor_->ReleasePreviewGpuScratch();
      }
    };

    const auto notify_thumbnail_failure_callbacks = [&task]() {
      if (task.options_.render_desc_.render_type_ != RenderType::THUMBNAIL) {
        return;
      }

      ImageBuffer empty_result;
      if (task.options_.is_callback_ && task.callback_) {
        try {
          (*task.callback_)(empty_result);
        } catch (...) {
        }
      }
      if (task.options_.is_seq_callback_ && task.seq_callback_) {
        try {
          (*task.seq_callback_)(empty_result, task.task_id_);
        } catch (...) {
        }
      }
    };

    const auto task_cancelled = [&task]() {
      if (!task.cancel_requested_) {
        return false;
      }
      try {
        return task.cancel_requested_();
      } catch (...) {
        return true;
      }
    };

    try {
      std::shared_ptr<ImageBuffer> result_copy;
      {
        if (task_cancelled()) {
          notify_thumbnail_failure_callbacks();
          set_blocking_value(nullptr);
          finish(false);
          return;
        }
        if (task.prepare_) {
          bool prepared = false;
          try {
            prepared = (*task.prepare_)(task);
          } catch (const std::exception& ex) {
            notify_thumbnail_failure_callbacks();
            set_blocking_exception();
            finish(false, ex.what());
            return;
          } catch (...) {
            notify_thumbnail_failure_callbacks();
            set_blocking_exception();
            finish(false, "Pipeline render failed");
            return;
          }
          if (!prepared) {
            notify_thumbnail_failure_callbacks();
            set_blocking_value(nullptr);
            finish(false);
            return;
          }
        }
        if (task_cancelled()) {
          notify_thumbnail_failure_callbacks();
          set_blocking_value(nullptr);
          finish(false);
          return;
        }
        if (task.input_desc_ && !task.input_) {
          // Load image data into buffer
          task.input_ = std::make_shared<ImageBuffer>(
              ByteBufferLoader::LoadByteBufferFromImage(task.input_desc_));
        }
        if (task_cancelled()) {
          notify_thumbnail_failure_callbacks();
          set_blocking_value(nullptr);
          finish(false);
          return;
        }
        if (task.input_) {
          // render_lock_ is sole live-pipeline ownership for the full task:
          // configure + Apply + present handoff. History / structural rebuild
          // queues on this lock (owner thread pumps events while waiting so
          // present slot waits can complete without dropping ownership).
          std::unique_lock<std::mutex> render_lock;
          auto&                        render_desc = task.options_.render_desc_;
          PipelineApplyRequest         apply_request;

          if (task.pipeline_executor_) {
            render_lock = std::unique_lock<std::mutex>(task.pipeline_executor_->GetRenderLock());

            if (task.configure_under_render_lock_) {
              bool prepared = false;
              try {
                prepared = (*task.configure_under_render_lock_)(task);
              } catch (const std::exception& ex) {
                notify_thumbnail_failure_callbacks();
                set_blocking_exception();
                finish(false, ex.what());
                return;
              } catch (...) {
                notify_thumbnail_failure_callbacks();
                set_blocking_exception();
                finish(false, "Pipeline render failed");
                return;
              }
              if (!prepared) {
                notify_thumbnail_failure_callbacks();
                set_blocking_value(nullptr);
                finish(false);
                return;
              }
            }

            apply_request = task.MakeApplyRequest();
          }

          IFrameSink* output_sink = apply_request.sink;
          if (IsStaleForSink(output_sink, task.request_id_)) {
            notify_thumbnail_failure_callbacks();
            set_blocking_value(nullptr);
            finish(false);
            return;
          }

          if (task_cancelled()) {
            apply_state_transition_after_render();
            notify_thumbnail_failure_callbacks();
            set_blocking_value(nullptr);
            finish(false);
            return;
          }

          if (IsStaleForSink(output_sink, task.request_id_)) {
            std::cout << "PipelineScheduler: Stale for sink detected!\n";
            apply_state_transition_after_render();
            notify_thumbnail_failure_callbacks();
            set_blocking_value(nullptr);
            finish(false);
            return;
          }

          MarkSinkApplyStarted(output_sink, task.request_id_);

          auto result         = task.pipeline_executor_->Apply(task.input_, apply_request);
          bool result_has_cpu = false;
          if (result && result->cpu_data_valid_) {
            try {
              result_has_cpu = !result->GetCPUData().empty();
            } catch (...) {
              result_has_cpu = false;
            }
          }
          const bool require_gpu_valid = (render_desc.render_type_ != RenderType::THUMBNAIL);
          const bool result_valid_for_copy =
              result && result_has_cpu && (!require_gpu_valid || result->gpu_data_valid_);

          if (IsStaleForSink(output_sink, task.request_id_)) {
            std::cout << "PipelineScheduler: Stale for sink detected!\n";
            apply_state_transition_after_render();
            if (render_desc.render_type_ == RenderType::THUMBNAIL) {
              notify_thumbnail_failure_callbacks();
            }
            set_blocking_value(nullptr);
            finish(false);
            return;
          }

          if (render_desc.render_type_ == RenderType::FAST_PREVIEW ||
              render_desc.render_type_ == RenderType::QUALITY_BASE_PREVIEW ||
              render_desc.render_type_ == RenderType::DETAIL_ROI_PREVIEW ||
              render_desc.render_type_ == RenderType::FULL_RES_PREVIEW || !result_valid_for_copy) {
            if (render_desc.render_type_ == RenderType::THUMBNAIL && !result_valid_for_copy) {
              notify_thumbnail_failure_callbacks();
            }
            const bool ok = result != nullptr;
            set_blocking_value(result);
            apply_state_transition_after_render();
            finish(ok);
            return;
          }

          result_copy = std::make_shared<ImageBuffer>(result->GetCPUData());
          apply_state_transition_after_render();
        }
      }

      if (result_copy) {
        if (task.options_.is_callback_ && task.callback_) {
          (*task.callback_)(*result_copy);
        }
        if (task.options_.is_seq_callback_ && task.seq_callback_) {
          (*task.seq_callback_)(*result_copy, task.task_id_);
        }
        set_blocking_value(result_copy);
        finish(true);
      } else {
        // In case of failure, set nullptr
        set_blocking_value(nullptr);
        finish(false);
      }
    } catch (const std::exception& ex) {
      notify_thumbnail_failure_callbacks();
      set_blocking_exception();
      finish(false, ex.what());
    } catch (...) {
      notify_thumbnail_failure_callbacks();
      set_blocking_exception();
      finish(false, "Pipeline render failed");
    }
  });
}
}  // namespace alcedo
