//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_support/controllers/pipeline_controller.hpp"

#include <json.hpp>

#include "edit/pipeline/default_pipeline_params.hpp"
#include "image/image.hpp"
namespace alcedo::ui::controllers {
namespace {

auto MergeMissingDefaults(nlohmann::json& target, const nlohmann::json& defaults) -> bool {
  if (!defaults.is_object()) {
    return false;
  }
  bool changed = false;
  if (!target.is_object()) {
    target  = nlohmann::json::object();
    changed = true;
  }
  for (auto it = defaults.begin(); it != defaults.end(); ++it) {
    if (!target.contains(it.key())) {
      target[it.key()] = it.value();
      changed          = true;
      continue;
    }
    if (it.value().is_object()) {
      if (!target[it.key()].is_object()) {
        target[it.key()] = it.value();
        changed          = true;
        continue;
      }
      for (auto nested_it = it.value().begin(); nested_it != it.value().end(); ++nested_it) {
        if (!target[it.key()].contains(nested_it.key())) {
          target[it.key()][nested_it.key()] = nested_it.value();
          changed                           = true;
        }
      }
    }
  }
  return changed;
}

void SyncOperatorEnabledFromParams(PipelineStage& stage, OperatorType type, const char* root_key,
                                   OperatorParams& global_params) {
  const auto op = stage.GetOperator(type);
  if (!op.has_value() || !op.value() || !op.value()->op_) {
    return;
  }

  bool       enabled = op.value()->enable_;
  const auto params  = op.value()->op_->GetParams();
  if (params.contains(root_key) && params[root_key].is_object()) {
    enabled = params[root_key].value("enabled", enabled);
  }
  stage.EnableOperator(type, enabled, global_params);
}

}  // namespace

void EnsureLoadingOperatorDefaults(const std::shared_ptr<CPUPipelineExecutor>& exec) {
  if (!exec) {
    return;
  }

  auto&      global_params   = exec->GetGlobalParams();
  auto&      loading         = exec->GetStage(PipelineStageName::Image_Loading);

  const auto ensure_defaults = [&](OperatorType type, const nlohmann::json& defaults,
                                   bool        pass_global_params,
                                   const char* enabled_root_key = nullptr) {
    const auto op = loading.GetOperator(type);
    if (!op.has_value() || !op.value() || !op.value()->op_) {
      if (pass_global_params) {
        loading.SetOperator(type, defaults, global_params);
      } else {
        loading.SetOperator(type, defaults);
      }
    } else {
      nlohmann::json params   = nlohmann::json::object();
      const auto     exported = op.value()->ExportOperatorParams();
      if (exported.contains("params") && exported["params"].is_object()) {
        params = exported["params"];
      }
      if (MergeMissingDefaults(params, defaults)) {
        if (pass_global_params) {
          loading.SetOperator(type, params, global_params);
        } else {
          loading.SetOperator(type, params);
        }
      }
    }

    if (pass_global_params && enabled_root_key) {
      SyncOperatorEnabledFromParams(loading, type, enabled_root_key, global_params);
    }
  };

  ensure_defaults(OperatorType::RAW_DECODE, pipeline_defaults::MakeDefaultRawDecodeParams(), false);
  ensure_defaults(OperatorType::LENS_CALIBRATION, pipeline_defaults::MakeDefaultLensCalibParams(),
                  true, "lens_calib");
}

void RebuildBaselinePipelineForImage(const std::shared_ptr<CPUPipelineExecutor>& exec,
                                     const std::shared_ptr<ImagePoolService>&    image_pool,
                                     image_id_t                                  image_id) {
  if (!exec) {
    return;
  }

  EnsureLoadingOperatorDefaults(exec);
  exec->ResetToCleanBaselineAdjustments();

  if (image_pool && image_id != 0) {
    try {
      auto img = image_pool->Read<std::shared_ptr<Image>>(
          image_id, [](const std::shared_ptr<Image>& image) { return image; });
      if (img && img->HasRawColorContext()) {
        exec->InjectRawMetadata(img->GetRawColorContext());
      }
    } catch (...) {
      // Match editor startup: metadata injection is best-effort for non-RAW inputs.
    }
  }

  exec->SetExecutionStages();
}

void AttachExecutionStages(const std::shared_ptr<CPUPipelineExecutor>& exec,
                           IFrameSink*                                 frame_sink) {
  if (!exec) {
    return;
  }
  exec->SetExecutionStages(frame_sink);
}

}  // namespace alcedo::ui::controllers
