//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/pipeline/pipeline_cpu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>

#include "edit/pipeline/pipeline.hpp"

#include <opencv2/opencv.hpp>

#include "edit/operators/op_base.hpp"
#include "edit/operators/raw/raw_decode_op.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/pipeline/pipeline_stage.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {

namespace {
using ProfileClock = std::chrono::steady_clock;

auto DurationToMs(const ProfileClock::duration duration) -> double {
  return std::chrono::duration<double, std::milli>(duration).count();
}

auto FormatDurationMs(const double duration_ms) -> std::string {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << duration_ms << " ms";
  return oss.str();
}

void SetCleanBaselineAdjustableOperators(
    std::array<PipelineStage, static_cast<int>(PipelineStageName::Stage_Count)>& stages,
    OperatorParams& global_params) {
  const nlohmann::json baseline = pipeline_defaults::MakeCleanBaselineAdjustableParams();

  auto set_enabled = [&](PipelineStageName stage_name, OperatorType op_type,
                         const nlohmann::json& params, bool enabled = true) {
    auto& stage = stages[static_cast<int>(stage_name)];
    stage.SetOperator(op_type, params, global_params);
    stage.EnableOperator(op_type, enabled, global_params);
  };

  set_enabled(PipelineStageName::Geometry_Adjustment, OperatorType::CROP_ROTATE,
              baseline.at("crop_rotate"), false);

  set_enabled(PipelineStageName::Basic_Adjustment, OperatorType::EXPOSURE, baseline.at("exposure"));
  set_enabled(PipelineStageName::Basic_Adjustment, OperatorType::CONTRAST, baseline.at("contrast"));
  set_enabled(PipelineStageName::Basic_Adjustment, OperatorType::WHITE, baseline.at("white"));
  set_enabled(PipelineStageName::Basic_Adjustment, OperatorType::BLACK, baseline.at("black"));
  set_enabled(PipelineStageName::Basic_Adjustment, OperatorType::HIGHLIGHTS,
              baseline.at("highlights"));
  set_enabled(PipelineStageName::Basic_Adjustment, OperatorType::SHADOWS, baseline.at("shadows"));
  set_enabled(PipelineStageName::Basic_Adjustment, OperatorType::CURVE, baseline.at("curve"));

  set_enabled(PipelineStageName::Color_Adjustment, OperatorType::SATURATION,
              baseline.at("saturation"));
  set_enabled(PipelineStageName::Color_Adjustment, OperatorType::VIBRANCE, baseline.at("vibrance"));
  set_enabled(PipelineStageName::Color_Adjustment, OperatorType::HLS, baseline.at("HLS"));
  set_enabled(PipelineStageName::Color_Adjustment, OperatorType::COLOR_WHEEL,
              baseline.at("color_wheel"));
  auto& color_stage = stages[static_cast<int>(PipelineStageName::Color_Adjustment)];
  color_stage.EnableOperator(OperatorType::LMT, true);
  color_stage.SetOperator(OperatorType::LMT, baseline.at("ocio_lmt"), global_params);

  set_enabled(PipelineStageName::Detail_Adjustment, OperatorType::SHARPEN, baseline.at("sharpen"));
  set_enabled(PipelineStageName::Detail_Adjustment, OperatorType::CLARITY, baseline.at("clarity"));

  set_enabled(PipelineStageName::Output_Transform, OperatorType::ODT, baseline.at("odt"));
  set_enabled(PipelineStageName::Output_Transform, OperatorType::FILM_GRAIN,
              baseline.at("film_grain"));
  set_enabled(PipelineStageName::Output_Transform, OperatorType::HALATION,
              baseline.at("halation"));
}

void PrintPipelineProfile(const ProfileClock::time_point apply_start,
                          const std::vector<std::string>& executor_steps,
                          const std::vector<std::string>& stage_profiles) {
  std::ostringstream summary;
  summary << "[PROFILE][PipelineCPU] total="
          << FormatDurationMs(DurationToMs(ProfileClock::now() - apply_start));
  for (const auto& step : executor_steps) {
    summary << " | " << step;
  }
  std::cout << summary.str() << '\n';

  for (const auto& profile : stage_profiles) {
    if (!profile.empty()) {
      std::cout << "[PROFILE][PipelineCPU] " << profile << '\n';
    }
  }
}

auto ToResizeAlgorithmParam(ResizeDownsampleAlgorithm algorithm) -> const char* {
  switch (algorithm) {
    case ResizeDownsampleAlgorithm::Bilinear:
      return "bilinear";
    case ResizeDownsampleAlgorithm::Area:
      return "inter_area";
  }

  throw std::runtime_error("CPUPipelineExecutor: unsupported resize downsample algorithm");
}

}  // namespace

CPUPipelineExecutor::CPUPipelineExecutor()
    : enable_cache_(false),
      stages_({{PipelineStageName::Image_Loading, enable_cache_, false},
               {PipelineStageName::Geometry_Adjustment, enable_cache_, false},
               {PipelineStageName::To_WorkingSpace, enable_cache_, true},
               {PipelineStageName::Basic_Adjustment, enable_cache_, true},
               {PipelineStageName::Color_Adjustment, enable_cache_, true},
               {PipelineStageName::Detail_Adjustment, enable_cache_, true},
               {PipelineStageName::Output_Transform, enable_cache_, true}}) {
  render_params_["resize"]                         = {};
  render_params_["resize"]["downsample_algorithm"] = ToResizeAlgorithmParam(
      ResizeDownsampleAlgorithm::Area);

  // Initialize default pipeline
  InitDefaultPipeline();
}

void CPUPipelineExecutor::ResetExecutionStagesCache() {
  for (auto& stage : exec_stages_) {
    stage->ResetRuntimeResources(PipelineStage::RuntimeResetMode::InvalidateCache);
  }
}

void CPUPipelineExecutor::ResetStages() {
  for (size_t i = 0; i < stages_.size(); i++) {
    stages_[i].ResetAll();
  }
}

void CPUPipelineExecutor::SetEnableCache(bool enable_cache) {
  if (enable_cache_ == enable_cache && enable_cache) return;
  enable_cache_ = enable_cache;
  // Reinitialize stages with the new cache setting
  ResetExecutionStagesCache();
  for (auto* stage : exec_stages_) {
    const bool stage_cache_enabled = (stage->stage_ == PipelineStageName::Merged_Stage)
                                         ? false
                                         : enable_cache_;
    stage->SetEnableCache(stage_cache_enabled);
  }
}

CPUPipelineExecutor::CPUPipelineExecutor(bool enable_cache)
    : enable_cache_(enable_cache),
      stages_({{PipelineStageName::Image_Loading, enable_cache_, false},
               {PipelineStageName::Geometry_Adjustment, enable_cache_, false},
               {PipelineStageName::To_WorkingSpace, enable_cache_, true},
               {PipelineStageName::Basic_Adjustment, enable_cache_, true},
               {PipelineStageName::Color_Adjustment, enable_cache_, true},
               {PipelineStageName::Detail_Adjustment, enable_cache_, true},
               {PipelineStageName::Output_Transform, enable_cache_, true}}) {
  render_params_["resize"]                         = {};
  render_params_["resize"]["downsample_algorithm"] = ToResizeAlgorithmParam(
      ResizeDownsampleAlgorithm::Area);
  // Initialize default pipeline
  InitDefaultPipeline();
}

auto CPUPipelineExecutor::GetBackend() -> PipelineBackend { return backend_; }

auto CPUPipelineExecutor::GetStage(PipelineStageName stage) -> PipelineStage& {
  return stages_[static_cast<int>(stage)];
}

auto CPUPipelineExecutor::Apply(std::shared_ptr<ImageBuffer> input)
    -> std::shared_ptr<ImageBuffer> {
  const auto apply_start = ProfileClock::now();
  if (exec_stages_.empty()) {
    return input;
  }

  // The RAW decode backend follows the resolved runtime preference on every
  // render; a stage op replaced by direct SetOperator writes can never drift
  // the decode away from the user's backend setting.
  ApplyRuntimeRawDecodeBackend();

  std::vector<std::string> executor_steps;
  std::vector<std::string> stage_profiles;
  auto*                    first_stage = exec_stages_.front();
  if (!first_stage) {
    return input;
  }

  std::shared_ptr<ImageBuffer> output;
  // Before the merged GPU stream, re-trigger SetGlobalParams for operators
  // in stages that feed into it, so they pick up runtime data (e.g. raw decode
  // context) written by earlier non-merged stages.
  auto refresh_before_merged = [&](PipelineStage* stage) {
    if (stage->stage_ == PipelineStageName::Merged_Stage) {
      for (size_t i = static_cast<size_t>(PipelineStageName::To_WorkingSpace);
           i < stages_.size(); ++i) {
        stages_[i].RefreshGlobalParams(global_params_);
      }
    }
  };

  const auto apply_stage = [&](PipelineStage* stage) {
    const auto refresh_start = ProfileClock::now();
    refresh_before_merged(stage);
    const auto refresh_elapsed = ProfileClock::now() - refresh_start;

    const auto stage_start = ProfileClock::now();
    stage->SetInputImage(output);
    stage->SetForceCPUOutput(force_cpu_output_);
    output = stage->ApplyStage(global_params_);

    std::string stage_profile = stage->GetLastProfileSummary();
    if (stage_profile.empty()) {
      std::ostringstream fallback;
      fallback << "stage=" << stage->GetStageNameString()
               << " total=" << FormatDurationMs(DurationToMs(ProfileClock::now() - stage_start));
      stage_profile = fallback.str();
    } else {
      stage_profile +=
          " | executor_call=" + FormatDurationMs(DurationToMs(ProfileClock::now() - stage_start));
    }

    if (stage->stage_ == PipelineStageName::Merged_Stage) {
      stage_profile += " | refresh_global_params=" +
                       FormatDurationMs(DurationToMs(refresh_elapsed));
    }
    stage_profiles.push_back(std::move(stage_profile));
  };

  if (enable_cache_) {
    if (!first_stage->CacheValid()) {
      const auto clone_start = ProfileClock::now();
      output = std::make_shared<ImageBuffer>(input->Clone());
      executor_steps.push_back("clone_input=" +
                               FormatDurationMs(DurationToMs(ProfileClock::now() - clone_start)));
      for (auto* stage : exec_stages_) {
        apply_stage(stage);
      }
    } else {
      // If cache is valid, use cached output
      const auto cache_fetch_start = ProfileClock::now();
      output = first_stage->GetOutputCache();
      const auto cache_fetch_elapsed = ProfileClock::now() - cache_fetch_start;
      executor_steps.push_back("first_stage_cache_fetch=" +
                               FormatDurationMs(DurationToMs(cache_fetch_elapsed)));
      stage_profiles.push_back("stage=" + first_stage->GetStageNameString() +
                               " mode=executor_cache cache=hit total=" +
                               FormatDurationMs(DurationToMs(cache_fetch_elapsed)) +
                               " | get_output_cache=" +
                               FormatDurationMs(DurationToMs(cache_fetch_elapsed)));
      for (auto* stage : exec_stages_) {
        if (stage != first_stage) {
          apply_stage(stage);
        }
      }
    }
  } else {
    // Cache is disabled, just process the stages sequentially
    const auto clone_start = ProfileClock::now();
    output = std::make_shared<ImageBuffer>(input->Clone());
    executor_steps.push_back("clone_input=" +
                             FormatDurationMs(DurationToMs(ProfileClock::now() - clone_start)));
    for (auto* stage : exec_stages_) {
      apply_stage(stage);
    }
  }

  PrintPipelineProfile(apply_start, executor_steps, stage_profiles);
  return output;
}

[[deprecated("SetPreviewMode is deprecated, set from pipeline scheduler instead")]] void
CPUPipelineExecutor::SetPreviewMode(bool) {
  // is_thumbnail_  = is_thumbnail;

  // render_params_ = {};  // TODO: Use default params for now
  // if (!is_thumbnail_) {
  //   // Disable resizing in image loading stage
  //   stages_[static_cast<int>(PipelineStageName::Image_Loading)].EnableOperator(
  //       OperatorType::RESIZE,
  //       false);  // If RESIZE operator not exist, this function will do nothing
  //   return;
  // }
  // stages_[static_cast<int>(PipelineStageName::Geometry_Adjustment)].SetOperator(
  //     OperatorType::RESIZE, render_params_);
}

void CPUPipelineExecutor::SetExecutionStages() {
  ResolveAcceleratorBackend();
  ApplyAcceleratorBackendToStages();
  ApplyRuntimeRawDecodeBackend();

  exec_stages_.clear();
  frame_sink_ = nullptr;
  std::vector<PipelineStage*> streamable_stages;

  // Merged GPU stream stage is always re-executed; keeping its cache enabled only retains
  // transient objects without reuse value.
  auto merged = std::make_unique<PipelineStage>(PipelineStageName::Merged_Stage, false, true);
  merged->SetAcceleratorBackend(resolved_accelerator_backend_);

  exec_stages_.push_back(&stages_[static_cast<int>(PipelineStageName::Image_Loading)]);
  exec_stages_.push_back(&stages_[static_cast<int>(PipelineStageName::Geometry_Adjustment)]);
  exec_stages_.push_back(merged.get());

  merged_stages_ = std::move(merged);

  for (size_t i = 2; i < stages_.size(); i++) {
    PipelineStage& stage = stages_[i];
    stage.AddDependent(merged_stages_.get());
  }

  // Chain the execution stages for caching
  for (size_t i = 0; i < exec_stages_.size(); i++) {
    PipelineStage* prev_stage = (i > 0) ? exec_stages_[i - 1] : nullptr;
    PipelineStage* next_stage = (i < exec_stages_.size() - 1) ? exec_stages_[i + 1] : nullptr;
    exec_stages_[i]->SetNeighbors(prev_stage, next_stage);
  }

  for (auto* stage : exec_stages_) {
    const bool stage_cache_enabled = (stage->stage_ == PipelineStageName::Merged_Stage)
                                         ? false
                                         : enable_cache_;
    stage->SetEnableCache(stage_cache_enabled);
  }
}

void CPUPipelineExecutor::ResolveAcceleratorBackend() {
  resolved_accelerator_backend_ = alcedo::ResolveAcceleratorBackend(accelerator_preference_);
}

void CPUPipelineExecutor::ApplyAcceleratorBackendToStages() {
  for (auto& stage : stages_) {
    stage.SetAcceleratorBackend(resolved_accelerator_backend_);
  }
  if (merged_stages_) {
    merged_stages_->SetAcceleratorBackend(resolved_accelerator_backend_);
  }
}

void CPUPipelineExecutor::ApplyRuntimeRawDecodeBackend() {
  // The accelerator backend is a runtime property of this process: it comes
  // only from the user's backend setting (resolved_accelerator_backend_) and is
  // pushed directly into the RAW decode op. Persisted operator params can
  // never carry or override it.
  auto& raw_stage = stages_[static_cast<int>(PipelineStageName::Image_Loading)];
  auto  entry     = raw_stage.GetOperator(OperatorType::RAW_DECODE);
  if (!entry.has_value() || !entry.value() || !entry.value()->op_) {
    return;
  }

  auto* raw_op = dynamic_cast<RawDecodeOp*>(entry.value()->op_.get());
  if (!raw_op) {
    return;
  }
  raw_op->SetRuntimeGpuBackend(resolved_accelerator_backend_);
  SyncRawDecodeRuntimeControls();
}

void CPUPipelineExecutor::SyncRawDecodeRuntimeControls() {
  auto& raw_stage = stages_[static_cast<int>(PipelineStageName::Image_Loading)];
  auto  entry     = raw_stage.GetOperator(OperatorType::RAW_DECODE);
  if (!entry.has_value() || !entry.value() || !entry.value()->op_) {
    return;
  }

  auto* raw_op = dynamic_cast<RawDecodeOp*>(entry.value()->op_.get());
  if (!raw_op) {
    return;
  }
  raw_op->SetCancelRequested(cancel_requested_);
}

void CPUPipelineExecutor::SetCancelRequested(std::function<bool()> cancel_requested) {
  cancel_requested_ = std::move(cancel_requested);
  SyncRawDecodeRuntimeControls();
}

void CPUPipelineExecutor::SetAcceleratorBackendPreference(
    const AcceleratorBackendPreference preference) {
  const auto resolved_backend = alcedo::ResolveAcceleratorBackend(preference);
  if (accelerator_preference_ == preference &&
      resolved_accelerator_backend_ == resolved_backend) {
    // A replaced RAW decode op (e.g. after direct stage writes) must still
    // follow the active runtime backend; re-apply even when nothing else
    // changed.
    ApplyRuntimeRawDecodeBackend();
    return;
  }

  accelerator_preference_          = preference;
  resolved_accelerator_backend_    = resolved_backend;
  ApplyRuntimeRawDecodeBackend();

  const auto previous_frame_sink = frame_sink_;
  ResetExecutionStages();
  ApplyAcceleratorBackendToStages();
  if (previous_frame_sink != nullptr) {
    SetExecutionStages(previous_frame_sink);
  } else {
    SetExecutionStages();
  }
}

void CPUPipelineExecutor::SetExecutionStages(IFrameSink* frame_sink) {
  // Only (re)build the stage graph when it is not already built. The graph
  // rebuild recreates the merged GPU stage — and with it the CUDA/Metal/OpenCL
  // pipeline that owns the LLF highlight/shadow stage's cross-frame reference
  // cache (cached_reference_base_/cached_source_key_/cached_width_/...).
  // Wiping that cache every render defeats the 42ed19b CanReuseReferenceForRoi
  // path: zoomed ROI/detail frames can no longer sample the full-image mask and
  // recompute instead, flickering on every pan/zoom. The QML production path
  // re-attaches the same sink per render, so guard on merged_stages_ and only
  // update the sink pointer when the graph is already up. merged_stages_ is null
  // on first build and after ResetExecutionStages(); backend switches route
  // through SetAcceleratorBackendPreference, which resets before re-attaching,
  // so a null guard still triggers a full rebuild there.
  if (!merged_stages_) {
    SetExecutionStages();
  }
  frame_sink_ = frame_sink;

  // Set frame sink for the last stage
  if (!exec_stages_.empty()) {
    exec_stages_.back()->SetFrameSink(frame_sink);
  }
}

void CPUPipelineExecutor::ResetExecutionStages() {
  frame_sink_ = nullptr;
  for (auto& stage : stages_) {
    stage.ResetDependents();
    stage.ResetNeighbors();
    stage.ResetRuntimeResources(PipelineStage::RuntimeResetMode::ClearIntermediateBuffersAndGpu);
  }

  if (merged_stages_) {
    merged_stages_->ResetRuntimeResources(
        PipelineStage::RuntimeResetMode::ClearIntermediateBuffersAndGpu);
  }

  exec_stages_.clear();
  merged_stages_.reset();
}

auto CPUPipelineExecutor::ExportPipelineParams() const -> nlohmann::json {
  nlohmann::json j;
  for (const auto& stage : stages_) {
    nlohmann::json stage_json     = stage.ExportStageParams();
    j[stage.GetStageNameString()] = std::move(stage_json);
  }
  return j;
}

void CPUPipelineExecutor::ImportPipelineParams(const nlohmann::json& j) {
  ResetExecutionStages();
  ResetStages();
  SetTemplateParams();
  RegisterAllOperators();
  for (auto& stage : stages_) {
    std::string stage_name = stage.GetStageNameString();
    if (j.contains(stage_name)) {
      nlohmann::json stage_json = j[stage_name];
      stage.MergeStageParams(stage_json, global_params_);
    }
  }
  // The accelerator backend is a runtime property of this process, not part of
  // the persisted edit state. Re-apply the active runtime backend to the RAW
  // decode so a backend switch takes effect on every load, snapshot, and
  // recovery, regardless of what the stored params contain.
  ApplyRuntimeRawDecodeBackend();
  SetExecutionStages();
}

void CPUPipelineExecutor::SetRenderRegion(int x, int y, float scale_factor_x,
                                          float scale_factor_y, int reference_width,
                                          int reference_height) {
  const nlohmann::json prev_resize_params =
      render_params_.contains("resize") ? render_params_["resize"] : nlohmann::json::object();
  auto& resize_params = render_params_["resize"];

  const float clamped_scale_x = std::clamp(scale_factor_x, 1e-4f, 1.0f);
  const float clamped_scale_y =
      std::clamp((scale_factor_y > 0.0f) ? scale_factor_y : scale_factor_x, 1e-4f, 1.0f);
  resize_params["enable_roi"] = (clamped_scale_x < (1.0f - 1e-4f)) ||
                                (clamped_scale_y < (1.0f - 1e-4f));
  resize_params["roi"]        = {{"x", std::max(0, x)},
                                 {"y", std::max(0, y)},
                                 {"resize_factor_x", clamped_scale_x},
                                 {"resize_factor_y", clamped_scale_y},
                                 {"resize_factor", std::max(clamped_scale_x, clamped_scale_y)},
                                 {"reference_width", std::max(0, reference_width)},
                                 {"reference_height", std::max(0, reference_height)}};

  global_params_.render_roi_enabled_ = resize_params["enable_roi"].get<bool>();
  global_params_.render_roi_x_ = std::max(0, x);
  global_params_.render_roi_y_ = std::max(0, y);
  global_params_.render_roi_scale_x_ = clamped_scale_x;
  global_params_.render_roi_scale_y_ = clamped_scale_y;
  global_params_.render_roi_reference_width_ = std::max(0, reference_width);
  global_params_.render_roi_reference_height_ = std::max(0, reference_height);

  if (resize_params != prev_resize_params) {
    stages_[static_cast<int>(PipelineStageName::Geometry_Adjustment)].SetOperator(
        OperatorType::RESIZE, render_params_);
  }
}

void CPUPipelineExecutor::SetRenderRes(bool full_res, int max_side_length) {
  const nlohmann::json prev_resize_params =
      render_params_.contains("resize") ? render_params_["resize"] : nlohmann::json::object();
  auto& resize_params           = render_params_["resize"];
  // render_params_["resize"] = {
  //   {"enable_scale", true},
  //   {"maximum_edge", max_side_length},
  // };
  resize_params["enable_scale"] = !full_res;
  resize_params["maximum_edge"] = max_side_length;

  if (resize_params != prev_resize_params) {
    stages_[static_cast<int>(PipelineStageName::Geometry_Adjustment)].SetOperator(
        OperatorType::RESIZE, render_params_);
  }
}

void CPUPipelineExecutor::SetResizeDownsampleAlgorithm(ResizeDownsampleAlgorithm algorithm) {
  const nlohmann::json prev_resize_params =
      render_params_.contains("resize") ? render_params_["resize"] : nlohmann::json::object();
  auto& resize_params = render_params_["resize"];

  resize_params["downsample_algorithm"] = ToResizeAlgorithmParam(algorithm);

  if (resize_params != prev_resize_params) {
    stages_[static_cast<int>(PipelineStageName::Geometry_Adjustment)].SetOperator(
        OperatorType::RESIZE, render_params_);
  }
}

void CPUPipelineExecutor::SetDecodeRes(DecodeRes res) {
  decode_res_ = res;

  // TODO: Abstraction leak, need better design later
  auto& raw_stage = GetStage(PipelineStageName::Image_Loading);
  auto  raw_param = raw_stage.GetOperator(OperatorType::RAW_DECODE).value()->ExportOperatorParams();
  auto& params = raw_param["params"];
  if (!params.contains("raw") || !params["raw"].is_object()) {
    params["raw"] = nlohmann::json::object();
  }
  if (params["raw"].value("decode_res", -1) != static_cast<int>(res)) {
    params["raw"]["decode_res"] = static_cast<int>(res);
    raw_stage.SetOperator(OperatorType::RAW_DECODE, raw_param["params"]);
  }
  SyncRawDecodeRuntimeControls();
}

auto CPUPipelineExecutor::GetViewportRenderRegion() const -> std::optional<ViewportRenderRegion> {
  if (!frame_sink_) {
    return std::nullopt;
  }
  return frame_sink_->GetViewportRenderRegion();
}

void CPUPipelineExecutor::DetachFrameSink() {
  frame_sink_ = nullptr;
  if (!exec_stages_.empty()) {
    exec_stages_.back()->SetFrameSink(nullptr);
  }
}

void CPUPipelineExecutor::AttachFrameSink(IFrameSink* frame_sink) {
  frame_sink_ = frame_sink;
  if (!exec_stages_.empty()) {
    exec_stages_.back()->SetFrameSink(frame_sink);
  }
}

void CPUPipelineExecutor::SetNextFramePresentationMode(FramePresentationMode mode) const {
  if (!frame_sink_) {
    return;
  }
  frame_sink_->SetNextFramePresentationMode(mode);
}

void CPUPipelineExecutor::SetNextFramePreviewMetadata(const FramePreviewMetadata& metadata) const {
  if (!frame_sink_) {
    return;
  }
  frame_sink_->SetNextFramePreviewMetadata(metadata);
}

void CPUPipelineExecutor::RegisterAllOperators() {
  // It is really silly to hardcode the operators here.
  // I should keep things more flexible in the future.
  SetCleanBaselineAdjustableOperators(stages_, global_params_);
}

void CPUPipelineExecutor::ResetToCleanBaselineAdjustments() {
  SetCleanBaselineAdjustableOperators(stages_, global_params_);
}

void CPUPipelineExecutor::SetTemplateParams() {
  ResolveAcceleratorBackend();

  // Set some common parameters for template pipelines
  auto&          raw_stage     = GetStage(PipelineStageName::Image_Loading);
  auto&          global_params = GetGlobalParams();
  // Template raw params deliberately carry no accelerator backend: the backend
  // is pushed at runtime via ApplyRuntimeRawDecodeBackend.
  nlohmann::json decode_params = pipeline_defaults::MakeDefaultRawDecodeParams();
  raw_stage.SetOperator(OperatorType::RAW_DECODE, decode_params);

  nlohmann::json lens_params = pipeline_defaults::MakeDefaultLensCalibParams();
  raw_stage.SetOperator(OperatorType::LENS_CALIBRATION, lens_params, global_params);
  raw_stage.EnableOperator(OperatorType::LENS_CALIBRATION,
                           lens_params["lens_calib"].value("enabled", true), global_params);

  nlohmann::json color_temp_params;
  auto&          to_ws_stage = GetStage(PipelineStageName::To_WorkingSpace);
  color_temp_params["color_temp"] = {{"mode", "as_shot"}, {"cct", 6500.0f}, {"tint", 0.0f}};
  to_ws_stage.SetOperator(OperatorType::COLOR_TEMP, color_temp_params, global_params);

  nlohmann::json output_params;
  auto&          output_stage = GetStage(PipelineStageName::Output_Transform);
  output_params               = pipeline_defaults::MakeDefaultODTParams();
  output_stage.SetOperator(OperatorType::ODT, output_params, global_params);
  output_stage.SetOperator(OperatorType::FILM_GRAIN, pipeline_defaults::MakeDefaultFilmGrainParams(),
                           global_params);
  output_stage.SetOperator(OperatorType::HALATION, pipeline_defaults::MakeDefaultHalationParams(),
                           global_params);
}

void CPUPipelineExecutor::InitDefaultPipeline() {
  SetTemplateParams();
  RegisterAllOperators();
  SetExecutionStages();
}

void CPUPipelineExecutor::InjectRawMetadata(const RawRuntimeColorContext& ctx) {
  global_params_.PopulateRawMetadata(ctx);

  // Also propagate to RawDecodeOp so it uses the pre-populated context.
  auto& stage = stages_[static_cast<int>(PipelineStageName::Image_Loading)];
  auto  entry = stage.GetOperator(OperatorType::RAW_DECODE);
  if (entry.has_value()) {
    auto* raw_op = dynamic_cast<RawDecodeOp*>(entry.value()->op_.get());
    if (raw_op) {
      raw_op->SetPrePopulatedContext(ctx);
    }
  }

  stages_[static_cast<int>(PipelineStageName::Image_Loading)].RefreshGlobalParams(global_params_);
  stages_[static_cast<int>(PipelineStageName::To_WorkingSpace)].RefreshGlobalParams(global_params_);
}

void CPUPipelineExecutor::ClearAllIntermediateBuffers() {
  for (auto& stage : exec_stages_) {
    stage->ResetRuntimeResources(PipelineStage::RuntimeResetMode::ClearIntermediateBuffers);
  }

  if (merged_stages_) {
    merged_stages_->ResetRuntimeResources(PipelineStage::RuntimeResetMode::ClearIntermediateBuffers);
  }
}

void CPUPipelineExecutor::ReleasePreviewGpuScratch() {
  if (merged_stages_) {
    merged_stages_->ResetRuntimeResources(PipelineStage::RuntimeResetMode::ReleaseGpuScratch);
  }
}

void CPUPipelineExecutor::ReleaseAllGPUResources() {
  for (auto& stage : exec_stages_) {
    stage->ResetRuntimeResources(PipelineStage::RuntimeResetMode::ReleaseGpuResources);
  }

  if (merged_stages_) {
    merged_stages_->ResetRuntimeResources(PipelineStage::RuntimeResetMode::ReleaseGpuResources);
  }
}

auto CPUPipelineExecutor::DebugGetMergedStageScratchBytes() const -> size_t {
  return merged_stages_ ? merged_stages_->DebugGetAllocatedGpuScratchBytes() : 0;
}

auto CPUPipelineExecutor::DebugGetMergedStageIdentity() const -> std::uintptr_t {
  return reinterpret_cast<std::uintptr_t>(merged_stages_.get());
}

};  // namespace alcedo
