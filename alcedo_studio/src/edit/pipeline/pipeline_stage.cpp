//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/pipeline/pipeline_stage.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <iomanip>
#include <iostream>
#include <memory>
#include <opencv2/highgui.hpp>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "edit/operators/op_base.hpp"
#include "edit/operators/operator_factory.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace {
using ProfileClock = std::chrono::steady_clock;

struct BufferDimensions {
  int width  = 0;
  int height = 0;
};

auto GetBufferDimensions(const std::shared_ptr<ImageBuffer>& img) -> BufferDimensions {
  if (!img) {
    return {};
  }
  if (img->gpu_data_valid_) {
    return {img->GetGPUWidth(), img->GetGPUHeight()};
  }
  if (img->cpu_data_valid_) {
    auto& data = img->GetCPUData();
    return {data.cols, data.rows};
  }
  return {};
}

auto ComputeRenderOutputScaleAxis(int input_length, int output_length, bool roi_enabled,
                                  float roi_scale, int roi_reference_length) -> float {
  if (input_length <= 0 || output_length <= 0) {
    return 1.0f;
  }

  const float safe_roi_scale = std::clamp(roi_scale, 1e-4f, 1.0f);
  const int   reference_length =
      roi_reference_length > 0 ? roi_reference_length : std::max(input_length, 1);
  const float content_length =
      roi_enabled ? std::max(1.0f, static_cast<float>(reference_length) * safe_roi_scale)
                  : static_cast<float>(std::max(input_length, 1));
  return std::clamp(static_cast<float>(output_length) / content_length, 1e-4f, 1.0f);
}

void UpdateRenderOutputScale(OperatorParams& params, const BufferDimensions& input_dims,
                             const BufferDimensions& output_dims) {
  params.render_output_scale_x_ =
      ComputeRenderOutputScaleAxis(input_dims.width, output_dims.width, params.render_roi_enabled_,
                                   params.render_roi_scale_x_, params.render_roi_reference_width_);
  params.render_output_scale_y_ = ComputeRenderOutputScaleAxis(
      input_dims.height, output_dims.height, params.render_roi_enabled_, params.render_roi_scale_y_,
      params.render_roi_reference_height_);
}

auto DurationToMs(const ProfileClock::duration duration) -> double {
  return std::chrono::duration<double, std::milli>(duration).count();
}

auto FormatDurationMs(const double duration_ms) -> std::string {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << duration_ms << " ms";
  return oss.str();
}

class StageProfileCollector {
 public:
  StageProfileCollector(std::string stage_name, std::string mode)
      : stage_name_(std::move(stage_name)), mode_(std::move(mode)), start_(ProfileClock::now()) {}

  void SetCacheState(std::string cache_state) { cache_state_ = std::move(cache_state); }

  void AddDuration(std::string label, const ProfileClock::duration duration) {
    entries_.push_back(std::move(label) + "=" + FormatDurationMs(DurationToMs(duration)));
  }

  void AddNote(std::string note) { entries_.push_back(std::move(note)); }

  auto Finish() const -> std::string {
    std::ostringstream oss;
    oss << "stage=" << stage_name_ << " mode=" << mode_ << " cache=" << cache_state_
        << " total=" << FormatDurationMs(DurationToMs(ProfileClock::now() - start_));
    for (const auto& entry : entries_) {
      oss << " | " << entry;
    }
    return oss.str();
  }

 private:
  std::string              stage_name_;
  std::string              mode_;
  std::string              cache_state_ = "off";
  std::vector<std::string> entries_;
  ProfileClock::time_point start_;
};

auto IsResizeEffectivelyNoOp(const IOperatorBase& op, int width, int height) -> bool {
  const nlohmann::json params = op.GetParams();
  if (!params.contains("resize") || !params["resize"].is_object()) {
    return false;
  }

  const auto& resize            = params["resize"];
  const bool  enable_scale      = resize.value("enable_scale", false);
  const bool  enable_roi        = resize.value("enable_roi", false);
  const int   maximum_edge      = resize.value("maximum_edge", 2048);
  bool        roi_is_full_frame = !enable_roi;

  if (enable_roi) {
    if (!resize.contains("roi") || !resize["roi"].is_object()) {
      return false;
    }
    const auto& roi      = resize["roi"];
    const float factor   = roi.value("resize_factor", 1.0f);
    const float factor_x = roi.value("resize_factor_x", factor);
    const float factor_y = roi.value("resize_factor_y", factor);
    roi_is_full_frame    = roi.value("x", 0) == 0 && roi.value("y", 0) == 0 &&
                        factor_x >= (1.0f - 1e-4f) && factor_y >= (1.0f - 1e-4f);
  }

  if (!enable_scale && !enable_roi) {
    return true;
  }

  if (width <= 0 || height <= 0) {
    return false;
  }

  const bool scale_is_noop = !enable_scale || std::max(width, height) <= maximum_edge;
  return roi_is_full_frame && scale_is_noop;
}

auto IsCropRotateEffectivelyNoOp(const IOperatorBase& op) -> bool {
  const nlohmann::json params = op.GetParams();
  if (!params.contains("crop_rotate") || !params["crop_rotate"].is_object()) {
    return false;
  }

  const auto& crop_rotate = params["crop_rotate"];
  if (!crop_rotate.value("enabled", false)) {
    return true;
  }

  const bool enable_crop = crop_rotate.value("enable_crop", false);
  if (!enable_crop) {
    return true;
  }

  const float angle = crop_rotate.value("angle_degrees", 0.0f);
  if (std::abs(angle) > 1e-4f) {
    return false;
  }

  if (crop_rotate.contains("aspect_ratio_preset") &&
      crop_rotate["aspect_ratio_preset"].is_string()) {
    const std::string preset = crop_rotate["aspect_ratio_preset"].get<std::string>();
    if (preset != "free") {
      return false;
    }
  }

  if (!crop_rotate.contains("crop_rect") || !crop_rotate["crop_rect"].is_object()) {
    return true;
  }
  const auto& rect = crop_rotate["crop_rect"];
  const float x    = rect.value("x", 0.0f);
  const float y    = rect.value("y", 0.0f);
  const float w    = rect.value("w", 1.0f);
  const float h    = rect.value("h", 1.0f);

  return std::abs(x) <= 1e-4f && std::abs(y) <= 1e-4f && std::abs(w - 1.0f) <= 1e-4f &&
         std::abs(h - 1.0f) <= 1e-4f;
}

auto CanShareGeometryGpuInputWithoutCopy(const std::map<OperatorType, OperatorEntry>& operators)
    -> bool {
  for (const auto& [op_type, op_entry] : operators) {
    if (!op_entry.enable_ || !op_entry.op_) {
      continue;
    }

    if (op_type != OperatorType::RESIZE && op_type != OperatorType::CROP_ROTATE) {
      return false;
    }
  }

  return true;
}
}  // namespace

PipelineStage::PipelineStage(PipelineStageName stage, bool enable_cache, bool is_streamable)
    : is_streamable_(is_streamable), enable_cache_(enable_cache), stage_(stage) {
  stage_role_ = DetermineStageRole(stage_, is_streamable_, accelerator_backend_);
  operators_  = std::make_unique<std::map<OperatorType, OperatorEntry>>();
  if (stage_ == PipelineStageName::Image_Loading) {
    input_cache_valid_ = true;  // No input for image loading stage, so input cache is always valid
  }
}

void PipelineStage::SetAcceleratorBackend(const GpuBackendKind backend) {
  if (accelerator_backend_ == backend) {
    return;
  }

  accelerator_backend_     = backend;
  const auto previous_role = stage_role_;
  stage_role_              = DetermineStageRole(stage_, is_streamable_, accelerator_backend_);
  gpu_executor_.SetBackend(stage_ == PipelineStageName::Merged_Stage ? accelerator_backend_
                                                                     : GpuBackendKind::None);
  gpu_setup_done_ = false;

  if (previous_role != stage_role_) {
    ResetRuntimeResources(RuntimeResetMode::ClearIntermediateBuffersAndGpu);
  } else {
    ResetRuntimeResources(RuntimeResetMode::InvalidateCache);
  }
}

void PipelineStage::SetOperator(OperatorType op_type, nlohmann::json param) {
  auto it = operators_->find(op_type);
  if (it == operators_->end()) {
    auto op = OperatorFactory::Instance().Create(op_type, param);
    operators_->emplace(op_type, OperatorEntry{true, op});
    SetOutputCacheValid(false);
    if (dependents_) dependents_->SetOutputCacheValid(false);
  } else {
    if (it->second.op_) {
      const auto current_param = it->second.op_->GetParams();
      if (current_param == param) {
        return;
      }
    }
    (it->second).op_->SetParams(param);
    SetOutputCacheValid(false);
    if (dependents_) dependents_->SetOutputCacheValid(false);
  }
}

void PipelineStage::SetOperator(OperatorType op_type, nlohmann::json param,
                                OperatorParams& global_params) {
  SetOperator(op_type, param);
  auto it = operators_->find(op_type);
  if (it != operators_->end()) {
    it->second.op_->SetGlobalParams(global_params);
  }
}

auto PipelineStage::GetOperator(OperatorType op_type) const -> std::optional<OperatorEntry*> {
  auto it = operators_->find(op_type);
  if (it == operators_->end()) {
    return std::nullopt;
  }
  return &(it->second);
}

void PipelineStage::EnableOperator(OperatorType op_type, bool enable) {
  auto it = operators_->find(op_type);
  if (it != operators_->end()) {
    if (it->second.enable_ != enable) {
      SetOutputCacheValid(false);
      if (dependents_) dependents_->SetInputCacheValid(false);
    }
    it->second.enable_ = enable;
  }
}

void PipelineStage::EnableOperator(OperatorType op_type, bool enable,
                                   OperatorParams& global_params) {
  EnableOperator(op_type, enable);
  auto it = operators_->find(op_type);
  if (it != operators_->end()) {
    // Apply enable/disable state first so SetGlobalParams sees the correct flag.
    it->second.op_->EnableGlobalParams(global_params, enable);
    it->second.op_->SetGlobalParams(global_params);
  }
}

void PipelineStage::SetInputImage(std::shared_ptr<ImageBuffer> input) {
  input_img_ = input;
  input_set_ = true;
}

void PipelineStage::SetInputCacheValid(bool valid) {
  input_cache_valid_ = valid;
  if (!valid) {
    // Invalidate output cache if input cache is invalidated
    input_img_.reset();
    SetOutputCacheValid(false);
  }
}

void PipelineStage::SetOutputCacheValid(bool valid) {
  if (output_cache_valid_ != valid && !valid) {
    output_cache_.reset();
    if (next_stage_) {
      next_stage_->SetInputCacheValid(false);
    }
  }
  output_cache_valid_ = valid;
}

void PipelineStage::RefreshGlobalParams(OperatorParams& global_params) {
  if (!operators_) return;
  for (auto& [op_type, op_entry] : *operators_) {
    if (op_entry.enable_ && op_entry.op_) {
      op_entry.op_->SetGlobalParams(global_params);
    }
  }
}

auto PipelineStage::ApplyStage(OperatorParams& global_params) -> std::shared_ptr<ImageBuffer> {
  if (!input_set_) {
    throw std::runtime_error("PipelineExecutor: No valid input image set");
  }

  switch (stage_role_) {
    case StageRole::DescriptorOnly:
      return ApplyDescriptorOnly();
    case StageRole::CpuOperators:
      return ApplyCpuOperators(global_params);
    case StageRole::GpuStreamable:
      return ApplyGpuStream(global_params);
    case StageRole::GpuOperators:
      return ApplyGpuOperators(global_params);
  }

  // Fallback, should never hit.
  return input_img_;
}

auto PipelineStage::HasInput() -> bool { return input_set_; }
void PipelineStage::ResetAll() {
  operators_->clear();
  input_img_.reset();
  output_cache_.reset();
  last_profile_summary_.clear();
  input_set_          = false;
  input_cache_valid_  = false;
  output_cache_valid_ = false;
  dependents_         = nullptr;
  prev_stage_         = nullptr;
  next_stage_         = nullptr;
  input_img_          = nullptr;
  output_cache_       = nullptr;
  input_set_          = false;
}

void PipelineStage::ResetRuntimeResources(RuntimeResetMode mode) {
  const auto clear_intermediate_buffers = [this]() {
    input_img_.reset();
    output_cache_.reset();
    last_profile_summary_.clear();
    input_set_          = false;
    input_cache_valid_  = false;
    output_cache_valid_ = false;
  };

  const auto release_gpu_resources = [this]() {
    if (stage_role_ != StageRole::GpuStreamable) {
      return;
    }
    gpu_executor_.ReleaseResources();
    gpu_setup_done_ = false;
  };

  const auto release_gpu_scratch = [this]() {
    if (stage_role_ != StageRole::GpuStreamable) {
      return;
    }
    gpu_executor_.ReleaseScratchBuffers();
  };

  switch (mode) {
    case RuntimeResetMode::InvalidateCache:
      output_cache_.reset();
      last_profile_summary_.clear();
      output_cache_valid_ = false;
      if (next_stage_) {
        next_stage_->SetInputCacheValid(false);
      }
      return;
    case RuntimeResetMode::ClearIntermediateBuffers:
      clear_intermediate_buffers();
      return;
    case RuntimeResetMode::ReleaseGpuScratch:
      release_gpu_scratch();
      return;
    case RuntimeResetMode::ReleaseGpuResources:
      release_gpu_resources();
      return;
    case RuntimeResetMode::ClearIntermediateBuffersAndGpu:
      clear_intermediate_buffers();
      release_gpu_resources();
      return;
  }
}

auto PipelineStage::DetermineStageRole(PipelineStageName stage, bool is_streamable,
                                       GpuBackendKind backend) -> StageRole {
  switch (stage) {
    case PipelineStageName::Image_Loading:
    case PipelineStageName::Geometry_Adjustment:
      return IsImplementedGeometryOperatorBackend(backend) ? StageRole::GpuOperators
                                                           : StageRole::CpuOperators;
    case PipelineStageName::Merged_Stage:
      return StageRole::GpuStreamable;
    default:
      (void)is_streamable;
      return StageRole::DescriptorOnly;
  }
}

bool PipelineStage::HasEnabledOperator() const {
  return std::any_of(operators_->begin(), operators_->end(),
                     [](const auto& entry) { return entry.second.enable_; });
}

std::shared_ptr<ImageBuffer> PipelineStage::ApplyDescriptorOnly() {
  StageProfileCollector profile(GetStageNameString(), "descriptor");

  if (!enable_cache_) {
    profile.SetCacheState("off");
    profile.AddNote("pass_through");
    SetOutputCacheValid(false);
    if (next_stage_) {
      next_stage_->SetInputCacheValid(false);
    }
    last_profile_summary_ = profile.Finish();
    return input_img_;
  }

  if (CacheValid()) {
    profile.SetCacheState("hit");
    profile.AddNote("reuse_stage_cache");
    if (next_stage_ && next_stage_->CacheValid()) {
      profile.AddNote("downstream_cache_also_valid");
      last_profile_summary_ = profile.Finish();
      return nullptr;
    }
    last_profile_summary_ = profile.Finish();
    return output_cache_;
  }

  profile.SetCacheState("miss");
  output_cache_ = input_img_;
  SetOutputCacheValid(true);
  if (next_stage_) {
    next_stage_->SetInputCacheValid(true);
  }

  profile.AddNote("cache_store_input_reference");
  last_profile_summary_ = profile.Finish();
  return output_cache_;
}

std::shared_ptr<ImageBuffer> PipelineStage::ApplyGpuOperators(OperatorParams& global_params) {
  StageProfileCollector profile(GetStageNameString(), "gpu_ops");

  auto                  execute_ops = [&]() {
    if (!HasEnabledOperator()) {
      profile.AddNote("no_enabled_ops");
      return input_img_;
    }

    const bool             is_geometry_stage = stage_ == PipelineStageName::Geometry_Adjustment;
    const BufferDimensions geometry_input_dims =
        is_geometry_stage ? GetBufferDimensions(input_img_) : BufferDimensions{};

    if (stage_ == PipelineStageName::Geometry_Adjustment) {
      const int width       = geometry_input_dims.width;
      const int height      = geometry_input_dims.height;

      bool      has_enabled = false;
      bool      all_noop    = true;
      for (const auto& [op_type, op_entry] : *operators_) {
        if (!op_entry.enable_ || !op_entry.op_) {
          continue;
        }
        has_enabled = true;
        if ((op_type == OperatorType::RESIZE &&
             !IsResizeEffectivelyNoOp(*op_entry.op_, width, height)) ||
            (op_type == OperatorType::CROP_ROTATE && !IsCropRotateEffectivelyNoOp(*op_entry.op_)) ||
            (op_type != OperatorType::RESIZE && op_type != OperatorType::CROP_ROTATE)) {
          all_noop = false;
          break;
        }
      }

      // Geometry stage is frequently configured as "full-res passthrough" during editing.
      // Reuse upstream cache directly to avoid holding another full-resolution GpuMat.
      if (has_enabled && all_noop) {
        profile.AddNote("geometry_noop_passthrough");
        UpdateRenderOutputScale(global_params, geometry_input_dims, geometry_input_dims);
        return input_img_;
      }
    }

    auto current_img = std::make_shared<ImageBuffer>();
    if (input_img_->gpu_data_valid_ && !input_img_->buffer_valid_) {
      const bool can_share_input = stage_ == PipelineStageName::Geometry_Adjustment &&
                                   CanShareGeometryGpuInputWithoutCopy(*operators_);
      if (can_share_input) {
        const auto share_start = ProfileClock::now();
        current_img->ShareGPUDataFrom(*input_img_);
        profile.AddDuration("share_input_gpu", ProfileClock::now() - share_start);
      } else {
        const auto alloc_start = ProfileClock::now();
        current_img->InitGPUData(input_img_->GetGPUWidth(), input_img_->GetGPUHeight(),
                                                  input_img_->GetGPUType(), accelerator_backend_);
        profile.AddDuration("alloc_stage_gpu", ProfileClock::now() - alloc_start);
        const auto copy_start = ProfileClock::now();
        input_img_->CopyGPUDataTo(*current_img);
        current_img->gpu_data_valid_ = true;
        profile.AddDuration("copy_input_gpu", ProfileClock::now() - copy_start);
      }
    } else if (input_img_->buffer_valid_) {
      const auto materialize_start = ProfileClock::now();
      auto       buffer            = input_img_->GetBuffer();
      current_img = std::make_shared<ImageBuffer>(std::move(buffer));
      profile.AddDuration("materialize_input_buffer", ProfileClock::now() - materialize_start);
    }

    const auto apply_gpu_operator = [&](OperatorType op_type, const OperatorEntry& op_entry) {
      if (!op_entry.enable_ || !op_entry.op_) {
        return;
      }
      const auto op_start = ProfileClock::now();
      if (op_type == OperatorType::LENS_CALIBRATION) {
        if (!current_img->gpu_data_valid_ && current_img->cpu_data_valid_) {
          current_img->SyncToGPU(accelerator_backend_);
        }
        op_entry.op_->SetGlobalParams(global_params);
        op_entry.op_->ApplyGPU(current_img);
      } else {
        if (op_type != OperatorType::RAW_DECODE && !current_img->gpu_data_valid_ &&
            current_img->cpu_data_valid_) {
          current_img->SyncToGPU(accelerator_backend_);
        }
        op_entry.op_->ApplyGPU(current_img);
        op_entry.op_->SetGlobalParams(global_params);
      }
      profile.AddDuration("op:" + OperatorTypeToString(op_type), ProfileClock::now() - op_start);
    };

    if (stage_ == PipelineStageName::Geometry_Adjustment) {
      if (const auto crop_it = operators_->find(OperatorType::CROP_ROTATE);
          crop_it != operators_->end()) {
        apply_gpu_operator(crop_it->first, crop_it->second);
      }
      for (const auto& [op_type, op_entry] : *operators_) {
        if (op_type == OperatorType::CROP_ROTATE) {
          continue;
        }
        apply_gpu_operator(op_type, op_entry);
      }
    } else {
      for (const auto& [op_type, op_entry] : *operators_) {
        apply_gpu_operator(op_type, op_entry);
      }
    }
    current_img->gpu_data_valid_ = true;
    if (is_geometry_stage) {
      UpdateRenderOutputScale(global_params, geometry_input_dims, GetBufferDimensions(current_img));
    }
    return current_img;
  };

  if (!input_img_->gpu_data_valid_ && !input_img_->buffer_valid_) {
    const auto sync_start = ProfileClock::now();
    input_img_->SyncToGPU(accelerator_backend_);
    profile.AddDuration("sync_input_to_gpu", ProfileClock::now() - sync_start);
  }

  // This is different from ApplyGpuStream, as this function applies individual GPU operators
  if (!enable_cache_) {
    profile.SetCacheState("off");
    SetOutputCacheValid(false);
    if (next_stage_) {
      next_stage_->SetInputCacheValid(false);
    }
    output_cache_ = execute_ops();
    SetOutputCacheValid(false);
    if (next_stage_) {
      next_stage_->SetInputCacheValid(false);
    }
    last_profile_summary_ = profile.Finish();
    return output_cache_;
  }

  if (CacheValid()) {
    profile.SetCacheState("hit");
    profile.AddNote("reuse_stage_cache");
    if (next_stage_ && next_stage_->CacheValid()) {
      profile.AddNote("downstream_cache_also_valid");
      last_profile_summary_ = profile.Finish();
      return nullptr;
    }
    last_profile_summary_ = profile.Finish();
    return output_cache_;
  }

  profile.SetCacheState("miss");
  output_cache_ = execute_ops();
  SetOutputCacheValid(true);
  if (next_stage_) {
    next_stage_->SetInputCacheValid(true);
  }
  last_profile_summary_ = profile.Finish();
  return output_cache_;
}

std::shared_ptr<ImageBuffer> PipelineStage::ApplyCpuOperators(OperatorParams& global_params) {
  StageProfileCollector profile(GetStageNameString(), "cpu_ops");

  auto                  execute_ops = [&]() {
    if (!HasEnabledOperator()) {
      profile.AddNote("no_enabled_ops");
      return input_img_;
    }

    const bool             is_geometry_stage = stage_ == PipelineStageName::Geometry_Adjustment;
    const BufferDimensions geometry_input_dims =
        is_geometry_stage ? GetBufferDimensions(input_img_) : BufferDimensions{};

    if (stage_ == PipelineStageName::Geometry_Adjustment) {
      const int width       = geometry_input_dims.width;
      const int height      = geometry_input_dims.height;

      bool      has_enabled = false;
      bool      all_noop    = true;
      for (const auto& [op_type, op_entry] : *operators_) {
        if (!op_entry.enable_ || !op_entry.op_) {
          continue;
        }
        has_enabled = true;
        if ((op_type == OperatorType::RESIZE &&
             !IsResizeEffectivelyNoOp(*op_entry.op_, width, height)) ||
            (op_type == OperatorType::CROP_ROTATE && !IsCropRotateEffectivelyNoOp(*op_entry.op_)) ||
            (op_type != OperatorType::RESIZE && op_type != OperatorType::CROP_ROTATE)) {
          all_noop = false;
          break;
        }
      }

      if (has_enabled && all_noop) {
        profile.AddNote("geometry_noop_passthrough");
        UpdateRenderOutputScale(global_params, geometry_input_dims, geometry_input_dims);
        return input_img_;
      }
    }

    const auto clone_start = ProfileClock::now();
    auto       current_img = std::make_shared<ImageBuffer>(input_img_->Clone());
    profile.AddDuration("clone_input", ProfileClock::now() - clone_start);

    const auto apply_cpu_operator = [&](OperatorType op_type, const OperatorEntry& op_entry) {
      if (!op_entry.enable_ || !op_entry.op_) {
        return;
      }
      const auto op_start = ProfileClock::now();
      if (op_type == OperatorType::LENS_CALIBRATION) {
        op_entry.op_->SetGlobalParams(global_params);
        op_entry.op_->Apply(current_img);
      } else {
        op_entry.op_->Apply(current_img);
        op_entry.op_->SetGlobalParams(global_params);
      }
      profile.AddDuration("op:" + OperatorTypeToString(op_type), ProfileClock::now() - op_start);
    };

    if (stage_ == PipelineStageName::Geometry_Adjustment) {
      if (const auto crop_it = operators_->find(OperatorType::CROP_ROTATE);
          crop_it != operators_->end()) {
        apply_cpu_operator(crop_it->first, crop_it->second);
      }
      for (const auto& [op_type, op_entry] : *operators_) {
        if (op_type == OperatorType::CROP_ROTATE) {
          continue;
        }
        apply_cpu_operator(op_type, op_entry);
      }
    } else {
      for (const auto& [op_type, op_entry] : *operators_) {
        apply_cpu_operator(op_type, op_entry);
      }
    }
    if (is_geometry_stage) {
      UpdateRenderOutputScale(global_params, geometry_input_dims, GetBufferDimensions(current_img));
    }
    return current_img;
  };

  if (!enable_cache_) {
    profile.SetCacheState("off");
    output_cache_ = execute_ops();
    SetOutputCacheValid(false);
    if (next_stage_) {
      next_stage_->SetInputCacheValid(false);
    }
    last_profile_summary_ = profile.Finish();
    return output_cache_;
  }

  if (CacheValid()) {
    profile.SetCacheState("hit");
    profile.AddNote("reuse_stage_cache");
    if (next_stage_ && next_stage_->CacheValid()) {
      profile.AddNote("downstream_cache_also_valid");
      last_profile_summary_ = profile.Finish();
      return nullptr;
    }
    last_profile_summary_ = profile.Finish();
    return output_cache_;
  }

  profile.SetCacheState("miss");
  output_cache_ = execute_ops();
  SetOutputCacheValid(true);
  if (next_stage_) {
    next_stage_->SetInputCacheValid(true);
    if (next_stage_->stage_role_ == StageRole::GpuStreamable &&
        next_stage_->gpu_executor_.HasAcceleratedBackend() && !output_cache_->gpu_data_valid_) {
      if (!output_cache_->cpu_data_valid_) {
        throw std::runtime_error(
            "PipelineStage: cannot prepare GPU input for merged stage without CPU or GPU data.");
      }
      const auto sync_start = ProfileClock::now();
      output_cache_->SyncToGPU(accelerator_backend_);
      profile.AddDuration("prepare_next_stage_gpu_input", ProfileClock::now() - sync_start);
    }
  }
  last_profile_summary_ = profile.Finish();
  return output_cache_;
}

std::shared_ptr<ImageBuffer> PipelineStage::ApplyGpuStream(OperatorParams& global_params) {
  StageProfileCollector profile(
      GetStageNameString(), gpu_executor_.HasAcceleratedBackend() ? "gpu_stream" : "cpu_stream");
  profile.SetCacheState("off");

  if (!gpu_executor_.HasAcceleratedBackend()) {
    if (!input_img_->cpu_data_valid_ && input_img_->gpu_data_valid_) {
      const auto sync_start = ProfileClock::now();
      input_img_->SyncToCPU();
      profile.AddDuration("sync_input_to_cpu", ProfileClock::now() - sync_start);
    }

    if (!static_tile_scheduler_) {
      const auto scheduler_setup_start = ProfileClock::now();
      SetStaticTileScheduler();
      profile.AddDuration("setup_static_tile_scheduler",
                          ProfileClock::now() - scheduler_setup_start);
    } else {
      const auto update_input_start = ProfileClock::now();
      static_tile_scheduler_->SetInputImage(input_img_);
      profile.AddDuration("update_static_tile_input", ProfileClock::now() - update_input_start);
    }

    const auto execute_start = ProfileClock::now();
    output_cache_            = static_tile_scheduler_->ApplyOps(global_params);
    profile.AddDuration("execute_static_tile_stream", ProfileClock::now() - execute_start);
    SetOutputCacheValid(false);
    if (next_stage_) {
      next_stage_->SetInputCacheValid(false);
    }
    last_profile_summary_ = profile.Finish();
    return output_cache_;
  }

  if (!gpu_setup_done_) {
    const auto setup_start = ProfileClock::now();
    SetGPUExecutor();
    profile.AddDuration("setup_gpu_executor", ProfileClock::now() - setup_start);
  }

  output_cache_               = std::make_shared<ImageBuffer>();
  const auto set_params_start = ProfileClock::now();
  gpu_executor_.SetParams(global_params);
  profile.AddDuration("set_fused_params", ProfileClock::now() - set_params_start);
  const auto set_input_start = ProfileClock::now();
  gpu_executor_.SetInputImage(input_img_);
  profile.AddDuration("set_gpu_input", ProfileClock::now() - set_input_start);
  const auto execute_start = ProfileClock::now();
  gpu_executor_.Execute(output_cache_);
  profile.AddDuration("execute_gpu_stream", ProfileClock::now() - execute_start);

  if (force_cpu_output_) {
    try {
      const auto sync_start = ProfileClock::now();
      output_cache_->SyncToCPU();
      profile.AddDuration("sync_stream_output_to_cpu", ProfileClock::now() - sync_start);
      const auto release_start = ProfileClock::now();
      output_cache_->ReleaseGPUData();  // Free GPU memory after sync
      profile.AddDuration("release_stream_gpu_output", ProfileClock::now() - release_start);
    } catch (const std::exception& e) {
      // Keep GPU result if CPU sync fails; caller will validate.
      profile.AddNote(std::string("sync_stream_output_to_cpu_failed=") + e.what());
      std::cerr << std::format(
                       "Failed to sync GPU stream output to CPU: {}. Keeping GPU data for "
                       "downstream stages.",
                       e.what())
                << std::endl;
    }
  }

  // GPU stage output is not cached; downstream stages receive fresh data.
  SetOutputCacheValid(false);
  if (next_stage_) {
    next_stage_->SetInputCacheValid(false);
  }
  last_profile_summary_ = profile.Finish();
  return output_cache_;
}

auto PipelineStage::GetStageNameString() const -> std::string {
  switch (stage_) {
    case PipelineStageName::Image_Loading:
      return "Image Loading";
    case PipelineStageName::To_WorkingSpace:
      return "To Working Space";
    case PipelineStageName::Basic_Adjustment:
      return "Basic Adjustment";
    case PipelineStageName::Color_Adjustment:
      return "Color Adjustment";
    case PipelineStageName::Detail_Adjustment:
      return "Detail Adjustment";
    case PipelineStageName::Output_Transform:
      return "Output Transform";
    case PipelineStageName::Geometry_Adjustment:
      return "Geometry Adjustment";
    case PipelineStageName::Merged_Stage:
      return "Merged Stage";
    default:
      return "Unknown Stage";
  }
}

auto PipelineStage::ExportStageParams() const -> nlohmann::json {
  nlohmann::json inner;
  for (const auto& [op_type, op_entry] : *operators_) {
    if (!op_entry.op_ || op_entry.op_->GetOperatorType() == OperatorType::RESIZE ||
        op_entry.op_->GetOperatorType() == OperatorType::UNKNOWN) {
      continue;
    }
    inner[op_entry.op_->GetScriptName()] = op_entry.ExportOperatorParams();
  }
  nlohmann::json j;
  j.emplace(GetStageNameString(), std::move(inner));
  return j;
}

void PipelineStage::ImportStageParams(const nlohmann::json& j) {
  ResetAll();

  std::string stage_name = GetStageNameString();
  if (!j.contains(stage_name)) {
    return;
  }
  nlohmann::json stage_json = j[stage_name];
  for (auto& [op_name, op_json] : stage_json.items()) {
    if (!op_json.contains("params")) {
      continue;
    }
    nlohmann::json params  = op_json.value("params", nlohmann::json::object());
    OperatorType   op_type = op_json.value("type", OperatorType::UNKNOWN);
    if (op_type != OperatorType::UNKNOWN && op_type != OperatorType::RESIZE) {
      SetOperator(op_type, params);
      EnableOperator(op_type, op_json.value("enable", true));
    }
  }
}

void PipelineStage::ImportStageParams(const nlohmann::json& j, OperatorParams& global_params) {
  ResetAll();
  MergeStageParams(j, global_params);
}

void PipelineStage::MergeStageParams(const nlohmann::json& j, OperatorParams& global_params) {
  std::string stage_name = GetStageNameString();
  if (!j.contains(stage_name)) {
    return;
  }
  nlohmann::json stage_json = j[stage_name];
  for (auto& [op_name, op_json] : stage_json.items()) {
    if (!op_json.contains("params")) {
      continue;
    }
    nlohmann::json params  = op_json.value("params", nlohmann::json::object());
    OperatorType   op_type = op_json.value("type", OperatorType::UNKNOWN);
    if (op_type != OperatorType::UNKNOWN && op_type != OperatorType::RESIZE) {
      SetOperator(op_type, params, global_params);
      EnableOperator(op_type, op_json.value("enable", true), global_params);
    }
  }
}
};  // namespace alcedo
